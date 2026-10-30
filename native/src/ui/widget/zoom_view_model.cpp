#include "ui/widget/zoom_view_model.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/value.hpp"
#include "ui/input.hpp"
#include "ui/presentation_geometry.hpp"
#include "ui/tree.hpp"
#include "ui/widget/description.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {
namespace {

using Fields = std::vector<std::pair<std::string, runtime::Value>>;

/** An eased zoom or fit in flight: where it is going, and the cursor it zooms about. */
constexpr std::string_view target_value = "$zoomTarget";
/** A pan in progress: the pointer, where it pressed and the view it pressed on. */
constexpr std::string_view pan_value = "$zoomPan";
/** Set for the one frame after an eased zoom lands, which draws it at its exact density. */
constexpr std::string_view settling_value = "$zoomSettling";
/** The view last reported through `onViewChange` or adopted from `view`. */
constexpr std::string_view reported_value = "$zoomReported";

/** How fast an eased zoom closes on its target: e-folds per second. */
constexpr double ease_rate = 16.0;
/** One wheel notch zooms by this factor. */
constexpr double wheel_zoom_step = 1.2;
/** One wheel notch pans this far (logical px) with Shift held, or sideways. */
constexpr double wheel_pan_step = 48.0;
/** One arrow key pans this far (logical px); with Shift, five times as far. */
constexpr double key_pan_step = 40.0;

[[nodiscard]] double finite(const runtime::Value* value, const double fallback) noexcept {
    return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
               ? *value->number()
               : fallback;
}

[[nodiscard]] std::optional<ZoomViewport> view_from(const runtime::Value* value) noexcept {
    if (value == nullptr || value->object() == nullptr)
        return std::nullopt;
    const double zoom = finite(value->field("zoom"), 1.0);
    if (!(zoom > 0.0))
        return std::nullopt;
    return ZoomViewport{finite(value->field("x"), 0.0), finite(value->field("y"), 0.0), zoom};
}

[[nodiscard]] runtime::Value view_value(const ZoomViewport& view) {
    return runtime::Value(Fields{
        {"x", runtime::Value(view.x)},
        {"y", runtime::Value(view.y)},
        {"zoom", runtime::Value(view.zoom)},
    });
}

[[nodiscard]] ZoomLimits limits(const WidgetInputScope& scope) noexcept {
    const double minimum = std::max(1.0e-3, scope.number("minZoom", 0.1));
    return ZoomLimits{minimum, std::max(minimum, scope.number("maxZoom", 8.0))};
}

/** The one child the view presents. */
[[nodiscard]] const RetainedNode* content(const RetainedNode& node) noexcept {
    return node.children().empty() ? nullptr : node.children().front().get();
}

/** Where the content is laid out: the origin views are relative to. */
[[nodiscard]] Point content_origin(const WidgetInputScope& scope) noexcept {
    const RetainedNode* child = content(scope.node());
    const LayoutRecord* record = child != nullptr ? scope.layout(*child) : nullptr;
    if (record != nullptr)
        return Point{record->bounds.x, record->bounds.y};
    const LayoutRecord* own = scope.layout();
    return own != nullptr ? Point{own->content_bounds.x, own->content_bounds.y} : Point{};
}

/**
 * Shows `view` now: the children are presented under it (they are marked so their drawing and hit
 * regions follow) and the change is reported through `onViewChange`.
 */
void show(WidgetInputScope& scope, const ZoomViewport& view) {
    if (zoom_view_current(scope.node()) == view && scope.retained(content_view_value) != nullptr) {
        return;
    }
    scope.set_retained(content_view_value, view_value(view), DirtyReason::animation);
    scope.invalidate_children(DirtyReason::animation);
    scope.set_retained(reported_value, view_value(view), DirtyReason::input);
    // Reported only to an app that listens: an animated zoom changes the view every frame.
    const auto listener = scope.node().description().properties.find("onViewChange");
    if (listener != scope.node().description().properties.end() &&
        listener->second.action() != nullptr) {
        scope.value_changed("onViewChange", "view-changed", view_value(view));
    }
}

/** Eases the view to `target`, about `anchor` when zooming at a point. */
void ease_to(WidgetInputScope& scope, const ZoomViewport& target,
             const std::optional<Point> anchor = std::nullopt) {
    const ZoomViewport current = zoom_view_current(scope.node());
    Fields fields{
        {"x", runtime::Value(target.x)},
        {"y", runtime::Value(target.y)},
        {"zoom", runtime::Value(target.zoom)},
    };
    if (anchor.has_value()) {
        // The content point under the anchor now stays under it while the zoom eases.
        fields.emplace_back("anchorX", runtime::Value(anchor->x));
        fields.emplace_back("anchorY", runtime::Value(anchor->y));
        fields.emplace_back("focusX", runtime::Value((anchor->x - current.x) / current.zoom));
        fields.emplace_back("focusY", runtime::Value((anchor->y - current.y) / current.zoom));
    }
    scope.set_retained(target_value, runtime::Value(std::move(fields)), DirtyReason::input);
    static_cast<void>(scope.request_frame(WidgetFrameCost::paint));
}

/** The view being eased to, when one is: the end a further wheel notch builds on. */
[[nodiscard]] std::optional<ZoomViewport> eased_target(const WidgetInputScope& scope) noexcept {
    return view_from(scope.retained(target_value));
}

void zoom_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("kind", runtime::Value("CANVAS"));
    scope.set("clip", runtime::Value(true));
    scope.set("width", widget_fill());
    scope.set("height", widget_fill());
}

