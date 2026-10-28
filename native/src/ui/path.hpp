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

/** One authored shape: an outline plus how it is filled, stroked, or both. */
struct PathShape final {
    Path path;
    std::optional<Paint> fill;
    std::optional<Paint> stroke;
    std::optional<StrokeStyle> stroke_style;
    PathFillRule fill_rule = PathFillRule::nonzero;
    [[nodiscard]] friend bool operator==(const PathShape&, const PathShape&) = default;
};

/**
 * Reads an authored shape object. `kind` selects the outline: `path`, `line`, `polyline`,
 * `polygon`, `rect`, `circle`, `ellipse` or `arc`. Throws std::invalid_argument describing the
 * first violated rule; returns nothing when the value is not a shape object at all.
 */
[[nodiscard]] std::optional<PathShape> path_shape_from_value(const runtime::Value* value);

/**
 * Reads an `image` shape's `points`: the top-left, top-right and bottom-left corners of the
 * parallelogram its image region is mapped onto. Throws std::invalid_argument unless there are
 * exactly three finite points.
 */
[[nodiscard]] std::array<Point, 3U> image_shape_corners(const runtime::Value& shape);

[[nodiscard]] std::string_view path_cap_name(PathCap cap) noexcept;
[[nodiscard]] std::string_view path_join_name(PathJoin join) noexcept;

} // namespace strata::ui
