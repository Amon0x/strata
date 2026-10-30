#include "ui/path.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <unordered_map>
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

PathShape ellipse_shape(const Point center, const double radius_x, const double radius_y,
                        std::optional<Paint> fill, std::optional<Paint> stroke,
                        std::optional<StrokeStyle> stroke_style) {
    if (stroke.has_value() && !stroke_style.has_value())
        stroke_style = StrokeStyle{};
    PathShape shape{Path::ellipse(center, radius_x, radius_y), std::move(fill), std::move(stroke),
                    std::move(stroke_style)};
    shape.primitive = ShapePrimitive{ShapePrimitiveKind::ellipse, center, radius_x, radius_y};
    return shape;
}

PathShape sector_shape(const Point center, const double radius_x, const double radius_y,
                       const double inner_fraction, const double start_degrees,
                       const double sweep_degrees, Paint fill) {
    PathShape shape{
        Path::sector(center, radius_x, radius_y, inner_fraction, start_degrees, sweep_degrees),
        std::move(fill)};
    shape.primitive = ShapePrimitive{ShapePrimitiveKind::sector, center, radius_x, radius_y,
                                     inner_fraction, start_degrees, sweep_degrees};
    return shape;
}

PathShape arc_shape(const Point center, const double radius_x, const double radius_y,
                    const double start_degrees, const double sweep_degrees, Paint stroke,
                    StrokeStyle stroke_style) {
    PathShape shape{Path::arc(center, radius_x, radius_y, start_degrees, sweep_degrees, false),
                    std::nullopt, std::move(stroke), std::move(stroke_style)};
    shape.primitive = ShapePrimitive{ShapePrimitiveKind::arc, center, radius_x, radius_y, 0.0,
                                     start_degrees, sweep_degrees};
    return shape;
}

PathShape segment_shape(const Point from, const Point to, Paint stroke, StrokeStyle stroke_style) {
    PathShape shape{Path::line(from, to), std::nullopt, std::move(stroke), std::move(stroke_style)};
    shape.primitive = ShapePrimitive{ShapePrimitiveKind::segment};
    shape.primitive->from = from;
    shape.primitive->to = to;
    return shape;
}

double path_pixel_length(const Path& path, const Size size) {
    double total = 0.0;
    const auto length = [&](const Point from, const Point to) {
        return std::hypot((to.x - from.x) * size.width, (to.y - from.y) * size.height);
    };
    for (const PathContour& contour :
         flatten_path(path, size, 0.25 / std::max(1.0, std::max(size.width, size.height)))) {
        for (std::size_t index = 1U; index < contour.points.size(); ++index)
            total += length(contour.points[index - 1U], contour.points[index]);
        if (contour.closed && contour.points.size() > 1U)
            total += length(contour.points.back(), contour.points.front());
    }
    return total;
}

