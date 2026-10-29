#include "ui/lottie/geometry.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace strata::ui::lottie {
namespace {

/** The handle length of a quarter circle drawn as one cubic, as After Effects uses it. */
constexpr double circle_handle = 0.5519;
/** Chords a segment's length is measured with when trimming. */
constexpr std::size_t length_samples = 24U;
/** Heights closer than this are one height to the sweep. */
constexpr double sweep_epsilon = 1.0e-9;
/** The most bands one sweep may cut: a pathological input stops rather than stalls a frame. */
constexpr std::size_t maximum_bands = 1U << 20U;

[[nodiscard]] Point operator+(const Point left, const Point right) noexcept {
    return Point{left.x + right.x, left.y + right.y};
}
[[nodiscard]] Point operator-(const Point left, const Point right) noexcept {
    return Point{left.x - right.x, left.y - right.y};
}
[[nodiscard]] Point operator*(const Point point, const double factor) noexcept {
    return Point{point.x * factor, point.y * factor};
}
[[nodiscard]] double length(const Point point) noexcept {
    return std::hypot(point.x, point.y);
}
[[nodiscard]] Point lerp(const Point from, const Point to, const double t) noexcept {
    return Point{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t};
}
[[nodiscard]] Point point(const resource::lottie::Vec2 value) noexcept {
    return Point{value.x, value.y};
}

[[nodiscard]] Point cubic_at(const Point p0, const Cubic& cubic, const double t) noexcept {
    const Point a = lerp(p0, cubic.control_a, t);
    const Point b = lerp(cubic.control_a, cubic.control_b, t);
    const Point c = lerp(cubic.control_b, cubic.to, t);
    const Point d = lerp(a, b, t);
    const Point e = lerp(b, c, t);
    return lerp(d, e, t);
}

/** The part of the cubic from `p0` between parameters `t0` and `t1`, from its point at `t0`. */
[[nodiscard]] std::pair<Point, Cubic> sub_cubic(const Point p0, const Cubic& cubic, const double t0,
                                                const double t1) noexcept {
    // Split at t1 and keep the head, then split the head at t0 / t1 and keep its tail.
    const auto split_head = [](const Point a, const Cubic& c, const double t) {
        const Point ab = lerp(a, c.control_a, t);
        const Point bc = lerp(c.control_a, c.control_b, t);
        const Point cd = lerp(c.control_b, c.to, t);
        const Point abc = lerp(ab, bc, t);
        const Point bcd = lerp(bc, cd, t);
        return Cubic{ab, abc, lerp(abc, bcd, t)};
    };
    const auto split_tail = [](const Point a, const Cubic& c, const double t) {
        const Point ab = lerp(a, c.control_a, t);
        const Point bc = lerp(c.control_a, c.control_b, t);
        const Point cd = lerp(c.control_b, c.to, t);
        const Point abc = lerp(ab, bc, t);
        const Point bcd = lerp(bc, cd, t);
        return std::pair{lerp(abc, bcd, t), Cubic{bcd, cd, c.to}};
    };
    const Cubic head = t1 < 1.0 ? split_head(p0, cubic, t1) : cubic;
    if (t0 <= 0.0)
        return {p0, head};
    const double local = t1 > 0.0 ? std::clamp(t0 / t1, 0.0, 1.0) : 0.0;
    return split_tail(p0, head, local);
}

[[nodiscard]] Point segment_start(const Contour& contour, const std::size_t index) noexcept {
    return index == 0U ? contour.start : contour.segments[index - 1U].to;
}

/** A segment's arc length at evenly spaced parameters. */
[[nodiscard]] std::array<double, length_samples + 1U> segment_lengths(const Point p0,
                                                                      const Cubic& cubic) noexcept {
    std::array<double, length_samples + 1U> table{};
    Point previous = p0;
    for (std::size_t sample = 1U; sample <= length_samples; ++sample) {
        const Point next = cubic_at(p0, cubic, static_cast<double>(sample) / length_samples);
        table[sample] = table[sample - 1U] + length(next - previous);
        previous = next;
    }
    return table;
}

/** A contour measured: each segment's length table, and the running totals. */
struct Measured final {
    std::vector<std::array<double, length_samples + 1U>> tables;
    std::vector<double> starts;
    double total = 0.0;
};

[[nodiscard]] Measured measure(const Contour& contour) {
    Measured result;
    result.tables.reserve(contour.segments.size());
    result.starts.reserve(contour.segments.size());
    for (std::size_t index = 0U; index < contour.segments.size(); ++index) {
        result.starts.push_back(result.total);
        result.tables.push_back(
            segment_lengths(segment_start(contour, index), contour.segments[index]));
        result.total += result.tables.back().back();
    }
    return result;
}

/** The segment and parameter at arc length `distance` along a measured contour. */
[[nodiscard]] std::pair<std::size_t, double>
locate(const Contour& contour, const Measured& measured, const double distance) noexcept {
    if (measured.tables.empty())
        return {0U, 0.0};
    const auto after = std::ranges::upper_bound(measured.starts, distance);
    std::size_t index = after == measured.starts.begin()
                            ? 0U
                            : static_cast<std::size_t>(after - measured.starts.begin()) - 1U;
    index = std::min(index, measured.tables.size() - 1U);
    const auto& table = measured.tables[index];
    const double local = std::clamp(distance - measured.starts[index], 0.0, table.back());
    const auto found = std::ranges::lower_bound(table, local);
    const std::size_t upper = std::clamp<std::size_t>(
        static_cast<std::size_t>(found - table.begin()), 1U, length_samples);
    // Within one sample's span the arc is nearly its chord: bisect the parameter until the chord
    // from the sample reaches the remaining distance.
    const Point from = segment_start(contour, index);
    const Cubic& cubic = contour.segments[index];
    const double t0 = static_cast<double>(upper - 1U) / length_samples;
    const Point anchor = cubic_at(from, cubic, t0);
    const double remaining = local - table[upper - 1U];
    double low = t0;
    double high = static_cast<double>(upper) / length_samples;
    for (int iteration = 0; iteration < 30; ++iteration) {
        const double middle = (low + high) * 0.5;
        if (length(cubic_at(from, cubic, middle) - anchor) < remaining)
            low = middle;
        else
            high = middle;
    }
    return {index, (low + high) * 0.5};
}

/** The open part of a measured contour between arc lengths `from` and `to`. */
[[nodiscard]] Contour extract(const Contour& contour, const Measured& measured, const double from,
                              const double to) {
    Contour result;
    if (contour.segments.empty() || to <= from)
        return result;
    const auto [first, first_t] = locate(contour, measured, from);
    const auto [last, last_t] = locate(contour, measured, to);
    if (first == last) {
        const auto [start, cubic] =
            sub_cubic(segment_start(contour, first), contour.segments[first], first_t, last_t);
        result.start = start;
        result.segments.push_back(cubic);
        return result;
    }
    const auto [start, head] =
        sub_cubic(segment_start(contour, first), contour.segments[first], first_t, 1.0);
    result.start = start;
    result.segments.push_back(head);
    for (std::size_t index = first + 1U; index < last; ++index)
        result.segments.push_back(contour.segments[index]);
    if (last_t > 0.0) {
        result.segments.push_back(
            sub_cubic(segment_start(contour, last), contour.segments[last], 0.0, last_t).second);
    }
    return result;
}

[[nodiscard]] bool same(const Point left, const Point right) noexcept {
    return std::abs(left.x - right.x) <= 1.0e-9 && std::abs(left.y - right.y) <= 1.0e-9;
}

/** A straight segment: both controls on the line between its ends. */
[[nodiscard]] bool straight(const Point from, const Cubic& cubic) noexcept {
    const Point direction = cubic.to - from;
    const double span = length(direction);
    const auto off_line = [&](const Point control) {
        const Point offset = control - from;
        if (span <= 1.0e-12)
            return length(offset) > 1.0e-9;
        const double cross = std::abs(direction.x * offset.y - direction.y * offset.x) / span;
        const double along = (direction.x * offset.x + direction.y * offset.y) / (span * span);
        return cross > 1.0e-9 || along < -1.0e-9 || along > 1.0 + 1.0e-9;
    };
    return !off_line(cubic.control_a) && !off_line(cubic.control_b);
}

} // namespace

