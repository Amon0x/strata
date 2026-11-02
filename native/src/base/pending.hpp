#pragma once

#include <atomic>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "base/task_pool.hpp"

namespace strata::base {

enum class PendingStatus : std::uint8_t { pending, ready, failed };

template <typename T>
class Pending;

/** Retained resource identity; only the serialized owner publishes readiness or metadata changes. */
class Readiness final {
  public:
    [[nodiscard]] PendingStatus status() const noexcept {
        return state_ != nullptr ? state_->status.load(std::memory_order_acquire)
                                 : PendingStatus::failed;
    }
    [[nodiscard]] std::uint64_t id() const noexcept {
        return state_ != nullptr ? state_->id : 0U;
    }
    /** Zero for one-shot preparation; a nonzero revision observes repeated ready publications. */
    [[nodiscard]] std::uint64_t revision() const noexcept {
        return state_ != nullptr ? state_->revision.load(std::memory_order_acquire) : 0U;
    }
    [[nodiscard]] TaskKind kind() const noexcept {
        return state_ != nullptr ? state_->kind : TaskKind::other;
    }
    [[nodiscard]] std::string_view label() const noexcept {
        return state_ != nullptr ? std::string_view(state_->label) : std::string_view{};
    }
    [[nodiscard]] TaskOutcome failure() const noexcept {
        return status() == PendingStatus::failed && state_ != nullptr ? state_->failure
                                                                     : TaskOutcome{};
    }
    [[nodiscard]] friend bool operator==(const Readiness& left, const Readiness& right) noexcept {
        return left.state_ == right.state_;
    }

  private:
    template <typename T>
    friend class Pending;
    friend class ResourceRevision;
    struct State {
        State(const std::uint64_t identity, const TaskKind category, std::string name)
            : id(identity), kind(category), label(std::move(name)) {}
        ~State() { cancellation.cancel(); }

        const std::uint64_t id;
        const TaskKind kind;
        const std::string label;
        std::atomic<PendingStatus> status{PendingStatus::pending};
        std::atomic<std::uint64_t> revision{0U};
        TaskOutcome failure;
        TaskCancellation cancellation;
    };
    std::shared_ptr<const State> state_;
};

/** A cached operation's observation of preparation state or repeatedly published metadata. */
struct ReadinessRead final {
    Readiness resource;
    PendingStatus observed = PendingStatus::pending;
    std::uint64_t observed_revision = resource.revision();
    [[nodiscard]] bool current() const noexcept {
        return resource.status() == observed &&
               (observed_revision == 0U || resource.revision() == observed_revision);
    }
    [[nodiscard]] bool watching() const noexcept {
        return observed == PendingStatus::pending || observed_revision != 0U;
    }
    [[nodiscard]] friend bool operator==(const ReadinessRead&, const ReadinessRead&) = default;
};

/**
 * Owner-published metadata changes, independent of pending work and pixel-only updates. A ready
 * resource can change or disappear repeatedly without pretending that preparation is pending.
 */
class ResourceRevision final {
  public:
    ResourceRevision(TaskOwner& owner, const TaskKind kind, std::string label)
        : state_(std::make_shared<Readiness::State>(owner.next_resource_identity(), kind,
                                                  std::move(label))) {
        state_->status.store(PendingStatus::ready, std::memory_order_relaxed);
        state_->revision.store(1U, std::memory_order_relaxed);
    }
    ResourceRevision(ResourceRevision&&) noexcept = default;
    ResourceRevision& operator=(ResourceRevision&&) noexcept = default;
    ResourceRevision(const ResourceRevision&) = delete;
    ResourceRevision& operator=(const ResourceRevision&) = delete;
    [[nodiscard]] Readiness readiness() const noexcept {
        Readiness result;
        result.state_ = state_;
        return result;
    }
    void publish() noexcept {
        state_->revision.fetch_add(1U, std::memory_order_release);
    }
    /** Owner-only retirement check; no worker can create a new observation of this signal. */
    [[nodiscard]] bool has_readers() const noexcept {
        return state_.use_count() > 1;
    }

  private:
    std::shared_ptr<Readiness::State> state_;
};

/**
 * A cache lookup with an optional drawable value and a preparation dependency. An absent dependency
 * is terminal: an empty value then means unsupported, not pending. A pending lookup may retain an
 * earlier valid value while its replacement prepares.
 */
template <typename T>
struct ResourceResult final {
    std::optional<T> value;
    Readiness readiness;

    [[nodiscard]] PendingStatus status() const noexcept {
        return readiness.id() != 0U ? readiness.status() : PendingStatus::ready;
    }
};

/**
 * One immutable preparation result. Reading never waits, and readiness changes only when the owner
 * adopts a completion. Dropping every resource/dependency handle cancels work not yet started.
 * Creation and explicit cancellation use the owner's serialized calls; published payloads are
 * immutable. Terminal resources do not retain their former task's callbacks or queue storage.
 * Pending<void> instead transfers private worker results through an owner-side adoption callback;
 * it exposes only readiness, so mutable decoder/session continuations never become shared payloads.
 */
template <typename T>
class Pending final {
  public:
    Pending() noexcept = default;

