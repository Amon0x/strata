#include "ui/notification.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace strata::ui {
namespace {

constexpr std::int64_t nanos_per_millisecond = 1'000'000LL;

[[nodiscard]] bool blank(const std::string& value) {
    return std::ranges::all_of(value, [](const unsigned char character) {
        return character == ' ' || character == '\t' || character == '\r' || character == '\n';
    });
}

[[nodiscard]] std::int64_t default_timeout_millis(const NotificationSeverity severity) {
    switch (severity) {
    case NotificationSeverity::info:
    case NotificationSeverity::success: return 4'000LL;
    case NotificationSeverity::warning: return 6'000LL;
    case NotificationSeverity::error: return 8'000LL;
    }
    throw std::logic_error("unknown notification severity");
}

} // namespace

NotificationService::NotificationService(
    ChangedCallback changed_callback,
    const std::size_t capacity
) : changed_(std::move(changed_callback)), capacity_(capacity) {
    if (capacity_ == 0U) throw std::invalid_argument("notification capacity must be positive");
}

std::uint64_t NotificationService::raise(NotificationRequest request) {
    if (request.message.empty() || blank(request.message)) {
        throw std::invalid_argument("notification message must not be blank");
    }
    if (request.timeout_millis.has_value() && *request.timeout_millis <= 0) {
        throw std::invalid_argument("notification timeout must be positive");
    }
    if (request.action_label.empty() != (request.action == nullptr)) {
        throw std::invalid_argument(
            "notification action label and action must be supplied together"
        );
    }
    if (next_id_ == 0U ||
        next_id_ > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::overflow_error("notification identity exhausted");
    }
    const std::uint64_t id = next_id_++;
    const std::optional<std::int64_t> timeout = timeout_nanos(request);
    std::optional<std::int64_t> expires;
    if (timeout.has_value()) {
        if (now_nanos_ > std::numeric_limits<std::int64_t>::max() - *timeout) {
            throw std::overflow_error("notification expiry exhausted");
        }
        expires = now_nanos_ + *timeout;
    }
    if (notifications_.size() == capacity_) notifications_.pop_front();
    notifications_.push_back(Notification{
        id,
        std::move(request),
        now_nanos_,
        expires,
        std::nullopt,
    });
    changed(NotificationChange{
        NotificationChangeKind::raised, id, true, true, true,
    });
    return id;
}

bool NotificationService::dismiss(const std::uint64_t id) {
    const auto found = std::ranges::find(notifications_, id, &Notification::id);
    if (found == notifications_.end()) return false;
    notifications_.erase(found);
    changed(NotificationChange{
        NotificationChangeKind::dismissed, id, true, true, true,
    });
    return true;
}

bool NotificationService::pause(const std::uint64_t id, const bool paused) {
    const auto found = std::ranges::find(notifications_, id, &Notification::id);
    if (found == notifications_.end()) return false;
    if (paused) {
        if (found->paused_at_nanos.has_value()) return false;
        found->paused_at_nanos = now_nanos_;
    } else {
        if (!found->paused_at_nanos.has_value()) return false;
        const std::int64_t duration = std::max<std::int64_t>(
            0,
            now_nanos_ - *found->paused_at_nanos
        );
        if (found->expires_at_nanos.has_value()) {
            if (*found->expires_at_nanos >
                std::numeric_limits<std::int64_t>::max() - duration) {
                throw std::overflow_error("paused notification expiry exhausted");
            }
            *found->expires_at_nanos += duration;
        }
        found->paused_at_nanos.reset();
    }
    changed(NotificationChange{
        paused ? NotificationChangeKind::paused : NotificationChangeKind::resumed,
        id,
        true,
        false,
        false,
    });
    return true;
}

void NotificationService::clear() {
    if (notifications_.empty()) return;
    notifications_.clear();
    changed(NotificationChange{
        NotificationChangeKind::cleared, std::nullopt, true, true, true,
    });
}

void NotificationService::advance_frame(const std::int64_t frame_time_nanos) {
    if (frame_time_nanos < 0 || frame_time_nanos < now_nanos_) {
        throw std::invalid_argument(
            "notification frame clock must be monotonic and non-negative"
        );
    }
    now_nanos_ = frame_time_nanos;
    std::vector<std::uint64_t> expired;
    for (const Notification& notification : notifications_) {
        if (!notification.paused_at_nanos.has_value() &&
            notification.expires_at_nanos.has_value() &&
            *notification.expires_at_nanos <= frame_time_nanos) {
            expired.push_back(notification.id);
        }
    }
    std::erase_if(notifications_, [frame_time_nanos](const Notification& notification) {
        return !notification.paused_at_nanos.has_value() &&
               notification.expires_at_nanos.has_value() &&
               *notification.expires_at_nanos <= frame_time_nanos;
    });
    if (!expired.empty()) {
        changed(NotificationChange{
            NotificationChangeKind::expired,
            expired.size() == 1U ? std::optional(expired.front()) : std::nullopt,
            true,
            true,
            true,
        });
    }
}

NotificationSnapshot NotificationService::snapshot(const std::size_t max_visible) const {
    if (max_visible == 0U) throw std::invalid_argument("visible notification count must be positive");
    const std::size_t count = std::min(max_visible, notifications_.size());
    NotificationSnapshot result;
    result.overflow_count = notifications_.size() - count;
    result.visible.reserve(count);
    const auto begin = notifications_.end() - static_cast<std::ptrdiff_t>(count);
    result.visible.insert(result.visible.end(), begin, notifications_.end());
    return result;
}

const Notification* NotificationService::find(const std::uint64_t id) const noexcept {
    const auto found = std::ranges::find(notifications_, id, &Notification::id);
    return found != notifications_.end() ? &*found : nullptr;
}

std::uint64_t NotificationService::generation() const noexcept { return generation_; }

bool NotificationService::has_expiring() const noexcept {
    return std::ranges::any_of(notifications_, [](const Notification& notification) {
        return notification.expires_at_nanos.has_value() &&
               !notification.paused_at_nanos.has_value();
    });
}

std::size_t NotificationService::size() const noexcept { return notifications_.size(); }
std::size_t NotificationService::capacity() const noexcept { return capacity_; }

std::optional<std::int64_t> NotificationService::timeout_nanos(
    const NotificationRequest& request
) {
    if (request.persistent) return std::nullopt;
    const std::int64_t millis = request.timeout_millis.value_or(
        default_timeout_millis(request.severity)
    );
    if (millis > std::numeric_limits<std::int64_t>::max() / nanos_per_millisecond) {
        throw std::overflow_error("notification timeout exhausted");
    }
    return millis * nanos_per_millisecond;
}

void NotificationService::changed(NotificationChange change) {
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("notification generation exhausted");
    }
    ++generation_;
    if (changed_) changed_(change);
}

} // namespace strata::ui
