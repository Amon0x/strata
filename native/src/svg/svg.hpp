#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace strata::svg {

struct Point final {
    double x = 0.0;
    double y = 0.0;
    [[nodiscard]] friend bool operator==(const Point&, const Point&) = default;
};

/** SVG's two-dimensional affine matrix: [a c e; b d f; 0 0 1]. */
struct AffineTransform final {
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 1.0;
    double e = 0.0;
    double f = 0.0;

    [[nodiscard]] Point apply(Point point) const noexcept;
    [[nodiscard]] friend bool operator==(const AffineTransform&, const AffineTransform&) = default;
};

struct Color final {
    std::uint8_t red = 0U;
    std::uint8_t green = 0U;
    std::uint8_t blue = 0U;
    std::uint8_t alpha = 255U;
    [[nodiscard]] friend bool operator==(const Color&, const Color&) = default;
};

enum class PathVerb : std::uint8_t { move, line, cubic, close };

/**
 * Absolute path segment. `control_a` and `control_b` are used by cubic segments. SVG quadratic
 * curves and elliptical arcs are converted to equivalent cubic segments while parsing.
 */
struct PathSegment final {
    PathVerb verb = PathVerb::move;
    Point control_a;
    Point control_b;
    Point to;
    [[nodiscard]] friend bool operator==(const PathSegment&, const PathSegment&) = default;
};

struct Path final {
    std::vector<PathSegment> segments;
    [[nodiscard]] bool empty() const noexcept {
        return segments.empty();
    }
    [[nodiscard]] friend bool operator==(const Path&, const Path&) = default;
};

enum class FillRule : std::uint8_t { nonzero, evenodd };
enum class LineCap : std::uint8_t { butt, round, square };
enum class LineJoin : std::uint8_t { miter, round, bevel };

struct PaintStyle final {
    bool has_fill = true;
    Color fill{0U, 0U, 0U, 255U};
    bool has_stroke = false;
    Color stroke{0U, 0U, 0U, 255U};
    double fill_opacity = 1.0;
    double stroke_opacity = 1.0;
    double opacity = 1.0;
    double stroke_width = 1.0;
    double miter_limit = 4.0;
    FillRule fill_rule = FillRule::nonzero;
    LineCap line_cap = LineCap::butt;
    LineJoin line_join = LineJoin::miter;
    [[nodiscard]] friend bool operator==(const PaintStyle&, const PaintStyle&) = default;
};

struct DrawCommand final {
    Path path;
    AffineTransform transform;
    PaintStyle paint;
    [[nodiscard]] friend bool operator==(const DrawCommand&, const DrawCommand&) = default;
};

struct ViewBox final {
    double minimum_x = 0.0;
    double minimum_y = 0.0;
    double width = 0.0;
    double height = 0.0;
    [[nodiscard]] friend bool operator==(const ViewBox&, const ViewBox&) = default;
};

enum class AspectAlign : std::uint8_t {
    none,
    minimum,
    middle,
    maximum,
};

struct PreserveAspectRatio final {
    AspectAlign x = AspectAlign::middle;
    AspectAlign y = AspectAlign::middle;
    bool slice = false;
    [[nodiscard]] friend bool operator==(const PreserveAspectRatio&,
                                         const PreserveAspectRatio&) = default;
};

/** A fully resolved, renderer-neutral static SVG display list. */
struct Document final {
    double width = 0.0;
    double height = 0.0;
    ViewBox view_box;
    PreserveAspectRatio preserve_aspect_ratio;
    std::vector<DrawCommand> commands;
    [[nodiscard]] friend bool operator==(const Document&, const Document&) = default;
};

/** Hard parser limits keep untrusted SVG work and allocation bounded. */
struct ParseLimits final {
    std::size_t maximum_input_bytes = 1024U * 1024U;
    std::size_t maximum_elements = 4096U;
    std::size_t maximum_attributes_per_element = 64U;
    std::size_t maximum_nesting_depth = 64U;
    std::size_t maximum_path_segments = 32768U;
};

struct ParseOptions final {
    ParseLimits limits;
    /** Resolves the static `currentColor` paint keyword without requiring CSS. */
    Color current_color{0U, 0U, 0U, 255U};
};

class ParseError final : public std::invalid_argument {
  public:
    ParseError(std::size_t byte_offset, std::string message);
    [[nodiscard]] std::size_t byte_offset() const noexcept {
        return byte_offset_;
    }

  private:
    std::size_t byte_offset_ = 0U;
};

/**
 * Parses the deliberately static SVG subset documented in `native/src/svg/README.md`.
 *
 * The parser has no script engine, CSS cascade, network/resource loader, entity expansion, image
 * decoder, animation timeline, filters, masks, or event model. Unsupported content is rejected.
 */
[[nodiscard]] Document parse(std::string_view source, const ParseOptions& options = {});

struct RasterOptions final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t samples_per_axis = 4U;
    Color background{0U, 0U, 0U, 0U};
};

struct Image final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    /** Straight-alpha RGBA8 pixels in top-to-bottom row-major order. */
    std::vector<std::uint8_t> rgba;

    [[nodiscard]] std::span<const std::uint8_t, 4U> pixel(std::uint32_t x, std::uint32_t y) const;
};

/** Draws a parsed document with a deterministic supersampled CPU rasterizer. */
[[nodiscard]] Image rasterize(const Document& document, const RasterOptions& options = {});

/** Encodes an image as a portable PAM (P7/RGB_ALPHA) byte stream for fixtures and CLI output. */
[[nodiscard]] std::vector<std::uint8_t> encode_pam(const Image& image);

} // namespace strata::svg
