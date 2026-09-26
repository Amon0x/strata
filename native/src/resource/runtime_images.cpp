#include "resource/runtime_images.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

#include "core/utf8.hpp"

namespace strata::resource {

std::uint64_t RuntimeImageStore::publish(std::string id, const ImageDimensions dimensions,
                                         const TextureSampling sampling,
                                         std::vector<std::uint8_t> pixels) {
    if (id.empty() || !core::valid_utf8(id)) {
        throw std::invalid_argument("a runtime image id must be non-empty UTF-8");
    }
    validate_image_dimensions(dimensions);
    if (pixels.size() != static_cast<std::uint64_t>(dimensions.width) * dimensions.height * 4U) {
        throw std::invalid_argument("runtime image pixels must be width * height RGBA8 texels");
    }
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("runtime image generation exhausted");
    }
    ++generation_;
    auto image = std::make_shared<RuntimeImage>(
        RuntimeImage{id, dimensions, sampling, std::move(pixels), generation_});
    images_.insert_or_assign(std::move(id), std::move(image));
    return generation_;
}

bool RuntimeImageStore::release(const std::string_view id) {
    const auto found = images_.find(id);
    if (found == images_.end())
        return false;
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("runtime image generation exhausted");
    }
    images_.erase(found);
    ++generation_;
    return true;
}

} // namespace strata::resource
