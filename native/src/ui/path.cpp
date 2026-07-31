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

constexpr std::size_t maximum_flattened_points = 8192U;
constexpr std::size_t maximum_curve_subdivisions = 256U;
/** A closed contour whose ends are nearer than this is treated as already closed. */
constexpr double contour_epsilon = 1e-9;

[[nodiscard]] bool finite(const Point point) noexcept {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

[[nodiscard]] Point lerp(const Point from, const Point to, const double factor) noexcept {
    return Point{
        from.x + (to.x - from.x) * factor,
        from.y + (to.y - from.y) * factor,
    };
}

[[nodiscard]] double distance(const Point from, const Point to) noexcept {
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    return std::sqrt(dx * dx + dy * dy);
}

/** Subdivision count from the control polygon length, which bounds the curve's own length. */
[[nodiscard]] std::size_t curve_steps(
    const std::span<const Point> control,
    const Size scale,
    const double tolerance
) {
    double length = 0.0;
    for (std::size_t index = 1U; index < control.size(); ++index) {
        const Point from{control[index - 1U].x * scale.width, control[index - 1U].y * scale.height};
        const Point to{control[index].x * scale.width, control[index].y * scale.height};
        length += distance(from, to);
    }
    const double steps = std::ceil(std::sqrt(length / std::max(tolerance, 0.01)) * 2.0);
    return static_cast<std::size_t>(std::clamp(
        steps, 1.0, static_cast<double>(maximum_curve_subdivisions)
    ));
}

void append_point(std::vector<Point>& points, const Point point) {
    if (points.size() >= maximum_flattened_points) return;
    if (!points.empty() && distance(points.back(), point) <= contour_epsilon) return;
    points.push_back(point);
}

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

class PathParser final {
public:
    explicit PathParser(const std::string_view source) noexcept : source_(source) {}

    [[nodiscard]] Path run() {
        Path path;
        char command = '\0';
        while (true) {
            skip_separators();
            if (offset_ >= source_.size()) break;
            const char next = source_[offset_];
            if (std::isalpha(static_cast<unsigned char>(next)) != 0) {
                command = next;
                ++offset_;
            } else if (command == '\0') {
                throw std::invalid_argument("path must begin with a command");
            }
            apply(command, path);
        }
        return path;
    }

private:
    void skip_separators() noexcept {
        while (offset_ < source_.size()) {
            const char next = source_[offset_];
            if (next == ' ' || next == ',' || next == '\t' || next == '\n' || next == '\r') {
                ++offset_;
                continue;
            }
            return;
        }
    }

    [[nodiscard]] double number() {
        skip_separators();
        const std::size_t begin = offset_;
        if (offset_ < source_.size() && (source_[offset_] == '-' || source_[offset_] == '+')) {
            ++offset_;
        }
        while (offset_ < source_.size() &&
               (std::isdigit(static_cast<unsigned char>(source_[offset_])) != 0 ||
                source_[offset_] == '.')) {
            ++offset_;
        }
        if (offset_ == begin) {
            throw std::invalid_argument("path command is missing a number");
        }
        return std::stod(std::string(source_.substr(begin, offset_ - begin)));
    }

    [[nodiscard]] Point coordinate(const bool relative, const Point origin) {
        const double x = number();
        const double y = number();
        return relative ? Point{origin.x + x, origin.y + y} : Point{x, y};
    }

    void apply(const char command, Path& path) {
        const bool relative = std::islower(static_cast<unsigned char>(command)) != 0;
        switch (std::toupper(static_cast<unsigned char>(command))) {
        case 'M': {
            current_ = coordinate(relative, current_);
            start_ = current_;
            path.move_to(current_);
            return;
        }
        case 'L': {
            current_ = coordinate(relative, current_);
            path.line_to(current_);
            return;
        }
        case 'H': {
            const double x = number();
            current_ = Point{relative ? current_.x + x : x, current_.y};
            path.line_to(current_);
            return;
        }
        case 'V': {
            const double y = number();
            current_ = Point{current_.x, relative ? current_.y + y : y};
            path.line_to(current_);
            return;
        }
        case 'Q': {
            const Point control = coordinate(relative, current_);
            current_ = coordinate(relative, current_);
            path.quadratic_to(control, current_);
            return;
        }
        case 'C': {
            const Point first = coordinate(relative, current_);
            const Point second = coordinate(relative, current_);
            current_ = coordinate(relative, current_);
            path.cubic_to(first, second, current_);
            return;
        }
        case 'Z': {
            path.close();
            current_ = start_;
            return;
        }
        default:
            throw std::invalid_argument(
                std::string("unsupported path command '") + command + '\''
            );
        }
    }

    std::string_view source_;
    std::size_t offset_ = 0U;
    Point current_;
    Point start_;
};

} // namespace

