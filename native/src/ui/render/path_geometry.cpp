#include "ui/render/path_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <vector>

namespace strata::ui {
namespace {

/** Feather width in device pixels. One pixel of coverage ramp reads as a clean edge. */
constexpr double feather_pixels = 1.0;
constexpr std::size_t maximum_round_segments = 24U;
constexpr std::size_t maximum_gradient_subdivisions = 3U;
/** A triangle spanning more gradient than this is split so a curved ramp stays curved. */
constexpr double gradient_subdivision_span = 0.12;
constexpr double geometry_epsilon = 1e-9;

struct Vector final {
    double x = 0.0;
    double y = 0.0;
};

[[nodiscard]] Vector operator-(const Point left, const Point right) noexcept {
    return Vector{left.x - right.x, left.y - right.y};
}

[[nodiscard]] Point operator+(const Point point, const Vector offset) noexcept {
    return Point{point.x + offset.x, point.y + offset.y};
}

[[nodiscard]] Vector operator*(const Vector value, const double factor) noexcept {
    return Vector{value.x * factor, value.y * factor};
}

[[nodiscard]] double length(const Vector value) noexcept {
    return std::sqrt(value.x * value.x + value.y * value.y);
}

[[nodiscard]] std::optional<Vector> normalized(const Vector value) noexcept {
    const double magnitude = length(value);
    if (magnitude <= geometry_epsilon) return std::nullopt;
    return Vector{value.x / magnitude, value.y / magnitude};
}

[[nodiscard]] Vector perpendicular(const Vector value) noexcept {
    return Vector{-value.y, value.x};
}

[[nodiscard]] double cross(const Vector left, const Vector right) noexcept {
    return left.x * right.y - left.y * right.x;
}

[[nodiscard]] double dot(const Vector left, const Vector right) noexcept {
    return left.x * right.x + left.y * right.y;
}

/** Paints one point of the shape, evaluating a gradient at that point's own position. */
class PaintSampler final {
public:
    PaintSampler(const Paint& paint, const Size shape) noexcept : paint_(&paint), shape_(shape) {}

    [[nodiscard]] bool varies() const noexcept { return paint_->is_gradient(); }

    [[nodiscard]] RenderColor at(const Point normalized_point) const noexcept {
        const Gradient* gradient = paint_->gradient();
        if (gradient == nullptr) return *paint_->color();
        return gradient->sample(gradient_parameter(*gradient, normalized_point, shape_));
    }

    [[nodiscard]] double parameter(const Point normalized_point) const noexcept {
        const Gradient* gradient = paint_->gradient();
        if (gradient == nullptr) return 0.0;
        return gradient_parameter(*gradient, normalized_point, shape_);
    }

    /** Radial ramps are not linear in position, so their triangles need finer geometry. */
    [[nodiscard]] bool subdivides() const noexcept {
        const Gradient* gradient = paint_->gradient();
        return gradient != nullptr && gradient->kind == GradientKind::radial;
    }

private:
    const Paint* paint_;
    Size shape_;
};

/** Accumulates triangles in logical shape pixels and converts them to normalized space on output. */
class MeshBuilder final {
public:
    MeshBuilder(const Size shape, const PaintSampler sampler) noexcept
        : shape_{shape.width > 0.0 ? shape.width : 1.0, shape.height > 0.0 ? shape.height : 1.0},
          sampler_(sampler) {}

    [[nodiscard]] PaintMesh take() noexcept { return std::move(mesh_); }

    [[nodiscard]] Point normalize(const Point point) const noexcept {
        return Point{point.x / shape_.width, point.y / shape_.height};
    }

