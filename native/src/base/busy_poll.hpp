#pragma once

#include <chrono>
#include <cstdint>

namespace strata::base {

/**
 * Lets a host look at its input while the UI thread is busy, without moving the UI off the window's
 * thread. Long-running loops call busy_tick(); once a frame has run for a while, the installed
 * callback runs about every millisecond. It must not enter the runtime: a window's hook takes wheel
 * messages from its queue and posts them to the render thread.
 */
class BusyPoll final {
  public:
    using Callback = void (*)(void* user_data);

    BusyPoll(const Callback callback, void* const user_data) noexcept
        : callback_(callback), user_data_(user_data), previous_(active()),
          next_(std::chrono::steady_clock::now() + first_delay) {
        active() = callback_ != nullptr ? this : previous_;
    }
    ~BusyPoll() { active() = previous_; }
    BusyPoll(const BusyPoll&) = delete;
    BusyPoll& operator=(const BusyPoll&) = delete;

    /** Cheap enough for a per-node loop: looks at the clock only every 64th call. */
    static void tick() noexcept {
        BusyPoll* const poll = active();
        if (poll != nullptr && (++poll->ticks_ & 63U) == 0U)
            poll->check();
    }

  private:
    // An ordinary frame never polls, so input takes its normal route unless the UI is really busy.
    static constexpr std::chrono::milliseconds first_delay{4};
    static constexpr std::chrono::milliseconds interval{1};

    [[nodiscard]] static BusyPoll*& active() noexcept {
        static thread_local BusyPoll* current = nullptr;
        return current;
    }

    void check() noexcept {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_)
            return;
        next_ = now + interval;
        callback_(user_data_);
    }

    Callback callback_;
    void* user_data_;
    BusyPoll* previous_;
    std::chrono::steady_clock::time_point next_;
    std::uint32_t ticks_ = 0U;
};

} // namespace strata::base