Affine Affine::translate(const double x, const double y) noexcept {
    return Affine{1.0, 0.0, 0.0, 1.0, x, y};
}

Affine Affine::scale(const double x, const double y) noexcept {
    return Affine{x, 0.0, 0.0, y, 0.0, 0.0};
}

Affine Affine::rotate(const double degrees) noexcept {
    const double radians = degrees * std::numbers::pi / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    return Affine{cosine, sine, -sine, cosine, 0.0, 0.0};
}

Point Affine::apply(const Point value) const noexcept {
    return Point{a * value.x + c * value.y + e, b * value.x + d * value.y + f};
}

Point Affine::apply_vector(const Point value) const noexcept {
    return Point{a * value.x + c * value.y, b * value.x + d * value.y};
}

std::optional<Affine> Affine::inverse() const noexcept {
    const double det = determinant();
    if (!std::isfinite(det) || std::abs(det) < 1.0e-12)
        return std::nullopt;
    const double inverse_det = 1.0 / det;
    const Affine result{d * inverse_det,
                        -b * inverse_det,
                        -c * inverse_det,
                        a * inverse_det,
                        (c * f - d * e) * inverse_det,
                        (b * e - a * f) * inverse_det};
    if (!result.finite())
        return std::nullopt;
    return result;
}

