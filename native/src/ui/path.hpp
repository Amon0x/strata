#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "geometry/path.hpp"
#include "runtime/value.hpp"
#include "ui/layout.hpp"
#include "ui/paint.hpp"

namespace strata::ui {

using geometry::flatten_path;
using geometry::maximum_path_segments;
using geometry::Path;
using geometry::PathContour;
using geometry::PathSegment;
using geometry::PathVerb;

enum class PathCap : std::uint32_t { butt = 0U, round = 1U, square = 2U };
enum class PathJoin : std::uint32_t { miter = 0U, round = 1U, bevel = 2U };
enum class PathFillRule : std::uint32_t { nonzero = 0U, evenodd = 1U };

/** Stroke geometry. `width` and the dash pattern are logical pixels, not normalized units. */
struct StrokeStyle final {
    double width = 1.0;
    PathCap cap = PathCap::butt;
    PathJoin join = PathJoin::miter;
    double miter_limit = 4.0;
    std::vector<double> dash{};
    double dash_offset = 0.0;

    void validate() const;
    [[nodiscard]] friend bool operator==(const StrokeStyle&, const StrokeStyle&) = default;
};

enum class ShapePrimitiveKind : std::uint8_t { ellipse, sector, arc, segment, rect };

/**
 * What a shape is when it is one the pixel stage draws analytically, in the shape's normalized
 * space like its path: an ellipse, a ring sector (a pie slice when the inner radius is zero), an
 * open arc, a line segment, or a rectangle (centre and half extents in the radii, its corner
 * radius in `corner`). The path stays the shape's exact outline for everything else.
 */
struct ShapePrimitive final {
    ShapePrimitiveKind kind = ShapePrimitiveKind::ellipse;
    Point center{0.5, 0.5};
    double radius_x = 0.5;
    double radius_y = 0.5;
    /** A sector's inner radius as a fraction of its radii. */
    double inner_fraction = 0.0;
    /** Degrees, clockwise from +x: a sector's or arc's start and sweep. */
    double start_degrees = 0.0;
    double sweep_degrees = 360.0;
    /** A segment's ends. */
    Point from;
    Point to;
    /** A rectangle's corner radius, normalized like its path's. */
    double corner = 0.0;
    [[nodiscard]] friend bool operator==(const ShapePrimitive&, const ShapePrimitive&) = default;
};

/** One authored shape: an outline plus how it is filled, stroked, or both. */
struct PathShape final {
    Path path;
    std::optional<Paint> fill;
    std::optional<Paint> stroke;
    std::optional<StrokeStyle> stroke_style;
    PathFillRule fill_rule = PathFillRule::nonzero;
    /** The analytic form the shape also has, when it has one. */
    std::optional<ShapePrimitive> primitive{};
    [[nodiscard]] friend bool operator==(const PathShape&, const PathShape&) = default;
};

/**
 * Shapes built in code, in the normalized space of the rectangle they are drawn into, carrying
 * their analytic form so the pixel stage draws them exactly: an ellipse filled and/or stroked, a
 * ring sector filled (a pie slice when `inner_fraction` is zero, a ring when it sweeps 360), an open
 * arc stroked, and a line segment stroked. Angles are degrees clockwise from +x.
 */
[[nodiscard]] PathShape ellipse_shape(Point center, double radius_x, double radius_y,
                                      std::optional<Paint> fill,
                                      std::optional<Paint> stroke = std::nullopt,
                                      std::optional<StrokeStyle> stroke_style = std::nullopt);
[[nodiscard]] PathShape sector_shape(Point center, double radius_x, double radius_y,
                                     double inner_fraction, double start_degrees,
                                     double sweep_degrees, Paint fill);
[[nodiscard]] PathShape arc_shape(Point center, double radius_x, double radius_y,
                                  double start_degrees, double sweep_degrees, Paint stroke,
                                  StrokeStyle stroke_style);
[[nodiscard]] PathShape segment_shape(Point from, Point to, Paint stroke,
                                      StrokeStyle stroke_style);

/**
 * Reads an authored shape object. `kind` selects the outline: `path`, `line`, `polyline`,
 * `polygon`, `rect`, `circle`, `ellipse` or `arc`. Throws std::invalid_argument describing the
 * first violated rule; returns nothing when the value is not a shape object at all. A shape drawn
 * (`painted`) needs a fill or a stroke; one that is only an outline, such as a hit shape, does not.
 */
[[nodiscard]] std::optional<PathShape> path_shape_from_value(const runtime::Value* value,
                                                             bool painted = true);

/**
 * `shape` as far as `amount` (0 to 1) has drawn it, at `size` logical pixels: a sector or arc
 * swept open from its start angle, a line grown from its start, any other stroke drawn along its
 * outline from where it starts (a circle or ellipse from its top, clockwise), and a fill faded in.
 * Nothing at 0; the shape itself at 1. A shape both filled and stroked comes back as two.
 */
[[nodiscard]] std::vector<PathShape> revealed_shapes(const PathShape& shape, double amount,
                                                     Size size);

/**
 * Whether two of `shapes`, drawn in one frame of `size` logical pixels, share part of an edge: a
 * stretch of one's outline, straight or curved, running along another's for half a pixel or more.
 * Such shapes leave a hairline of whatever is behind them where they meet unless they are drawn
 * as one picture. Shapes touching at a point only do not count: one pixel there bleeds unseen.
 */
[[nodiscard]] bool shapes_share_edges(std::span<const PathShape> shapes, Size size);

/**
 * The numbers authored shapes carry (coordinates, radii, angles, widths, dash lengths and colour
 * channels) in a fixed traversal order, led by a signature of everything else: kinds and other
 * strings, booleans, list lengths and field names. Two shape lists with the same structure give
 * lists of the same length and signature, so they interpolate number by number.
 */
[[nodiscard]] std::vector<double> shape_numbers(const runtime::Value& shapes);

/**
 * `shapes` with its numbers replaced by `numbers`, in shape_numbers' order; nothing when `numbers`
 * came from shapes of another structure.
 */
[[nodiscard]] std::optional<runtime::Value> with_shape_numbers(const runtime::Value& shapes,
                                                              std::span<const double> numbers);

/**
 * Reads an `image` shape's `points`: the top-left, top-right and bottom-left corners of the
 * parallelogram its image region is mapped onto. Throws std::invalid_argument unless there are
 * exactly three finite points.
 */
[[nodiscard]] std::array<Point, 3U> image_shape_corners(const runtime::Value& shape);

[[nodiscard]] std::string_view path_cap_name(PathCap cap) noexcept;
[[nodiscard]] std::string_view path_join_name(PathJoin join) noexcept;

} // namespace strata::ui
