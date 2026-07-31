#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace strata::ui {

/** Surface-owned transient action/command feedback consumed by ordinary StatusBar widgets. */
class StatusFeedbackService final {
public:
    using ChangedCallback = std::function<void()>;

    explicit StatusFeedbackService(ChangedCallback changed = {});

    void publish(std::string message);
    void advance_frame(std::int64_t frame_time_nanos);
    void clear();
    [[nodiscard]] std::optional<std::string_view> snapshot() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;

private:
    static constexpr std::int64_t timeout_nanos = 3'000'000'000LL;

    ChangedCallback changed_;
    std::optional<std::string> feedback_;
    std::optional<std::int64_t> expires_at_nanos_;
    std::int64_t now_nanos_ = 0;
    std::uint64_t generation_ = 0U;
};

} // namespace strata::ui
