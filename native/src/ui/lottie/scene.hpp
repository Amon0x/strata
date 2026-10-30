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
 * moves, turns or scales keeps its outline (and the rasterized mask the backend caches for it).
 * Text layers draw their letters as outlines of the system font the composition names (or of the
 * glyphs it exports), and image layers draw their decoded images.
 *
 * A layer that must be seen as one picture is drawn apart, as an isolated layer (a CONTENT effect
 * the backends composite): to fade as a whole when its paints overlap, to blend with what is under
 * it by its blend mode, to run its effects (blur, drop shadow, fill, tint, tritone, levels) over
 * its pixels, and to be cut by its track matte, whose layer is drawn apart inside it and keeps it
 * by its alpha or luminance (destination-in or -out). Masks are resolved as geometry where that is
 * exact: the content is intersected with the mask shapes into regions of equal coverage. A
 * feathered mask, or one over an image, is drawn apart instead: each mask white in a layer blurred
 * by its feather and combined with those before it by its mode, the stack keeping the layer where
 * it covers. An animation that blends or reaches past what it draws is itself one layer the box
 * cuts, so its layers blend with one another and not with the page.
 */
void append_frame(std::vector<RenderCommand>& output, const resource::lottie::Animation& animation,
                  double frame, Rect viewport, Rect clip);

} // namespace strata::ui::lottie