Path::Path(std::vector<PathSegment> segments) : segments_(std::move(segments)) {}

void Path::move_to(const Point point) {
    segments_.push_back(PathSegment{PathVerb::move, {}, {}, point});
}

void Path::line_to(const Point point) {
    segments_.push_back(PathSegment{PathVerb::line, {}, {}, point});
}

void Path::quadratic_to(const Point control, const Point point) {
    segments_.push_back(PathSegment{PathVerb::quadratic, control, {}, point});
}

void Path::cubic_to(const Point first_control, const Point second_control, const Point point) {
    segments_.push_back(PathSegment{PathVerb::cubic, first_control, second_control, point});
}

void Path::close() {
    segments_.push_back(PathSegment{PathVerb::close, {}, {}, {}});
}

bool Path::empty() const noexcept { return segments_.empty(); }

std::span<const PathSegment> Path::segments() const noexcept { return segments_; }

void Path::validate() const {
    if (segments_.empty()) throw std::invalid_argument("a path requires at least one segment");
    if (segments_.size() > maximum_path_segments) {
        throw std::invalid_argument(
            "a path supports at most " + std::to_string(maximum_path_segments) + " segments"
        );
    }
    if (segments_.front().verb != PathVerb::move) {
        throw std::invalid_argument("a path must begin with a move");
    }
    for (const PathSegment& segment : segments_) {
        const bool valid = segment.verb == PathVerb::close
            ? true
            : finite(segment.to) &&
              (segment.verb != PathVerb::quadratic || finite(segment.control_a)) &&
              (segment.verb != PathVerb::cubic ||
               (finite(segment.control_a) && finite(segment.control_b)));
        if (!valid) throw std::invalid_argument("every path point must be finite");
    }
}

Path Path::line(const Point from, const Point to) {
    Path path;
    path.move_to(from);
    path.line_to(to);
    return path;
}

Path Path::polyline(const std::span<const Point> points, const bool closed) {
    if (points.empty()) throw std::invalid_argument("a polyline requires at least one point");
    Path path;
    path.move_to(points.front());
    for (const Point point : points.subspan(1U)) path.line_to(point);
    if (closed) path.close();
    return path;
}

Path Path::rectangle(const Rect bounds, const double radius) {
    const double limit = std::min(bounds.width, bounds.height) * 0.5;
    const double corner = std::clamp(radius, 0.0, std::max(limit, 0.0));
    Path path;
    if (corner <= 0.0) {
        path.move_to(Point{bounds.x, bounds.y});
        path.line_to(Point{bounds.right(), bounds.y});
        path.line_to(Point{bounds.right(), bounds.bottom()});
        path.line_to(Point{bounds.x, bounds.bottom()});
        path.close();
        return path;
    }
    // A circular corner is drawn as the standard cubic approximation of a quarter turn.
    constexpr double handle = 0.5522847498307936;
    const double offset = corner * handle;
    path.move_to(Point{bounds.x + corner, bounds.y});
    path.line_to(Point{bounds.right() - corner, bounds.y});
    path.cubic_to(
        Point{bounds.right() - corner + offset, bounds.y},
        Point{bounds.right(), bounds.y + corner - offset},
        Point{bounds.right(), bounds.y + corner}
    );
    path.line_to(Point{bounds.right(), bounds.bottom() - corner});
    path.cubic_to(
        Point{bounds.right(), bounds.bottom() - corner + offset},
        Point{bounds.right() - corner + offset, bounds.bottom()},
        Point{bounds.right() - corner, bounds.bottom()}
    );
    path.line_to(Point{bounds.x + corner, bounds.bottom()});
    path.cubic_to(
        Point{bounds.x + corner - offset, bounds.bottom()},
        Point{bounds.x, bounds.bottom() - corner + offset},
        Point{bounds.x, bounds.bottom() - corner}
    );
    path.line_to(Point{bounds.x, bounds.y + corner});
    path.cubic_to(
        Point{bounds.x, bounds.y + corner - offset},
        Point{bounds.x + corner - offset, bounds.y},
        Point{bounds.x + corner, bounds.y}
    );
    path.close();
    return path;
}

Path Path::ellipse(const Point center, const double radius_x, const double radius_y) {
    Path path = arc(center, radius_x, radius_y, 0.0, 360.0, false);
    path.close();
    return path;
}

