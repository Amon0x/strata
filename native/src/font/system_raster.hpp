#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "font/opentype.hpp"

namespace strata::font {

enum class SubpixelOrder : std::uint8_t;
enum class GlyphHinting : std::uint8_t;

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

/** How this machine is set to draw text. Read once; on Linux it comes from fontconfig. */
struct SystemTextSettings final {
    GlyphHinting hinting;
    SubpixelOrder subpixel_order;
};
[[nodiscard]] const SystemTextSettings& system_text_settings();

/** The contrast and gamma coverage blends through (host::text_coverage). */
struct TextCoverageCurve final {
    float contrast = 0.0F;
    float gamma = 0.0F;
};

/**
 * The curve this platform's text stack blends with, read off Chromium's pixels there: heavy on
 * Windows, gentler per subpixel on Linux, and gentler still for Linux's gray text.
 */
[[nodiscard]] TextCoverageCurve system_text_curve(bool subpixel) noexcept;

/**
 * Rasterizes through DirectWrite on Windows, as Skia does: it runs a font's own hinting instructions
 * and ClearType's filter, so text matches what the rest of the system draws. An empty glyph is an
 * empty mask. Nothing where the platform's stack is FreeType, which the caller drives itself, or
 * where this face or request cannot be drawn.
 */
[[nodiscard]] std::optional<SystemGlyphMask>
rasterize_system_glyph(const OpenTypeFont& font, std::uint16_t glyph, double physical_pixel_size,
                       double phase_offset, double italic_shear, SubpixelOrder order);

} // namespace strata::font
