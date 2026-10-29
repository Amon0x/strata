#include "resource/svg_image.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace strata::resource {

SvgImageRegistry::SvgImageRegistry(std::vector<SvgImageResource> images,
                                   std::map<std::string, ImageDimensions, std::less<>> rasters,
                                   const RuntimeImageStore* const runtime_images)
    : rasters_(std::move(rasters)), runtime_images_(runtime_images) {
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