    /** Appends a solid vertex whose colour is the paint at its own position. */
    [[nodiscard]] std::uint32_t vertex(const Point point, const double alpha = 1.0) {
        const Point normalized_point = normalize(point);
        RenderColor color = sampler_.at(normalized_point);
        color.alpha = static_cast<std::uint8_t>(
            std::lround(static_cast<double>(color.alpha) * std::clamp(alpha, 0.0, 1.0))
        );
        mesh_.vertices.push_back(PaintVertex{normalized_point, color});
        return static_cast<std::uint32_t>(mesh_.vertices.size() - 1U);
    }

    void triangle(const std::uint32_t a, const std::uint32_t b, const std::uint32_t c) {
        if (a == b || b == c || a == c) return;
        mesh_.indices.push_back(a);
        mesh_.indices.push_back(b);
        mesh_.indices.push_back(c);
    }

    /** Emits an opaque triangle, subdividing it while a curved ramp would visibly flatten. */
    void shaded_triangle(
        const Point a,
        const Point b,
        const Point c,
        const std::size_t depth = 0U
    ) {
        if (sampler_.subdivides() && depth < maximum_gradient_subdivisions) {
            const double first = sampler_.parameter(normalize(a));
            const double second = sampler_.parameter(normalize(b));
            const double third = sampler_.parameter(normalize(c));
            const double span = std::max({first, second, third}) - std::min({first, second, third});
            if (span > gradient_subdivision_span) {
                const Point ab{(a.x + b.x) * 0.5, (a.y + b.y) * 0.5};
                const Point bc{(b.x + c.x) * 0.5, (b.y + c.y) * 0.5};
                const Point ca{(c.x + a.x) * 0.5, (c.y + a.y) * 0.5};
                shaded_triangle(a, ab, ca, depth + 1U);
                shaded_triangle(ab, b, bc, depth + 1U);
                shaded_triangle(ca, bc, c, depth + 1U);
                shaded_triangle(ab, bc, ca, depth + 1U);
                return;
            }
        }
        triangle(vertex(a), vertex(b), vertex(c));
    }

