#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace strata::resource {

enum class TextureSampling : std::uint32_t { nearest = 0U, linear = 1U };
enum class ImageEncoding : std::uint32_t { png = 0U, rgba8 = 1U };

struct ImageDimensions final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;

    [[nodiscard]] friend bool operator==(const ImageDimensions&, const ImageDimensions&) = default;
};

/** Immutable image metadata consumed by geometry planning independently of encoded upload bytes. */
struct TextureResourceDescriptor final {
    /** Author-facing id referenced by render commands. */
    std::string logical_id;
    /** Surface-scoped id used by host resource and draw records. */
    std::string host_id;
    TextureSampling sampling = TextureSampling::linear;
    ImageEncoding encoding = ImageEncoding::png;
    ImageDimensions dimensions;

    [[nodiscard]] friend bool operator==(
        const TextureResourceDescriptor&,
        const TextureResourceDescriptor&
    ) = default;
};

/** Immutable, surface-owned encoded image transferred to a host render adapter. */
struct EncodedTextureResource final {
    /** Author-facing id referenced by render commands. */
    std::string logical_id;
    /** Surface-scoped id used by host resource and draw records. */
    std::string host_id;
    TextureSampling sampling = TextureSampling::linear;
    ImageEncoding encoding = ImageEncoding::png;
    ImageDimensions dimensions;
    std::vector<std::uint8_t> bytes;

    [[nodiscard]] TextureResourceDescriptor descriptor() const {
        if (logical_id.empty() || host_id.empty()) {
            throw std::invalid_argument(
                "encoded texture requires non-empty logical and surface host ids"
            );
        }
        return TextureResourceDescriptor{
            logical_id, host_id, sampling, encoding, dimensions,
        };
    }
};

/** Throws unless the dimensions fit the portable texture limits shared by every image source. */
void validate_image_dimensions(ImageDimensions dimensions);

/** Validates the bounded PNG envelope and reads its mandatory IHDR dimensions. */
[[nodiscard]] ImageDimensions inspect_png(const std::vector<std::uint8_t>& bytes);

/**
 * Decodes a PNG into tightly packed, top-down RGBA8 rows with straight (unpremultiplied) alpha:
 * the texel convention every packet image carries. Samples are taken as stored, with no gamma,
 * sRGB or ICC conversion; 16-bit samples keep their high byte; low bit depths expand by bit
 * replication; palette and tRNS transparency become alpha, and images without alpha are opaque.
 * Throws std::invalid_argument for a malformed stream or dimensions other than `expected`.
 */
[[nodiscard]] std::vector<std::uint8_t> decode_png_rgba8(std::span<const std::uint8_t> bytes,
                                                         ImageDimensions expected);

} // namespace strata::resource
