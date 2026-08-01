#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "runtime/expression.hpp"

namespace strata::ui {

enum class NotificationSeverity { info, success, warning, error };

struct NotificationRequest final {
    std::string message;
    NotificationSeverity severity = NotificationSeverity::info;
    bool persistent = false;
    std::optional<std::int64_t> timeout_millis = std::nullopt;
    std::string action_label{};
    std::shared_ptr<const runtime::ActionValue> action{};
};

struct Notification final {
    std::uint64_t id = 0U;
    NotificationRequest request;
    std::int64_t created_at_nanos = 0;
    std::optional<std::int64_t> expires_at_nanos;
    std::optional<std::int64_t> paused_at_nanos;
};

struct NotificationSnapshot final {
    std::vector<Notification> visible;
    std::size_t overflow_count = 0U;
};

enum class NotificationChangeKind { raised, dismissed, paused, resumed, cleared, expired };

/** Targeted invalidation contract published without rebuilding the description tree. */
struct NotificationChange final {
    NotificationChangeKind kind = NotificationChangeKind::raised;
    std::optional<std::uint64_t> notification_id;
    bool input = false;
    bool render = false;
    bool semantics = false;
};

/** Surface-owned, bounded notification queue with monotonic stable identities. */
class NotificationService final {
public:
    using ChangedCallback = std::function<void(const NotificationChange&)>;

    explicit NotificationService(
        ChangedCallback changed = {},
        std::size_t capacity = 128U
    );

    [[nodiscard]] std::uint64_t raise(NotificationRequest request);
    [[nodiscard]] bool dismiss(std::uint64_t id);
    [[nodiscard]] bool pause(std::uint64_t id, bool paused);
    void clear();
    void advance_frame(std::int64_t frame_time_nanos);

    [[nodiscard]] NotificationSnapshot snapshot(std::size_t max_visible = 3U) const;
    [[nodiscard]] const Notification* find(std::uint64_t id) const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] bool has_expiring() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;

private:
    [[nodiscard]] static std::optional<std::int64_t> timeout_nanos(
        const NotificationRequest& request
    );
    void changed(NotificationChange change);

    ChangedCallback changed_;
    std::deque<Notification> notifications_;
    std::size_t capacity_ = 128U;
    std::uint64_t next_id_ = 1U;
    std::uint64_t generation_ = 0U;
    std::int64_t now_nanos_ = 0;
};

} // namespace strata::ui