    /** Emits the coverage ramp between an opaque edge and its outward offset. */
    void feather_quad(const Point from, const Point to, const Vector outward) {
        const std::uint32_t inner_from = vertex(from);
        const std::uint32_t inner_to = vertex(to);
        const std::uint32_t outer_from = vertex(from + outward, 0.0);
        const std::uint32_t outer_to = vertex(to + outward, 0.0);
        triangle(inner_from, outer_from, outer_to);
        triangle(inner_from, outer_to, inner_to);
    }

private:
    Size shape_;
    PaintSampler sampler_;
    PaintMesh mesh_;
};

[[nodiscard]] std::vector<Point> scaled(const std::vector<Point>& points, const Size shape) {
    std::vector<Point> result;
    result.reserve(points.size());
    for (const Point point : points) {
        result.push_back(Point{point.x * shape.width, point.y * shape.height});
    }
    return result;
}

[[nodiscard]] double signed_area(const std::vector<Point>& polygon) noexcept {
    double total = 0.0;
    for (std::size_t index = 0U; index < polygon.size(); ++index) {
        const Point current = polygon[index];
        const Point next = polygon[(index + 1U) % polygon.size()];
        total += current.x * next.y - next.x * current.y;
    }
    return total * 0.5;
}

[[nodiscard]] bool inside_triangle(
    const Point point,
    const Point a,
    const Point b,
    const Point c
) noexcept {
    const double first = cross(b - a, point - a);
    const double second = cross(c - b, point - b);
    const double third = cross(a - c, point - c);
    const bool negative = first < 0.0 || second < 0.0 || third < 0.0;
    const bool positive = first > 0.0 || second > 0.0 || third > 0.0;
    return !(negative && positive);
}

/**
 * Ear clipping over one simple contour. Self-intersecting outlines have no well-defined interior;
 * clipping stops when no ear remains rather than emitting geometry that crosses itself.
 */
void fill_contour(MeshBuilder& mesh, std::vector<Point> polygon) {
    if (polygon.size() < 3U) return;
    if (signed_area(polygon) < 0.0) std::ranges::reverse(polygon);
    std::vector<std::size_t> remaining(polygon.size());
    for (std::size_t index = 0U; index < polygon.size(); ++index) remaining[index] = index;
    std::size_t guard = remaining.size() * remaining.size();
    while (remaining.size() > 3U && guard-- > 0U) {
        bool clipped = false;
        for (std::size_t index = 0U; index < remaining.size(); ++index) {
            const std::size_t previous = remaining[(index + remaining.size() - 1U) % remaining.size()];
            const std::size_t current = remaining[index];
            const std::size_t next = remaining[(index + 1U) % remaining.size()];
            const Point a = polygon[previous];
            const Point b = polygon[current];
            const Point c = polygon[next];
            if (cross(b - a, c - b) <= 0.0) continue;
            const bool contains = std::ranges::any_of(remaining, [&](const std::size_t candidate) {
                if (candidate == previous || candidate == current || candidate == next) return false;
                return inside_triangle(polygon[candidate], a, b, c);
            });
            if (contains) continue;
            mesh.shaded_triangle(a, b, c);
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(index));
            clipped = true;
            break;
        }
        if (!clipped) return;
    }
    if (remaining.size() == 3U) {
        mesh.shaded_triangle(polygon[remaining[0]], polygon[remaining[1]], polygon[remaining[2]]);
    }
}

/** Wraps a filled contour in an outward coverage ramp so its silhouette is antialiased. */
void feather_contour(MeshBuilder& mesh, std::vector<Point> polygon, const double feather) {
    if (polygon.size() < 3U || feather <= 0.0) return;
    if (signed_area(polygon) < 0.0) std::ranges::reverse(polygon);
    for (std::size_t index = 0U; index < polygon.size(); ++index) {
        const Point from = polygon[index];
        const Point to = polygon[(index + 1U) % polygon.size()];
        const std::optional<Vector> direction = normalized(to - from);
        if (!direction.has_value()) continue;
        // The contour winds counter-clockwise in a y-down space, so its outward side is the
        // clockwise perpendicular of each edge.
        mesh.feather_quad(from, to, perpendicular(*direction) * -feather);
    }
}

[[nodiscard]] std::vector<std::vector<Point>> dashed(
    const std::vector<Point>& polyline,
    const bool closed,
    const StrokeStyle& style
) {
    std::vector<Point> points = polyline;
    if (closed && points.size() > 2U) points.push_back(points.front());
    if (style.dash.empty()) return {points};
    double period = 0.0;
    for (const double entry : style.dash) period += entry;
    if (period <= 0.0) return {points};
    std::vector<std::vector<Point>> result;
    std::vector<Point> current;
    std::size_t index = 0U;
    double remaining = style.dash[0];
    bool painting = true;
    // A negative offset walks backwards through the pattern; normalize it into one period first.
    double offset = std::fmod(style.dash_offset, period);
    if (offset < 0.0) offset += period;
    while (offset > 0.0) {
        const double consumed = std::min(offset, remaining);
        remaining -= consumed;
        offset -= consumed;
        if (remaining <= geometry_epsilon) {
            index = (index + 1U) % style.dash.size();
            remaining = style.dash[index];
            painting = !painting;
        }
    }
    if (painting) current.push_back(points.front());
    for (std::size_t segment = 1U; segment < points.size(); ++segment) {
        Point from = points[segment - 1U];
        const Point to = points[segment];
        double span = length(to - from);
        while (span > geometry_epsilon) {
            const double consumed = std::min(span, remaining);
            const std::optional<Vector> direction = normalized(to - from);
            if (!direction.has_value()) break;
            const Point next = from + *direction * consumed;
            if (painting) current.push_back(next);
            remaining -= consumed;
            span -= consumed;
            from = next;
            if (remaining > geometry_epsilon) continue;
            if (painting && current.size() >= 2U) result.push_back(current);
            current.clear();
            index = (index + 1U) % style.dash.size();
            remaining = style.dash[index];
            painting = !painting;
            if (painting) current.push_back(from);
        }
    }
    if (painting && current.size() >= 2U) result.push_back(current);
    return result;
}

void round_fan(
    MeshBuilder& mesh,
    const Point center,
    const Vector from,
    const double sweep,
    const double radius,
    const double feather
) {
    const auto segments = static_cast<std::size_t>(std::clamp(
        std::ceil(std::abs(sweep) / (std::numbers::pi / 8.0)),
        1.0,
        static_cast<double>(maximum_round_segments)
    ));
    Vector previous = from;
    for (std::size_t step = 1U; step <= segments; ++step) {
        const double angle = sweep * static_cast<double>(step) / static_cast<double>(segments);
        const double sine = std::sin(angle);
        const double cosine = std::cos(angle);
        const Vector next{
            from.x * cosine - from.y * sine,
            from.x * sine + from.y * cosine,
        };
        mesh.shaded_triangle(center, center + previous * radius, center + next * radius);
        mesh.feather_quad(
            center + previous * radius,
            center + next * radius,
            Vector{(previous.x + next.x) * 0.5, (previous.y + next.y) * 0.5} * feather
        );
        previous = next;
    }
}

void stroke_cap(
    MeshBuilder& mesh,
    const Point point,
    const Vector direction,
    const double half_width,
    const double feather,
    const PathCap cap
) {
    const Vector side = perpendicular(direction);
    switch (cap) {
    case PathCap::butt:
        mesh.feather_quad(
            point + side * half_width, point + side * -half_width, direction * feather
        );
        return;
    case PathCap::square: {
        const Point near_left = point + side * half_width;
        const Point near_right = point + side * -half_width;
        const Point far_left = near_left + direction * half_width;
        const Point far_right = near_right + direction * half_width;
        mesh.shaded_triangle(near_left, far_left, far_right);
        mesh.shaded_triangle(near_left, far_right, near_right);
        mesh.feather_quad(far_left, far_right, direction * feather);
        mesh.feather_quad(near_left, far_left, side * feather);
        mesh.feather_quad(far_right, near_right, side * -feather);
        return;
    }
    case PathCap::round:
        round_fan(mesh, point, side, -std::numbers::pi, half_width, feather);
        return;
    }
}

void stroke_join(
    MeshBuilder& mesh,
    const Point point,
    const Vector incoming,
    const Vector outgoing,
    const double half_width,
    const double feather,
    const StrokeStyle& style
) {
    const double turn = cross(incoming, outgoing);
    if (std::abs(turn) <= geometry_epsilon) return;
    // The outer side of a turn is the one the corner opens away from.
    const double side_sign = turn > 0.0 ? -1.0 : 1.0;
    const Vector incoming_side = perpendicular(incoming) * (half_width * side_sign);
    const Vector outgoing_side = perpendicular(outgoing) * (half_width * side_sign);
    const Point first = point + incoming_side;
    const Point second = point + outgoing_side;
    if (style.join == PathJoin::round) {
        const std::optional<Vector> from = normalized(incoming_side);
        if (!from.has_value()) return;
        double sweep = std::atan2(
            cross(*from, Vector{outgoing_side.x, outgoing_side.y}),
            dot(*from, Vector{outgoing_side.x, outgoing_side.y})
        );
        round_fan(mesh, point, *from, sweep, half_width, feather);
        return;
    }
    const std::optional<Vector> bisector = normalized(Vector{
        incoming_side.x + outgoing_side.x, incoming_side.y + outgoing_side.y,
    });
    if (style.join == PathJoin::miter && bisector.has_value()) {
        const double cosine = dot(*bisector, perpendicular(incoming) * side_sign);
        if (cosine > geometry_epsilon) {
            const double extent = half_width / cosine;
            if (extent / half_width <= style.miter_limit) {
                const Point tip = point + *bisector * extent;
                mesh.shaded_triangle(point, first, tip);
                mesh.shaded_triangle(point, tip, second);
                mesh.feather_quad(first, tip, *bisector * feather);
                mesh.feather_quad(tip, second, *bisector * feather);
                return;
            }
        }
    }
    mesh.shaded_triangle(point, first, second);
    if (bisector.has_value()) mesh.feather_quad(first, second, *bisector * feather);
}

void stroke_polyline(
    MeshBuilder& mesh,
    const std::vector<Point>& points,
    const StrokeStyle& style,
    const double feather
) {
    if (points.size() < 2U) return;
    const double half_width = std::max(style.width, feather) * 0.5;
    std::vector<Vector> directions;
    directions.reserve(points.size() - 1U);
    for (std::size_t index = 1U; index < points.size(); ++index) {
        const std::optional<Vector> direction = normalized(points[index] - points[index - 1U]);
        if (!direction.has_value()) continue;
        const Vector side = perpendicular(*direction) * half_width;
        const Point from = points[index - 1U];
        const Point to = points[index];
        mesh.shaded_triangle(from + side, to + side, to + side * -1.0);
        mesh.shaded_triangle(from + side, to + side * -1.0, from + side * -1.0);
        mesh.feather_quad(to + side, from + side, perpendicular(*direction) * feather);
        mesh.feather_quad(from + side * -1.0, to + side * -1.0, perpendicular(*direction) * -feather);
        directions.push_back(*direction);
    }
    if (directions.empty()) return;
    const bool closed = length(points.back() - points.front()) <= geometry_epsilon;
    for (std::size_t index = 1U; index < directions.size(); ++index) {
        stroke_join(
            mesh, points[index], directions[index - 1U], directions[index], half_width, feather, style
        );
    }
    if (closed) {
        stroke_join(
            mesh, points.front(), directions.back(), directions.front(), half_width, feather, style
        );
        return;
    }
    stroke_cap(
        mesh, points.front(), directions.front() * -1.0, half_width, feather, style.cap
    );
    stroke_cap(mesh, points.back(), directions.back(), half_width, feather, style.cap);
}

} // namespace