double Affine::mean_scale() const noexcept {
    return std::sqrt(std::abs(determinant()));
}

bool Affine::finite() const noexcept {
    return std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d) &&
           std::isfinite(e) && std::isfinite(f);
}

Affine operator*(const Affine& left, const Affine& right) noexcept {
    return Affine{
        left.a * right.a + left.c * right.b,          left.b * right.a + left.d * right.b,
        left.a * right.c + left.c * right.d,          left.b * right.c + left.d * right.d,
        left.a * right.e + left.c * right.f + left.e, left.b * right.e + left.d * right.f + left.f,
    };
}

Contour contour_from_bezier(const resource::lottie::BezierShape& shape) {
    Contour result;
    const std::size_t count = shape.vertices.size();
    if (count == 0U)
        return result;
    result.start = point(shape.vertices[0]);
    result.closed = shape.closed;
    const auto segment = [&](const std::size_t from, const std::size_t to) {
        const Point a = point(shape.vertices[from]);
        const Point b = point(shape.vertices[to]);
        return Cubic{a + point(shape.out_tangents[from]), b + point(shape.in_tangents[to]), b};
    };
    result.segments.reserve(count);
    for (std::size_t index = 1U; index < count; ++index)
        result.segments.push_back(segment(index - 1U, index));
    if (shape.closed && count >= 2U)
        result.segments.push_back(segment(count - 1U, 0U));
    return result;
}

Contour reversed(const Contour& contour) {
    Contour result;
    result.closed = contour.closed;
    if (contour.segments.empty()) {
        result.start = contour.start;
        return result;
    }
    if (contour.closed) {
        // The same start, the other way round: the last segment, backwards, comes first.
        result.start = contour.start;
        for (std::size_t index = contour.segments.size(); index-- > 0U;) {
            const Cubic& segment = contour.segments[index];
            result.segments.push_back(
                Cubic{segment.control_b, segment.control_a, segment_start(contour, index)});
        }
        return result;
    }
    result.start = contour.segments.back().to;
    for (std::size_t index = contour.segments.size(); index-- > 0U;) {
        const Cubic& segment = contour.segments[index];
        result.segments.push_back(
            Cubic{segment.control_b, segment.control_a, segment_start(contour, index)});
    }
    return result;
}

void transform(CubicPath& path, const Affine& map) {
    for (Contour& contour : path) {
        contour.start = map.apply(contour.start);
        for (Cubic& segment : contour.segments) {
            segment.control_a = map.apply(segment.control_a);
            segment.control_b = map.apply(segment.control_b);
            segment.to = map.apply(segment.to);
        }
    }
}

Contour rectangle_contour(const Point center, const Point size, const double roundness,
                          const int direction) {
    const double half_width = std::abs(size.x) * 0.5;
    const double half_height = std::abs(size.y) * 0.5;
    const double radius = std::clamp(roundness, 0.0, std::min(half_width, half_height));
    const double handle = radius * circle_handle;
    const double left = center.x - half_width;
    const double right = center.x + half_width;
    const double top = center.y - half_height;
    const double bottom = center.y + half_height;
    Contour result;
    result.closed = true;
    result.start = Point{right, top + radius};
    const auto line = [&result](const Point to) {
        const Point from = result.segments.empty() ? result.start : result.segments.back().to;
        result.segments.push_back(Cubic{from, to, to});
    };
    const auto corner = [&result](const Point control_a, const Point control_b, const Point to) {
        result.segments.push_back(Cubic{control_a, control_b, to});
    };
    // Down the right edge, along the bottom, up the left edge and back along the top.
    line(Point{right, bottom - radius});
    if (radius > 0.0)
        corner(Point{right, bottom - radius + handle}, Point{right - radius + handle, bottom},
               Point{right - radius, bottom});
    line(Point{left + radius, bottom});
    if (radius > 0.0)
        corner(Point{left + radius - handle, bottom}, Point{left, bottom - radius + handle},
               Point{left, bottom - radius});
    line(Point{left, top + radius});
    if (radius > 0.0)
        corner(Point{left, top + radius - handle}, Point{left + radius - handle, top},
               Point{left + radius, top});
    line(Point{right - radius, top});
    if (radius > 0.0)
        corner(Point{right - radius + handle, top}, Point{right, top + radius - handle},
               Point{right, top + radius});
    else
        line(result.start);
    return direction == resource::lottie::reversed_direction ? reversed(result) : result;
}

