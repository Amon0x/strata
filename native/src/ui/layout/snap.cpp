#include "ui/layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

#include "ui/layout/detail_algorithms.hpp"

namespace strata::ui {
using namespace layout_detail;

Point snap_point(Point point, const LayoutEnvironment& environment) {
    if (environment.point_snapping == PointSnapPolicy::none) return point;
    point.x = std::round(point.x * environment.scale) / environment.scale;
    point.y = std::round(point.y * environment.scale) / environment.scale;
    return point;
}

Rect snap_rectangle(Rect rectangle, const LayoutEnvironment& environment) {
    if (environment.rectangle_snapping == RectangleSnapPolicy::none) return rectangle;
    if (environment.rectangle_snapping == RectangleSnapPolicy::nearest) {
        const Point origin = snap_point(Point{rectangle.x, rectangle.y}, environment);
        const Point extent = snap_point(Point{rectangle.right(), rectangle.bottom()}, environment);
        return Rect{origin.x, origin.y, std::max(0.0, extent.x - origin.x), std::max(0.0, extent.y - origin.y)};
    }
    const double left = std::floor(rectangle.x * environment.scale) / environment.scale;
    const double top = std::floor(rectangle.y * environment.scale) / environment.scale;
    const double right_value = std::ceil(rectangle.right() * environment.scale) / environment.scale;
    const double bottom_value = std::ceil(rectangle.bottom() * environment.scale) / environment.scale;
    return Rect{left, top, std::max(0.0, right_value - left), std::max(0.0, bottom_value - top)};
}

std::string_view layout_kind_name(const LayoutKind value) noexcept {
    switch (value) {
    case LayoutKind::stack: return "stack";
    case LayoutKind::row: return "row";
    case LayoutKind::column: return "column";
    case LayoutKind::grid: return "grid";
    case LayoutKind::panel: return "panel";
    case LayoutKind::overlay: return "overlay";
    case LayoutKind::spacer: return "spacer";
    case LayoutKind::scroll: return "scroll";
    case LayoutKind::portal: return "portal";
    }
    return "panel";
}
} // namespace strata::ui
