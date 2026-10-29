#pragma once

#include <cstddef>
#include <compare>
#include <cstdint>
#include <optional>
#include <vector>

#include "font/opentype.hpp"

namespace strata::font {

[[nodiscard]] inline std::uint32_t synthetic_font_style_flags(
    const OpenTypeFont& font,
    const std::uint32_t requested
) noexcept {
    return resolve_font_style_geometry(requested, font.metadata().style_flags).synthetic_flags;
}

enum class GlyphRasterMode : std::uint8_t {
    coverage,
    msdf,
};

struct SubpixelPhase final {
    std::uint16_t index = 0U;
    std::uint16_t divisions = 1U;

    [[nodiscard]] double offset() const;
    [[nodiscard]] static SubpixelPhase quantize(double physical_position, std::uint16_t divisions);
    [[nodiscard]] friend auto operator<=>(const SubpixelPhase&, const SubpixelPhase&) = default;
};

struct RasterPlaneBounds final {
    double left = 0.0;
    double bottom = 0.0;
    double right = 0.0;
    double top = 0.0;
};

struct GlyphRasterBitmap final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<std::uint8_t> bytes;
    RasterPlaneBounds plane_bounds_font_units;
    RasterPlaneBounds plane_bounds_layout_pixels;
    double atlas_pixel_range = 1.0;
    double layout_pixel_range = 1.0;
    double layout_pixels_per_font_unit = 0.0;
    double atlas_pixels_per_font_unit = 0.0;
    GlyphRasterMode mode = GlyphRasterMode::coverage;
};

enum class GlyphHinting : std::uint8_t {
    none,
    light,
    full,
};

struct CoverageRasterConfig final {
    std::uint32_t maximum_dimension = 1'024U;
    std::uint32_t padding_pixels = 1U;
    /**
     * Vertical-only grid fitting (FreeType's light auto-hinter, Skia's "slight"). Glyphs sit at
     * horizontal subpixel phases, so horizontal hinting would only fight that placement: native
     * TrueType instructions still move stems sideways in fonts that opt out of FreeType's
     * backward-compatibility mode (Segoe UI, Arial), and fonts without instructions would get
     * the full auto-hinter, which snaps stems horizontally as well.
     */
    GlyphHinting hinting = GlyphHinting::light;
    /**
     * Optional physical-pixel optical emboldening for small grayscale glyphs; off by default. The
     * draw-time coverage weighting (host::text_coverage) already gives grayscale text the weight
     * Skia's gamma/contrast preblend does, and emboldening a hinted outline only adds a soft
     * column beside every stem. When enabled it widens stems only: the outline is already
     * grid-fitted vertically, and growing it vertically as well would lift every snapped top edge
     * off the pixel grid into a faint fringe row.
     */
    double stem_darkening_pixels = 0.0;
    double stem_darkening_taper_start_physical_pixel_size = 14.0;
    double stem_darkening_taper_end_physical_pixel_size = 28.0;
    /**
     * Midtone weighting baked into the bitmap. Zero by default: the atlas holds plain coverage, and
     * the weighting is applied at draw time from the ink's lightness (host::text_coverage), because
     * light ink on dark needs far more of it than dark ink on light.
     */
    double transfer_strength = 0.0;
};

struct MsdfRasterConfig final {
    std::uint32_t maximum_dimension = 1'024U;
    std::uint32_t padding_pixels = 6U;
    std::uint32_t oversampling = 2U;
    double pixel_range = 6.0;
    double flattening_tolerance_pixels = 0.22;
    double corner_dot_threshold = 0.65;
};

/**
 * Rasterizes one native TrueType glyph into a size-specific, hinted R8 grayscale bitmap. Empty
 * glyphs return nullopt; malformed requests and configured resource-bound violations throw
 * FontError.
 */
[[nodiscard]] std::optional<GlyphRasterBitmap> rasterize_coverage(
    const OpenTypeFont& font,
    std::uint16_t glyph,
    double pixel_size,
    double display_scale,
    SubpixelPhase phase = {},
    const CoverageRasterConfig& config = {},
    std::uint32_t font_style_flags = 0U
);

/** Rasterizes one outline into an RGBA multi-channel signed-distance field. */
[[nodiscard]] std::optional<GlyphRasterBitmap> rasterize_msdf(
    const OpenTypeFont& font,
    std::uint16_t glyph,
    double pixel_size,
    const MsdfRasterConfig& config = {},
    std::uint32_t font_style_flags = 0U
);

/** A polygon vertex in bitmap pixels: x right, y down, (0, 0) the bitmap's top-left corner. */
struct CoveragePoint final {
    double x = 0.0;
    double y = 0.0;
};

/**
 * The exact area each pixel of a `width` x `height` bitmap shares with closed polygons, as 8-bit
 * coverage: what text glyphs get, for any outline. Overlapping polygons combine by the nonzero
 * rule (a union, when they wind the same way) or by even-odd. The bitmap's top-left corner sits at
 * `origin` in the polygons' space, so one outline rasterizes tile by tile. Returns nothing when the
 * outline is larger than one pass can take (65,535 points or 32,767 contours).
 */
[[nodiscard]] std::optional<std::vector<std::uint8_t>>
rasterize_polygons(std::span<const std::vector<CoveragePoint>> contours, std::uint32_t width,
                   std::uint32_t height, bool even_odd, CoveragePoint origin = {});

} // namespace strata::font
