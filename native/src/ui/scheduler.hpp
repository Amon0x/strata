#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "ui/tree.hpp"

namespace strata::ui {

enum class WorkKind { animation, widget, service };
using WorkId = std::uint64_t;

struct WorkTickResult final {
    std::size_t visited = 0U;
    std::size_t completed = 0U;
    std::size_t failed = 0U;
    std::size_t remaining = 0U;
};

/**
 * Per-runtime active-work index. A settled tick is O(1): retained nodes are never polled to
 * discover animations, widgets, or services that might need time.
 */
class WorkScheduler final {
public:
    using Callback = std::function<bool(std::int64_t now_nanos)>;
    using ErrorHandler = std::function<void(WorkKind, std::uint64_t, std::string_view, std::string_view)>;

    explicit WorkScheduler(ErrorHandler error_handler = {});
    ~WorkScheduler();

    WorkScheduler(const WorkScheduler&) = delete;
    WorkScheduler& operator=(const WorkScheduler&) = delete;

    [[nodiscard]] WorkId schedule(
        WorkKind kind,
        std::uint64_t owner_identity,
        std::string key,
        Callback callback
    );
    /** Cancels scheduled work and returns whether it was active. */
    [[nodiscard]] bool cancel(WorkId id) noexcept;
    /** Cancels every item owned by one retained identity. */
    [[nodiscard]] std::size_t cancel_owner(std::uint64_t owner_identity) noexcept;
    /** Binds owner cancellation to exact retained detach cleanup. */
    void bind_owner(RetainedNode& owner);
    [[nodiscard]] WorkTickResult tick(std::int64_t now_nanos);
    [[nodiscard]] std::size_t active_count() const noexcept;
    [[nodiscard]] std::size_t active_count(WorkKind kind) const noexcept;
    void clear() noexcept;

private:
    struct Core;
    std::shared_ptr<Core> core_;
};

} // namespace strata::ui
