#include "resource/svg_image.hpp"

#include <stdexcept>
#include <utility>

namespace strata::resource {

SvgImageRegistry::SvgImageRegistry(std::vector<SvgImageResource> images) {
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

} // namespace strata::resource
