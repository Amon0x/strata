#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>

namespace strata::ui {

/**
 * How long a frame has, from the pace frames actually arrive at, so a frame split over several takes
 * the same time on any display rather than the same number of frames. A host framing as fast as its
 * display allows shows that display's interval here, with no need to report it.
 */
class FramePace final {
  public:
    using Duration = std::chrono::nanoseconds;

    void record(const Duration between) noexcept {
        // An idle gap or a long stall says nothing about the display.
        if (between <= Duration::zero() || between > longest_interval)
            return;
        samples_[next_] = between;
        next_ = (next_ + 1U) % samples_.size();
        count_ = std::min(count_ + 1U, samples_.size());
        if (count_ < minimum_samples)
            return;
        // Low in the range: frames that overran their interval must not talk this into allowing more.
        std::array<Duration, sample_count> sorted = samples_;
        const auto end = sorted.begin() + static_cast<std::ptrdiff_t>(count_);
        const auto quartile = sorted.begin() + static_cast<std::ptrdiff_t>(count_ / 4U);
        std::nth_element(sorted.begin(), quartile, end);
        interval_ = *quartile;
    }

    /** Zero until enough frames have arrived to tell. */
    [[nodiscard]] Duration interval() const noexcept { return interval_; }

    /**
     * Content nobody sees yet can stop whenever it likes, so it stops early enough for one more stage
     * (5-7 ms on a large page) to fit this frame.
     */
    [[nodiscard]] Duration hidden_budget() const noexcept {
        return std::max<Duration>(std::chrono::microseconds(1500), interval_ - std::chrono::milliseconds(7));
    }

    /** Anything on screen stays whole unless the frame has already spent nearly all it has. */
    [[nodiscard]] Duration visible_budget() const noexcept {
        return std::max<Duration>(std::chrono::milliseconds(5), interval_ - std::chrono::milliseconds(3));
    }

  private:
    static constexpr std::size_t sample_count = 32U;
    static constexpr std::size_t minimum_samples = 8U;
    static constexpr Duration longest_interval = std::chrono::milliseconds(34);

    std::array<Duration, sample_count> samples_{};
    std::size_t next_ = 0U;
    std::size_t count_ = 0U;
    Duration interval_{0};
};

} // namespace strata::ui
