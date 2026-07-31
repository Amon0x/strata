#include "ui/status.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace strata::ui {

StatusFeedbackService::StatusFeedbackService(ChangedCallback changed)
    : changed_(std::move(changed)) {}

void StatusFeedbackService::publish(std::string message) {
    if (message.empty()) throw std::invalid_argument("status feedback must not be empty");
    if (now_nanos_ > std::numeric_limits<std::int64_t>::max() - timeout_nanos) {
        throw std::overflow_error("status feedback expiry exhausted");
    }
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("status feedback generation exhausted");
    }
    feedback_ = std::move(message);
    expires_at_nanos_ = now_nanos_ + timeout_nanos;
    ++generation_;
    if (changed_) changed_();
}

void StatusFeedbackService::advance_frame(const std::int64_t frame_time_nanos) {
    if (frame_time_nanos < 0 || frame_time_nanos < now_nanos_) {
        throw std::invalid_argument("status feedback frame clock must be monotonic and non-negative");
    }
    now_nanos_ = frame_time_nanos;
    if (feedback_.has_value() && expires_at_nanos_.has_value() &&
        *expires_at_nanos_ <= frame_time_nanos) {
        clear();
    }
}

void StatusFeedbackService::clear() {
    if (!feedback_.has_value()) return;
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("status feedback generation exhausted");
    }
    feedback_.reset();
    expires_at_nanos_.reset();
    ++generation_;
    if (changed_) changed_();
}

std::optional<std::string_view> StatusFeedbackService::snapshot() const noexcept {
    return feedback_.has_value()
               ? std::optional<std::string_view>(*feedback_)
               : std::nullopt;
}

std::uint64_t StatusFeedbackService::generation() const noexcept { return generation_; }

} // namespace strata::ui