Contour ellipse_contour(const Point center, const Point size, const int direction) {
    const double rx = size.x * 0.5;
    const double ry = size.y * 0.5;
    const double hx = rx * circle_handle;
    const double hy = ry * circle_handle;
    Contour result;
    result.closed = true;
    // From the top, clockwise on screen: right, bottom, left and back to the top.
    result.start = Point{center.x, center.y - ry};
    result.segments = {
        Cubic{Point{center.x + hx, center.y - ry}, Point{center.x + rx, center.y - hy},
              Point{center.x + rx, center.y}},
        Cubic{Point{center.x + rx, center.y + hy}, Point{center.x + hx, center.y + ry},
              Point{center.x, center.y + ry}},
        Cubic{Point{center.x - hx, center.y + ry}, Point{center.x - rx, center.y + hy},
              Point{center.x - rx, center.y}},
        Cubic{Point{center.x - rx, center.y - hy}, Point{center.x - hx, center.y - ry},
              Point{center.x, center.y - ry}},
    };
    return direction == resource::lottie::reversed_direction ? reversed(result) : result;
}

Contour polystar_contour(const bool star, const Point center, const double points,
                         const double rotation, const double outer_radius,
                         const double inner_radius, const double outer_roundness,
                         const double inner_roundness, const int direction) {
    Contour result;
    result.closed = true;
    const double count = std::floor(std::clamp(points, 0.0, 1000.0));
    if (count < 2.0)
        return result;
    const std::size_t vertices = static_cast<std::size_t>(star ? count * 2.0 : count);
    const double angle = 2.0 * std::numbers::pi / static_cast<double>(vertices);
    const double turn = direction == resource::lottie::reversed_direction ? -1.0 : 1.0;
    double current = -std::numbers::pi / 2.0 + rotation * std::numbers::pi / 180.0;
    struct Vertex final {
        Point at;
        Point in;
        Point out;
    };
    std::vector<Vertex> nodes;
    nodes.reserve(vertices);
    for (std::size_t index = 0U; index < vertices; ++index) {
        const bool outer = !star || index % 2U == 0U;
        const double radius = outer ? outer_radius : inner_radius;
        const double roundness = (outer ? outer_roundness : inner_roundness) / 100.0;
        // The tangent's length: a share of the perimeter each vertex spans.
        const double perimeter =
            2.0 * std::numbers::pi * radius / (static_cast<double>(vertices) * (star ? 2.0 : 4.0));
        const double x = radius * std::cos(current);
        const double y = radius * std::sin(current);
        const double norm = std::hypot(x, y);
        const double ox = norm > 0.0 ? y / norm : 0.0;
        const double oy = norm > 0.0 ? -x / norm : 0.0;
        const Point at{center.x + x, center.y + y};
        const double reach = perimeter * roundness * turn;
        nodes.push_back(Vertex{at, Point{at.x + ox * reach, at.y + oy * reach},
                               Point{at.x - ox * reach, at.y - oy * reach}});
        current += angle * turn;
    }
    result.start = nodes.front().at;
    for (std::size_t index = 1U; index <= nodes.size(); ++index) {
        const Vertex& from = nodes[index - 1U];
        const Vertex& to = nodes[index % nodes.size()];
        result.segments.push_back(Cubic{from.out, to.in, to.at});
    }
    return result;
}

double contour_length(const Contour& contour) {
    return measure(contour).total;
}

std::optional<std::vector<std::pair<double, double>>>
trim_ranges(const double start, const double end, const double offset) {
    double shift = std::fmod(offset, 1.0);
    if (shift < 0.0)
        shift += 1.0;
    double from = std::clamp(start, 0.0, 1.0) + shift;
    double to = std::clamp(end, 0.0, 1.0) + shift;
    if (from > to)
        std::swap(from, to);
    // As After Effects rounds them, so a trim that is meant to close does.
    from = std::round(from * 10000.0) / 10000.0;
    to = std::round(to * 10000.0) / 10000.0;
    if (to - from >= 1.0)
        return std::nullopt;
    std::vector<std::pair<double, double>> ranges;
    if (to - from <= 0.0)
        return ranges;
    if (to <= 1.0) {
        ranges.emplace_back(from, to);
    } else if (from >= 1.0) {
        ranges.emplace_back(from - 1.0, to - 1.0);
    } else {
        ranges.emplace_back(from, 1.0);
        ranges.emplace_back(0.0, to - 1.0);
    }
    return ranges;
}

