#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "resource/image.hpp"

namespace strata::resource {

/**
 * An animation played from its encoded bytes as it goes, for one too long to hold decoded: a
 * worker decodes a few frames ahead and the player shows each in its turn, so memory holds a
 * handful of frames however many the animation has. Each play decodes the file again; the file's
 * loop count decides how many plays there are.
 *
 * show() and pixels() belong to one thread (the Surface's); the worker only fills the queue.
 */
class ImageStream final {
  public:
    ImageStream(std::vector<std::uint8_t> encoded, ImageDimensions canvas,
                std::vector<std::uint8_t> first_frame, std::uint32_t first_delay_ms);
    ~ImageStream();

    ImageStream(const ImageStream&) = delete;
    ImageStream& operator=(const ImageStream&) = delete;

    /**
     * Moves to the frame showing now. The stream keeps its own clock, from the first call, so
     * every Image showing it shows the same frame; the first call starts decoding.
     */
    void advance();
    /** The frame showing now: straight-alpha RGBA8 rows of the canvas. */
    [[nodiscard]] std::span<const std::uint8_t> pixels() const noexcept {
        return current_;
    }
    /** Moves whenever pixels() changes. */
    [[nodiscard]] std::uint64_t revision() const noexcept {
        return revision_;
    }
    [[nodiscard]] ImageDimensions dimensions() const noexcept {
        return canvas_;
    }

  private:
    struct Frame final {
        std::vector<std::uint8_t> rgba;
        std::int64_t delay_nanos = 0;
    };

    /** Starts a play from the file's first frame, on a fresh worker. */
    void start_play();
    void stop_worker() noexcept;
    void decode_pass(std::stop_token stop);

    const std::vector<std::uint8_t> encoded_;
    const ImageDimensions canvas_;

    // The worker's side, behind the mutex.
    std::mutex mutex_;
    std::condition_variable_any space_;
    std::deque<Frame> ready_;
    bool pass_done_ = false;
    bool pass_failed_ = false;
    std::uint32_t loop_count_ = 0U;
    std::jthread worker_;

    // The player's side.
    std::vector<std::uint8_t> current_;
    std::uint64_t revision_ = 1U;
    bool started_ = false;
    bool finished_ = false;
    std::uint32_t plays_ = 0U;
    std::chrono::steady_clock::time_point started_at_{};
    /** When the frame showing now gives way, on the playback clock. */
    std::int64_t frame_end_ = 0;
};

} // namespace strata::resource
