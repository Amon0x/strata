#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace strata::resource {

enum class TextureSampling : std::uint32_t { nearest = 0U, linear = 1U };
enum class ImageEncoding : std::uint32_t { png = 0U };

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

/** Validates the bounded PNG envelope and reads its mandatory IHDR dimensions. */
[[nodiscard]] ImageDimensions inspect_png(const std::vector<std::uint8_t>& bytes);

} // namespace strata::resource
