#include "ui/path.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>

namespace strata::ui {
namespace {
[[nodiscard]] const runtime::Value* field(const runtime::Value& value, const std::string_view name) {
    return value.field(name);
}

[[nodiscard]] std::optional<double> number_field(
    const runtime::Value& value,
    const std::string_view name
) {
    const runtime::Value* found = field(value, name);
    if (found == nullptr || found->number() == nullptr) return std::nullopt;
    if (!std::isfinite(*found->number())) {
        throw std::invalid_argument("shape field '" + std::string(name) + "' must be finite");
    }
    return *found->number();
}

[[nodiscard]] std::optional<Point> point_field(
    const runtime::Value& value,
    const std::string_view name
) {
    const runtime::Value* found = field(value, name);
    if (found == nullptr || found->object() == nullptr) return std::nullopt;
    const std::optional<double> x = number_field(*found, "x");
    const std::optional<double> y = number_field(*found, "y");
    if (!x.has_value() || !y.has_value()) {
        throw std::invalid_argument("shape point '" + std::string(name) + "' requires x and y");
    }
    return Point{*x, *y};
}

[[nodiscard]] std::vector<Point> point_list(
    const runtime::Value& value,
    const std::string_view name
) {
    const runtime::Value* found = field(value, name);
    const runtime::ValueList* list = found != nullptr ? found->list() : nullptr;
    if (list == nullptr) {
        throw std::invalid_argument("shape field '" + std::string(name) + "' requires a point list");
    }
    std::vector<Point> points;
    points.reserve(list->values.size());
    for (const runtime::Value& entry : list->values) {
        const std::optional<double> x = number_field(entry, "x");
        const std::optional<double> y = number_field(entry, "y");
        if (!x.has_value() || !y.has_value()) {
            throw std::invalid_argument("every shape point requires finite x and y");
        }
        points.push_back(Point{*x, *y});
    }
    return points;
}

[[nodiscard]] PathCap cap_from_text(const std::string& text) {
    if (text == "butt") return PathCap::butt;
    if (text == "round") return PathCap::round;
    if (text == "square") return PathCap::square;
    throw std::invalid_argument("stroke cap must be butt, round or square");
}

[[nodiscard]] PathJoin join_from_text(const std::string& text) {
    if (text == "miter") return PathJoin::miter;
    if (text == "round") return PathJoin::round;
    if (text == "bevel") return PathJoin::bevel;
    throw std::invalid_argument("stroke join must be miter, round or bevel");
}

} // namespace

void StrokeStyle::validate() const {
    if (!std::isfinite(width) || width <= 0.0) {
        throw std::invalid_argument("a stroke width must be finite and positive");
    }
    if (!std::isfinite(miter_limit) || miter_limit < 1.0) {
        throw std::invalid_argument("a stroke miter limit must be at least one");
    }
    if (!std::isfinite(dash_offset)) {
        throw std::invalid_argument("a stroke dash offset must be finite");
    }
    if (dash.size() > 16U) {
        throw std::invalid_argument("a stroke dash pattern supports at most sixteen entries");
    }
    double total = 0.0;
    for (const double entry : dash) {
        if (!std::isfinite(entry) || entry < 0.0) {
            throw std::invalid_argument("stroke dash entries must be finite and non-negative");
        }
        total += entry;
    }
    if (!dash.empty() && total <= 0.0) {
        throw std::invalid_argument("a stroke dash pattern must have a positive period");
    }
}

std::array<Point, 3U> image_shape_corners(const runtime::Value& shape) {
    const std::vector<Point> points = point_list(shape, "points");
    if (points.size() != 3U) {
        throw std::invalid_argument(
            "an image shape requires three points: top-left, top-right and bottom-left");
    }
    return {points[0], points[1], points[2]};
}

std::string_view path_cap_name(const PathCap cap) noexcept {
    switch (cap) {
    case PathCap::butt: return "butt";
    case PathCap::round: return "round";
    case PathCap::square: return "square";
    }
    return "butt";
}

std::string_view path_join_name(const PathJoin join) noexcept {
    switch (join) {
    case PathJoin::miter: return "miter";
    case PathJoin::round: return "round";
    case PathJoin::bevel: return "bevel";
    }
    return "miter";
}

std::optional<PathShape> path_shape_from_value(const runtime::Value* const value) {
    if (value == nullptr || value->object() == nullptr) return std::nullopt;
    const runtime::Value* kind_value = field(*value, "kind");
    if (kind_value == nullptr || kind_value->string() == nullptr) return std::nullopt;
    const std::string& kind = *kind_value->string();
    PathShape shape;
    if (kind == "path") {
        const runtime::Value* commands = field(*value, "commands");
        if (commands == nullptr || commands->string() == nullptr) {
            throw std::invalid_argument("a path shape requires its 'commands' outline");
        }
        shape.path = Path::parse(*commands->string());
    } else if (kind == "line") {
        const std::optional<Point> from = point_field(*value, "from");
        const std::optional<Point> to = point_field(*value, "to");
        if (!from.has_value() || !to.has_value()) {
            throw std::invalid_argument("a line shape requires 'from' and 'to'");
        }
        shape.path = Path::line(*from, *to);
    } else if (kind == "polyline" || kind == "polygon") {
        shape.path = Path::polyline(point_list(*value, "points"), kind == "polygon");
    } else if (kind == "rect") {
        const std::optional<double> x = number_field(*value, "x");
        const std::optional<double> y = number_field(*value, "y");
        const std::optional<double> width = number_field(*value, "width");
        const std::optional<double> height = number_field(*value, "height");
        if (!width.has_value() || !height.has_value()) {
            throw std::invalid_argument("a rect shape requires 'width' and 'height'");
        }
        shape.path = Path::rectangle(
            Rect{x.value_or(0.0), y.value_or(0.0), *width, *height},
            number_field(*value, "radius").value_or(0.0)
        );
    } else if (kind == "circle" || kind == "ellipse") {
        const Point center = point_field(*value, "center").value_or(Point{0.5, 0.5});
        const std::optional<double> radius = number_field(*value, "radius");
        const double radius_x = number_field(*value, "radiusX").value_or(radius.value_or(0.5));
        const double radius_y = number_field(*value, "radiusY").value_or(radius.value_or(0.5));
        shape.path = Path::ellipse(center, radius_x, radius_y);
    } else if (kind == "arc") {
        const Point center = point_field(*value, "center").value_or(Point{0.5, 0.5});
        const std::optional<double> radius = number_field(*value, "radius");
        const std::optional<double> start = number_field(*value, "start");
        const std::optional<double> sweep = number_field(*value, "sweep");
        if (!start.has_value() || !sweep.has_value()) {
            throw std::invalid_argument("an arc shape requires 'start' and 'sweep' angles");
        }
        shape.path = Path::arc(
            center,
            number_field(*value, "radiusX").value_or(radius.value_or(0.5)),
            number_field(*value, "radiusY").value_or(radius.value_or(0.5)),
            *start,
            *sweep,
            field(*value, "includeCenter") != nullptr &&
                field(*value, "includeCenter")->boolean() != nullptr &&
                *field(*value, "includeCenter")->boolean()
        );
    } else {
        throw std::invalid_argument("unsupported shape kind '" + kind + "'");
    }
    shape.path.validate();
    shape.fill = paint_from_value(field(*value, "fill"));
    shape.stroke = paint_from_value(field(*value, "stroke"));
    if (const runtime::Value* fill_rule = field(*value, "fillRule");
        fill_rule != nullptr && fill_rule->string() != nullptr) {
        if (*fill_rule->string() == "evenodd") {
            shape.fill_rule = PathFillRule::evenodd;
        } else if (*fill_rule->string() != "nonzero") {
            throw std::invalid_argument("a shape fill rule must be nonzero or evenodd");
        }
    }
    const runtime::Value* stroke_style = field(*value, "strokeStyle");
    if (stroke_style != nullptr && stroke_style->object() != nullptr) {
        StrokeStyle style;
        style.width = number_field(*stroke_style, "width").value_or(1.0);
        style.miter_limit = number_field(*stroke_style, "miterLimit").value_or(4.0);
        style.dash_offset = number_field(*stroke_style, "dashOffset").value_or(0.0);
        if (const runtime::Value* cap = field(*stroke_style, "cap");
            cap != nullptr && cap->string() != nullptr) {
            style.cap = cap_from_text(*cap->string());
        }
        if (const runtime::Value* join = field(*stroke_style, "join");
            join != nullptr && join->string() != nullptr) {
            style.join = join_from_text(*join->string());
        }
        if (const runtime::Value* dash = field(*stroke_style, "dash"); dash != nullptr) {
            const runtime::ValueList* list = dash->list();
            if (list == nullptr) throw std::invalid_argument("a stroke dash must be a number list");
            for (const runtime::Value& entry : list->values) {
                if (entry.number() == nullptr) {
                    throw std::invalid_argument("a stroke dash must be a number list");
                }
                style.dash.push_back(*entry.number());
            }
        }
        style.validate();
        shape.stroke_style = style;
    }
    if (shape.stroke.has_value() && !shape.stroke_style.has_value()) {
        shape.stroke_style = StrokeStyle{};
    }
    if (!shape.fill.has_value() && !shape.stroke.has_value()) {
        throw std::invalid_argument("a shape requires a fill, a stroke, or both");
    }
    return shape;
}

} // namespace strata::ui