/**
 * A press on the canvas itself, or on content that does not take the pointer (a card's body, not
 * its buttons), pans once it moves past the slop; a middle-button press pans from anywhere.
 * `panButton: "secondary"` pans with the secondary button from anywhere instead, and leaves a
 * primary press on the canvas to the content (a marquee, say); `"middle"` pans with the middle
 * button alone.
 */
bool zoom_view_pointer(WidgetInputScope& scope) {
    const PointerInputEvent* pointer = scope.pointer();
    if (pointer == nullptr)
        return false;
    const runtime::Value* pan = scope.retained(pan_value);
    const bool panning = pan != nullptr && pan->object() != nullptr &&
                         finite(pan->field("pointerId"), -1.0) == pointer->pointer_id;
    if (pointer->type == PointerEventType::press) {
        const runtime::Value* button_value = scope.property("panButton");
        const std::string_view pan_button =
            button_value != nullptr && button_value->string() != nullptr
                ? std::string_view(*button_value->string())
                : std::string_view("primary");
        // A press that pans from anywhere: the middle button, or the secondary when asked.
        const bool middle =
            pointer->button == 2 || (pan_button == "secondary" && pointer->button == 1);
        const bool from_canvas =
            pan_button == "primary" && pointer->button == 0 && scope.passive_pointer_target();
        if (panning || !(middle || from_canvas))
            return false;
        const ZoomViewport view = zoom_view_current(scope.node());
        scope.set_retained(
            pan_value,
            runtime::Value(Fields{
                {"pointerId", runtime::Value(static_cast<double>(pointer->pointer_id))},
                {"startX", runtime::Value(pointer->position.x)},
                {"startY", runtime::Value(pointer->position.y)},
                {"viewX", runtime::Value(view.x)},
                {"viewY", runtime::Value(view.y)},
                {"middle", runtime::Value(middle)},
                {"panned", runtime::Value(false)},
            }),
            DirtyReason::input);
        // A press catches an eased zoom where it is.
        scope.set_retained(target_value, runtime::Value{}, DirtyReason::input);
        return middle;
    }
    if (!panning)
        return false;
    const bool middle = pan->field("middle") != nullptr &&
                        pan->field("middle")->boolean() != nullptr &&
                        *pan->field("middle")->boolean();
    const bool panned = pan->field("panned") != nullptr &&
                        pan->field("panned")->boolean() != nullptr &&
                        *pan->field("panned")->boolean();
    if (pointer->type == PointerEventType::move) {
        if (!panned && !middle) {
            if (!scope.press_moved_beyond_slop())
                return false;
            if (!scope.claim_gesture()) {
                scope.set_retained(pan_value, runtime::Value{}, DirtyReason::input);
                return false;
            }
        }
        // Read before the session is replaced, which releases what `pan` points to.
        const ZoomViewport next{
            finite(pan->field("viewX"), 0.0) + pointer->position.x -
                finite(pan->field("startX"), pointer->position.x),
            finite(pan->field("viewY"), 0.0) + pointer->position.y -
                finite(pan->field("startY"), pointer->position.y),
            zoom_view_current(scope.node()).zoom,
        };
        if (!panned) {
            Fields fields = pan->object()->fields;
            for (auto& [name, value] : fields) {
                if (name == "panned")
                    value = runtime::Value(true);
            }
            scope.set_retained(pan_value, runtime::Value(std::move(fields)), DirtyReason::input);
        }
        show(scope, next);
        return true;
    }
    if (pointer->type == PointerEventType::release || pointer->type == PointerEventType::cancel) {
        scope.set_retained(pan_value, runtime::Value{}, DirtyReason::input);
        return panned || middle;
    }
    return false;
}

