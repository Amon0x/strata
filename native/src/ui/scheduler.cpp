#include "ui/scheduler.hpp"

#include <exception>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace strata::ui {

struct WorkScheduler::Core final {
    struct Key final {
        WorkKind kind;
        std::uint64_t owner_identity;
        std::string name;
        [[nodiscard]] friend bool operator<(const Key& left, const Key& right) noexcept {
            return std::tie(left.kind, left.owner_identity, left.name) <
                   std::tie(right.kind, right.owner_identity, right.name);
        }
    };

    struct Entry final {
        WorkId id;
        Key key;
        Callback callback;
    };

    explicit Core(ErrorHandler handler) : error_handler(std::move(handler)) {}

    [[nodiscard]] bool cancel(const WorkId id) noexcept {
        const auto found = entries.find(id);
        if (found == entries.end()) return false;
        keys.erase(found->second.key);
        entries.erase(found);
        return true;
    }

    [[nodiscard]] std::size_t cancel_owner(const std::uint64_t owner_identity) noexcept {
        std::vector<WorkId> cancelled;
        for (const auto& [id, entry] : entries) {
            if (entry.key.owner_identity == owner_identity) cancelled.push_back(id);
        }
        for (const WorkId id : cancelled) static_cast<void>(cancel(id));
        return cancelled.size();
    }

    ErrorHandler error_handler;
    WorkId next_id = 0U;
    std::map<WorkId, Entry> entries;
    std::map<Key, WorkId> keys;
};

WorkScheduler::WorkScheduler(ErrorHandler error_handler)
    : core_(std::make_shared<Core>(std::move(error_handler))) {}

WorkScheduler::~WorkScheduler() { clear(); }

WorkId WorkScheduler::schedule(
    const WorkKind kind,
    const std::uint64_t owner_identity,
    std::string key,
    Callback callback
) {
    if (key.empty()) throw std::invalid_argument("scheduled work key must not be empty");
    if (!callback) throw std::invalid_argument("scheduled work callback must not be empty");
    Core::Key index{kind, owner_identity, std::move(key)};
    if (const auto found = core_->keys.find(index); found != core_->keys.end()) {
        core_->entries.at(found->second).callback = std::move(callback);
        return found->second;
    }
    if (core_->next_id == std::numeric_limits<WorkId>::max()) {
        throw std::overflow_error("scheduled work identity space exhausted");
    }
    const WorkId id = ++core_->next_id;
    core_->keys.emplace(index, id);
    core_->entries.emplace(id, Core::Entry{id, std::move(index), std::move(callback)});
    return id;
}

bool WorkScheduler::cancel(const WorkId id) noexcept { return core_->cancel(id); }

std::size_t WorkScheduler::cancel_owner(const std::uint64_t owner_identity) noexcept {
    return core_->cancel_owner(owner_identity);
}

void WorkScheduler::bind_owner(RetainedNode& owner) {
    const std::weak_ptr<Core> weak = core_;
    const std::uint64_t identity = owner.identity();
    owner.add_cleanup([weak, identity] {
        if (const std::shared_ptr<Core> core = weak.lock(); core != nullptr) {
            static_cast<void>(core->cancel_owner(identity));
        }
    });
}

WorkTickResult WorkScheduler::tick(const std::int64_t now_nanos) {
    WorkTickResult result;
    std::vector<WorkId> active;
    active.reserve(core_->entries.size());
    for (const auto& [id, entry] : core_->entries) {
        static_cast<void>(entry);
        active.push_back(id);
    }
    for (const WorkId id : active) {
        const auto found = core_->entries.find(id);
        if (found == core_->entries.end()) continue;
        const WorkKind kind = found->second.key.kind;
        const std::uint64_t owner_identity = found->second.key.owner_identity;
        const std::string key = found->second.key.name;
        const Callback callback = found->second.callback;
        ++result.visited;
        bool remains_active = false;
        try {
            remains_active = callback(now_nanos);
        } catch (const std::exception& error) {
            ++result.failed;
            if (core_->error_handler) {
                core_->error_handler(kind, owner_identity, key, error.what());
            }
        } catch (...) {
            ++result.failed;
            if (core_->error_handler) {
                core_->error_handler(kind, owner_identity, key, "unknown scheduled-work failure");
            }
        }
        if (!remains_active) {
            if (core_->cancel(id)) ++result.completed;
        }
    }
    result.remaining = core_->entries.size();
    return result;
}

std::size_t WorkScheduler::active_count() const noexcept { return core_->entries.size(); }

std::size_t WorkScheduler::active_count(const WorkKind kind) const noexcept {
    std::size_t count = 0U;
    for (const auto& [id, entry] : core_->entries) {
        static_cast<void>(id);
        if (entry.key.kind == kind) ++count;
    }
    return count;
}

void WorkScheduler::clear() noexcept {
    core_->entries.clear();
    core_->keys.clear();
}

} // namespace strata::ui
