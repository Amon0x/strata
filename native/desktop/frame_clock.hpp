#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace strata::desktop {

/**
 * A window's Surface clock: wall time, less what frames spent stalled beyond a slow frame
 * (a shader compiled on first use, a large image decoded). Motion after a stall picks up where
 * the screen left off instead of jumping past what was never shown; time spent waiting for input
 * between frames still counts.
 */
class FrameClock final {
  public:
    /** Longer than this, a frame stalled: the clock counts this much of it and no more. */
    static constexpr std::int64_t slow_frame_nanos = 50'000'000;

    /** Nanoseconds since the clock began, never less than any time it gave before. */
    [[nodiscard]] std::int64_t now() const noexcept {
        last_ = std::max(last_, wall() - stalled_);
        return last_;
    }

    /** Marks the start of a frame's work, before its time is read. */
    void begin_frame() noexcept { frame_started_ = wall(); }

    /** Marks the end of a frame's work: what it took past a slow frame does not count. */
    void end_frame() noexcept {
        const std::int64_t spent = wall() - frame_started_;
        if (spent > slow_frame_nanos)
            stalled_ += spent - slow_frame_nanos;
    }

  private:
    [[nodiscard]] std::int64_t wall() const noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - epoch_)
            .count();
    }

    std::chrono::steady_clock::time_point epoch_ = std::chrono::steady_clock::now();
    std::int64_t stalled_ = 0;
    mutable std::int64_t last_ = 0;
    std::int64_t frame_started_ = 0;
};

} // namespace strata::desktop
