#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/value.hpp"
#include "ui/layout.hpp"
#include "ui/paint.hpp"

namespace strata::ui {

enum class PathVerb : std::uint32_t { move = 0U, line = 1U, quadratic = 2U, cubic = 3U, close = 4U };

struct PathSegment final {
    PathVerb verb = PathVerb::move;
    /** First control point of a quadratic or cubic segment. */
    Point control_a;
    /** Second control point of a cubic segment. */
    Point control_b;
    Point to;
    [[nodiscard]] friend bool operator==(const PathSegment&, const PathSegment&) = default;
};

/** The largest authored or generated segment count. Bounded so tessellation cost is predictable. */
inline constexpr std::size_t maximum_path_segments = 4096U;

/**
 * A resolution-independent outline authored in the normalized space of the shape it is drawn into:
 * (0,0) is its top-left corner and (1,1) its bottom-right one. Curves are kept as curves and are
 * only flattened when a frame knows its device scale.
 */
class Path final {
public:
    Path() = default;
    explicit Path(std::vector<PathSegment> segments);

    void move_to(Point point);
    void line_to(Point point);
    void quadratic_to(Point control, Point point);
    void cubic_to(Point first_control, Point second_control, Point point);
    void close();

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::span<const PathSegment> segments() const noexcept;
    /** Throws std::invalid_argument when the outline is unbounded, malformed or oversized. */
    void validate() const;

    [[nodiscard]] static Path line(Point from, Point to);
    [[nodiscard]] static Path polyline(std::span<const Point> points, bool closed);
    [[nodiscard]] static Path rectangle(Rect bounds, double radius = 0.0);
    [[nodiscard]] static Path ellipse(Point center, double radius_x, double radius_y);
    /**
     * Elliptical arc from `start_degrees` sweeping `sweep_degrees`, clockwise for a positive sweep,
     * with 0 degrees pointing right. `include_center` closes the arc through its centre, which is
     * what a pie slice needs; an open arc is what a progress ring strokes.
     */
    [[nodiscard]] static Path arc(
        Point center,
        double radius_x,
        double radius_y,
        double start_degrees,
        double sweep_degrees,
        bool include_center = false
    );
    /**
     * Parses the compact outline form: `M`/`L`/`H`/`V`/`Q`/`C`/`Z`, each also in its relative
     * lowercase spelling. Throws std::invalid_argument naming the offending command.
     */
    [[nodiscard]] static Path parse(std::string_view commands);

    [[nodiscard]] friend bool operator==(const Path&, const Path&) = default;

private:
    std::vector<PathSegment> segments_;
};

/** One flattened outline. `closed` reports whether the contour returns to its first point. */
struct PathContour final {
    std::vector<Point> points;
    bool closed = false;
};

/**
 * Flattens every contour into polylines whose chordal error stays under `tolerance`, expressed in
 * the same units as `scale` (the size the normalized outline is drawn at, in device pixels).
 */
[[nodiscard]] std::vector<PathContour> flatten_path(
    const Path& path,
    Size scale,
    double tolerance = 0.25
);

enum class PathCap : std::uint32_t { butt = 0U, round = 1U, square = 2U };
enum class PathJoin : std::uint32_t { miter = 0U, round = 1U, bevel = 2U };

/** Stroke geometry. `width` and the dash pattern are logical pixels, not normalized units. */
struct StrokeStyle final {
    double width = 1.0;
    PathCap cap = PathCap::butt;
    PathJoin join = PathJoin::miter;
    double miter_limit = 4.0;
    std::vector<double> dash;
    double dash_offset = 0.0;

    void validate() const;
    [[nodiscard]] friend bool operator==(const StrokeStyle&, const StrokeStyle&) = default;
};

/** One authored shape: an outline plus how it is filled, stroked, or both. */
struct PathShape final {
    Path path;
    std::optional<Paint> fill;
    std::optional<Paint> stroke;
    std::optional<StrokeStyle> stroke_style;
    [[nodiscard]] friend bool operator==(const PathShape&, const PathShape&) = default;
};

/**
 * Reads an authored shape object. `kind` selects the outline: `path`, `line`, `polyline`,
 * `polygon`, `rect`, `circle`, `ellipse` or `arc`. Throws std::invalid_argument describing the
 * first violated rule; returns nothing when the value is not a shape object at all.
 */
[[nodiscard]] std::optional<PathShape> path_shape_from_value(const runtime::Value* value);

[[nodiscard]] std::string_view path_cap_name(PathCap cap) noexcept;
[[nodiscard]] std::string_view path_join_name(PathJoin join) noexcept;

} // namespace strata::ui
