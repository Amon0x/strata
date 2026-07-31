#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "resource/resource.hpp"

namespace strata::font {

/** Frozen FontStyleFlags v1 bits shared by authored text, face resolution, and rasterization. */
inline constexpr std::uint32_t font_style_bold = std::uint32_t{1} << 0U;
inline constexpr std::uint32_t font_style_italic = std::uint32_t{1} << 1U;
inline constexpr std::uint32_t known_font_style_flags = font_style_bold | font_style_italic;
/** Shared design-space synthetic geometry used by shaping and both raster modes. */
inline constexpr double synthetic_bold_em_fraction = 1.0 / 24.0;
inline constexpr double synthetic_italic_shear = 0.2125565616700221; // tan(12 degrees)

struct FontStyleGeometry final {
    std::uint32_t synthetic_flags = 0U;

    [[nodiscard]] constexpr bool bold() const noexcept {
        return (synthetic_flags & font_style_bold) != 0U;
    }
    [[nodiscard]] constexpr bool italic() const noexcept {
        return (synthetic_flags & font_style_italic) != 0U;
    }
    /** Baseline-anchored design/physical-space shear; the transform is scale invariant. */
    [[nodiscard]] constexpr double transform_x(const double x, const double y) const noexcept {
        return italic() ? x + y * synthetic_italic_shear : x;
    }
    [[nodiscard]] constexpr double bold_strength(const double em_size) const noexcept {
        return bold() ? em_size * synthetic_bold_em_fraction : 0.0;
    }
};

[[nodiscard]] constexpr FontStyleGeometry resolve_font_style_geometry(
    const std::uint32_t requested,
    const std::uint32_t intrinsic
) noexcept {
    return FontStyleGeometry{
        requested & known_font_style_flags & ~intrinsic,
    };
}

class FontError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct TextMetrics final {
    double width = 0.0;
    double height = 0.0;
    double natural_line_height = 0.0;
    std::size_t glyph_count = 0U;
    std::size_t line_count = 0U;
};

struct ShapedGlyph final {
    std::uint16_t glyph_id = 0U;
    std::uint32_t code_point = 0U;
    std::size_t text_start_offset = 0U;
    std::size_t text_end_offset = 0U;
    double x = 0.0;
    double baseline = 0.0;
    double advance = 0.0;
    double x_placement = 0.0;
    double y_placement = 0.0;
    double y_advance = 0.0;
    std::size_t line_index = 0U;
    /** Requested frozen style bits; known bits are face-resolved, unknown bits remain opaque. */
    std::uint32_t font_style_flags = 0U;
};

/** Positioned text cluster, including whitespace that intentionally has no draw command. */
struct ShapedCluster final {
    std::uint32_t code_point = 0U;
    std::size_t text_start_offset = 0U;
    std::size_t text_end_offset = 0U;
    double x = 0.0;
    double advance = 0.0;
    std::size_t line_index = 0U;
    /** Source whitespace trimmed at a soft word-wrap boundary; it has zero visual advance. */
    bool soft_wrap_gap = false;
};

struct ShapedText final {
    std::vector<ShapedGlyph> glyphs;
    std::vector<ShapedCluster> clusters;
    /** Code points omitted because this face resolved them to the .notdef sentinel. */
    std::vector<std::uint32_t> missing_code_points;
    TextMetrics metrics;
};

struct PairPositionValue final {
    std::int16_t x_placement = 0;
    std::int16_t y_placement = 0;
    std::int16_t x_advance = 0;
    std::int16_t y_advance = 0;
};

struct PairPositionAdjustment final {
    PairPositionValue first;
    PairPositionValue second;
};

/**
 * Generic GPOS result for one glyph. Placements move the glyph without moving the pen;
 * advances move the pen used by following glyphs. Values remain in font design units.
 */
struct GlyphPositionAdjustment final {
    double x_placement = 0.0;
    double y_placement = 0.0;
    double x_advance = 0.0;
    double y_advance = 0.0;
};

/**
 * One glyph plus the source/ligature association needed by ordered GPOS attachment lookups.
 * ligature_component is zero-based for marks associated with a multi-component ligature.
 */
struct GlyphPositioningInput final {
    std::uint16_t glyph_id = 0U;
    std::size_t source_cluster = 0U;
    std::uint16_t ligature_component = 0U;
    std::uint16_t ligature_component_count = 1U;
};

/** One source glyph before GSUB, expressed in canonical UTF-16 offsets. */
struct GlyphRunInput final {
    std::uint16_t glyph_id = 0U;
    std::size_t text_start_offset = 0U;
    std::size_t text_end_offset = 0U;
};

/** A GSUB-resolved glyph and its GPOS result with ligature provenance preserved. */
struct PositionedRunGlyph final {
    std::uint16_t glyph_id = 0U;
    std::size_t text_start_offset = 0U;
    std::size_t text_end_offset = 0U;
    std::vector<std::size_t> ligature_component_clusters;
    GlyphPositionAdjustment position;
};

struct FontMetadata final {
    std::string family;
    std::string subfamily;
    std::string full_name;
    std::string postscript_name;
    std::uint16_t weight_class = 0U;
    std::uint16_t width_class = 0U;
    /** Intrinsic BOLD/ITALIC bits decoded from OS/2.fsSelection. */
    std::uint32_t style_flags = 0U;
};

struct GlyphPoint final {
    double x = 0.0;
    double y = 0.0;
    bool on_curve = false;

