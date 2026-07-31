#pragma once

#include <cstdint>
#include <vector>

#include "ui/paint.hpp"
#include "ui/render.hpp"

namespace strata::ui {

/**
 * One tessellated gradient vertex. `normalized` is the position inside the filled shape, which is
 * both the gradient's authoring space and the shape-local coordinate the signed-distance materials
 * read, so tessellated geometry keeps the shape's rounding, border and antialiasing.
 */
struct PaintVertex final {
    Point normalized;
    RenderColor color;
    [[nodiscard]] friend bool operator==(const PaintVertex&, const PaintVertex&) = default;
};

struct PaintMesh final {
    std::vector<PaintVertex> vertices;
    std::vector<std::uint32_t> indices;
    [[nodiscard]] friend bool operator==(const PaintMesh&, const PaintMesh&) = default;
};

/**
 * Expands a gradient into indexed triangles covering `shape`, whose vertex colours reproduce the
 * ramp exactly at every stop boundary. `pixel_scale` converts shape units to device pixels and
 * only bounds angular tessellation. The mesh may extend past the shape; the shape's own material
 * masks the surplus.
 */
[[nodiscard]] PaintMesh tessellate_gradient(
    const Gradient& gradient,
    Size shape,
    double pixel_scale
);

} // namespace strata::ui
