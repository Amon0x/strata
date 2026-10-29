#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "resource/lottie.hpp"
#include "ui/path.hpp"

/**
 * The outline arithmetic a Lottie frame needs: affine maps, cubic contours, trim paths, rounded
 * corners, parametric shapes, and a region sweep that intersects any number of filled polygon sets
 * at once, for masks, mattes and merged paths.
 */
namespace strata::ui::lottie {

/** x' = a x + c y + e; y' = b x + d y + f (SVG's matrix order). */
struct Affine final {
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 1.0;
    double e = 0.0;
    double f = 0.0;

    [[nodiscard]] static Affine translate(double x, double y) noexcept;
    [[nodiscard]] static Affine scale(double x, double y) noexcept;
    /** Clockwise on screen (y down) for positive degrees. */
    [[nodiscard]] static Affine rotate(double degrees) noexcept;
    [[nodiscard]] Point apply(Point point) const noexcept;
    /** Applies the linear part only: a direction or a tangent. */
    [[nodiscard]] Point apply_vector(Point vector) const noexcept;
    [[nodiscard]] double determinant() const noexcept {
        return a * d - b * c;
    }
    [[nodiscard]] std::optional<Affine> inverse() const noexcept;
    /** How much the map stretches lengths, taken as the root of its area scale. */
    [[nodiscard]] double mean_scale() const noexcept;
    [[nodiscard]] bool finite() const noexcept;
    [[nodiscard]] friend bool operator==(const Affine&, const Affine&) = default;
};

/** `left` after `right`: (left * right)(p) = left(right(p)). */
[[nodiscard]] Affine operator*(const Affine& left, const Affine& right) noexcept;

struct Cubic final {
    Point control_a;
    Point control_b;
    Point to;
};

/** A run of cubic segments from `start`. A closed contour's last segment returns to `start`. */
struct Contour final {
    Point start;
    std::vector<Cubic> segments;
    bool closed = false;
};

using CubicPath = std::vector<Contour>;

/** Lottie's outline as a contour: the closing segment of a closed outline becomes explicit. */
[[nodiscard]] Contour contour_from_bezier(const resource::lottie::BezierShape& shape);
/** The same contour traversed the other way, from the same start when it is closed. */
[[nodiscard]] Contour reversed(const Contour& contour);
void transform(CubicPath& path, const Affine& map);

/**
 * Lottie's parametric shapes, starting and winding as After Effects draws them (direction 3
 * reverses): a rectangle from the top of its right edge, an ellipse from its top, and a star or
 * polygon from its first point straight above the centre.
 */
[[nodiscard]] Contour rectangle_contour(Point center, Point size, double roundness, int direction);
[[nodiscard]] Contour ellipse_contour(Point center, Point size, int direction);
[[nodiscard]] Contour polystar_contour(bool star, Point center, double points, double rotation,
                                       double outer_radius, double inner_radius,
                                       double outer_roundness, double inner_roundness,
                                       int direction);

/** A contour's arc length. */
[[nodiscard]] double contour_length(const Contour& contour);

/**
 * The fractions of a path a trim keeps, from `start` to `end` (fractions of its length) moved on
 * by `offset` (a fraction of a turn), wrapping past the end to the start: nothing when the trim
 * keeps the whole path, an empty list when it keeps none of it.
 */
[[nodiscard]] std::optional<std::vector<std::pair<double, double>>>
trim_ranges(double start, double end, double offset);

/** The open part of `contour` between arc lengths `from` and `to`. */
[[nodiscard]] Contour contour_span(const Contour& contour, double from, double to);

/**
 * Trim paths: the part of `path` between `start` and `end`, fractions of its length moved on by
 * `offset` (a fraction of a turn). Each contour is trimmed by its own length, or (`individually`)
 * the contours are measured as one path in order and trimmed together.
 */
[[nodiscard]] CubicPath trim(const CubicPath& path, double start, double end, double offset,
                             bool individually);

/** Rounds every sharp vertex (one whose tangents are both zero) of `path` by `radius`. */
[[nodiscard]] CubicPath round_corners(const CubicPath& path, double radius);

/** The contours as an outline of logical units (no normalization). */
[[nodiscard]] Path to_path(const CubicPath& path);

/**
 * Flattens `path` into polylines within `tolerance` of its curves. Closed contours do not repeat
 * their first point.
 */
[[nodiscard]] std::vector<PathContour> flatten(const CubicPath& path, double tolerance);

/** Polygons filled together by one rule: one input of a sweep. */
struct PolygonSet final {
    std::vector<std::vector<Point>> polygons;
    bool even_odd = false;
};

/** A region of constant value: trapezoids whose top and bottom edges are horizontal. */
struct Trapezoid final {
    double top = 0.0;
    double bottom = 0.0;
    double top_left = 0.0;
    double top_right = 0.0;
    double bottom_left = 0.0;
    double bottom_right = 0.0;
};

struct SweepLevel final {
    /** The region's value, in steps of 1/255. */
    double value = 0.0;
    std::vector<Trapezoid> trapezoids;
};

/**
 * Sweeps the plane through every set at once and returns where `value` (given, per set, whether a
 * point is inside it) is positive, grouped by that value. Between two consecutive vertex heights
 * and crossings no edge crosses another, so the plane cuts into trapezoids whose insides each lie
 * in a fixed combination of sets; equal neighbours merge. The trapezoids of one level tile its
 * region exactly, sharing edges, so a coverage rasterizer draws them seamlessly as one shape.
 */
[[nodiscard]] std::vector<SweepLevel>
sweep(std::span<const PolygonSet> sets,
      const std::function<double(std::span<const std::uint8_t> inside)>& value);

/** The trapezoids as closed polygons, each wound clockwise on screen. */
[[nodiscard]] std::vector<std::vector<Point>> trapezoid_polygons(std::span<const Trapezoid> region);

/**
 * The outline of a region made of trapezoids (one level of a sweep): closed loops, wound so the
 * region is on their right, with every edge two trapezoids share left out.
 */
[[nodiscard]] std::vector<std::vector<Point>> region_outline(std::span<const Trapezoid> region);

} // namespace strata::ui::lottie