    [[nodiscard]] friend bool operator==(const GlyphPoint&, const GlyphPoint&) = default;
};

enum class GlyphSegmentKind : std::uint8_t {
    line,
    quadratic,
};

struct GlyphSegment final {
    GlyphSegmentKind kind = GlyphSegmentKind::line;
    GlyphPoint from;
    GlyphPoint control;
    GlyphPoint to;
};

struct GlyphContour final {
    std::vector<GlyphPoint> points;
    std::vector<GlyphSegment> segments;
};

struct GlyphBounds final {
    double left = 0.0;
    double bottom = 0.0;
    double right = 0.0;
    double top = 0.0;
};

/** Lazily decoded TrueType outline. The font owns and caches the returned immutable object. */
struct GlyphOutline final {
    std::optional<GlyphBounds> bounds;
    std::vector<GlyphContour> contours;
    bool composite = false;

    [[nodiscard]] bool empty() const noexcept { return contours.empty(); }
};

/** Bounds-checked, allocation-owned OpenType metrics and horizontal positioning model. */
class OpenTypeFont final {
public:
    [[nodiscard]] static OpenTypeFont parse(resource::ResourceBytes bytes);

    [[nodiscard]] std::uint16_t units_per_em() const noexcept;
    [[nodiscard]] std::int16_t ascender() const noexcept;
    [[nodiscard]] std::int16_t descender() const noexcept;
    [[nodiscard]] std::int16_t line_gap() const noexcept;
    [[nodiscard]] std::uint16_t glyph_count() const noexcept;
    [[nodiscard]] const FontMetadata& metadata() const noexcept;
    /**
     * Process-local identity for immutable parsed font content. Consumers may retain this token
     * while caching derived immutable data; equal tokens always refer to the same font bytes.
     */
    [[nodiscard]] std::shared_ptr<const void> cache_identity() const noexcept;
    /** Immutable source bytes retained by cache_identity(); used by replaceable raster backends. */
    [[nodiscard]] std::span<const std::uint8_t> source_bytes() const noexcept;
    /** Optional-table failures retained without rejecting the mandatory usable face. */
    [[nodiscard]] const std::vector<std::string>& optional_diagnostics() const noexcept;
    [[nodiscard]] std::uint16_t glyph_id(std::uint32_t code_point) const noexcept;
    [[nodiscard]] std::uint16_t horizontal_advance(std::uint16_t glyph) const noexcept;
    [[nodiscard]] std::shared_ptr<const GlyphOutline> glyph_outline(std::uint16_t glyph) const;
    [[nodiscard]] std::int16_t pair_advance_adjustment(
        std::uint16_t left,
        std::uint16_t right
    ) const noexcept;
    [[nodiscard]] PairPositionAdjustment pair_position_adjustment(
        std::uint16_t left,
        std::uint16_t right
    ) const noexcept;
    /** Applies ordered PairPos and mark-attachment lookups to one same-face glyph run. */
    [[nodiscard]] std::vector<GlyphPositionAdjustment> position_glyphs(
        std::span<const std::uint16_t> glyphs,
        std::uint32_t font_style_flags = 0U
    ) const;
    [[nodiscard]] std::vector<GlyphPositionAdjustment> position_glyphs(
        std::span<const GlyphPositioningInput> glyphs,
        std::uint32_t font_style_flags = 0U
    ) const;
    /** Applies active GSUB ligatures and then GPOS to a live same-face source run. */
    [[nodiscard]] std::vector<PositionedRunGlyph> shape_glyph_run(
        std::span<const GlyphRunInput> glyphs,
        std::uint32_t font_style_flags = 0U
    ) const;
    [[nodiscard]] TextMetrics measure_utf8(
        std::string_view text,
        double pixel_size,
        double line_height_multiplier = 1.0
    ) const;
    [[nodiscard]] ShapedText shape_utf8(
        std::string_view text,
        double pixel_size,
        double line_height_multiplier = 1.0,
        double letter_spacing = 0.0
    ) const;

private:
    struct Impl;
    explicit OpenTypeFont(std::shared_ptr<const Impl> implementation);
    std::shared_ptr<const Impl> implementation_;
};

} // namespace strata::font