std::optional<PathShape> path_shape_from_value(const runtime::Value* const value,
                                               const bool painted) {
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
        shape.primitive = ShapePrimitive{ShapePrimitiveKind::segment};
        shape.primitive->from = *from;
        shape.primitive->to = *to;
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
        const Rect area{x.value_or(0.0), y.value_or(0.0), *width, *height};
        const double radius = number_field(*value, "radius").value_or(0.0);
        shape.path = Path::rectangle(area, radius);
        if (area.width > 0.0 && area.height > 0.0) {
            ShapePrimitive primitive{ShapePrimitiveKind::rect,
                                     Point{area.x + area.width * 0.5, area.y + area.height * 0.5},
                                     area.width * 0.5, area.height * 0.5};
            primitive.corner =
                std::clamp(radius, 0.0, std::min(area.width, area.height) * 0.5);
            shape.primitive = primitive;
        }
    } else if (kind == "circle" || kind == "ellipse") {
        const Point center = point_field(*value, "center").value_or(Point{0.5, 0.5});
        const std::optional<double> radius = number_field(*value, "radius");
        const double radius_x = number_field(*value, "radiusX").value_or(radius.value_or(0.5));
        const double radius_y = number_field(*value, "radiusY").value_or(radius.value_or(0.5));
        shape.path = Path::ellipse(center, radius_x, radius_y);
        shape.primitive = ShapePrimitive{ShapePrimitiveKind::ellipse, center, radius_x, radius_y};
    } else if (kind == "sector" || kind == "ring") {
        const Point center = point_field(*value, "center").value_or(Point{0.5, 0.5});
        const std::optional<double> radius = number_field(*value, "radius");
        const double radius_x = number_field(*value, "radiusX").value_or(radius.value_or(0.5));
        const double radius_y = number_field(*value, "radiusY").value_or(radius.value_or(0.5));
        const double inner = number_field(*value, "innerRadius").value_or(kind == "ring" ? 0.3 : 0.0);
        const double outer = std::max(radius_x, radius_y);
        if (!(outer > 0.0) || inner < 0.0 || inner >= outer) {
            throw std::invalid_argument("a sector's innerRadius must be at least 0 and below its radius");
        }
        const double start = kind == "ring" ? 0.0 : number_field(*value, "start").value_or(0.0);
        const double sweep = kind == "ring" ? 360.0 : number_field(*value, "sweep").value_or(360.0);
        shape.path = Path::sector(center, radius_x, radius_y, inner / outer, start, sweep);
        shape.primitive = ShapePrimitive{ShapePrimitiveKind::sector, center, radius_x, radius_y,
                                         inner / outer, start, sweep};
    } else if (kind == "arc") {
        const Point center = point_field(*value, "center").value_or(Point{0.5, 0.5});
        const std::optional<double> radius = number_field(*value, "radius");
        const std::optional<double> start = number_field(*value, "start");
        const std::optional<double> sweep = number_field(*value, "sweep");
        if (!start.has_value() || !sweep.has_value()) {
            throw std::invalid_argument("an arc shape requires 'start' and 'sweep' angles");
        }
        const double radius_x = number_field(*value, "radiusX").value_or(radius.value_or(0.5));
        const double radius_y = number_field(*value, "radiusY").value_or(radius.value_or(0.5));
        const bool include_center = field(*value, "includeCenter") != nullptr &&
                                    field(*value, "includeCenter")->boolean() != nullptr &&
                                    *field(*value, "includeCenter")->boolean();
        shape.path = Path::arc(center, radius_x, radius_y, *start, *sweep, include_center);
        shape.primitive = ShapePrimitive{
            include_center ? ShapePrimitiveKind::sector : ShapePrimitiveKind::arc,
            center, radius_x, radius_y, 0.0, *start, *sweep};
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
    if (painted && !shape.fill.has_value() && !shape.stroke.has_value()) {
        throw std::invalid_argument("a shape requires a fill, a stroke, or both");
    }
    return shape;
}

namespace {

/** FNV-1a over the structure of a value: everything shape_numbers does not list. */
class ShapeSignature final {
  public:
    void add(const std::string_view text) noexcept {
        for (const char byte : text)
            mix(static_cast<unsigned char>(byte));
        mix(0xFFU);
    }
    void add(const std::uint64_t value) noexcept {
        for (int shift = 0; shift < 64; shift += 8)
            mix(static_cast<unsigned char>(value >> shift));
    }
    /** Exact as a double: 52 bits. */
    [[nodiscard]] double value() const noexcept {
        return static_cast<double>(state_ >> 12U);
    }

  private:
    void mix(const unsigned char byte) noexcept {
        state_ ^= byte;
        state_ *= 1099511628211ULL;
    }
    std::uint64_t state_ = 1469598103934665603ULL;
};

void collect_shape_numbers(const runtime::Value& value, std::vector<double>& numbers,
                           ShapeSignature& signature) {
    signature.add(static_cast<std::uint64_t>(value.kind()));
    if (const double* number = value.number(); number != nullptr) {
        numbers.push_back(*number);
    } else if (const runtime::ColorValue* color = value.color(); color != nullptr) {
        numbers.push_back(color->red);
        numbers.push_back(color->green);
        numbers.push_back(color->blue);
        numbers.push_back(color->alpha);
    } else if (const std::string* text = value.string(); text != nullptr) {
        signature.add(*text);
    } else if (const bool* boolean = value.boolean(); boolean != nullptr) {
        signature.add(static_cast<std::uint64_t>(*boolean));
    } else if (const runtime::ValueList* list = value.list(); list != nullptr) {
        signature.add(static_cast<std::uint64_t>(list->values.size()));
        for (const runtime::Value& entry : list->values)
            collect_shape_numbers(entry, numbers, signature);
    } else if (const runtime::ValueObject* object = value.object(); object != nullptr) {
        signature.add(static_cast<std::uint64_t>(object->fields.size()));
        for (const auto& [name, entry] : object->fields) {
            signature.add(name);
            collect_shape_numbers(entry, numbers, signature);
        }
    }
}

[[nodiscard]] runtime::Value replace_shape_numbers(const runtime::Value& value,
                                                   std::span<const double> numbers,
                                                   std::size_t& next) {
    if (value.number() != nullptr)
        return runtime::Value(numbers[next++]);
    if (value.color() != nullptr) {
        const auto channel = [&]() {
            return static_cast<std::uint8_t>(std::clamp(std::round(numbers[next++]), 0.0, 255.0));
        };
        const std::uint8_t red = channel();
        const std::uint8_t green = channel();
        const std::uint8_t blue = channel();
        const std::uint8_t alpha = channel();
        return runtime::Value(runtime::ColorValue{red, green, blue, alpha});
    }
    if (const runtime::ValueList* list = value.list(); list != nullptr) {
        std::vector<runtime::Value> values;
        values.reserve(list->values.size());
        for (const runtime::Value& entry : list->values)
            values.push_back(replace_shape_numbers(entry, numbers, next));
        return runtime::Value(std::move(values));
    }
    if (const runtime::ValueObject* object = value.object(); object != nullptr) {
        std::vector<std::pair<std::string, runtime::Value>> fields;
        fields.reserve(object->fields.size());
        for (const auto& [name, entry] : object->fields)
            fields.emplace_back(name, replace_shape_numbers(entry, numbers, next));
        return runtime::Value(std::move(fields));
    }
    return value;
}

} // namespace