PaintMesh tessellate_shape(
    const PathShape& shape,
    const Size shape_size,
    const double pixel_scale
) {
    const Size size{
        shape_size.width > 0.0 ? shape_size.width : 1.0,
        shape_size.height > 0.0 ? shape_size.height : 1.0,
    };
    const double scale = std::max(pixel_scale, 0.1);
    const double feather = feather_pixels / scale;
    const std::vector<PathContour> contours = flatten_path(shape.path, size, 0.25 / scale);
    PaintMesh result;
    if (shape.fill.has_value()) {
        MeshBuilder mesh(size, PaintSampler(*shape.fill, size));
        for (const PathContour& contour : contours) {
            std::vector<Point> polygon = scaled(contour.points, size);
            fill_contour(mesh, polygon);
            feather_contour(mesh, std::move(polygon), feather);
        }
        result = mesh.take();
    }
    if (!shape.stroke.has_value()) return result;
    MeshBuilder mesh(size, PaintSampler(*shape.stroke, size));
    const StrokeStyle style = shape.stroke_style.value_or(StrokeStyle{});
    for (const PathContour& contour : contours) {
        for (const std::vector<Point>& polyline :
             dashed(scaled(contour.points, size), contour.closed, style)) {
            stroke_polyline(mesh, polyline, style, feather);
        }
    }
    PaintMesh stroke = mesh.take();
    const auto base = static_cast<std::uint32_t>(result.vertices.size());
    result.vertices.insert(result.vertices.end(), stroke.vertices.begin(), stroke.vertices.end());
    for (const std::uint32_t index : stroke.indices) result.indices.push_back(base + index);
    return result;
}

} // namespace strata::ui
