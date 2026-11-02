#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "font/opentype.hpp"

namespace strata::font {

enum class SubpixelOrder : std::uint8_t;

/** One glyph as the operating system's text rasterizer draws it: tight rows, no padding. */
struct SystemGlyphMask final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    /** Pixels from the pen to the mask's left edge, and from the baseline up to its top edge. */
    std::int32_t left = 0;
    std::int32_t top = 0;
    /** One byte a pixel when gray; else red, green, blue and the largest of them. */
    std::vector<std::uint8_t> bytes;
};

/**
 * Rasterizes through the platform's own text stack, as Skia does: DirectWrite on Windows, which runs
 * a font's own hinting instructions and ClearType's filter, so text matches what the rest of the
 * system draws. An empty glyph is an empty mask. Nothing where the platform has no such rasterizer
 * or cannot draw this face or request; the caller's own rasterizer draws it then.
 */
[[nodiscard]] std::optional<SystemGlyphMask>
rasterize_system_glyph(const OpenTypeFont& font, std::uint16_t glyph, double physical_pixel_size,
                       double phase_offset, double italic_shear, SubpixelOrder order);

} // namespace strata::font
