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

} // namespace strata::ui