Contour contour_span(const Contour& contour, const double from, const double to) {
    return extract(contour, measure(contour), from, to);
}

CubicPath trim(const CubicPath& path, const double start, const double end, const double offset,
               const bool individually) {
    const std::optional<std::vector<std::pair<double, double>>> kept =
        trim_ranges(start, end, offset);
    if (!kept.has_value())
        return path;
    const std::vector<std::pair<double, double>>& ranges = *kept;
    if (ranges.empty())
        return {};
    std::vector<Measured> measured;
    measured.reserve(path.size());
    double total = 0.0;
    for (const Contour& contour : path) {
        measured.push_back(measure(contour));
        total += measured.back().total;
    }
    CubicPath result;
    if (!individually) {
        for (std::size_t index = 0U; index < path.size(); ++index) {
            const Contour& contour = path[index];
            const double length_here = measured[index].total;
            if (length_here <= 0.0)
                continue;
            std::vector<Contour> pieces;
            for (const auto& [low, high] : ranges) {
                Contour piece =
                    extract(contour, measured[index], low * length_here, high * length_here);
                if (!piece.segments.empty())
                    pieces.push_back(std::move(piece));
            }
            // A wrapped trim of a closed contour runs on through its start as one stroke.
            if (pieces.size() == 2U && contour.closed && ranges.size() == 2U) {
                Contour joined = std::move(pieces[0]);
                joined.segments.insert(joined.segments.end(), pieces[1].segments.begin(),
                                       pieces[1].segments.end());
                result.push_back(std::move(joined));
                continue;
            }
            for (Contour& piece : pieces)
                result.push_back(std::move(piece));
        }
        return result;
    }
    double before = 0.0;
    for (std::size_t index = 0U; index < path.size(); ++index) {
        const double length_here = measured[index].total;
        for (const auto& [low, high] : ranges) {
            const double a = std::max(low * total, before);
            const double b = std::min(high * total, before + length_here);
            if (b > a) {
                Contour piece = extract(path[index], measured[index], a - before, b - before);
                if (!piece.segments.empty())
                    result.push_back(std::move(piece));
            }
        }
        before += length_here;
    }
    return result;
}

CubicPath round_corners(const CubicPath& path, const double radius) {
    if (!(radius > 0.0))
        return path;
    CubicPath result;
    result.reserve(path.size());
    struct Node final {
        Point at;
        Point in;
        Point out;
    };
    for (const Contour& contour : path) {
        const std::size_t segments = contour.segments.size();
        if (segments == 0U) {
            result.push_back(contour);
            continue;
        }
        // Vertices with absolute handles; a closed contour's last vertex is its first.
        std::vector<Node> nodes;
        const std::size_t count = contour.closed ? segments : segments + 1U;
        nodes.reserve(count);
        for (std::size_t index = 0U; index < count; ++index) {
            const Point at = segment_start(contour, index);
            Point in = at;
            if (index > 0U)
                in = contour.segments[index - 1U].control_b;
            else if (contour.closed)
                in = contour.segments.back().control_b;
            const Point out = index < segments ? contour.segments[index].control_a : at;
            nodes.push_back(Node{at, in, out});
        }
        std::vector<Node> rounded;
        rounded.reserve(nodes.size() * 2U);
        for (std::size_t index = 0U; index < nodes.size(); ++index) {
            const Node& node = nodes[index];
            const bool interior = contour.closed || (index != 0U && index + 1U != nodes.size());
            if (!interior || !same(node.in, node.at) || !same(node.out, node.at)) {
                rounded.push_back(node);
                continue;
            }
            const Point previous = nodes[(index + nodes.size() - 1U) % nodes.size()].at;
            const Point next = nodes[(index + 1U) % nodes.size()].at;
            const auto toward = [&](const Point other) {
                const double distance = length(other - node.at);
                const double reach =
                    distance > 0.0 ? std::min(distance * 0.5, radius) / distance : 0.0;
                return node.at + (other - node.at) * reach;
            };
            const Point a = toward(previous);
            const Point b = toward(next);
            rounded.push_back(Node{a, a, a + (node.at - a) * circle_handle});
            rounded.push_back(Node{b, b + (node.at - b) * circle_handle, b});
        }
        Contour next;
        next.closed = contour.closed;
        next.start = rounded.front().at;
        for (std::size_t index = 1U; index < rounded.size(); ++index) {
            next.segments.push_back(
                Cubic{rounded[index - 1U].out, rounded[index].in, rounded[index].at});
        }
        if (contour.closed) {
            next.segments.push_back(
                Cubic{rounded.back().out, rounded.front().in, rounded.front().at});
        }
        result.push_back(std::move(next));
    }
    return result;
}

