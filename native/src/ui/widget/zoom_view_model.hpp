#pragma once

#include "ui/layout.hpp"

namespace strata::ui {

class RetainedNode;
class WidgetRegistry;

/**
 * What a ZoomView shows of its content: the content's top-left lands at (x, y) in the viewport
 * (relative to where the content is laid out) and the content is scaled by `zoom`.
 */
struct ZoomViewport final {
    double x = 0.0;
    double y = 0.0;
    double zoom = 1.0;
    [[nodiscard]] friend bool operator==(const ZoomViewport&, const ZoomViewport&) = default;
};

/** The zoom limits a ZoomView keeps to (`minZoom`, `maxZoom`). */
struct ZoomLimits final {
    double minimum = 0.1;
    double maximum = 8.0;
    [[nodiscard]] double clamp(double zoom) const noexcept;
};

/** The view at `zoom` that keeps the content point under `anchor` (viewport-local) where it is. */
[[nodiscard]] ZoomViewport zoom_view_about(const ZoomViewport& view, Point anchor,
                                           double zoom) noexcept;

/**
 * The view that shows all of `content` (in content coordinates) centred in a viewport of `size`,
 * `padding` clear of its edges, at the largest zoom within `limits`.
 */
[[nodiscard]] ZoomViewport zoom_view_fit(Size size, Rect content, double padding,
                                         const ZoomLimits& limits) noexcept;

/**
 * One frame of a view easing to `target` over `seconds`: zoom eases geometrically, the pan
 * linearly. With `anchor` (viewport-local) and `focus` (the content point under it), the pan is
 * whatever keeps `focus` under `anchor`, so a zoom at the cursor stays at the cursor throughout.
 */
[[nodiscard]] ZoomViewport zoom_view_step(const ZoomViewport& current, const ZoomViewport& target,
                                          double seconds, const Point* anchor,
                                          const Point* focus) noexcept;

/** The view a ZoomView currently shows, from its retained state; the identity view without one. */
[[nodiscard]] ZoomViewport zoom_view_current(const RetainedNode& node) noexcept;

/** Registers ZoomView's description, input, frame, semantics and persistence phases. */
void register_zoom_view_widget(WidgetRegistry& registry);

} // namespace strata::ui