/**
 * The wheel zooms about the pointer, easing; with Shift it pans instead, and a sideways wheel or
 * trackpad swipe pans across. A scroll container inside the canvas under the pointer scrolls first.
 */
bool zoom_view_event(WidgetInputScope& scope) {
    if (scope.kind() != InputEventKind::scroll || scope.scroll() == nullptr ||
        scope.phase() == InputEventPhase::capture) {
        return false;
    }
    for (const RetainedNode* current = scope.pointer_target();
         current != nullptr && current != &scope.node(); current = current->parent()) {
        const LayoutRecord* record = scope.layout(*current);
        if (record != nullptr && record->kind == LayoutKind::scroll)
            return false;
    }
    const ScrollInputEvent& wheel = *scope.scroll();
    const ZoomViewport current = zoom_view_current(scope.node());
    const std::optional<ZoomViewport> eased = eased_target(scope);
    const ZoomViewport base = eased.value_or(current);
    if (wheel.modifiers.shift || (wheel.delta_y == 0.0 && wheel.delta_x != 0.0)) {
        const double across = wheel.modifiers.shift ? wheel.delta_y + wheel.delta_x : wheel.delta_x;
        const double down = wheel.modifiers.shift ? 0.0 : wheel.delta_y;
        scope.set_retained(target_value, runtime::Value{}, DirtyReason::input);
        show(scope, ZoomViewport{base.x + across * wheel_pan_step, base.y + down * wheel_pan_step,
                                 base.zoom});
        scope.consume();
        return true;
    }
    const Point origin = content_origin(scope);
    const Point anchor{wheel.position.x - origin.x, wheel.position.y - origin.y};
    const double zoom = limits(scope).clamp(base.zoom * std::pow(wheel_zoom_step, wheel.delta_y));
    ease_to(scope, zoom_view_about(current, anchor, zoom), anchor);
    scope.consume();
    return true;
}

void zoom_view_frame(WidgetInputScope& scope, const WidgetFrameInfo& frame) {
    const runtime::Value* target_state = scope.retained(target_value);
    const std::optional<ZoomViewport> target = view_from(target_state);
    if (!target.has_value()) {
        // The frame after the view came to rest: drawn again at a zoom that held since the last
        // frame, the content is rasterized at exactly that zoom rather than the nearest step a
        // moving zoom takes (the frame hook marks the node to be drawn).
        if (scope.retained(settling_value) != nullptr)
            scope.set_retained(settling_value, runtime::Value{}, DirtyReason::input);
        return;
    }
    const ZoomViewport current = zoom_view_current(scope.node());
    const runtime::Value* anchor_x = target_state->field("anchorX");
    const std::optional<Point> anchor =
        anchor_x != nullptr
            ? std::optional<Point>(
                  Point{finite(anchor_x, 0.0), finite(target_state->field("anchorY"), 0.0)})
            : std::nullopt;
    const std::optional<Point> focus =
        anchor.has_value() ? std::optional<Point>(Point{finite(target_state->field("focusX"), 0.0),
                                                        finite(target_state->field("focusY"), 0.0)})
                           : std::nullopt;
    const double seconds =
        frame.reduced_motion
            ? 1.0e9
            : std::clamp(static_cast<double>(frame.delta_nanos) * 1.0e-9, 0.0, 0.1);
    const ZoomViewport next =
        zoom_view_step(current, *target, seconds, anchor.has_value() ? &*anchor : nullptr,
                       focus.has_value() ? &*focus : nullptr);
    const bool arrived = std::abs(std::log(next.zoom / target->zoom)) < 1.0e-3 &&
                         std::abs(next.x - target->x) < 0.25 && std::abs(next.y - target->y) < 0.25;
    if (arrived) {
        scope.set_retained(target_value, runtime::Value{}, DirtyReason::input);
        show(scope, *target);
        scope.set_retained(settling_value, runtime::Value(true), DirtyReason::input);
        static_cast<void>(scope.request_frame(WidgetFrameCost::paint));
        return;
    }
    show(scope, next);
    static_cast<void>(scope.request_frame(WidgetFrameCost::paint));
}