Path Path::arc(
    const Point center,
    const double radius_x,
    const double radius_y,
    const double start_degrees,
    const double sweep_degrees,
    const bool include_center
) {
    if (!std::isfinite(radius_x) || !std::isfinite(radius_y) || radius_x <= 0.0 || radius_y <= 0.0) {
        throw std::invalid_argument("an arc requires finite positive radii");
    }
    if (!std::isfinite(start_degrees) || !std::isfinite(sweep_degrees)) {
        throw std::invalid_argument("an arc requires finite angles");
    }
    constexpr double degrees_to_radians = std::numbers::pi / 180.0;
    const double sweep = std::clamp(sweep_degrees, -360.0, 360.0);
    const double start = start_degrees * degrees_to_radians;
    const double total = sweep * degrees_to_radians;
    // Cubic quarter-turn approximation stays under a thousandth of a radius of error.
    const auto quarters = static_cast<std::size_t>(
        std::ceil(std::abs(sweep) / 90.0 - 1e-9)
    );
    const std::size_t steps = std::max<std::size_t>(quarters, 1U);
    const double step = total / static_cast<double>(steps);
    const double handle = 4.0 / 3.0 * std::tan(step * 0.25);
    const auto at = [&](const double angle) {
        return Point{
            center.x + std::cos(angle) * radius_x,
            center.y + std::sin(angle) * radius_y,
        };
    };
    const auto tangent = [&](const double angle) {
        return Point{-std::sin(angle) * radius_x, std::cos(angle) * radius_y};
    };
    Path path;
    if (include_center) {
        path.move_to(center);
        path.line_to(at(start));
    } else {
        path.move_to(at(start));
    }
    for (std::size_t index = 0U; index < steps; ++index) {
        const double from = start + step * static_cast<double>(index);
        const double to = from + step;
        const Point origin = at(from);
        const Point target = at(to);
        const Point origin_tangent = tangent(from);
        const Point target_tangent = tangent(to);
        path.cubic_to(
            Point{origin.x + origin_tangent.x * handle, origin.y + origin_tangent.y * handle},
            Point{target.x - target_tangent.x * handle, target.y - target_tangent.y * handle},
            target
        );
    }
    if (include_center) path.close();
    return path;
}

Path Path::parse(const std::string_view commands) {
    Path path = PathParser(commands).run();
    path.validate();
    return path;
}

std::vector<PathContour> flatten_path(
    const Path& path,
    const Size scale,
    const double tolerance
) {
    std::vector<PathContour> contours;
    PathContour current;
    Point cursor;
    Point origin;
    const auto commit = [&contours, &current]() {
        if (current.points.size() >= 2U) contours.push_back(current);
        current = PathContour{};
    };
    for (const PathSegment& segment : path.segments()) {
        switch (segment.verb) {
        case PathVerb::move:
            commit();
            cursor = segment.to;
            origin = segment.to;
            append_point(current.points, cursor);
            break;
        case PathVerb::line:
            append_point(current.points, segment.to);
            cursor = segment.to;
            break;
        case PathVerb::quadratic: {
            const std::array<Point, 3U> control{cursor, segment.control_a, segment.to};
            const std::size_t steps = curve_steps(control, scale, tolerance);
            for (std::size_t step = 1U; step <= steps; ++step) {
                const double factor = static_cast<double>(step) / static_cast<double>(steps);
                const Point first = lerp(cursor, segment.control_a, factor);
                const Point second = lerp(segment.control_a, segment.to, factor);
                append_point(current.points, lerp(first, second, factor));
            }
            cursor = segment.to;
            break;
        }
        case PathVerb::cubic: {
            const std::array<Point, 4U> control{
                cursor, segment.control_a, segment.control_b, segment.to,
            };
            const std::size_t steps = curve_steps(control, scale, tolerance);
            for (std::size_t step = 1U; step <= steps; ++step) {
                const double factor = static_cast<double>(step) / static_cast<double>(steps);
                const Point first = lerp(cursor, segment.control_a, factor);
                const Point second = lerp(segment.control_a, segment.control_b, factor);
                const Point third = lerp(segment.control_b, segment.to, factor);
                const Point fourth = lerp(first, second, factor);
                const Point fifth = lerp(second, third, factor);
                append_point(current.points, lerp(fourth, fifth, factor));
            }
            cursor = segment.to;
            break;
        }
        case PathVerb::close:
            if (!current.points.empty()) {
                current.closed = true;
                commit();
            }
            cursor = origin;
            append_point(current.points, cursor);
            break;
        }
    }
    commit();
    for (PathContour& contour : contours) {
        if (contour.closed && contour.points.size() > 2U &&
            distance(contour.points.front(), contour.points.back()) <= contour_epsilon) {
            contour.points.pop_back();
        }
    }
    std::erase_if(contours, [](const PathContour& contour) { return contour.points.size() < 2U; });
    return contours;
}

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
