#include "ui/render/path_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <numeric>
#include <optional>
#include <stdexcept>
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
constexpr std::size_t maximum_fill_edges = 64U * 1024U;
constexpr std::size_t maximum_fill_intersection_checks = 4U * 1024U * 1024U;
constexpr std::size_t maximum_fill_intersections = 16384U;
constexpr std::size_t maximum_fill_sweep_work = 32U * 1024U * 1024U;

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
    if (magnitude <= geometry_epsilon)
        return std::nullopt;
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

    [[nodiscard]] bool varies() const noexcept {
        return paint_->is_gradient();
    }

    [[nodiscard]] RenderColor at(const Point normalized_point) const noexcept {
        const Gradient* gradient = paint_->gradient();
        if (gradient == nullptr)
            return *paint_->color();
        return gradient->sample(gradient_parameter(*gradient, normalized_point, shape_));
    }

    [[nodiscard]] double parameter(const Point normalized_point) const noexcept {
        const Gradient* gradient = paint_->gradient();
        if (gradient == nullptr)
            return 0.0;
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

/** Accumulates triangles in logical shape pixels and converts them to normalized space on output.
 */
class MeshBuilder final {
  public:
    MeshBuilder(const Size shape, const PaintSampler sampler) noexcept
        : shape_{shape.width > 0.0 ? shape.width : 1.0, shape.height > 0.0 ? shape.height : 1.0},
          sampler_(sampler) {}

    [[nodiscard]] PaintMesh take() noexcept {
        return std::move(mesh_);
    }

    [[nodiscard]] Point normalize(const Point point) const noexcept {
        return Point{point.x / shape_.width, point.y / shape_.height};
    }

    /** Appends a solid vertex whose colour is the paint at its own position. */
    [[nodiscard]] std::uint32_t vertex(const Point point, const double alpha = 1.0) {
        const Point normalized_point = normalize(point);
        RenderColor color = sampler_.at(normalized_point);
        color.alpha = static_cast<std::uint8_t>(
            std::lround(static_cast<double>(color.alpha) * std::clamp(alpha, 0.0, 1.0)));
        mesh_.vertices.push_back(PaintVertex{normalized_point, color});
        return static_cast<std::uint32_t>(mesh_.vertices.size() - 1U);
    }

    void triangle(const std::uint32_t a, const std::uint32_t b, const std::uint32_t c) {
        if (a == b || b == c || a == c)
            return;
        mesh_.indices.push_back(a);
        mesh_.indices.push_back(b);
        mesh_.indices.push_back(c);
    }

    /** Emits an opaque triangle, subdividing it while a curved ramp would visibly flatten. */
    void shaded_triangle(const Point a, const Point b, const Point c,
                         const std::size_t depth = 0U) {
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
        const Point scaled_point{point.x * shape.width, point.y * shape.height};
        if (!std::isfinite(scaled_point.x) || !std::isfinite(scaled_point.y)) {
            throw std::invalid_argument("path scaling exceeds the finite geometry range");
        }
        result.push_back(scaled_point);
    }
    return result;
}

struct FillEdge final {
    Point from;
    Point to;
    std::vector<double> cuts{0.0, 1.0};

    [[nodiscard]] Point at(const double parameter) const noexcept {
        return Point{
            from.x + (to.x - from.x) * parameter,
            from.y + (to.y - from.y) * parameter,
        };
    }

    [[nodiscard]] double x_at(const double y) const noexcept {
        return from.x + (to.x - from.x) * (y - from.y) / (to.y - from.y);
    }
};

[[nodiscard]] bool fill_contains(const std::vector<FillEdge>& edges, const Point point,
                                 const PathFillRule rule) noexcept {
    int winding = 0;
    bool odd = false;
    for (const FillEdge& edge : edges) {
        const bool crosses_down = edge.from.y <= point.y && edge.to.y > point.y;
        const bool crosses_up = edge.from.y > point.y && edge.to.y <= point.y;
        if (!crosses_down && !crosses_up)
            continue;
        const double side = cross(edge.to - edge.from, point - edge.from);
        if ((crosses_down && side > 0.0) || (crosses_up && side < 0.0)) {
            if (rule == PathFillRule::evenodd) {
                odd = !odd;
            } else {
                winding += crosses_down ? 1 : -1;
            }
        }
    }
    return rule == PathFillRule::evenodd ? odd : winding != 0;
}

[[nodiscard]] bool append_cut(FillEdge& edge, const double parameter,
                              std::size_t& intersection_count) {
    if (parameter <= geometry_epsilon || parameter >= 1.0 - geometry_epsilon ||
        std::ranges::any_of(edge.cuts, [parameter](const double existing) {
            return std::abs(existing - parameter) <= geometry_epsilon;
        })) {
        return false;
    }
    if (intersection_count >= maximum_fill_intersections) {
        throw std::invalid_argument("path fill exceeds the intersection limit");
    }
    edge.cuts.push_back(parameter);
    ++intersection_count;
    return true;
}

void collect_fill_intersections(std::vector<FillEdge>& edges, std::vector<double>& y_levels) {
    std::size_t intersection_checks = 0U;
    std::size_t intersection_count = 0U;
    std::vector<std::size_t> order(edges.size());
    std::iota(order.begin(), order.end(), 0U);
    std::ranges::sort(order, [&edges](const std::size_t left, const std::size_t right) {
        return std::min(edges[left].from.x, edges[left].to.x) <
            std::min(edges[right].from.x, edges[right].to.x);
    });
    for (std::size_t first_position = 0U; first_position < order.size(); ++first_position) {
        const std::size_t first_index = order[first_position];
        FillEdge& first = edges[first_index];
        const Vector first_vector = first.to - first.from;
        const double first_length_squared = dot(first_vector, first_vector);
        const double first_maximum_x = std::max(first.from.x, first.to.x);
        for (std::size_t second_position = first_position + 1U; second_position < order.size();
             ++second_position) {
            const std::size_t second_index = order[second_position];
            FillEdge& second = edges[second_index];
            if (std::min(second.from.x, second.to.x) > first_maximum_x + geometry_epsilon) break;
            if (intersection_checks >= maximum_fill_intersection_checks) {
                throw std::invalid_argument("path fill exceeds the intersection-check limit");
            }
            ++intersection_checks;
            const Vector second_vector = second.to - second.from;
            const Vector offset = second.from - first.from;
            const double denominator = cross(first_vector, second_vector);
            const double scale = std::max({1.0, length(first_vector), length(second_vector)});
            if (std::abs(denominator) <= geometry_epsilon * scale * scale) {
                if (std::abs(cross(offset, first_vector)) > geometry_epsilon * scale * scale) {
                    continue;
                }
                const double second_from_parameter =
                    dot(offset, first_vector) / first_length_squared;
                const double second_to_parameter =
                    dot(second.to - first.from, first_vector) / first_length_squared;
                static_cast<void>(
                    append_cut(first, second_from_parameter, intersection_count));
                static_cast<void>(append_cut(first, second_to_parameter, intersection_count));
                const double second_length_squared = dot(second_vector, second_vector);
                static_cast<void>(append_cut(
                    second, dot(first.from - second.from, second_vector) / second_length_squared,
                    intersection_count));
                static_cast<void>(append_cut(
                    second, dot(first.to - second.from, second_vector) / second_length_squared,
                    intersection_count));
                continue;
            }
            const double first_parameter = cross(offset, second_vector) / denominator;
            const double second_parameter = cross(offset, first_vector) / denominator;
            if (first_parameter < -geometry_epsilon || first_parameter > 1.0 + geometry_epsilon ||
                second_parameter < -geometry_epsilon || second_parameter > 1.0 + geometry_epsilon) {
                continue;
            }
            const bool first_interior =
                first_parameter > geometry_epsilon && first_parameter < 1.0 - geometry_epsilon;
            const bool second_interior =
                second_parameter > geometry_epsilon && second_parameter < 1.0 - geometry_epsilon;
            if (!first_interior && !second_interior)
                continue;
            const bool first_added = append_cut(first, first_parameter, intersection_count);
            const bool second_added = append_cut(second, second_parameter, intersection_count);
            if (first_added || second_added) {
                y_levels.push_back(first.at(std::clamp(first_parameter, 0.0, 1.0)).y);
            }
        }
    }
}

void compact_sorted(std::vector<double>& values) {
    std::ranges::sort(values);
    std::vector<double> compact;
    compact.reserve(values.size());
    for (const double value : values) {
        if (compact.empty() || value - compact.back() > geometry_epsilon) {
            compact.push_back(value);
        }
    }
    values = std::move(compact);
}

void fill_compound_path(MeshBuilder& mesh, const std::vector<PathContour>& source, const Size size,
                        const PathFillRule rule, const double feather) {
    std::vector<FillEdge> edges;
    std::vector<double> y_levels;
    for (const PathContour& contour : source) {
        if (contour.points.size() < 3U)
            continue;
        const std::vector<Point> points = scaled(contour.points, size);
        for (std::size_t index = 0U; index < points.size(); ++index) {
            const Point from = points[index];
            const Point to = points[(index + 1U) % points.size()];
            if (length(to - from) <= geometry_epsilon)
                continue;
            if (edges.size() >= maximum_fill_edges) {
                throw std::invalid_argument("path fill exceeds the edge limit");
            }
            edges.push_back(FillEdge{from, to});
            y_levels.push_back(from.y);
            y_levels.push_back(to.y);
        }
    }
    if (edges.empty())
        return;
    collect_fill_intersections(edges, y_levels);
    compact_sorted(y_levels);
    const std::size_t band_count = y_levels.empty() ? 0U : y_levels.size() - 1U;
    if ((band_count != 0U && edges.size() > maximum_fill_sweep_work / band_count) ||
        (feather > 0.0 && edges.size() != 0U &&
         edges.size() > maximum_fill_sweep_work / edges.size())) {
        throw std::invalid_argument("path fill exceeds the sweep-work limit");
    }

    struct Crossing final {
        std::size_t edge = 0U;
        double x = 0.0;
        int winding_delta = 0;
    };
    for (std::size_t band = 0U; band + 1U < y_levels.size(); ++band) {
        const double top = y_levels[band];
        const double bottom = y_levels[band + 1U];
        if (bottom - top <= geometry_epsilon)
            continue;
        const double middle = top + (bottom - top) * 0.5;
        std::vector<Crossing> crossings;
        crossings.reserve(edges.size());
        for (std::size_t edge_index = 0U; edge_index < edges.size(); ++edge_index) {
            const FillEdge& edge = edges[edge_index];
            if (edge.from.y == edge.to.y || middle <= std::min(edge.from.y, edge.to.y) ||
                middle >= std::max(edge.from.y, edge.to.y)) {
                continue;
            }
            crossings.push_back(Crossing{
                edge_index,
                edge.x_at(middle),
                edge.from.y > edge.to.y ? 1 : -1,
            });
        }
        std::ranges::sort(crossings, [](const Crossing& left, const Crossing& right) {
            return left.x < right.x;
        });
        int winding = 0;
        bool odd = false;
        std::optional<std::size_t> left_edge;
        for (std::size_t crossing = 0U; crossing < crossings.size();) {
            const std::size_t group_begin = crossing;
            const double group_x = crossings[crossing].x;
            while (crossing < crossings.size() &&
                   std::abs(crossings[crossing].x - group_x) <=
                       geometry_epsilon * std::max(1.0, std::abs(group_x))) {
                ++crossing;
            }
            const bool before = rule == PathFillRule::evenodd ? odd : winding != 0;
            for (std::size_t member = group_begin; member < crossing; ++member) {
                odd = !odd;
                winding += crossings[member].winding_delta;
            }
            const bool after = rule == PathFillRule::evenodd ? odd : winding != 0;
            if (!before && after) {
                left_edge = crossings[group_begin].edge;
            } else if (before && !after && left_edge.has_value()) {
                const FillEdge& left = edges[*left_edge];
                const FillEdge& right = edges[crossings[group_begin].edge];
                const Point top_left{left.x_at(top), top};
                const Point top_right{right.x_at(top), top};
                const Point bottom_left{left.x_at(bottom), bottom};
                const Point bottom_right{right.x_at(bottom), bottom};
                if (std::abs(top_right.x - top_left.x) > geometry_epsilon ||
                    std::abs(bottom_right.x - bottom_left.x) > geometry_epsilon) {
                    mesh.shaded_triangle(top_left, top_right, bottom_right);
                    mesh.shaded_triangle(top_left, bottom_right, bottom_left);
                }
                left_edge.reset();
            }
        }
    }

    if (feather <= 0.0)
        return;
    for (FillEdge& edge : edges) {
        compact_sorted(edge.cuts);
        for (std::size_t cut = 0U; cut + 1U < edge.cuts.size(); ++cut) {
            const Point from = edge.at(edge.cuts[cut]);
            const Point to = edge.at(edge.cuts[cut + 1U]);
            const std::optional<Vector> direction = normalized(to - from);
            if (!direction.has_value())
                continue;
            const Vector normal = perpendicular(*direction);
            const Point middle{
                from.x + (to.x - from.x) * 0.5,
                from.y + (to.y - from.y) * 0.5,
            };
            const double probe = std::max(geometry_epsilon * 32.0,
                                          std::min(feather * 0.01, length(to - from) * 1.0e-4));
            const bool left_filled = fill_contains(edges, middle + normal * probe, rule);
            const bool right_filled = fill_contains(edges, middle + normal * -probe, rule);
            if (left_filled == right_filled)
                continue;
            const Vector outward = normal * (left_filled ? -feather : feather);
            mesh.feather_quad(from, to, outward);
        }
    }
}

[[nodiscard]] std::vector<std::vector<Point>> dashed(const std::vector<Point>& polyline,
                                                     const bool closed, const StrokeStyle& style) {
    std::vector<Point> points = polyline;
    if (closed && points.size() > 2U)
        points.push_back(points.front());
    if (style.dash.empty())
        return {points};
    double period = 0.0;
    for (const double entry : style.dash)
        period += entry;
    if (period <= 0.0)
        return {points};
    std::vector<std::vector<Point>> result;
    std::vector<Point> current;
    std::size_t index = 0U;
    double remaining = style.dash[0];
    bool painting = true;
    // A negative offset walks backwards through the pattern; normalize it into one period first.
    double offset = std::fmod(style.dash_offset, period);
    if (offset < 0.0)
        offset += period;
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
    if (painting)
        current.push_back(points.front());
    for (std::size_t segment = 1U; segment < points.size(); ++segment) {
        Point from = points[segment - 1U];
        const Point to = points[segment];
        double span = length(to - from);
        while (span > geometry_epsilon) {
            const double consumed = std::min(span, remaining);
            const std::optional<Vector> direction = normalized(to - from);
            if (!direction.has_value())
                break;
            const Point next = from + *direction * consumed;
            if (painting)
                current.push_back(next);
            remaining -= consumed;
            span -= consumed;
            from = next;
            if (remaining > geometry_epsilon)
                continue;
            if (painting && current.size() >= 2U)
                result.push_back(current);
            current.clear();
            index = (index + 1U) % style.dash.size();
            remaining = style.dash[index];
            painting = !painting;
            if (painting)
                current.push_back(from);
        }
    }
    if (painting && current.size() >= 2U)
        result.push_back(current);
    return result;
}

[[nodiscard]] double polygon_area(const std::vector<Point>& points) noexcept {
    double area = 0.0;
    for (std::size_t index = 0U; index < points.size(); ++index) {
        const Point current = points[index];
        const Point next = points[(index + 1U) % points.size()];
        area += current.x * next.y - next.x * current.y;
    }
    return area * 0.5;
}

void append_stroke_region(std::vector<PathContour>& regions, std::vector<Point> points,
                          const Size size) {
    if (points.size() < 3U) return;
    const double area = polygon_area(points);
    if (!std::isfinite(area)) {
        throw std::invalid_argument("stroke outline exceeds the finite geometry range");
    }
    if (std::abs(area) <= geometry_epsilon) return;
    if (area < 0.0) std::ranges::reverse(points);
    for (Point& point : points) {
        point.x /= size.width;
        point.y /= size.height;
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            throw std::invalid_argument("stroke outline normalization is non-finite");
        }
    }
    regions.push_back(PathContour{std::move(points), true});
}

