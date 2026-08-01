#include "runtime/undo.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace strata::runtime {
namespace {
constexpr std::size_t maximum_entries = 200U;
constexpr std::int64_t coalescing_window_nanos = 600'000'000;

[[nodiscard]] std::string normalized_label(std::string label) {
    return label.empty() ? std::string("Change") : std::move(label);
}

[[nodiscard]] StateSnapshot patched_snapshot(
    const StateSnapshot& current,
    const StateSnapshot& expected,
    const StateSnapshot& target
) {
    std::map<StateAddress, StateSnapshotEntry> values;
    for (const StateSnapshotEntry& entry : current.entries) values.emplace(entry.address, entry);
    std::map<StateAddress, StateSnapshotEntry> expected_values;
    for (const StateSnapshotEntry& entry : expected.entries) {
        expected_values.emplace(entry.address, entry);
    }
    std::map<StateAddress, StateSnapshotEntry> target_values;
    for (const StateSnapshotEntry& entry : target.entries) target_values.emplace(entry.address, entry);
    std::set<StateAddress> addresses;
    for (const auto& [address, entry] : expected_values) {
        static_cast<void>(entry);
        addresses.insert(address);
    }
    for (const auto& [address, entry] : target_values) {
        static_cast<void>(entry);
        addresses.insert(address);
    }
    for (const StateAddress& address : addresses) {
        const auto before = expected_values.find(address);
        const auto after = target_values.find(address);
        const bool changed = (before == expected_values.end()) != (after == target_values.end()) ||
            (before != expected_values.end() && after != target_values.end() &&
             before->second != after->second);
        if (!changed) continue;
        values.erase(address);
        if (after != target_values.end()) values.emplace(address, after->second);
    }
    StateSnapshot result;
    result.entries.reserve(values.size());
    for (auto& [address, entry] : values) {
        static_cast<void>(address);
        result.entries.push_back(std::move(entry));
    }
    return result;
}

} // namespace

UndoManager::UndoManager(InvalidationDiagnostic invalidation_diagnostic)
    : invalidation_diagnostic_(std::move(invalidation_diagnostic)) {}

void UndoManager::record(
    std::string scope,
    StateSnapshot before,
    StateSnapshot after,
    UndoRecordOptions options
) {
    if (scope.empty()) throw std::invalid_argument("undo scope must not be empty");
    if (before == after) return;
    Stack& stack = stacks_[std::move(scope)];
    Entry entry{
        std::move(before),
        std::move(after),
        normalized_label(std::move(options.label)),
        std::move(options.coalesce_key),
        options.timestamp_nanos,
    };
    if (stack.group_depth != 0U) {
        if (!stack.pending_group.has_value()) {
            stack.pending_group = std::move(entry);
        } else {
            stack.pending_group->after = std::move(entry.after);
            stack.pending_group->timestamp_nanos = entry.timestamp_nanos;
        }
        return;
    }
    append(stack, std::move(entry));
}

void UndoManager::begin_group(std::string scope, std::string label) {
    if (scope.empty()) throw std::invalid_argument("undo scope must not be empty");
    Stack& stack = stacks_[std::move(scope)];
    if (stack.group_depth++ == 0U) {
        stack.pending_group.reset();
        stack.group_label = std::move(label);
    }
}

void UndoManager::end_group(const std::string_view scope, const bool commit) {
    const auto found = stacks_.find(scope);
    if (found == stacks_.end() || found->second.group_depth == 0U) return;
    Stack& stack = found->second;
    if (--stack.group_depth != 0U) return;
    if (commit && stack.pending_group.has_value() &&
        stack.pending_group->before != stack.pending_group->after) {
        if (!stack.group_label.empty()) stack.pending_group->label = std::move(stack.group_label);
        append(stack, std::move(*stack.pending_group));
    }
    stack.pending_group.reset();
    stack.group_label.clear();
}

bool UndoManager::undo(const std::string_view scope, StateStore& state) {
    const auto found = stacks_.find(scope);
    if (found == stacks_.end() || found->second.undo.empty()) return false;
    Stack& stack = found->second;
    Entry entry = std::move(stack.undo.back());
    stack.undo.pop_back();
    if (!state.restore(patched_snapshot(state.snapshot(), entry.after, entry.before))) {
        stack.undo.push_back(std::move(entry));
        return false;
    }
    stack.redo.push_back(std::move(entry));
    ++generation_;
    return true;
}

bool UndoManager::redo(const std::string_view scope, StateStore& state) {
    const auto found = stacks_.find(scope);
    if (found == stacks_.end() || found->second.redo.empty()) return false;
    Stack& stack = found->second;
    Entry entry = std::move(stack.redo.back());
    stack.redo.pop_back();
    if (!state.restore(patched_snapshot(state.snapshot(), entry.before, entry.after))) {
        stack.redo.push_back(std::move(entry));
        return false;
    }
    stack.undo.push_back(std::move(entry));
    ++generation_;
    return true;
}

void UndoManager::invalidate(
    const std::string_view scope,
    const std::string_view action_id
) {
    const auto found = stacks_.find(scope);
    if (found == stacks_.end()) return;
    Stack& stack = found->second;
    const bool populated = !stack.undo.empty() || !stack.redo.empty() ||
        stack.pending_group.has_value();
    stack = Stack{};
    if (populated) ++generation_;
    if (populated && invalidation_diagnostic_) invalidation_diagnostic_(scope, action_id);
}

void UndoManager::invalidate_other_scopes(
    const std::string_view preserved_scope,
    const std::string_view action_id
) {
    std::vector<std::string> scopes;
    for (const auto& [scope, stack] : stacks_) {
        static_cast<void>(stack);
        if (scope != preserved_scope) scopes.push_back(scope);
    }
    for (const std::string& scope : scopes) invalidate(scope, action_id);
}

void UndoManager::clear(const std::string_view scope) {
    const auto found = stacks_.find(scope);
    if (found == stacks_.end()) return;
    stacks_.erase(found);
    ++generation_;
}
void UndoManager::clear_all() noexcept {
    if (!stacks_.empty()) ++generation_;
    stacks_.clear();
}

UndoStackStatus UndoManager::status(const std::string_view scope) const {
    const auto found = stacks_.find(scope);
    if (found == stacks_.end()) return {};
    const Stack& stack = found->second;
    UndoStackStatus result;
    result.can_undo = !stack.undo.empty();
    result.can_redo = !stack.redo.empty();
    if (result.can_undo) result.undo_label = stack.undo.back().label;
    if (result.can_redo) result.redo_label = stack.redo.back().label;
    return result;
}

std::uint64_t UndoManager::generation() const noexcept { return generation_; }

void UndoManager::append(Stack& stack, Entry entry) {
    if (entry.before == entry.after) return;
    if (!stack.undo.empty() && entry.coalesce_key.has_value()) {
        Entry& previous = stack.undo.back();
        const std::int64_t elapsed = entry.timestamp_nanos - previous.timestamp_nanos;
        if (previous.coalesce_key == entry.coalesce_key && elapsed >= 0 &&
            elapsed <= coalescing_window_nanos) {
            previous.after = std::move(entry.after);
            previous.label = std::move(entry.label);
            previous.timestamp_nanos = entry.timestamp_nanos;
            if (previous.before == previous.after) stack.undo.pop_back();
            stack.redo.clear();
            ++generation_;
            return;
        }
    }
    stack.undo.push_back(std::move(entry));
    if (stack.undo.size() > maximum_entries) stack.undo.erase(stack.undo.begin());
    stack.redo.clear();
    ++generation_;
}

} // namespace strata::runtime
