#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <strata/svg.hpp>

#include "resource/image.hpp"
#include "resource/runtime_images.hpp"

namespace strata::resource {

/** One parsed, surface-owned SVG image identified by the author-facing image id. */
struct SvgImageResource final {
    std::string logical_id;
    svg::Document document;
};

/**
 * Immutable lookup table used by widget presentation to project SVGs into vector render commands,
 * and to learn any image's natural size: an SVG's own, a static raster's, or one the host publishes
 * while the runtime runs.
 */
/**
 * How an animated image (GIF, APNG, animated WebP) plays: when each frame ends, from the start of
 * a play, and how many plays (0 forever). Frame k is the texture `<id>#k`; frame 0 is `<id>`.
 */
struct ImageAnimation final {
    std::vector<std::int64_t> frame_ends_nanos;
    std::uint32_t loop_count = 0U;

    [[nodiscard]] std::int64_t duration_nanos() const noexcept {
        return frame_ends_nanos.empty() ? 0 : frame_ends_nanos.back();
    }
    /** The texture showing `elapsed` into the playback (all plays), for an image `id`. */
    [[nodiscard]] std::string frame_texture(std::string_view id, std::int64_t elapsed) const;
};

class SvgImageRegistry final {
public:
    SvgImageRegistry() = default;
    explicit SvgImageRegistry(std::vector<SvgImageResource> images,
                              std::map<std::string, ImageDimensions, std::less<>> rasters = {},
                              const RuntimeImageStore* runtime_images = nullptr,
                              std::map<std::string, ImageAnimation, std::less<>> animations = {});
    /** How the image animates, or nothing for a still one. */
    [[nodiscard]] const ImageAnimation* animation(std::string_view logical_id) const noexcept;

    [[nodiscard]] const svg::Document* find(std::string_view logical_id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    /** The image's natural size in its own units, or nothing for an unknown id. */
    [[nodiscard]] std::optional<ImageDimensions> dimensions(std::string_view logical_id) const;

private:
    std::map<std::string, svg::Document, std::less<>> images_;
    std::map<std::string, ImageDimensions, std::less<>> rasters_;
    const RuntimeImageStore* runtime_images_ = nullptr;
    std::map<std::string, ImageAnimation, std::less<>> animations_;
};

} // namespace strata::resource
