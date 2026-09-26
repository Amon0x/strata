#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "resource/image.hpp"

namespace strata::resource {

/** One host-published raster image: tightly packed, straight-alpha RGBA8 rows. */
struct RuntimeImage final {
    std::string id;
    ImageDimensions dimensions;
    TextureSampling sampling = TextureSampling::linear;
    std::vector<std::uint8_t> pixels;
    /** Store-wide monotonic revision; a replacement always gets a new one. */
    std::uint64_t revision = 0U;
};

/**
 * Images a host publishes while the Runtime runs (downloaded pictures, generated previews) and
 * replaces or releases at any time. Surfaces reference them by id like their static images and
 * upload one only when a frame samples it. Owned by the Runtime thread.
 */
class RuntimeImageStore final {
  public:
    using Images = std::map<std::string, std::shared_ptr<const RuntimeImage>, std::less<>>;

    /** Publishes or replaces an image; returns its revision. */
    std::uint64_t publish(std::string id, ImageDimensions dimensions, TextureSampling sampling,
                          std::vector<std::uint8_t> pixels);
    /** Removes an image; false when no image had that id. */
    bool release(std::string_view id);
    [[nodiscard]] const Images& images() const noexcept { return images_; }
    /** Advances on every publish and release, so a Surface skips unchanged stores cheaply. */
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

  private:
    Images images_;
    std::uint64_t generation_ = 0U;
};

} // namespace strata::resource