void append_round_region(std::vector<PathContour>& regions, const Point center, const Vector from,
                         const double sweep, const double radius, const Size size) {
    const auto segments = static_cast<std::size_t>(std::clamp(
        std::ceil(std::abs(sweep) / (std::numbers::pi / 8.0)), 1.0,
        static_cast<double>(maximum_round_segments)));
    std::vector<Point> points;
    points.reserve(segments + 2U);
    points.push_back(center);
    points.push_back(center + from * radius);
    for (std::size_t step = 1U; step <= segments; ++step) {
        const double angle = sweep * static_cast<double>(step) / static_cast<double>(segments);
        const double sine = std::sin(angle);
        const double cosine = std::cos(angle);
        const Vector next{
            from.x * cosine - from.y * sine,
            from.x * sine + from.y * cosine,
        };
        points.push_back(center + next * radius);
    }
    append_stroke_region(regions, std::move(points), size);
}

void append_stroke_cap(std::vector<PathContour>& regions, const Point point,
                       const Vector direction, const double half_width, const PathCap cap,
                       const Size size) {
    const Vector side = perpendicular(direction);
    switch (cap) {
    case PathCap::butt:
        return;
    case PathCap::square: {
        const Point near_left = point + side * half_width;
        const Point near_right = point + side * -half_width;
        append_stroke_region(
            regions,
            {near_left, near_left + direction * half_width,
             near_right + direction * half_width, near_right},
            size);
        return;
    }
    case PathCap::round:
        append_round_region(regions, point, side, -std::numbers::pi, half_width, size);
        return;
    }
}

