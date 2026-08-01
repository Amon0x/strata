#pragma once

#include <vector>

#include <strata/svg.hpp>

#include "ui/render.hpp"

namespace strata::ui {

/** Validates the bounded worst-case UI tessellation before a document becomes a live resource. */
void validate_svg_image_geometry(const svg::Document& document);

/**
 * Projects one parsed SVG display list into ordinary clipped path commands. Geometry remains
 * resolution-independent until submission tessellation, so desktop and headless hosts share the
 * exact same render-packet path and require no SVG-specific backend feature.
 */
void append_svg_image(
    std::vector<RenderCommand>& output,
    const svg::Document& document,
    Rect bounds,
    TextureRegion source,
    RenderColor tint,
    double opacity
);

} // namespace strata::ui
