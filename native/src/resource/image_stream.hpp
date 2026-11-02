#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "base/pending.hpp"
#include "resource/image.hpp"

namespace strata::resource {

/** Usable first pixels and, for an animation, the second frame and exclusive decoder continuation. */
struct ImagePreparation final {
    ImageDimensions dimensions;
    ImageFrame first;
    std::optional<ImageFrame> second;
    std::optional<ImageDecoder> continuation;
};

/** Decodes at most two frames; a still result retains neither encoded bytes nor a decoder. */
[[nodiscard]] ImagePreparation prepare_image(std::vector<std::uint8_t> encoded);

/**
 * Owner-clock playback with three-frame lookahead. Each pool job owns its decoder exclusively and
 * returns when its reserved slots are full; no worker waits for the player. Completion adopts on
 * the supplied task owner, which must outlive this stream. Closing cancels without joining.
 */
class ImageStream final {
  public:
    ImageStream(ImagePreparation preparation, base::TaskOwner& tasks, std::string label);
    ~ImageStream();

    ImageStream(const ImageStream&) = delete;
    ImageStream& operator=(const ImageStream&) = delete;

    /**
     * Starts from the first call's owner timestamp. Repeated samples share one clock; earlier
     * timestamps do not rewind. A late decoder holds the current canvas instead of rushing frames.
     */
    void advance(std::int64_t frame_time_nanos);
    /** The frame showing now: straight-alpha RGBA8 rows of the canvas. */
    [[nodiscard]] std::span<const std::uint8_t> pixels() const noexcept;
    /** Moves whenever pixels() changes. */
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] ImageDimensions dimensions() const noexcept;
    [[nodiscard]] bool finished() const noexcept;
    /** A frame callback observes decoding so fixed-timestamp settling can resume a starved player. */
    [[nodiscard]] base::Readiness readiness() const noexcept;

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace strata::resource
