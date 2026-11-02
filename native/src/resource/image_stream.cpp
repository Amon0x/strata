#include "resource/image_stream.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace strata::resource {
namespace {

/** Frames decoded ahead of the one showing. */
constexpr std::size_t frames_ahead = 3U;

/** Browsers show a frame that asks for under 20 ms for 100 ms instead; so does the player. */
[[nodiscard]] std::int64_t shown_delay(const std::uint32_t delay_ms) noexcept {
    return static_cast<std::int64_t>(delay_ms < 20U ? 100U : delay_ms) * 1'000'000;
}

[[nodiscard]] std::int64_t deadline(const std::int64_t start, const std::int64_t duration) noexcept {
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    return start > maximum - duration ? maximum : start + duration;
}

} // namespace

ImagePreparation prepare_image(std::vector<std::uint8_t> encoded) {
    ImageDecoder decoder(std::move(encoded));
    ImagePreparation result;
    result.dimensions = decoder.dimensions();
    result.first = std::move(decoder.next().value());
    result.second = decoder.next();
    if (result.second.has_value())
        result.continuation.emplace(std::move(decoder));
    return result;
}

/** Serialized player state; jobs own only Batch, and completion holds a weak adoption endpoint. */
struct ImageStream::State final : std::enable_shared_from_this<State> {
    struct Frame final {
        std::vector<std::uint8_t> rgba;
        std::int64_t delay_nanos = 0;
    };
    struct Continuation final {
        ImageDecoder decoder;
        std::uint64_t completed_plays = 0U;
    };
    struct Batch final {
        std::optional<Continuation> continuation;
        std::array<Frame, frames_ahead> frames;
        std::size_t count = 0U;
        bool ended = false;
    };

    State(ImagePreparation prepared, base::TaskOwner& tasks, std::string name)
        : owner(tasks), label(std::move(name)), canvas(prepared.dimensions),
          current(std::move(prepared.first.rgba)), frame_end(shown_delay(prepared.first.delay_ms)) {
        if (!prepared.second.has_value() || !prepared.continuation.has_value())
            throw std::invalid_argument("image stream requires an animation continuation");
        continuation.emplace(Continuation{std::move(*prepared.continuation)});
        ready[0U] = Frame{std::move(prepared.second->rgba), shown_delay(prepared.second->delay_ms)};
        ready_count = 1U;
    }

    void consume() {
        while (!finished && elapsed >= frame_end &&
               frame_end != std::numeric_limits<std::int64_t>::max()) {
            if (ready_count == 0U) {
                finished = ended;
                stalled = !ended;
                return;
            }
            Frame next = std::move(ready[ready_first]);
            ready_first = (ready_first + 1U) % frames_ahead;
            --ready_count;
            current = std::move(next.rgba);
            ++revision;
            frame_end = deadline(stalled ? std::max(frame_end, elapsed) : frame_end,
                                 next.delay_nanos);
            stalled = false;
        }
    }

    void refill() {
        if (in_flight || ended || ready_count == frames_ahead)
            return;
        const std::size_t slots = frames_ahead - ready_count;
        auto batch = std::make_shared<Batch>();
        batch->continuation = std::move(continuation);
        continuation.reset();
        in_flight = true;
        try {
            auto task = base::Pending<void>::start(
                owner, base::TaskKind::image, base::TaskPriority::prefetch, label,
                [batch, slots](const base::TaskCancellation& cancellation) {
                    try {
                        while (batch->count < slots) {
                            if (cancellation.cancelled()) {
                                batch->continuation.reset();
                                return;
                            }
                            auto& cursor = *batch->continuation;
                            auto frame = cursor.decoder.next();
                            if (!frame.has_value()) {
                                ++cursor.completed_plays;
                                const auto plays = cursor.decoder.loop_count();
                                if (plays != 0U && cursor.completed_plays >= plays) {
                                    batch->ended = true;
                                    batch->continuation.reset();
                                    return;
                                }
                                cursor.decoder.rewind();
                                continue;
                            }
                            batch->frames[batch->count++] =
                                Frame{std::move(frame->rgba), shown_delay(frame->delay_ms)};
                        }
                    } catch (...) {
                        batch->continuation.reset();
                        throw;
                    }
                },
                [weak = weak_from_this(), batch](const base::TaskOutcome& outcome) {
                    const auto player = weak.lock();
                    if (player == nullptr)
                        return;
                    player->in_flight = false;
                    player->continuation = std::move(batch->continuation);
                    player->ended = batch->ended || outcome.error != nullptr;
                    for (std::size_t index = 0U; index < batch->count; ++index) {
                        const auto slot = (player->ready_first + player->ready_count) % frames_ahead;
                        player->ready[slot] = std::move(batch->frames[index]);
                        ++player->ready_count;
                    }
                });
            if (in_flight)
                pending = std::move(task);
        } catch (...) {
            continuation = std::move(batch->continuation);
            in_flight = false;
            throw;
        }
    }

    void service() {
        // Completion only publishes frames. Playback deadlines always use this owner's new clock.
        consume();
        refill();
        consume();
    }

    base::TaskOwner& owner;
    const std::string label;
    const ImageDimensions canvas;
    std::optional<Continuation> continuation;
    std::array<Frame, frames_ahead> ready;
    std::size_t ready_first = 0U;
    std::size_t ready_count = 0U;
    base::Pending<void> pending;
    bool in_flight = false;
    bool ended = false;
    std::vector<std::uint8_t> current;
    std::uint64_t revision = 1U;
    bool started = false;
    bool finished = false;
    bool stalled = false;
    std::int64_t started_at = 0;
    std::int64_t elapsed = 0;
    std::int64_t frame_end = 0;
};

ImageStream::ImageStream(ImagePreparation preparation, base::TaskOwner& tasks, std::string label)
    : state_(std::make_shared<State>(std::move(preparation), tasks, std::move(label))) {}

ImageStream::~ImageStream() {
    state_->pending.cancel();
}

void ImageStream::advance(const std::int64_t frame_time_nanos) {
    if (frame_time_nanos < 0)
        throw std::invalid_argument("image playback requires a non-negative owner timestamp");
    auto& player = *state_;
    if (!player.started) {
        player.started = true;
        player.started_at = frame_time_nanos;
    }
    if (frame_time_nanos >= player.started_at)
        player.elapsed = std::max(player.elapsed, frame_time_nanos - player.started_at);
    player.service();
}

std::span<const std::uint8_t> ImageStream::pixels() const noexcept {
    return state_->current;
}

std::uint64_t ImageStream::revision() const noexcept {
    return state_->revision;
}

ImageDimensions ImageStream::dimensions() const noexcept {
    return state_->canvas;
}

bool ImageStream::finished() const noexcept {
    return state_->finished;
}

base::Readiness ImageStream::readiness() const noexcept {
    return state_->pending.readiness();
}

} // namespace strata::resource
