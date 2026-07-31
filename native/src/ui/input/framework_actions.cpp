#include "ui/input.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/action.hpp"
#include "runtime/expression.hpp"
#include "runtime/value.hpp"
#include "ui/command.hpp"
#include "ui/input/detail.hpp"
#include "ui/status.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {
using namespace input_detail;

std::optional<runtime::ActionDispatchOutcome> InputRouter::execute_surface_action(
    const runtime::ActionEvent& event,
    const runtime::Action& action,
    const RetainedNode& node,
    InputOperationResult& result
) {
    static_cast<void>(event);
    const std::string& id = action.id();
    const auto surface_outcome = [&action](
                                     const runtime::ActionDispatchStatus status,
                                     std::optional<std::string> message = std::nullopt
                                 ) {
        return runtime::ActionDispatchOutcome{
            status,
            action.id(),
            {"strata.surface.declarative"},
            std::move(message),
        };
    };
    if (id == "focus.request") {
        const std::string* key = string_value(action.payload.field("key"));
        RetainedNode* target = key != nullptr && tree_ != nullptr ? tree_->find_key(*key) : nullptr;
        if (target == nullptr || !focusable(*target)) {
            return surface_outcome(
                runtime::ActionDispatchStatus::failed,
                "Focus target '" + std::string(key != nullptr ? *key : "") +
                    "' is not attached or focusable."
            );
        }
        focus(*target, "programmatic", result);
        return surface_outcome(runtime::ActionDispatchStatus::handled);
    }
    if (id == "focus.clear") {
        if (!focused_.has_value()) return surface_outcome(runtime::ActionDispatchStatus::ignored);
        clear_focus("programmatic", result);
        return surface_outcome(runtime::ActionDispatchStatus::handled);
    }
    if (id == "reveal.request") {
        const std::string* key = string_value(action.payload.field("key"));
        if (key == nullptr || key->empty()) {
            return surface_outcome(
                runtime::ActionDispatchStatus::failed,
                "Reveal action requires a keyed target."
            );
        }
        const std::string* scroll = string_value(action.payload.field("scroll"));
        std::erase_if(pending_reveals_, [key](const PendingReveal& pending) {
            return pending.key == *key;
        });
        pending_reveals_.push_back(PendingReveal{
            *key,
            scroll != nullptr ? std::optional<std::string>(*scroll) : std::nullopt,
            boolean_value(action.payload.field("focus"), false),
            number_value(action.payload.field("padding"), 6.0),
            0U,
        });
        return surface_outcome(runtime::ActionDispatchStatus::handled);
    }
    if (id.starts_with("tree.")) return execute_tree_action(action, result);
    if (id == "palette.set") {
        const std::string* key = string_value(action.payload.field("key"));
        RetainedNode* palette = key != nullptr && tree_ != nullptr ? tree_->find_key(*key) : nullptr;
        if (palette == nullptr && key == nullptr && tree_ != nullptr) {
            const std::vector<RetainedNode*>* palettes = tree_->find_type("CommandPalette");
            if (palettes != nullptr && !palettes->empty()) palette = palettes->front();
        }
        if (palette == nullptr || palette->description().type != "CommandPalette") {
            return surface_outcome(
                runtime::ActionDispatchStatus::failed,
                key != nullptr
                    ? std::optional("Command palette '" + *key + "' is not attached.")
                    : std::optional<std::string>("Command palette is not attached.")
            );
        }
        const bool open = boolean_value(action.payload.field("open"), true);
        if (open) dismiss_transient_popups(palette, result);
        static_cast<void>(tree_->set_retained_value(
            palette->identity(),
            "strata.palette.open",
            runtime::Value(open),
            DirtyReason::input
        ));
        static_cast<void>(tree_->set_retained_value(
            palette->identity(), "$paletteActive", runtime::Value(0.0), DirtyReason::input
        ));
        if (!open) static_cast<void>(clear_editor_text(*palette, result));
        sync_modal_focus(result);
        return surface_outcome(runtime::ActionDispatchStatus::handled);
    }
    if (id == "notification.raise") {
        const std::string* message = string_value(action.payload.field("message"));
        if (message == nullptr || blank(*message)) {
            return surface_outcome(
                runtime::ActionDispatchStatus::failed,
                "Notification message must not be blank."
            );
        }
        const std::string* severity_value = string_value(action.payload.field("severity"));
        NotificationSeverity severity = NotificationSeverity::info;
        if (severity_value != nullptr) {
            std::string normalized = *severity_value;
            std::ranges::transform(normalized, normalized.begin(), [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            if (normalized == "success") severity = NotificationSeverity::success;
            else if (normalized == "warning") severity = NotificationSeverity::warning;
            else if (normalized == "error") severity = NotificationSeverity::error;
        }
        std::optional<std::int64_t> timeout;
        if (const runtime::Value* value = action.payload.field("timeoutMillis");
            value != nullptr && value->number() != nullptr) {
            if (!std::isfinite(*value->number()) || *value->number() < 1.0) {
                return surface_outcome(
                    runtime::ActionDispatchStatus::failed,
                    "Notification timeout must be at least one millisecond."
                );
            }
            constexpr std::int64_t maximum_timeout_millis =
                std::numeric_limits<std::int64_t>::max() / 1'000'000LL;
            timeout = static_cast<std::int64_t>(std::min(
                *value->number(),
                static_cast<double>(maximum_timeout_millis)
            ));
        }
        static_cast<void>(notifications_.raise(NotificationRequest{
            *message,
            severity,
            boolean_value(action.payload.field("persistent"), false),
            timeout,
            {},
            nullptr,
        }));
        return surface_outcome(runtime::ActionDispatchStatus::handled, *message);
    }
    if (id == "command.execute") return execute_command_action(action, node, result);
    if (id == "environment.set" && surface_framework_executor_) {
        return surface_framework_executor_(action);
    }
    static_cast<void>(node);
    return std::nullopt;
}

runtime::ActionDispatchOutcome InputRouter::execute_tree_action(
    const runtime::Action& action,
    InputOperationResult& result
) {
    const std::string* tree_key = string_value(action.payload.field("tree"));
    const std::string* item_key = string_value(action.payload.field("item"));
    RetainedNode* tree = tree_key != nullptr && tree_ != nullptr ? tree_->find_key(*tree_key) : nullptr;
    const auto failure = [&action, tree_key, item_key] {
        const std::string operation = action.id().substr(std::string("tree.").size());
        return runtime::ActionDispatchOutcome{
            runtime::ActionDispatchStatus::failed,
            action.id(),
            {"strata.surface.declarative"},
            "Tree '" + std::string(tree_key != nullptr ? *tree_key : "") + "' cannot " +
                operation + " '" + std::string(item_key != nullptr ? *item_key : "") + "'.",
        };
    };
    if (tree == nullptr || tree->description().type != "TreeView" || item_key == nullptr) {
        return failure();
    }
    const runtime::Value* items = scalar_property(*tree, "items");
    if (items == nullptr || items->list() == nullptr) return failure();
    std::map<std::string, std::optional<std::string>, std::less<>> parents;
    for (const runtime::Value& item : items->list()->values) {
        const std::string* key = string_value(item.field("key"));
        if (key == nullptr) continue;
        const std::string* parent = string_value(item.field("parentKey"));
        parents.insert_or_assign(
            *key,
            parent != nullptr ? std::optional<std::string>(*parent) : std::nullopt
        );
    }
    if (!parents.contains(*item_key)) return failure();

    std::set<std::string, std::less<>> expanded;
    const runtime::Value* retained = tree->retained_value("strata.tree.expanded");
    const runtime::Value* source_expanded = retained != nullptr
                                                ? retained
                                                : scalar_property(*tree, "defaultExpandedKeys");
    if (source_expanded != nullptr && source_expanded->list() != nullptr) {
        for (const runtime::Value& value : source_expanded->list()->values) {
            if (const std::string* key = string_value(&value); key != nullptr) expanded.insert(*key);
        }
    }
    const auto store = [this, tree](const std::set<std::string, std::less<>>& keys) {
        std::vector<runtime::Value> values;
        values.reserve(keys.size());
        for (const std::string& key : keys) values.emplace_back(runtime::KeyValue{key});
        static_cast<void>(tree_->set_retained_value(
            tree->identity(),
            "strata.tree.expanded",
            runtime::Value(std::move(values)),
            DirtyReason::structure
        ));
    };
    const auto publish = [this, tree, &result](
                             const std::string& key,
                             const bool is_expanded,
                             const std::set<std::string, std::less<>>& keys
                         ) {
        std::vector<JsonValue> encoded_keys;
        encoded_keys.reserve(keys.size());
        for (const std::string& current : keys) encoded_keys.emplace_back(current);
        std::shared_ptr<const runtime::ActionValue> event_action;
        const auto property = tree->description().properties.find("onExpansionChange");
        if (property != tree->description().properties.end() && property->second.action() != nullptr) {
            event_action = *property->second.action();
        }
        JsonValue event = object({
            {"action", event_action != nullptr ? canonical_action(*event_action, *tree) : JsonValue{}},
            {"expanded", JsonValue(is_expanded)},
            {"expandedKeys", array(std::move(encoded_keys))},
            {"key", JsonValue(key)},
            {"source", source(*tree)},
            {"type", JsonValue("tree-expansion-changed")},
        });
        emit(std::move(event), event_action, *tree, runtime::Value(runtime::KeyValue{key}), result);
    };

    bool changed = false;
    if (action.id() == "tree.collapse") {
        changed = expanded.erase(*item_key) != 0U;
        if (changed) {
            store(expanded);
            publish(*item_key, false, expanded);
        }
    } else if (action.id() == "tree.expand") {
        changed = expanded.insert(*item_key).second;
        if (changed) {
            store(expanded);
            publish(*item_key, true, expanded);
        }
    } else if (action.id() == "tree.toggle") {
        const bool next = !expanded.contains(*item_key);
        if (next) expanded.insert(*item_key);
        else expanded.erase(*item_key);
        changed = true;
        store(expanded);
        publish(*item_key, next, expanded);
    } else if (action.id() == "tree.reveal") {
        std::vector<std::string> requested;
        std::optional<std::string> parent = parents.at(*item_key);
        while (parent.has_value()) {
            if (!expanded.contains(*parent)) requested.push_back(*parent);
            const auto found = parents.find(*parent);
            parent = found != parents.end() ? found->second : std::nullopt;
        }
        for (auto iterator = requested.rbegin(); iterator != requested.rend(); ++iterator) {
            expanded.insert(*iterator);
            store(expanded);
            publish(*iterator, true, expanded);
        }
        static_cast<void>(tree_->set_retained_value(
            tree->identity(),
            "strata.collection.active",
            runtime::Value(runtime::KeyValue{*item_key}),
            DirtyReason::semantics
        ));
        changed = true;
    }
    if (!changed) return failure();
    return runtime::ActionDispatchOutcome{
        runtime::ActionDispatchStatus::handled,
        action.id(),
        {"strata.surface.declarative"},
        std::nullopt,
    };
}

InputRouter::CommandInvocation InputRouter::invoke_command(
    const std::string_view id,
    const RetainedNode& source_node,
    const bool invoked_from_active_modal,
    InputOperationResult& result
) {
    const CommandSnapshot* command = commands_ != nullptr ? commands_->find(id) : nullptr;
    CommandInvocation invocation;
    RetainedNode* scope = nullptr;
    if (command == nullptr) {
        invocation.status = "unknown";
        invocation.message = "Command '" + std::string(id) + "' is not registered.";
    } else if (!command->enabled) {
        invocation.status = "disabled";
        invocation.message = "Command '" + command->id + "' is disabled.";
    } else {
        if (command->owning_scope.has_value() && tree_ != nullptr) {
            scope = tree_->find_key(*command->owning_scope);
        }
        const RetainedNode* focused = focused_.has_value() && tree_ != nullptr
                                          ? tree_->find_identity(*focused_)
                                          : nullptr;
        RetainedNode* modal = active_modal();
        if (command->owning_scope.has_value() && scope == nullptr) {
            invocation.status = "out_of_scope";
            invocation.message = "Command scope '" + *command->owning_scope + "' is not attached.";
        } else if (scope != nullptr && (focused == nullptr || !descendant_of(*focused, *scope))) {
            invocation.status = "out_of_scope";
            invocation.message = "Command '" + command->id + "' is outside the focused scope.";
        } else if (modal != nullptr && scope == nullptr && !command->allowed_while_modal &&
                   !invoked_from_active_modal) {
            invocation.status = "blocked_by_modal";
            invocation.message = "Command '" + command->id + "' is blocked by the active modal.";
        } else if (modal != nullptr && scope != nullptr && !descendant_of(*scope, *modal)) {
            invocation.status = "blocked_by_modal";
            invocation.message = "Command scope is outside the active modal.";
        } else {
            invocation.status = "executed";
            invocation.message = "Executed " + command->label + ".";
            invocation.executed = true;
            commands_->record_execution(command->id);
        }
    }

    const std::shared_ptr<const runtime::ActionValue> event_action =
        invocation.executed && command != nullptr ? command->action : nullptr;
    JsonValue event = object({
        {"action", event_action != nullptr ? canonical_action(*event_action, source_node) : JsonValue{}},
        {"commandId", JsonValue(std::string(id))},
        {"commandMessage", JsonValue(invocation.message)},
        {"commandStatus", JsonValue(invocation.status)},
        {"source", source(source_node)},
        {"type", JsonValue("command-invoked")},
    });
    emit(std::move(event), event_action, source_node, runtime::Value{}, result);
    if (command == nullptr) {
        pending_diagnostics_.push_back(runtime::RuntimeDiagnostic{
            "STRATA.UI.COMMAND_UNKNOWN_REFERENCE",
            invocation.message,
            std::string(source_node.structural_path()),
            "an enabled command available in the active focus scope",
            runtime::DiagnosticSeverity::error,
            std::nullopt,
        });
    }
    return invocation;
}

runtime::ActionDispatchOutcome InputRouter::execute_command_action(
    const runtime::Action& action,
    const RetainedNode& source_node,
    InputOperationResult& result
) {
    const std::string* authored_id = string_value(action.payload.field("id"));
    const std::string_view id = authored_id != nullptr ? std::string_view(*authored_id)
                                                       : std::string_view{};
    RetainedNode* modal = active_modal();
    const RetainedNode* event_source = focused_.has_value() && tree_ != nullptr
                                           ? tree_->find_identity(*focused_)
                                           : tree_ != nullptr ? tree_->root() : &source_node;
    const CommandInvocation invocation = invoke_command(
        id,
        *event_source,
        modal != nullptr && descendant_of(source_node, *modal),
        result
    );
    return runtime::ActionDispatchOutcome{
        invocation.executed ? runtime::ActionDispatchStatus::handled
            : invocation.status == "disabled" ? runtime::ActionDispatchStatus::ignored
                                                : runtime::ActionDispatchStatus::failed,
        action.id(),
        {"strata.surface.declarative"},
        invocation.message,
    };
}

bool InputRouter::route_command_shortcut(
    const std::string_view key,
    const KeyModifiers modifiers,
    const KeyEventType type,
    const bool editing_owns_key,
    InputOperationResult& result
) {
    if (type == KeyEventType::release) return false;
    if (commands_ == nullptr || tree_ == nullptr || tree_->root() == nullptr) return false;
    struct Candidate final {
        const CommandSnapshot* command = nullptr;
        RetainedNode* scope = nullptr;
        int depth = -1;
        bool executable = false;
    };
    std::vector<Candidate> candidates;
    RetainedNode* focused = focused_.has_value() ? tree_->find_identity(*focused_) : nullptr;
    RetainedNode* modal = active_modal();
    for (const auto& [id, command] : commands_->entries()) {
        static_cast<void>(id);
        if (!command.shortcut_key.has_value() || *command.shortcut_key != key ||
            command.shortcut_shift != modifiers.shift ||
            command.shortcut_control != modifiers.control ||
            command.shortcut_alt != modifiers.alt ||
            command.shortcut_super != modifiers.super_key ||
            (type == KeyEventType::repeat && !command.accepts_repeat) ||
            (editing_owns_key && !command.allowed_while_text_editing)) {
            continue;
        }
        RetainedNode* scope = command.owning_scope.has_value()
                                  ? tree_->find_key(*command.owning_scope)
                                  : nullptr;
        int depth = -1;
        for (RetainedNode* node = scope; node != nullptr; node = node->parent()) ++depth;
        const bool scope_attached = !command.owning_scope.has_value() || scope != nullptr;
        const bool in_scope = scope == nullptr || (focused != nullptr && descendant_of(*focused, *scope));
        const bool modal_allowed = modal == nullptr ||
            (scope != nullptr ? descendant_of(*scope, *modal) : command.allowed_while_modal);
        candidates.push_back(Candidate{
            &command,
            scope,
            depth,
            command.enabled && scope_attached && in_scope && modal_allowed,
        });
    }
    if (candidates.empty()) return false;
    const auto ranking = [](const Candidate& left, const Candidate& right) {
        return left.depth != right.depth ? left.depth < right.depth
                                         : left.command->priority < right.command->priority;
    };
    const Candidate* selected = nullptr;
    const Candidate* selected_executable = nullptr;
    for (const Candidate& candidate : candidates) {
        if (selected == nullptr || ranking(*selected, candidate)) selected = &candidate;
        if (candidate.executable && (selected_executable == nullptr ||
                                     ranking(*selected_executable, candidate))) {
            selected_executable = &candidate;
        }
    }
    if (selected_executable != nullptr) selected = selected_executable;
    const RetainedNode* event_source = selected->scope != nullptr
                                           ? selected->scope
                                           : focused != nullptr ? focused : tree_->root();
    static_cast<void>(invoke_command(selected->command->id, *event_source, false, result));
    return true;
}
} // namespace strata::ui