void append_stroke_join(std::vector<PathContour>& regions, const Point point,
                        const Vector incoming, const Vector outgoing, const double half_width,
                        const StrokeStyle& style, const Size size) {
    const double turn = cross(incoming, outgoing);
    if (std::abs(turn) <= geometry_epsilon) return;
    const double side_sign = turn > 0.0 ? -1.0 : 1.0;
    const Vector incoming_side = perpendicular(incoming) * (half_width * side_sign);
    const Vector outgoing_side = perpendicular(outgoing) * (half_width * side_sign);
    const Point first = point + incoming_side;
    const Point second = point + outgoing_side;
    if (style.join == PathJoin::round) {
        const std::optional<Vector> from = normalized(incoming_side);
        if (!from.has_value()) return;
        const double sweep = std::atan2(cross(*from, outgoing_side), dot(*from, outgoing_side));
        append_round_region(regions, point, *from, sweep, half_width, size);
        return;
    }
    const std::optional<Vector> bisector = normalized(Vector{
        incoming_side.x + outgoing_side.x,
        incoming_side.y + outgoing_side.y,
    });
    if (style.join == PathJoin::miter && bisector.has_value()) {
        const double cosine = dot(*bisector, perpendicular(incoming) * side_sign);
        if (cosine > geometry_epsilon) {
            const double extent = half_width / cosine;
            if (extent / half_width <= style.miter_limit) {
                append_stroke_region(
                    regions, {point, first, point + *bisector * extent, second}, size);
                return;
            }
        }
    }
    append_stroke_region(regions, {point, first, second}, size);
}