/** Shows all of the content, `padding` clear of the edges, easing there. */
bool fit_content(WidgetInputScope& scope, const double padding) {
    const RetainedNode* child = content(scope.node());
    const LayoutRecord* child_record = child != nullptr ? scope.layout(*child) : nullptr;
    const LayoutRecord* own = scope.layout();
    if (child_record == nullptr || own == nullptr)
        return false;
    // The viewport as seen from the content's origin, which may sit inset in the view.
    const Point origin = content_origin(scope);
    const Rect viewport = own->content_bounds;
    const ZoomViewport fitted =
        zoom_view_fit(Size{viewport.width, viewport.height},
                      Rect{0.0, 0.0, child_record->bounds.width, child_record->bounds.height},
                      padding, limits(scope));
    ease_to(scope, ZoomViewport{fitted.x + viewport.x - origin.x, fitted.y + viewport.y - origin.y,
                                fitted.zoom});
    return true;
}

/**
 * With the view focused, `+` or `=` zooms in and `-` out (by a wheel notch, about the middle of
 * the view, with or without Ctrl), `0` goes back to the content's own size, Shift+1 fits all of it,
 * and the arrows pan (five times as far with Shift). Each builds on a zoom that is still easing.
 * The zoom keys reach the view from a focused control on its canvas too.
 */
bool zoom_view_key(WidgetInputScope& scope) {
    if (scope.modifiers().alt || scope.modifiers().super_key)
        return false;
    const std::string_view key = scope.key();
    const ZoomViewport current = zoom_view_current(scope.node());
    const ZoomViewport base = eased_target(scope).value_or(current);
    const bool shift = scope.modifiers().shift;
    if (key == "0" && !shift) {
        ease_to(scope, ZoomViewport{});
        return true;
    }
    if (key == "1" && shift && !scope.modifiers().control)
        return fit_content(scope, 24.0);
    double factor = 0.0;
    if (key == "+" || key == "=")
        factor = wheel_zoom_step;
    else if (key == "-")
        factor = 1.0 / wheel_zoom_step;
    if (factor > 0.0) {
        const LayoutRecord* own = scope.layout();
        if (own == nullptr)
            return false;
        const Point origin = content_origin(scope);
        const Point anchor{own->content_bounds.x + own->content_bounds.width * 0.5 - origin.x,
                           own->content_bounds.y + own->content_bounds.height * 0.5 - origin.y};
        const double zoom = limits(scope).clamp(base.zoom * factor);
        ease_to(scope, zoom_view_about(base, anchor, zoom));
        return true;
    }
    // The arrows pan only when the view itself has focus: a control on the canvas keeps them.
    if (scope.modifiers().control || scope.phase() != InputEventPhase::target)
        return false;
    const double step = key_pan_step * (shift ? 5.0 : 1.0);
    Point by{};
    if (key == "left")
        by.x = step;
    else if (key == "right")
        by.x = -step;
    else if (key == "up")
        by.y = step;
    else if (key == "down")
        by.y = -step;
    else
        return false;
    ease_to(scope, ZoomViewport{base.x + by.x, base.y + by.y, base.zoom});
    return true;
}

