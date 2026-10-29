#pragma once

#include <vector>

#include "resource/lottie.hpp"
#include "ui/render.hpp"

namespace strata::ui::lottie {

/**
 * Draws one frame of `animation` as ordinary render commands: the composition's (0, 0)-(w, h) box
 * mapped onto `viewport`, everything cut to `clip`.
 *
 * Each fill and stroke becomes one path command under its own transform, so a shape that only
 * moves, turns or scales keeps its outline (and the rasterized mask the backend caches for it);
 * layer opacity is an opacity scope. A layer under masks or a track matte is resolved as geometry
 * instead: its outlines (strokes as their outlines) are intersected with the mask and matte shapes
 * in a sweep that yields the regions of each distinct coverage value, which draw as filled paths
 * scaled by that value. Masks combine by mode (add, subtract, intersect, lighten, darken,
 * difference), opacity, inversion and expansion; mattes by alpha or luma, inverted or not, with
 * the matte layer's own opacity, colours and masks. Feathered mask edges and mattes whose shapes
 * vary in colour or alpha within themselves (gradients) are taken at their mean.
 */
void append_frame(std::vector<RenderCommand>& output, const resource::lottie::Animation& animation,
                  double frame, Rect viewport, Rect clip);

} // namespace strata::ui::lottie