bool shapes_share_edges(const std::span<const PathShape> shapes, const Size size) {
    if (shapes.size() < 2U || !(size.width > 0.0) || !(size.height > 0.0))
        return false;
    // An edge is shared where a stretch of one shape's outline runs along another's, straight or
    // curved, for half a pixel or more in all: parallel and within a twentieth of a pixel.
    constexpr double reach = 0.05;
    constexpr double flatness = 0.02;
    // Only shapes whose control-point boxes come within reach of another's can share an edge.
    std::vector<Rect> boxes(shapes.size());
    std::vector<bool> candidate(shapes.size(), false);
    for (std::size_t index = 0U; index < shapes.size(); ++index) {
        const PathShape& shape = shapes[index];
        if (!shape.fill.has_value())
            continue;
        double left = std::numeric_limits<double>::infinity();
        double top = left;
        double right = -left;
        double bottom = -left;
        for (const PathSegment& segment : shape.path.segments()) {
            if (segment.verb == PathVerb::close)
                continue;
            const auto include = [&](const Point point) {
                left = std::min(left, point.x * size.width);
                top = std::min(top, point.y * size.height);
                right = std::max(right, point.x * size.width);
                bottom = std::max(bottom, point.y * size.height);
            };
            include(segment.to);
            if (segment.verb == PathVerb::quadratic || segment.verb == PathVerb::cubic)
                include(segment.control_a);
            if (segment.verb == PathVerb::cubic)
                include(segment.control_b);
        }
        if (std::isfinite(left))
            boxes[index] = Rect{left - reach, top - reach, right - left + reach * 2.0,
                                bottom - top + reach * 2.0};
    }
    for (std::size_t first = 0U; first < shapes.size(); ++first) {
        for (std::size_t second = first + 1U; second < shapes.size(); ++second) {
            const Rect& one = boxes[first];
            const Rect& two = boxes[second];
            if (one.width > 0.0 && two.width > 0.0 && one.x <= two.right() && two.x <= one.right() &&
                one.y <= two.bottom() && two.y <= one.bottom()) {
                candidate[first] = true;
                candidate[second] = true;
            }
        }
    }

    // Circular sectors, rings and circles are compared exactly: their arcs as arcs, their cuts as
    // the straight edges they are. Everything else is flattened finely enough that two flattenings
    // of one curve stay within a few hundredths of a pixel of each other.
    struct Arc final {
        std::size_t shape = 0U;
        Point center;
        double radius = 0.0;
        double start = 0.0;
        double sweep = 0.0;
    };
    struct Edge final {
        std::size_t shape = 0U;
        Point from;
        Point to;
    };
    std::vector<Arc> arcs;
    std::vector<Edge> lines;
    // Shapes with curves other than a circle's: their flattened chords are compared by proximity.
    std::vector<bool> curved(shapes.size(), false);
    for (std::size_t index = 0U; index < shapes.size(); ++index) {
        if (!candidate[index])
            continue;
        const PathShape& shape = shapes[index];
        if (shape.primitive.has_value() &&
            (shape.primitive->kind == ShapePrimitiveKind::sector ||
             shape.primitive->kind == ShapePrimitiveKind::ellipse)) {
            const ShapePrimitive& primitive = *shape.primitive;
            const double radius = primitive.radius_x * size.width;
            if (std::abs(radius - primitive.radius_y * size.height) <= 1.0e-6 * std::max(radius, 1.0)) {
                const Point center{primitive.center.x * size.width, primitive.center.y * size.height};
                const bool sector = primitive.kind == ShapePrimitiveKind::sector;
                double start = sector ? primitive.start_degrees : 0.0;
                double sweep = sector ? std::clamp(primitive.sweep_degrees, -360.0, 360.0) : 360.0;
                if (sweep < 0.0) {
                    start += sweep;
                    sweep = -sweep;
                }
                const double inner = sector ? radius * primitive.inner_fraction : 0.0;
                arcs.push_back(Arc{index, center, radius, start, sweep});
                if (inner > 0.0)
                    arcs.push_back(Arc{index, center, inner, start, sweep});
                if (sector && sweep < 360.0) {
                    for (const double angle : {start, start + sweep}) {
                        const double radians = angle * std::numbers::pi / 180.0;
                        const Point direction{std::cos(radians), std::sin(radians)};
                        lines.push_back(Edge{index,
                                             Point{center.x + direction.x * inner,
                                                   center.y + direction.y * inner},
                                             Point{center.x + direction.x * radius,
                                                   center.y + direction.y * radius}});
                    }
                }
                continue;
            }
        }
        curved[index] = std::ranges::any_of(shape.path.segments(), [](const PathSegment& segment) {
            return segment.verb == PathVerb::quadratic || segment.verb == PathVerb::cubic;
        });
        std::vector<PathContour> contours;
        try {
            contours = flatten_path(shape.path, size, flatness);
        } catch (const std::exception&) {
            continue;
        }
        for (const PathContour& contour : contours) {
            const std::size_t count = contour.points.size();
            for (std::size_t at = 0U; count >= 2U && at < count; ++at) {
                const Point a = contour.points[at];
                const Point b = contour.points[(at + 1U) % count];
                lines.push_back(Edge{index, Point{a.x * size.width, a.y * size.height},
                                     Point{b.x * size.width, b.y * size.height}});
            }
        }
    }

    // Arcs on one circle (one centre, one radius) whose angles overlap by half a pixel.
    for (std::size_t first = 0U; first < arcs.size(); ++first) {
        for (std::size_t second = first + 1U; second < arcs.size(); ++second) {
            const Arc& one = arcs[first];
            const Arc& two = arcs[second];
            if (one.shape == two.shape || std::abs(one.radius - two.radius) > reach ||
                std::hypot(one.center.x - two.center.x, one.center.y - two.center.y) > reach) {
                continue;
            }
            // The overlap of the two angle ranges, either way round the circle.
            double overlap = 0.0;
            for (const double turn : {-360.0, 0.0, 360.0}) {
                overlap += std::max(0.0, std::min(one.start + one.sweep, two.start + two.sweep + turn) -
                                             std::max(one.start, two.start + turn));
            }
            if (overlap * std::numbers::pi / 180.0 * one.radius >= 0.5)
                return true;
        }
    }

    // Straight edges file under their line, a direction and a distance from the origin rounded to
    // a twentieth of a pixel, with their extent along it; two shapes' edges on one line that
    // overlap by half a pixel are shared.
    struct Extent final {
        std::size_t shape = 0U;
        double from = 0.0;
        double to = 0.0;
    };
    std::unordered_map<std::uint64_t, std::vector<Extent>> on_line;
    for (const Edge& line : lines) {
        const double length = std::hypot(line.to.x - line.from.x, line.to.y - line.from.y);
        if (!(length >= 0.5) || !std::isfinite(length))
            continue;
        double dx = (line.to.x - line.from.x) / length;
        double dy = (line.to.y - line.from.y) / length;
        if (dx < 0.0 || (dx == 0.0 && dy < 0.0)) {
            dx = -dx;
            dy = -dy;
        }
        const auto direction = static_cast<std::uint32_t>(std::llround(std::atan2(dy, dx) * 1.0e4));
        const auto offset = static_cast<std::uint32_t>(
            std::llround((dx * line.from.y - dy * line.from.x) * 20.0));
        std::vector<Extent>& extents = on_line[(std::uint64_t{direction} << 32U) | offset];
        const double t0 = dx * line.from.x + dy * line.from.y;
        const double t1 = dx * line.to.x + dy * line.to.y;
        for (const Extent& other : extents) {
            if (other.shape != line.shape &&
                std::min(other.to, std::max(t0, t1)) - std::max(other.from, std::min(t0, t1)) >= 0.5) {
                return true;
            }
        }
        extents.push_back(Extent{line.shape, std::min(t0, t1), std::max(t0, t1)});
    }

    // The chords of curves two shapes share differ wherever their flattenings did: those are
    // compared by proximity, in pieces no longer than a cell so a long chord is found wherever any
    // stretch of it is shared and indexes into a handful of cells.
    if (std::ranges::count(curved, true) < 2)
        return false;
    constexpr double cell = 16.0;
    std::vector<Edge> edges;
    for (const Edge& line : lines) {
        if (!curved[line.shape])
            continue;
        const double length = std::hypot(line.to.x - line.from.x, line.to.y - line.from.y);
        if (!(length > 1.0e-6) || !std::isfinite(length))
            continue;
        const auto pieces = static_cast<std::size_t>(std::min(std::ceil(length / cell), 1.0e5));
        for (std::size_t piece = 0U; piece < pieces; ++piece) {
            const double t0 = static_cast<double>(piece) / static_cast<double>(pieces);
            const double t1 = static_cast<double>(piece + 1U) / static_cast<double>(pieces);
            edges.push_back(Edge{line.shape,
                                 Point{line.from.x + (line.to.x - line.from.x) * t0,
                                       line.from.y + (line.to.y - line.from.y) * t0},
                                 Point{line.from.x + (line.to.x - line.from.x) * t1,
                                       line.from.y + (line.to.y - line.from.y) * t1}});
        }
    }
    const auto cell_of = [](const double value) {
        return static_cast<std::int64_t>(std::floor(value / cell));
    };
    const auto key = [](const std::int64_t x, const std::int64_t y) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32U) |
               static_cast<std::uint32_t>(y);
    };
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> cells;
    for (std::size_t index = 0U; index < edges.size(); ++index) {
        const Edge& edge = edges[index];
        const std::int64_t left = cell_of(std::min(edge.from.x, edge.to.x) - reach);
        const std::int64_t right = cell_of(std::max(edge.from.x, edge.to.x) + reach);
        const std::int64_t top = cell_of(std::min(edge.from.y, edge.to.y) - reach);
        const std::int64_t bottom = cell_of(std::max(edge.from.y, edge.to.y) + reach);
        for (std::int64_t y = top; y <= bottom; ++y) {
            for (std::int64_t x = left; x <= right; ++x)
                cells[key(x, y)].push_back(index);
        }
    }
    std::vector<double> shared(shapes.size(), 0.0);
    for (const Edge& edge : edges) {
        const double dx = edge.to.x - edge.from.x;
        const double dy = edge.to.y - edge.from.y;
        const double length = std::hypot(dx, dy);
        const Point middle{(edge.from.x + edge.to.x) * 0.5, (edge.from.y + edge.to.y) * 0.5};
        const auto found = cells.find(key(cell_of(middle.x), cell_of(middle.y)));
        if (found == cells.end())
            continue;
        for (const std::size_t other_index : found->second) {
            const Edge& other = edges[other_index];
            if (other.shape == edge.shape)
                continue;
            const double ox = other.to.x - other.from.x;
            const double oy = other.to.y - other.from.y;
            const double other_length = std::hypot(ox, oy);
            // Parallel either way round, and the middle on the other edge, between its ends.
            if (std::abs(dx * ox + dy * oy) < 0.99 * length * other_length)
                continue;
            const double along =
                ((middle.x - other.from.x) * ox + (middle.y - other.from.y) * oy) /
                (other_length * other_length);
            if (along < 0.0 || along > 1.0)
                continue;
            const double across =
                std::abs((middle.x - other.from.x) * oy - (middle.y - other.from.y) * ox) /
                other_length;
            if (across > reach)
                continue;
            shared[edge.shape] += length;
            if (shared[edge.shape] >= 0.5)
                return true;
            break;
        }
    }
    return false;
}

std::vector<double> shape_numbers(const runtime::Value& shapes) {
    std::vector<double> numbers{0.0};
    ShapeSignature signature;
    collect_shape_numbers(shapes, numbers, signature);
    numbers.front() = signature.value();
    return numbers;
}

std::optional<runtime::Value> with_shape_numbers(const runtime::Value& shapes,
                                                 const std::span<const double> numbers) {
    const std::vector<double> own = shape_numbers(shapes);
    if (numbers.size() != own.size() || numbers.empty() || numbers.front() != own.front())
        return std::nullopt;
    std::size_t next = 1U;
    return replace_shape_numbers(shapes, numbers, next);
}

} // namespace strata::ui
