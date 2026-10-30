#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "font/opentype.hpp"

namespace strata::font {

/** A straight piece of an outline, in font units, y up. */
struct VectorSegment final {
    float x0 = 0.0F;
    float y0 = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;
};

/**
 * A height the grid fit lands on whole pixels, font units: a stem (a bar or a stroke, its bottom
 * and top), moved whole so its thickness stays what it is, or a single edge (low equals high).
 */
struct VectorHint final {
    float low = 0.0F;
    float high = 0.0F;
    /**
     * Which edge lands on a pixel: whichever is nearer one (0), or the low (1) or the high (2),
     * which is at one of the font's zones: it lands where the zone does, its overshoot past the
     * zone kept as FreeType keeps it (none under half a pixel, half to three quarters, else one).
     */
    std::uint8_t anchor = 0U;
    float zone = 0.0F;
};

/**
 * A glyph as the GPU draws it: the boundary of what its outline covers as straight segments in
 * font units, flattened finely enough to stay smooth at any size, overlapping contours merged so
 * nothing is covered twice, grouped into horizontal bands so a pixel reads only the segments
 * crossing its row. Built once per glyph and drawn at every size, position, scale and rotation
 * from the same data: nothing is rasterized ahead of drawing.
 */
struct VectorGlyph final {
    /** The outline's bounds, font units. */
    float left = 0.0F;
    float bottom = 0.0F;
    float right = 0.0F;
    float top = 0.0F;
    std::vector<VectorSegment> segments;
    /**
     * Bands of equal height from `bottom` to `top`: band b holds the segments
     * band_segments[band_offsets[b]] to band_segments[band_offsets[b + 1] - 1], every segment
     * whose vertical extent meets the band.
     */
    std::vector<std::uint32_t> band_offsets;
    std::vector<std::uint32_t> band_segments;
    /**
     * What the vertical grid fit lands on whole pixels, ascending: the glyph's horizontal stems
     * and strokes, each moved whole (FreeType's light hinting does the same, keeping a bar's
     * weight), anchored at the font's zones (baseline, x-height, cap height, ascender,
     * descender) where they meet one.
     */
    std::vector<VectorHint> hints;
    /** Its font's FontHintZones::x_overshoot and max_height, which the grid fit scales by. */
    float x_overshoot = 0.0F;
    float max_height = 0.0F;

    [[nodiscard]] bool empty() const noexcept {
        return segments.empty();
    }
    [[nodiscard]] std::uint32_t band_count() const noexcept {
        return band_offsets.empty() ? 0U : static_cast<std::uint32_t>(band_offsets.size() - 1U);
    }
};

/** The heights a font's text lines up on, font units: see font_hint_zones. */
struct FontHintZones final {
    std::vector<float> heights;
    /**
     * The top of the round small letters ('o'): the grid fit scales a glyph's height so it lands
     * on a whole pixel, as FreeType's light hinting does, rounding up from three eighths.
     */
    float x_overshoot = 0.0F;
    /** How far any zone reaches from the baseline, at least an em: that scaling moves no height of
     * it more than two pixels. */
    float max_height = 0.0F;
};

/**
 * A font's zones, measured from its own glyphs: the baseline, the x-height ('x'), the cap height
 * ('H'), the ascender ('d') and the descender ('p'), where the font has them.
 */
[[nodiscard]] FontHintZones font_hint_zones(const OpenTypeFont& font);

/**
 * Flattens a glyph's outline for drawing on the GPU. Quadratic curves become chords within
 * `tolerance` font units of the curve (the default, an em over 4096, keeps a glyph an em of 1024
 * pixels tall within a quarter pixel of its curves).
 */
[[nodiscard]] VectorGlyph build_vector_glyph(const GlyphOutline& outline,
                                             std::uint16_t units_per_em, const FontHintZones& zones,
                                             double tolerance = 0.0);

/** Where a glyph is drawn: device pixels a font unit, and its pen position on the device, y up. */
struct VectorPlacement final {
    double scale = 1.0;
    double pen_x = 0.0;
    double pen_y = 0.0;
};

/**
 * The glyph fitted to the pixel grid for drawing at `scale` device pixels a font unit, upright,
 * its baseline on a whole pixel: its hints (stems moved whole, so a bar keeps its weight, and
 * anchored at the font's zones) land on whole pixels and what lies between them moves in
 * proportion, as FreeType's light hinting moves it. Only heights move; the result has no hints.
 */
[[nodiscard]] VectorGlyph fit_vector_glyph(const VectorGlyph& glyph, double scale);

/**
 * The fraction of the device pixel [x, x + 1] x [y, y + 1] the glyph covers: the exact area of its
 * outline within the pixel, as FreeType's gray rasterizer measures it. The reference the GPU
 * reproduces.
 */
[[nodiscard]] double vector_pixel_coverage(const VectorGlyph& glyph,
                                           const VectorPlacement& placement, double x, double y);

/**
 * The pixel's coverage per subpixel, red, green and blue from the left: its thirds' coverages
 * weighted by host::vector_lcd_filter as FreeType's LCD rendering weights them.
 */
[[nodiscard]] std::array<double, 3> vector_subpixel_coverage(const VectorGlyph& glyph,
                                                             const VectorPlacement& placement,
                                                             double x, double y);

} // namespace strata::font
