#pragma once

#include <cstddef>
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
class SvgImageRegistry final {
public:
    SvgImageRegistry() = default;
    explicit SvgImageRegistry(std::vector<SvgImageResource> images,
                              std::map<std::string, ImageDimensions, std::less<>> rasters = {},
                              const RuntimeImageStore* runtime_images = nullptr);

    [[nodiscard]] const svg::Document* find(std::string_view logical_id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    /** The image's natural size in its own units, or nothing for an unknown id. */
    [[nodiscard]] std::optional<ImageDimensions> dimensions(std::string_view logical_id) const;

private:
    std::map<std::string, svg::Document, std::less<>> images_;
    std::map<std::string, ImageDimensions, std::less<>> rasters_;
    const RuntimeImageStore* runtime_images_ = nullptr;
};

} // namespace strata::resource