Path to_path(const CubicPath& path) {
    Path result;
    for (const Contour& contour : path) {
        if (contour.segments.empty())
            continue;
        result.move_to(contour.start);
        for (std::size_t index = 0U; index < contour.segments.size(); ++index) {
            const Cubic& segment = contour.segments[index];
            const Point from = segment_start(contour, index);
            // A closed contour's straight way home is the close itself.
            if (contour.closed && index + 1U == contour.segments.size() &&
                straight(from, segment)) {
                break;
            }
            if (straight(from, segment))
                result.line_to(segment.to);
            else
                result.cubic_to(segment.control_a, segment.control_b, segment.to);
        }
        if (contour.closed)
            result.close();
    }
    return result;
}

std::vector<PathContour> flatten(const CubicPath& path, const double tolerance) {
    const Path outline = to_path(path);
    if (outline.empty())
        return {};
    std::vector<PathContour> contours =
        flatten_path(outline, Size{1.0, 1.0}, std::max(tolerance, 1.0e-4));
    for (PathContour& contour : contours) {
        if (contour.closed && contour.points.size() > 1U &&
            same(contour.points.front(), contour.points.back())) {
            contour.points.pop_back();
        }
    }
    return contours;
}

std::vector<SweepLevel>
sweep(const std::span<const PolygonSet> sets,
      const std::function<double(std::span<const std::uint8_t> inside)>& value) {
    struct Edge final {
        double x0 = 0.0;
        double y0 = 0.0;
        double x1 = 0.0;
        double y1 = 0.0;
        double slope = 0.0;
        int winding = 0;
        std::uint32_t set = 0U;
        [[nodiscard]] double x_at(const double y) const noexcept {
            if (y <= y0)
                return x0;
            if (y >= y1)
                return x1;
            return x0 + (y - y0) * slope;
        }
    };
    std::vector<Edge> edges;
    std::vector<double> heights;
    for (std::size_t set = 0U; set < sets.size(); ++set) {
        for (const std::vector<Point>& polygon : sets[set].polygons) {
            const std::size_t count = polygon.size();
            if (count < 3U)
                continue;
            for (std::size_t index = 0U; index < count; ++index) {
                const Point a = polygon[index];
                const Point b = polygon[(index + 1U) % count];
                if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) ||
                    !std::isfinite(b.y) || a.y == b.y) {
                    continue;
                }
                Edge edge;
                edge.set = static_cast<std::uint32_t>(set);
                if (a.y < b.y) {
                    edge = Edge{a.x, a.y, b.x, b.y, 0.0, 1, edge.set};
                } else {
                    edge = Edge{b.x, b.y, a.x, a.y, 0.0, -1, edge.set};
                }
                edge.slope = (edge.x1 - edge.x0) / (edge.y1 - edge.y0);
                edges.push_back(edge);
                heights.push_back(edge.y0);
                heights.push_back(edge.y1);
            }
        }
    }
    if (edges.empty())
        return {};
    std::ranges::sort(heights);
    heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
    std::ranges::sort(edges, {}, &Edge::y0);

    struct Open final {
        std::size_t left = 0U;
        std::size_t right = 0U;
        int level = 0;
        Trapezoid shape;
    };
    std::map<int, std::vector<Trapezoid>> levels;
    std::vector<Open> open;
    std::vector<Open> next_open;
    std::vector<std::size_t> active;
    std::vector<int> winding(sets.size(), 0);
    std::vector<std::uint8_t> inside(sets.size(), 0U);
    std::size_t next_edge = 0U;
    std::size_t bands = 0U;

    const auto close_all = [&](std::vector<Open>& list) {
        for (const Open& entry : list)
            levels[entry.level].push_back(entry.shape);
        list.clear();
    };
    // One band in which no edges cross: walk it left to right, cutting it where the value changes.
    const auto band = [&](const double top, const double bottom) {
        const double middle = (top + bottom) * 0.5;
        std::ranges::sort(active, [&](const std::size_t left, const std::size_t right) {
            const double a = edges[left].x_at(middle);
            const double b = edges[right].x_at(middle);
            return a != b ? a < b : left < right;
        });
        std::ranges::fill(winding, 0);
        next_open.clear();
        // Continues a trapezoid of the band above that runs between the same edges, else opens one.
        const auto emit = [&](const std::size_t left, const std::size_t right, const int level) {
            const auto found = std::ranges::find_if(open, [&](const Open& entry) {
                return entry.left == left && entry.right == right && entry.level == level &&
                       entry.shape.bottom == top;
            });
            if (found != open.end()) {
                Open continued = *found;
                continued.shape.bottom = bottom;
                continued.shape.bottom_left = edges[left].x_at(bottom);
                continued.shape.bottom_right = edges[right].x_at(bottom);
                next_open.push_back(continued);
                *found = open.back();
                open.pop_back();
                return;
            }
            next_open.push_back(
                Open{left, right, level,
                     Trapezoid{top, bottom, edges[left].x_at(top), edges[right].x_at(top),
                               edges[left].x_at(bottom), edges[right].x_at(bottom)}});
        };
        std::optional<std::size_t> run_left;
        int run_level = 0;
        for (std::size_t position = 0U; position < active.size(); ++position) {
            const Edge& edge = edges[active[position]];
            winding[edge.set] += edge.winding;
            if (position + 1U == active.size())
                break;
            for (std::size_t set = 0U; set < sets.size(); ++set) {
                inside[set] = static_cast<std::uint8_t>(sets[set].even_odd ? (winding[set] & 1) != 0
                                                                           : winding[set] != 0);
            }
            const double result = value(inside);
            const int level =
                std::isfinite(result)
                    ? static_cast<int>(std::lround(std::clamp(result, 0.0, 1.0) * 255.0))
                    : 0;
            if (run_left.has_value() && level == run_level)
                continue;
            if (run_left.has_value() && run_level > 0)
                emit(*run_left, active[position], run_level);
            run_left = active[position];
            run_level = level;
        }
        if (run_left.has_value() && run_level > 0 && active.size() >= 2U)
            emit(*run_left, active.back(), run_level);
        close_all(open);
        std::swap(open, next_open);
    };

    for (std::size_t index = 0U; index + 1U < heights.size(); ++index) {
        const double top = heights[index];
        const double bottom = heights[index + 1U];
        std::erase_if(active, [&](const std::size_t edge) { return edges[edge].y1 <= top; });
        while (next_edge < edges.size() && edges[next_edge].y0 <= top) {
            if (edges[next_edge].y1 > top)
                active.push_back(next_edge);
            ++next_edge;
        }
        if (active.size() < 2U) {
            close_all(open);
            continue;
        }
        // Cut the band at every crossing, earliest first: the earliest is always between two
        // edges adjacent just above it.
        double low = top;
        while (low < bottom) {
            if (++bands > maximum_bands)
                throw std::invalid_argument("a mask or matte is too complex to resolve");
            std::ranges::sort(active, [&](const std::size_t left, const std::size_t right) {
                const double a = edges[left].x_at(low);
                const double b = edges[right].x_at(low);
                if (a != b)
                    return a < b;
                return edges[left].x_at(bottom) < edges[right].x_at(bottom);
            });
            double high = bottom;
            for (std::size_t position = 0U; position + 1U < active.size(); ++position) {
                const Edge& left = edges[active[position]];
                const Edge& right = edges[active[position + 1U]];
                if (left.x_at(bottom) <= right.x_at(bottom))
                    continue;
                const double gap = right.x_at(low) - left.x_at(low);
                const double closing = left.slope - right.slope;
                if (closing <= 0.0)
                    continue;
                const double crossing = low + gap / closing;
                if (crossing > low + sweep_epsilon && crossing < high)
                    high = crossing;
            }
            if (high - low <= sweep_epsilon) {
                low = high;
                continue;
            }
            band(low, high);
            low = high;
        }
    }
    close_all(open);
    std::vector<SweepLevel> result;
    result.reserve(levels.size());
    for (auto& [level, trapezoids] : levels)
        result.push_back(SweepLevel{static_cast<double>(level) / 255.0, std::move(trapezoids)});
    return result;
}