void append_stroke_polyline(std::vector<PathContour>& regions, const std::vector<Point>& points,
                            const StrokeStyle& style, const double feather, const Size size) {
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
        append_stroke_region(
            regions, {from + side, to + side, to + side * -1.0, from + side * -1.0}, size);
        directions.push_back(*direction);
    }
    if (directions.empty()) return;
    const bool closed = length(points.back() - points.front()) <= geometry_epsilon;
    for (std::size_t index = 1U; index < directions.size(); ++index) {
        append_stroke_join(
            regions, points[index], directions[index - 1U], directions[index], half_width, style,
            size);
    }
    if (closed) {
        append_stroke_join(
            regions, points.front(), directions.back(), directions.front(), half_width, style,
            size);
        return;
    }
    append_stroke_cap(
        regions, points.front(), directions.front() * -1.0, half_width, style.cap, size);
    append_stroke_cap(regions, points.back(), directions.back(), half_width, style.cap, size);
}

} // namespace

PaintMesh tessellate_shape(const PathShape& shape, const Size shape_size,
                           const double pixel_scale) {
    const Size size{
        std::isfinite(shape_size.width) && shape_size.width > 0.0 ? shape_size.width : 1.0,
        std::isfinite(shape_size.height) && shape_size.height > 0.0 ? shape_size.height : 1.0,
    };
    const double scale = std::isfinite(pixel_scale) ? std::max(pixel_scale, 0.1) : 1.0;
    const double feather = feather_pixels / scale;
    const std::vector<PathContour> contours = flatten_path(shape.path, size, 0.25 / scale);
    PaintMesh result;
    if (shape.fill.has_value()) {
        MeshBuilder mesh(size, PaintSampler(*shape.fill, size));
        fill_compound_path(mesh, contours, size, shape.fill_rule, feather);
        result = mesh.take();
    }
    if (!shape.stroke.has_value())
        return result;
    MeshBuilder mesh(size, PaintSampler(*shape.stroke, size));
    const StrokeStyle style = shape.stroke_style.value_or(StrokeStyle{});
    std::vector<PathContour> stroke_regions;
    for (const PathContour& contour : contours) {
        for (const std::vector<Point>& polyline :
             dashed(scaled(contour.points, size), contour.closed, style)) {
            append_stroke_polyline(stroke_regions, polyline, style, feather, size);
        }
    }
    fill_compound_path(mesh, stroke_regions, size, PathFillRule::nonzero, feather);
    PaintMesh stroke = mesh.take();
    const auto base = static_cast<std::uint32_t>(result.vertices.size());
    result.vertices.insert(result.vertices.end(), stroke.vertices.begin(), stroke.vertices.end());
    for (const std::uint32_t index : stroke.indices)
        result.indices.push_back(base + index);
    return result;
}

} // namespace strata::ui