/** A `view` the app set (or restored) that the canvas has not shown yet is eased to. */
bool zoom_view_synchronize(WidgetInputScope& scope) {
    const std::optional<ZoomViewport> controlled = view_from(scope.property("view"));
    if (!controlled.has_value())
        return false;
    const ZoomViewport wanted{controlled->x, controlled->y, limits(scope).clamp(controlled->zoom)};
    const std::optional<ZoomViewport> reported = view_from(scope.retained(reported_value));
    if (reported.has_value() && *reported == *controlled)
        return false;
    if (scope.retained(content_view_value) == nullptr) {
        // The first view is shown as it is, not eased to from the identity.
        show(scope, wanted);
        return true;
    }
    scope.set_retained(reported_value, view_value(*controlled), DirtyReason::input);
    ease_to(scope, wanted);
    return true;
}

/**
 * `zoom.fit {key, padding}` eases to show all of the content (padding 24 by default); `zoom.reset`
 * eases back to the content at its own size, where it is laid out.
 */
bool zoom_view_action(WidgetInputScope& scope, const std::string_view operation,
                      const runtime::Value& payload) {
    if (operation == "reset") {
        ease_to(scope, ZoomViewport{});
        return true;
    }
    if (operation != "fit")
        return false;
    return fit_content(scope, std::max(0.0, finite(payload.field("padding"), 24.0)));
}

} // namespace

double ZoomLimits::clamp(const double zoom) const noexcept {
    return std::clamp(std::isfinite(zoom) ? zoom : 1.0, minimum, std::max(minimum, maximum));
}

ZoomViewport zoom_view_about(const ZoomViewport& view, const Point anchor,
                             const double zoom) noexcept {
    const Point focus{(anchor.x - view.x) / view.zoom, (anchor.y - view.y) / view.zoom};
    return ZoomViewport{anchor.x - focus.x * zoom, anchor.y - focus.y * zoom, zoom};
}

ZoomViewport zoom_view_fit(const Size size, const Rect content, const double padding,
                           const ZoomLimits& limits) noexcept {
    if (!(content.width > 0.0) || !(content.height > 0.0))
        return ZoomViewport{};
    const double room_x = std::max(1.0, size.width - padding * 2.0);
    const double room_y = std::max(1.0, size.height - padding * 2.0);
    const double zoom = limits.clamp(std::min(room_x / content.width, room_y / content.height));
    return ZoomViewport{
        (size.width - content.width * zoom) * 0.5 - content.x * zoom,
        (size.height - content.height * zoom) * 0.5 - content.y * zoom,
        zoom,
    };
}

ZoomViewport zoom_view_step(const ZoomViewport& current, const ZoomViewport& target,
                            const double seconds, const Point* anchor,
                            const Point* focus) noexcept {
    const double progress = 1.0 - std::exp(-ease_rate * std::max(0.0, seconds));
    const double zoom = std::exp(std::log(current.zoom) +
                                 (std::log(target.zoom) - std::log(current.zoom)) * progress);
    if (anchor != nullptr && focus != nullptr)
        return ZoomViewport{anchor->x - focus->x * zoom, anchor->y - focus->y * zoom, zoom};
    return ZoomViewport{current.x + (target.x - current.x) * progress,
                        current.y + (target.y - current.y) * progress, zoom};
}

ZoomViewport zoom_view_current(const RetainedNode& node) noexcept {
    return view_from(node.retained_value(content_view_value)).value_or(ZoomViewport{});
}

void register_zoom_view_widget(WidgetRegistry& registry) {
    WidgetLifecycle lifecycle;
    lifecycle.type = "ZoomView";
    lifecycle.describe = WidgetDescribePhase{&zoom_defaults, nullptr, {}, "ZoomView", true};
    lifecycle.input.pointer = &zoom_view_pointer;
    lifecycle.input.event = &zoom_view_event;
    lifecycle.input.key = &zoom_view_key;
    lifecycle.input.focusable = true;
    lifecycle.input.action_property = "onViewChange";
    lifecycle.input.framework_actions = "zoom";
    lifecycle.input.framework_action = &zoom_view_action;
    lifecycle.input.synchronize = &zoom_view_synchronize;
    lifecycle.frame.advance = &zoom_view_frame;
    lifecycle.semantics = WidgetSemanticsPhase{"group", {}, nullptr, false, false};
    registry.register_lifecycle(std::move(lifecycle));
}

} // namespace strata::ui