    /** Publishes an acquisition failure before any worker was submitted; the caller reports it. */
    [[nodiscard]] static Pending failed(TaskOwner& owner, const TaskKind kind,
                                       std::string label, std::exception_ptr error) {
        if (error == nullptr)
            throw std::invalid_argument("failed preparation requires an error");
        Pending result;
        result.state_ =
            std::make_shared<State>(owner.next_resource_identity(), kind, std::move(label));
        result.state_->failure.error = std::move(error);
        result.state_->status.store(PendingStatus::failed, std::memory_order_release);
        return result;
    }

    template <typename Factory>
        requires std::invocable<Factory&, const TaskCancellation&> &&
                 std::constructible_from<T, std::invoke_result_t<Factory&, const TaskCancellation&>>
    [[nodiscard]] static Pending start(TaskOwner& owner, const TaskKind kind,
                                      const TaskPriority priority, std::string label,
                                      Factory&& prepare) {
        auto state = std::make_shared<State>(owner.next_resource_identity(), kind, std::move(label));
        const std::weak_ptr<State> weak = state;
        TaskCancellation ticket = owner.post(
            kind, priority,
            [weak, factory = std::forward<Factory>(prepare)](const TaskCancellation& cancellation) mutable {
                if (const auto target = weak.lock(); target != nullptr)
                    target->value.emplace(std::invoke(factory, cancellation));
            },
            [weak](const TaskOutcome& outcome) {
                const auto target = weak.lock();
                if (target == nullptr)
                    return;
                target->cancellation = {};
                if (outcome.cancelled || outcome.error != nullptr) {
                    target->value.reset();
                    target->failure = outcome;
                    target->status.store(PendingStatus::failed, std::memory_order_release);
                } else {
                    target->status.store(PendingStatus::ready, std::memory_order_release);
                }
            });
        if (state->status.load(std::memory_order_acquire) == PendingStatus::pending)
            state->cancellation = std::move(ticket);
        Pending result;
        result.state_ = std::move(state);
        return result;
    }

    [[nodiscard]] static Pending start(TaskOwner& owner, const TaskKind kind,
                                      const TaskPriority priority, std::string label,
                                      TaskOwner::Work work, TaskOwner::Completion adopt)
        requires std::is_void_v<T>
    {
        if (!adopt)
            throw std::invalid_argument("owner preparation requires an adoption callback");
        auto state = std::make_shared<State>(owner.next_resource_identity(), kind, std::move(label));
        const std::weak_ptr<State> weak = state;
        TaskCancellation ticket = owner.post(
            kind, priority, std::move(work),
            [weak, adopt = std::move(adopt)](const TaskOutcome& outcome) mutable {
                const auto target = weak.lock();
                if (target == nullptr)
                    return;
                target->cancellation = {};
                try {
                    if (!outcome.cancelled)
                        adopt(outcome);
                } catch (...) {
                    target->failure.error = std::current_exception();
                    target->status.store(PendingStatus::failed, std::memory_order_release);
                    throw;
                }
                if (outcome.cancelled || outcome.error != nullptr)
                    target->failure = outcome;
                target->status.store(outcome.cancelled || outcome.error != nullptr
                                         ? PendingStatus::failed : PendingStatus::ready,
                                     std::memory_order_release);
            });
        if (state->status.load(std::memory_order_acquire) == PendingStatus::pending)
            state->cancellation = std::move(ticket);
        Pending result;
        result.state_ = std::move(state);
        return result;
    }

    [[nodiscard]] Readiness readiness() const noexcept {
        Readiness result;
        result.state_ = state_;
        return result;
    }
    [[nodiscard]] std::uint64_t id() const noexcept {
        return state_ != nullptr ? state_->id : 0U;
    }
    [[nodiscard]] PendingStatus status() const noexcept {
        return state_ != nullptr ? state_->status.load(std::memory_order_acquire)
                                 : PendingStatus::failed;
    }
    [[nodiscard]] const T* get() const noexcept requires (!std::is_void_v<T>) {
        return state_ != nullptr &&
                       state_->status.load(std::memory_order_acquire) == PendingStatus::ready
                   ? &*state_->value : nullptr;
    }
    void cancel() const noexcept {
        if (state_ != nullptr)
            state_->cancellation.cancel();
    }

  private:
    struct NoValue final {};
    struct State final : Readiness::State {
        State(const std::uint64_t identity, const TaskKind category, std::string name)
            : Readiness::State(identity, category, std::move(name)) {}
        // The worker constructs this before publication; only the owner can expose or discard it.
        std::conditional_t<std::is_void_v<T>, NoValue, std::optional<T>> value;
    };
    std::shared_ptr<State> state_;
};

} // namespace strata::base