std::vector<std::vector<Point>> trapezoid_polygons(const std::span<const Trapezoid> region) {
    std::vector<std::vector<Point>> result;
    result.reserve(region.size());
    for (const Trapezoid& shape : region) {
        std::vector<Point> polygon{Point{shape.top_left, shape.top},
                                   Point{shape.top_right, shape.top}};
        polygon.push_back(Point{shape.bottom_right, shape.bottom});
        polygon.push_back(Point{shape.bottom_left, shape.bottom});
        result.push_back(std::move(polygon));
    }
    return result;
}

std::vector<std::vector<Point>> region_outline(const std::span<const Trapezoid> region) {
    struct Segment final {
        Point from;
        Point to;
    };
    std::vector<Segment> segments;
    // Sides: up the left (the region on the right of the way round), down the right.
    std::map<double, std::vector<std::pair<double, double>>> tops;
    std::map<double, std::vector<std::pair<double, double>>> bottoms;
    for (const Trapezoid& shape : region) {
        segments.push_back(
            Segment{Point{shape.bottom_left, shape.bottom}, Point{shape.top_left, shape.top}});
        segments.push_back(
            Segment{Point{shape.top_right, shape.top}, Point{shape.bottom_right, shape.bottom}});
        tops[shape.top].emplace_back(shape.top_left, shape.top_right);
        bottoms[shape.bottom].emplace_back(shape.bottom_left, shape.bottom_right);
    }
    // At each height, what is covered below and not above is a top edge (left to right), and what
    // is covered above and not below a bottom edge (right to left).
    std::vector<double> heights;
    for (const auto& entry : tops)
        heights.push_back(entry.first);
    for (const auto& entry : bottoms)
        heights.push_back(entry.first);
    std::ranges::sort(heights);
    heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
    for (const double y : heights) {
        std::vector<std::pair<double, int>> events;
        if (const auto found = tops.find(y); found != tops.end()) {
            for (const auto& [left, right] : found->second) {
                events.emplace_back(left, 1);
                events.emplace_back(right, -1);
            }
        }
        if (const auto found = bottoms.find(y); found != bottoms.end()) {
            for (const auto& [left, right] : found->second) {
                events.emplace_back(left, -1);
                events.emplace_back(right, 1);
            }
        }
        std::ranges::sort(
            events, [](const auto& left, const auto& right) { return left.first < right.first; });
        // Below minus above: positive is a top edge, negative a bottom edge.
        int balance = 0;
        for (std::size_t index = 0U; index + 1U < events.size(); ++index) {
            balance += events[index].second;
            const double x0 = events[index].first;
            const double x1 = events[index + 1U].first;
            if (x1 - x0 <= 1.0e-12 || balance == 0)
                continue;
            if (balance > 0)
                segments.push_back(Segment{Point{x0, y}, Point{x1, y}});
            else
                segments.push_back(Segment{Point{x1, y}, Point{x0, y}});
        }
    }
    // Chain segments end to start, by position.
    const auto key = [](const Point point) {
        const auto snap = [](const double value) {
            return static_cast<long long>(std::llround(value * 1.0e6));
        };
        return std::pair{snap(point.x), snap(point.y)};
    };
    struct KeyHash final {
        std::size_t operator()(const std::pair<long long, long long>& value) const noexcept {
            return std::hash<long long>{}(value.first) * 31U ^ std::hash<long long>{}(value.second);
        }
    };
    std::unordered_multimap<std::pair<long long, long long>, std::size_t, KeyHash> starts;
    for (std::size_t index = 0U; index < segments.size(); ++index) {
        if (key(segments[index].from) != key(segments[index].to))
            starts.emplace(key(segments[index].from), index);
    }
    std::vector<bool> used(segments.size(), false);
    std::vector<std::vector<Point>> loops;
    for (std::size_t first = 0U; first < segments.size(); ++first) {
        if (used[first] || key(segments[first].from) == key(segments[first].to))
            continue;
        std::vector<Point> loop;
        std::size_t current = first;
        while (true) {
            used[current] = true;
            loop.push_back(segments[current].from);
            const auto [begin, end] = starts.equal_range(key(segments[current].to));
            std::optional<std::size_t> next;
            for (auto it = begin; it != end; ++it) {
                if (!used[it->second]) {
                    next = it->second;
                    break;
                }
            }
            if (!next.has_value())
                break;
            current = *next;
        }
        // Drop points in the middle of straight runs.
        std::vector<Point> simplified;
        for (std::size_t index = 0U; index < loop.size(); ++index) {
            const Point previous = loop[(index + loop.size() - 1U) % loop.size()];
            const Point here = loop[index];
            const Point next = loop[(index + 1U) % loop.size()];
            const double cross = (here.x - previous.x) * (next.y - here.y) -
                                 (here.y - previous.y) * (next.x - here.x);
            if (std::abs(cross) > 1.0e-12 || loop.size() <= 3U)
                simplified.push_back(here);
        }
        if (simplified.size() >= 3U)
            loops.push_back(std::move(simplified));
    }
    return loops;
}

} // namespace strata::ui::lottie
