#include "resource/svg_image.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace strata::resource {

std::string ImageAnimation::frame_texture(const std::string_view id,
                                          const std::int64_t elapsed) const {
    const std::int64_t duration = duration_nanos();
    if (frame_ends_nanos.size() < 2U || duration <= 0)
        return std::string(id);
    // Past the last play, the last frame stays.
    const bool finished = loop_count != 0U && elapsed >= duration * loop_count;
    const std::int64_t within =
        finished ? duration - 1 : std::max<std::int64_t>(0, elapsed) % duration;
    const auto frame = std::ranges::upper_bound(frame_ends_nanos, within);
    const std::size_t index = std::min(static_cast<std::size_t>(frame - frame_ends_nanos.begin()),
                                       frame_ends_nanos.size() - 1U);
    return index == 0U ? std::string(id) : std::string(id) + "#" + std::to_string(index);
}

SvgImageRegistry::SvgImageRegistry(std::vector<SvgImageResource> images,
                                   std::map<std::string, ImageDimensions, std::less<>> rasters,
                                   const RuntimeImageStore* const runtime_images,
                                   std::map<std::string, ImageAnimation, std::less<>> animations)
    : rasters_(std::move(rasters)), runtime_images_(runtime_images),
      animations_(std::move(animations)) {
    for (SvgImageResource& image : images) {
        if (image.logical_id.empty()) {
            throw std::invalid_argument("SVG image requires a non-empty logical id");
        }
        if (!images_.emplace(std::move(image.logical_id), std::move(image.document)).second) {
            throw std::invalid_argument("SVG image logical ids must be unique");
        }
    }
}

const svg::Document* SvgImageRegistry::find(const std::string_view logical_id) const noexcept {
    const auto found = images_.find(logical_id);
    return found == images_.end() ? nullptr : &found->second;
}

const ImageAnimation*
SvgImageRegistry::animation(const std::string_view logical_id) const noexcept {
    const auto found = animations_.find(logical_id);
    return found == animations_.end() ? nullptr : &found->second;
}

std::size_t SvgImageRegistry::size() const noexcept {
    return images_.size();
}

std::optional<ImageDimensions> SvgImageRegistry::dimensions(
    const std::string_view logical_id) const {
    if (const svg::Document* document = find(logical_id); document != nullptr) {
        const double width = document->width > 0.0 ? document->width : document->view_box.width;
        const double height = document->height > 0.0 ? document->height : document->view_box.height;
        if (width > 0.0 && height > 0.0) {
            return ImageDimensions{static_cast<std::uint32_t>(std::ceil(width)),
                                   static_cast<std::uint32_t>(std::ceil(height))};
        }
        return std::nullopt;
    }
    if (const auto raster = rasters_.find(logical_id); raster != rasters_.end())
        return raster->second;
    if (runtime_images_ != nullptr) {
        if (const auto image = runtime_images_->images().find(logical_id);
            image != runtime_images_->images().end() && image->second != nullptr) {
            return image->second->dimensions;
        }
    }
    return std::nullopt;
}

} // namespace strata::resource
