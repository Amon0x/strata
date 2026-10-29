#include "resource/image_stream.hpp"

#include <algorithm>
#include <exception>
#include <optional>
#include <utility>

namespace strata::resource {
namespace {

/** Frames decoded ahead of the one showing. */
constexpr std::size_t frames_ahead = 3U;

/** Browsers show a frame that asks for under 20 ms for 100 ms instead; so does the player. */
[[nodiscard]] std::int64_t shown_delay(const std::uint32_t delay_ms) noexcept {
    return static_cast<std::int64_t>(delay_ms < 20U ? 100U : delay_ms) * 1'000'000;
}

} // namespace

ImageStream::ImageStream(std::vector<std::uint8_t> encoded, const ImageDimensions canvas,
                         std::vector<std::uint8_t> first_frame, const std::uint32_t first_delay_ms)
    : encoded_(std::move(encoded)), canvas_(canvas), current_(std::move(first_frame)) {
    frame_end_ = shown_delay(first_delay_ms);
}

ImageStream::~ImageStream() {
    stop_worker();
}

void ImageStream::stop_worker() noexcept {
    if (!worker_.joinable())
        return;
    worker_.request_stop();
    space_.notify_all();
    worker_.join();
}

void ImageStream::decode_pass(const std::stop_token stop) {
    try {
        const std::uint32_t loops = stream_image(
            encoded_, [this, &stop](std::vector<std::uint8_t>& rgba, const std::uint32_t delay_ms) {
                std::unique_lock lock(mutex_);
                space_.wait(lock, stop, [this] { return ready_.size() < frames_ahead; });
                if (stop.stop_requested())
                    return false;
                ready_.push_back(Frame{std::move(rgba), shown_delay(delay_ms)});
                return true;
            });
        const std::scoped_lock lock(mutex_);
        loop_count_ = loops;
        pass_done_ = true;
    } catch (const std::exception&) {
        const std::scoped_lock lock(mutex_);
        pass_failed_ = true;
        pass_done_ = true;
    }
}

void ImageStream::start_play() {
    stop_worker();
    {
        const std::scoped_lock lock(mutex_);
        ready_.clear();
        pass_done_ = false;
    }
    worker_ = std::jthread([this](const std::stop_token stop) { decode_pass(stop); });
}

void ImageStream::advance() {
    const auto now = std::chrono::steady_clock::now();
    if (!started_) {
        started_ = true;
        started_at_ = now;
        frame_end_ = 0;
        start_play();
    }
    const std::int64_t elapsed_nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - started_at_).count();
    while (!finished_ && elapsed_nanos >= frame_end_) {
        std::optional<Frame> next;
        bool play_over = false;
        bool failed = false;
        std::uint32_t loops = 0U;
        {
            const std::scoped_lock lock(mutex_);
            if (!ready_.empty()) {
                next = std::move(ready_.front());
                ready_.pop_front();
            } else if (pass_done_) {
                play_over = true;
                failed = pass_failed_;
                loops = loop_count_;
            }
        }
        if (next.has_value()) {
            space_.notify_all();
            current_ = std::move(next->rgba);
            ++revision_;
            // A decoder that fell behind does not make the next frames rush to catch up.
            frame_end_ =
                std::max(frame_end_, elapsed_nanos - next->delay_nanos) + next->delay_nanos;
            continue;
        }
        if (!play_over)
            break; // The decoder is behind: this frame stays a little longer.
        ++plays_;
        if (failed || (loops != 0U && plays_ >= loops)) {
            finished_ = true; // The last frame stays.
            break;
        }
        start_play();
        break;
    }
}

} // namespace strata::resource
