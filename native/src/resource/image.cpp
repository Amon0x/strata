#include "resource/image.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace strata::resource {
namespace {

[[nodiscard]] std::uint32_t big_endian_u32(
    const std::vector<std::uint8_t>& bytes,
    const std::size_t offset
) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) << 24U |
           static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U |
           static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U |
           static_cast<std::uint32_t>(bytes[offset + 3U]);
}

} // namespace

void validate_image_dimensions(const ImageDimensions dimensions) {
    constexpr std::uint32_t maximum_dimension = 32'768U;
    constexpr std::uint64_t maximum_pixels = 268'435'456U;
    if (dimensions.width == 0U || dimensions.height == 0U ||
        dimensions.width > maximum_dimension || dimensions.height > maximum_dimension ||
        static_cast<std::uint64_t>(dimensions.width) * dimensions.height > maximum_pixels) {
        throw std::invalid_argument("image dimensions exceed the portable texture limits");
    }
}

ImageDimensions inspect_png(const std::vector<std::uint8_t>& bytes) {
    constexpr std::array<std::uint8_t, 8U> signature{
        0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU,
    };
    constexpr std::size_t ihdr_envelope_size = 33U;
    if (bytes.size() < ihdr_envelope_size ||
        !std::equal(signature.begin(), signature.end(), bytes.begin()) ||
        big_endian_u32(bytes, 8U) != 13U || bytes[12U] != 'I' || bytes[13U] != 'H' ||
        bytes[14U] != 'D' || bytes[15U] != 'R') {
        throw std::invalid_argument("texture resource is not a PNG with a leading IHDR chunk");
    }
    const ImageDimensions dimensions{big_endian_u32(bytes, 16U), big_endian_u32(bytes, 20U)};
    validate_image_dimensions(dimensions);
    return dimensions;
}

} // namespace strata::resource
