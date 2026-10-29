#pragma once

#include "ui/path.hpp"
#include "ui/render/paint_geometry.hpp"

namespace strata::ui {

/**
 * Expands one authored shape into indexed triangles in the normalized space of the shape it is
 * drawn into. Fills are triangulated, strokes are expanded to their own geometry, and both carry
 * a one-device-pixel feather so edges stay smooth without a signed-distance material.
 *
 * `shape` is the size the normalized outline is drawn at in logical pixels and `pixel_scale`
 * converts those to device pixels; together they keep stroke widths, curve flattening and the
 * feather correct at any UI scale.
 */
[[nodiscard]] PaintMesh tessellate_shape(
    const PathShape& shape,
    Size shape_size,
    double pixel_scale
);

/**
 * The region a stroke covers, as closed polygons in the same space as `centrelines` (already
 * flattened): each segment's band, its joins and its caps, after dashing, with round geometry
 * within `tolerance` of true arcs. Every polygon winds the same way, so the nonzero rule unions
 * their overlaps into exactly the stroke.
 */
[[nodiscard]] std::vector<PathContour> stroke_outline(const std::vector<PathContour>& centrelines,
                                                      const StrokeStyle& style, double tolerance);

/** A run of a stroke's centreline: its points, and whether its last joins its first. */
struct StrokeRun final {
    std::vector<Point> points;
    bool closed = false;
};

/**
 * The runs a stroke's centrelines (already flattened) draw as, after dashing: what the GPU
 * expands into the stroke itself.
 */
[[nodiscard]] std::vector<StrokeRun> stroke_runs(const std::vector<PathContour>& centrelines,
                                                 const StrokeStyle& style);

} // namespace strata::ui
