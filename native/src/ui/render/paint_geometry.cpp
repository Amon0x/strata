#include "ui/render/paint_geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>

namespace strata::ui {
namespace {

/** Bounds tessellation for repeating gradients, whose boundary count is not authored directly. */
constexpr std::size_t maximum_bands = 256U;
constexpr std::size_t minimum_radial_segments = 12U;
constexpr std::size_t maximum_radial_segments = 96U;
/**
 * The widest wedge a conic sweep is cut into, in turns. Colour interpolates linearly across a
 * wedge's chord rather than its angle; at two degrees the difference is far below one 8-bit level.
 */
constexpr double maximum_conic_wedge = 1.0 / 180.0;

using Frame = std::array<double, 6U>;

/** The inverse of an affine frame, or nothing when it collapses the plane. */
[[nodiscard]] std::optional<Frame> inverse(const Frame& map) noexcept {
    const double determinant = map[0] * map[4] - map[1] * map[3];
    if (!(std::abs(determinant) > 1.0e-18) || !std::isfinite(determinant))
        return std::nullopt;
    const double a = map[4] / determinant;
    const double b = -map[1] / determinant;
    const double d = -map[3] / determinant;
    const double e = map[0] / determinant;
    return Frame{a, b, -(a * map[2] + b * map[5]), d, e, -(d * map[2] + e * map[5])};
}

[[nodiscard]] Point apply(const Frame& map, const Point point) noexcept {
    return Point{map[0] * point.x + map[1] * point.y + map[2],
                 map[3] * point.x + map[4] * point.y + map[5]};
}

/** The covered region's corners, in normalized shape space. */
[[nodiscard]] std::array<Point, 4U> corners(const Rect region) noexcept {
    return {Point{region.x, region.y}, Point{region.right(), region.y},
            Point{region.right(), region.bottom()}, Point{region.x, region.bottom()}};
}

[[nodiscard]] std::vector<double> band_boundaries(
    const Gradient& gradient,
    const double first,
    const double last
) {
    std::vector<double> boundaries{first};
    // Boundaries closer together than this collapse: a sliver band would cost geometry without
    // changing a single pixel, and coincident stops must not produce degenerate triangles.
    const double epsilon = std::max(std::abs(last - first), 1.0) * 1e-9;
    const auto push = [&boundaries, last, epsilon](const double value) {
        if (value <= boundaries.back() + epsilon || value >= last - epsilon) return;
        boundaries.push_back(value);
    };
    if (gradient.extend == GradientExtend::clamp || gradient.kind == GradientKind::conic) {
        for (const GradientStop& stop : gradient.stops) push(stop.offset);
    } else {
        // Repeat and mirror tile the authored ramp, so every tile contributes its own boundaries
        // until the shape is covered or the tessellation budget is spent.
        const double period = gradient.extend == GradientExtend::mirror ? 2.0 : 1.0;
        const double start_tile = std::floor(first / period);
        for (double tile = start_tile;
             tile * period < last && boundaries.size() < maximum_bands;
             tile += 1.0) {
            const double base = tile * period;
            for (const GradientStop& stop : gradient.stops) push(base + stop.offset);
            if (gradient.extend == GradientExtend::mirror) {
                for (const GradientStop& stop : gradient.stops) {
                    push(base + 2.0 - stop.offset);
                }
            }
            push(base + period);
        }
        std::ranges::sort(boundaries);
        boundaries.erase(
            std::ranges::unique(boundaries, [epsilon](const double left, const double right) {
                return std::abs(left - right) <= epsilon;
            }).begin(),
            boundaries.end()
        );
    }
    if (boundaries.size() >= maximum_bands) boundaries.resize(maximum_bands - 1U);
    boundaries.push_back(last);
    return boundaries;
}

/**
 * The colours either side of every boundary: the ramp approached from below and from above. They
 * differ only at a hard stop (two stops at one offset) or where a repeating ramp starts over, and
 * there the geometry splits so the step stays sharp instead of smearing across a band.
 */
struct BoundaryColors final {
    RenderColor before;
    RenderColor after;
};

[[nodiscard]] std::vector<BoundaryColors> boundary_colors(const Gradient& gradient,
                                                          const std::vector<double>& boundaries) {
    std::vector<BoundaryColors> colors;
    colors.reserve(boundaries.size());
    for (std::size_t index = 0U; index < boundaries.size(); ++index) {
        const double here = boundaries[index];
        const double below = index > 0U ? (here - boundaries[index - 1U]) * 1.0e-7 : 0.0;
        const double above =
            index + 1U < boundaries.size() ? (boundaries[index + 1U] - here) * 1.0e-7 : 0.0;
        colors.push_back(
            BoundaryColors{gradient.sample(here - below), gradient.sample(here + above)});
    }
    return colors;
}

void triangle(PaintMesh& mesh, const std::uint32_t a, const std::uint32_t b, const std::uint32_t c) {
    mesh.indices.push_back(a);
    mesh.indices.push_back(b);
    mesh.indices.push_back(c);
}

[[nodiscard]] std::uint32_t vertex(PaintMesh& mesh, const Point point, const RenderColor color) {
    mesh.vertices.push_back(PaintVertex{point, color});
    return static_cast<std::uint32_t>(mesh.vertices.size() - 1U);
}

[[nodiscard]] PaintMesh linear_mesh(const Gradient& gradient, const Size shape, const Rect region) {
    PaintMesh mesh;
    // The frame's first row is the ramp parameter and its second measures across it, so a band
    // is a rectangle in frame space that maps back to a parallelogram in the shape.
    const Frame map = gradient.frame(shape);
    const std::optional<Frame> back = inverse(map);
    if (!back.has_value())
        return mesh;
    double first = 0.0;
    double last = 0.0;
    double across_low = 0.0;
    double across_high = 0.0;
    const std::array<Point, 4U> region_corners = corners(region);
    for (std::size_t index = 0U; index < region_corners.size(); ++index) {
        const Point mapped = apply(map, region_corners[index]);
        if (index == 0U) {
            first = last = mapped.x;
            across_low = across_high = mapped.y;
            continue;
        }
        first = std::min(first, mapped.x);
        last = std::max(last, mapped.x);
        across_low = std::min(across_low, mapped.y);
        across_high = std::max(across_high, mapped.y);
    }
    if (last <= first) return mesh;
    const std::vector<double> boundaries = band_boundaries(gradient, first, last);
    const std::vector<BoundaryColors> colors = boundary_colors(gradient, boundaries);
    mesh.vertices.reserve(boundaries.size() * 2U);
    mesh.indices.reserve((boundaries.size() - 1U) * 6U);
    // Each boundary's pair of vertices in the colour arriving at it and, where that differs, a
    // second pair in the colour leaving it.
    std::uint32_t leaving = 0U;
    for (std::size_t index = 0U; index < boundaries.size(); ++index) {
        const Point low = apply(*back, Point{boundaries[index], across_low});
        const Point high = apply(*back, Point{boundaries[index], across_high});
        const std::uint32_t arriving = vertex(mesh, low, colors[index].before);
        static_cast<void>(vertex(mesh, high, colors[index].before));
        if (index > 0U) {
            triangle(mesh, leaving, leaving + 1U, arriving + 1U);
            triangle(mesh, leaving, arriving + 1U, arriving);
        }
        leaving = arriving;
        if (colors[index].after != colors[index].before) {
            leaving = vertex(mesh, low, colors[index].after);
            static_cast<void>(vertex(mesh, high, colors[index].after));
        }
    }
    return mesh;
}

[[nodiscard]] std::size_t radial_segments(
    const Gradient& gradient,
    const Size shape,
    const double pixel_scale,
    const double radius
) {
    const double extent = std::max(
        std::abs(gradient.radius_x * radius * shape.width),
        std::abs(gradient.radius_y * radius * shape.height)
    ) * std::max(pixel_scale, 0.1);
    // One segment per few device pixels of arc keeps the ring boundaries visually circular.
    const double requested = std::ceil(extent * 2.0 * std::numbers::pi / 6.0);
    return static_cast<std::size_t>(std::clamp(
        requested,
        static_cast<double>(minimum_radial_segments),
        static_cast<double>(maximum_radial_segments)
    ));
}

[[nodiscard]] PaintMesh radial_mesh(const Gradient& gradient, const Size shape,
                                    const double pixel_scale, const Rect region) {
    PaintMesh mesh;
    // In the frame the ellipse is the unit circle and the parameter is the distance from its
    // centre, so rings are circles there and map back to the turned, scaled ellipse.
    const Frame map = gradient.frame(shape);
    const std::optional<Frame> back = inverse(map);
    if (!back.has_value())
        return mesh;
    double last = 0.0;
    for (const Point corner : corners(region)) {
        const Point mapped = apply(map, corner);
        last = std::max(last, std::sqrt(mapped.x * mapped.x + mapped.y * mapped.y));
    }
    if (last <= 0.0)
        return mesh;
    const std::size_t segments = radial_segments(gradient, shape, pixel_scale, last);
    // The outermost ring's chords must still reach the farthest corner.
    const double reach = last / std::cos(std::numbers::pi / static_cast<double>(segments));
    std::vector<double> boundaries = band_boundaries(gradient, 0.0, last);
    boundaries.back() = reach;
    const std::vector<BoundaryColors> colors = boundary_colors(gradient, boundaries);
    std::vector<Point> unit;
    unit.reserve(segments);
    for (std::size_t segment = 0U; segment < segments; ++segment) {
        const double angle = 2.0 * std::numbers::pi *
            static_cast<double>(segment) / static_cast<double>(segments);
        unit.push_back(Point{std::cos(angle), std::sin(angle)});
    }
    const auto ring = [&](const double radius, const RenderColor color) {
        const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
        for (const Point direction : unit) {
            static_cast<void>(vertex(
                mesh, apply(*back, Point{direction.x * radius, direction.y * radius}), color));
        }
        return first;
    };
    mesh.vertices.reserve(boundaries.size() * segments);
    // The innermost boundary is the centre itself, so its ring degenerates to a point and the
    // first band fans out of it without a special case.
    std::uint32_t leaving = 0U;
    for (std::size_t index = 0U; index < boundaries.size(); ++index) {
        const std::uint32_t arriving = ring(boundaries[index], colors[index].before);
        if (index > 0U) {
            for (std::size_t segment = 0U; segment < segments; ++segment) {
                const auto step = static_cast<std::uint32_t>(segment);
                const auto next = static_cast<std::uint32_t>((segment + 1U) % segments);
                triangle(mesh, leaving + step, arriving + step, arriving + next);
                triangle(mesh, leaving + step, arriving + next, leaving + next);
            }
        }
        leaving = arriving;
        if (colors[index].after != colors[index].before)
            leaving = ring(boundaries[index], colors[index].after);
    }
    return mesh;
}

/**
 * A conic sweep as a fan of thin wedges about its centre, cut at every stop so each stop's colour
 * lands exactly on its ray. Each wedge has its own apex, coloured at the wedge's middle, so the
 * colour stays a function of angle all the way in; the start ray, where the sweep wraps from its
 * last stop to its first, splits so the seam stays sharp.
 */
[[nodiscard]] PaintMesh conic_mesh(const Gradient& gradient, const Size shape, const Rect region) {
    PaintMesh mesh;
    const Frame map = gradient.frame(shape);
    const std::optional<Frame> back = inverse(map);
    if (!back.has_value())
        return mesh;
    double radius = 0.0;
    for (const Point corner : corners(region)) {
        const Point mapped = apply(map, corner);
        radius = std::max(radius, std::sqrt(mapped.x * mapped.x + mapped.y * mapped.y));
    }
    if (radius <= 0.0)
        return mesh;
    // Turns at which the fan is cut: every stop, then enough between them that no wedge is wider
    // than the interpolation tolerance.
    const std::vector<double> stops = band_boundaries(gradient, 0.0, 1.0);
    std::vector<double> cuts;
    for (std::size_t index = 0U; index + 1U < stops.size(); ++index) {
        const double from = stops[index];
        const double span = stops[index + 1U] - from;
        const auto pieces =
            static_cast<std::size_t>(std::max(1.0, std::ceil(span / maximum_conic_wedge)));
        for (std::size_t piece = 0U; piece < pieces; ++piece)
            cuts.push_back(from + span * static_cast<double>(piece) / static_cast<double>(pieces));
    }
    cuts.push_back(1.0);
    const std::vector<BoundaryColors> colors = boundary_colors(gradient, cuts);
    // The widest wedge's chord must still reach the farthest corner.
    double widest = 0.0;
    for (std::size_t index = 0U; index + 1U < cuts.size(); ++index)
        widest = std::max(widest, cuts[index + 1U] - cuts[index]);
    const double reach = radius / std::cos(std::numbers::pi * widest) * 1.0001;
    const auto rim = [&](const double turn, const RenderColor color) {
        const double angle = turn * 2.0 * std::numbers::pi;
        // The frame's start direction is up (-y) and turns advance clockwise.
        return vertex(mesh, apply(*back, Point{std::sin(angle) * reach, -std::cos(angle) * reach}),
                      color);
    };
    const Point apex = apply(*back, Point{0.0, 0.0});
    mesh.vertices.reserve(cuts.size() * 2U + cuts.size());
    mesh.indices.reserve(cuts.size() * 3U);
    // The start ray's leaving colour is the first stop's; its arriving one, at a full turn, the
    // last stop's.
    std::uint32_t leaving = rim(cuts.front(), colors.front().after);
    for (std::size_t index = 1U; index < cuts.size(); ++index) {
        const std::uint32_t arriving = rim(cuts[index], colors[index].before);
        const std::uint32_t centre =
            vertex(mesh, apex, gradient.sample((cuts[index - 1U] + cuts[index]) * 0.5));
        triangle(mesh, centre, leaving, arriving);
        leaving = colors[index].after != colors[index].before
                      ? rim(cuts[index], colors[index].after)
                      : arriving;
    }
    return mesh;
}

} // namespace

PaintMesh tessellate_gradient(const Gradient& gradient, const Size shape, const double pixel_scale,
                              const Rect region) {
    if (gradient.stops.empty()) return PaintMesh{};
    const Size safe{
        shape.width > 0.0 ? shape.width : 1.0,
        shape.height > 0.0 ? shape.height : 1.0,
    };
    switch (gradient.kind) {
    case GradientKind::linear:
        return linear_mesh(gradient, safe, region);
    case GradientKind::radial:
        return radial_mesh(gradient, safe, pixel_scale, region);
    case GradientKind::conic:
        return conic_mesh(gradient, safe, region);
    }
    return PaintMesh{};
}

} // namespace strata::ui
