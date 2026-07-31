#include "ui/render/paint_geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace strata::ui {
namespace {

/** Bounds tessellation for repeating gradients, whose boundary count is not authored directly. */
constexpr std::size_t maximum_bands = 256U;
constexpr std::size_t minimum_radial_segments = 12U;
constexpr std::size_t maximum_radial_segments = 96U;

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
    if (gradient.extend == GradientExtend::clamp) {
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

void triangle(PaintMesh& mesh, const std::uint32_t a, const std::uint32_t b, const std::uint32_t c) {
    mesh.indices.push_back(a);
    mesh.indices.push_back(b);
    mesh.indices.push_back(c);
}

[[nodiscard]] PaintMesh linear_mesh(const Gradient& gradient, const Size shape) {
    const auto [start, end] = gradient.axis(shape);
    const Point direction{end.x - start.x, end.y - start.y};
    const double length_squared = direction.x * direction.x + direction.y * direction.y;
    PaintMesh mesh;
    if (length_squared <= 0.0) return mesh;
    const double length = std::sqrt(length_squared);
    // The perpendicular frame is expressed in the same normalized space, so a band quad built from
    // it maps back to the shape without a second coordinate conversion.
    const Point along{direction.x / length, direction.y / length};
    const Point across{-along.y, along.x};
    constexpr std::array<Point, 4U> corners{
        Point{0.0, 0.0}, Point{1.0, 0.0}, Point{1.0, 1.0}, Point{0.0, 1.0},
    };
    double first = 0.0;
    double last = 0.0;
    double across_low = 0.0;
    double across_high = 0.0;
    for (std::size_t index = 0U; index < corners.size(); ++index) {
        const Point offset{corners[index].x - start.x, corners[index].y - start.y};
        const double projected = (offset.x * direction.x + offset.y * direction.y) / length_squared;
        const double lateral = offset.x * across.x + offset.y * across.y;
        if (index == 0U) {
            first = last = projected;
            across_low = across_high = lateral;
            continue;
        }
        first = std::min(first, projected);
        last = std::max(last, projected);
        across_low = std::min(across_low, lateral);
        across_high = std::max(across_high, lateral);
    }
    if (last <= first) return mesh;
    const std::vector<double> boundaries = band_boundaries(gradient, first, last);
    mesh.vertices.reserve(boundaries.size() * 2U);
    mesh.indices.reserve((boundaries.size() - 1U) * 6U);
    for (const double boundary : boundaries) {
        const Point center{
            start.x + direction.x * boundary,
            start.y + direction.y * boundary,
        };
        const RenderColor color = gradient.sample(boundary);
        mesh.vertices.push_back(PaintVertex{
            Point{center.x + across.x * across_low, center.y + across.y * across_low}, color,
        });
        mesh.vertices.push_back(PaintVertex{
            Point{center.x + across.x * across_high, center.y + across.y * across_high}, color,
        });
    }
    for (std::size_t band = 0U; band + 1U < boundaries.size(); ++band) {
        const auto base = static_cast<std::uint32_t>(band * 2U);
        triangle(mesh, base, base + 1U, base + 3U);
        triangle(mesh, base, base + 3U, base + 2U);
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

[[nodiscard]] PaintMesh radial_mesh(
    const Gradient& gradient,
    const Size shape,
    const double pixel_scale
) {
    PaintMesh mesh;
    constexpr std::array<Point, 4U> corners{
        Point{0.0, 0.0}, Point{1.0, 0.0}, Point{1.0, 1.0}, Point{0.0, 1.0},
    };
    double last = 0.0;
    for (const Point corner : corners) {
        const double x = (corner.x - gradient.center.x) / gradient.radius_x;
        const double y = (corner.y - gradient.center.y) / gradient.radius_y;
        last = std::max(last, std::sqrt(x * x + y * y));
    }
    if (last <= 0.0) return mesh;
    const std::vector<double> boundaries = band_boundaries(gradient, 0.0, last);
    const std::size_t segments = radial_segments(gradient, shape, pixel_scale, last);
    std::vector<Point> unit;
    unit.reserve(segments);
    for (std::size_t segment = 0U; segment < segments; ++segment) {
        const double angle = 2.0 * std::numbers::pi *
            static_cast<double>(segment) / static_cast<double>(segments);
        unit.push_back(Point{std::cos(angle), std::sin(angle)});
    }
    const auto ring = [&](const double radius) {
        const RenderColor color = gradient.sample(radius);
        for (const Point direction : unit) {
            mesh.vertices.push_back(PaintVertex{
                Point{
                    gradient.center.x + direction.x * gradient.radius_x * radius,
                    gradient.center.y + direction.y * gradient.radius_y * radius,
                },
                color,
            });
        }
    };
    mesh.vertices.reserve(boundaries.size() * segments);
    // The innermost boundary is the centre itself, so its ring degenerates to a point and the
    // first band fans out of it without a special case.
    for (const double boundary : boundaries) ring(boundary);
    for (std::size_t band = 0U; band + 1U < boundaries.size(); ++band) {
        const auto inner = static_cast<std::uint32_t>(band * segments);
        const auto outer = static_cast<std::uint32_t>(inner + segments);
        for (std::size_t segment = 0U; segment < segments; ++segment) {
            const auto step = static_cast<std::uint32_t>(segment);
            const auto next = static_cast<std::uint32_t>((segment + 1U) % segments);
            triangle(mesh, inner + step, outer + step, outer + next);
            triangle(mesh, inner + step, outer + next, inner + next);
        }
    }
    return mesh;
}

} // namespace

PaintMesh tessellate_gradient(
    const Gradient& gradient,
    const Size shape,
    const double pixel_scale
) {
    if (gradient.stops.empty()) return PaintMesh{};
    const Size safe{
        shape.width > 0.0 ? shape.width : 1.0,
        shape.height > 0.0 ? shape.height : 1.0,
    };
    return gradient.kind == GradientKind::linear
        ? linear_mesh(gradient, safe)
        : radial_mesh(gradient, safe, pixel_scale);
}

} // namespace strata::ui
