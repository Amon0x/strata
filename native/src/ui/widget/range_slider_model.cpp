#include "ui/widget/range_slider_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "ui/input.hpp"
#include "ui/text.hpp"
#include "ui/tree.hpp"
#include "ui/widget/description.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/presentation.hpp"
#include "ui/widget/registry.hpp"
#include "ui/widget/semantics.hpp"
#include "ui/widget/subtarget.hpp"

namespace strata::ui {
namespace {

constexpr double thumb_size = 16.0;
constexpr double track_thickness = 4.0;

[[nodiscard]] const runtime::Value* property(const RetainedNode& node,
                                             const std::string_view name) noexcept {
    const auto found = node.description().properties.find(name);
    return found != node.description().properties.end() ? found->second.value() : nullptr;
}

[[nodiscard]] std::optional<double> finite(const runtime::Value* value) noexcept {
    return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
               ? std::optional<double>(*value->number())
               : std::nullopt;
}

/** A controlled bound if the property is present, else what the user set, else the default. */
[[nodiscard]] double bound(const RetainedNode& node, const std::string_view controlled,
                           const std::string_view retained, const std::string_view initial,
                           const double fallback) noexcept {
    if (property(node, controlled) != nullptr)
        return finite(property(node, controlled)).value_or(fallback);
    if (const std::optional<double> value = finite(node.retained_value(retained)); value.has_value())
        return *value;
    return finite(property(node, initial)).value_or(fallback);
}

[[nodiscard]] std::string number_text(const double value) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
    std::string text = buffer;
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text == "-0" ? "0" : text;
}

[[nodiscard]] Rect thumb_rect(const Rect track, const double fraction) noexcept {
    const double x = track.x + track.width * fraction;
    const double y = track.y + track.height * 0.5;
    return Rect{x - thumb_size * 0.5, y - thumb_size * 0.5, thumb_size, thumb_size};
}

void range_slider_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", runtime::Value(28.0));
    scope.set("width", widget_fill());
    scope.intrinsic(200.0, 28.0);
}

void range_slider_content(WidgetRenderScope& scope) {
    const RangeSliderState state = range_slider_state(scope.node());
    const Rect track = range_slider_track(scope.layout().bounds);
    const double radius = track.height * 0.5;
    scope.rounded_rect(track, scope.visual().track, std::nullopt, radius);
    const double from = track.x + track.width * state.fraction(state.start);
    const double to = track.x + track.width * state.fraction(state.end);
    if (to > from)
        scope.rounded_rect(Rect{from, track.y, to - from, track.height}, scope.visual().fill,
                           std::nullopt, radius);
    const bool show_values = scope.boolean("showValue", false);
    const std::string suffix = scope.string("valueSuffix");
    for (std::size_t index = 0U; index < 2U; ++index) {
        const double value = index == 0U ? state.start : state.end;
        const Rect thumb = thumb_rect(track, state.fraction(value));
        const std::string id = index == 0U ? "thumb:start" : "thumb:end";
        const bool hot = scope.input().subtarget_hovered(scope.node().identity(), id) ||
                         (scope.active() && state.thumb == index);
        if (hot) {
            RenderColor halo = scope.visual().fill.representative();
            halo.alpha = static_cast<std::uint8_t>(halo.alpha / 4U);
            scope.rounded_rect(Rect{thumb.x - 4.0, thumb.y - 4.0, thumb.width + 8.0,
                                    thumb.height + 8.0},
                               Paint(halo), std::nullopt, thumb.width * 0.5 + 4.0);
        }
        scope.rounded_rect(thumb, scope.visual().thumb,
                           RenderBorder{2.0, scope.visual().fill.representative(), true},
                           thumb.width * 0.5);
        if (scope.focus_visible() && state.thumb == index)
            scope.focus(thumb);
        if (show_values && (hot || scope.focus_visible()) && scope.text_engine() != nullptr) {
            const std::string label = number_text(value) + suffix;
            const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), label);
            const double width = std::max(20.0, shaped.metrics.width + 12.0);
            const Rect bubble{thumb.x + thumb.width * 0.5 - width * 0.5, thumb.y - 24.0, width,
                              18.0};
            RenderColor edge = scope.visual().foreground;
            edge.alpha = 30U;
            scope.rounded_rect(bubble, Paint(scope.palette("popupSurface")),
                               RenderBorder{1.0, edge, true}, 4.0);
            scope.text(label, Point{bubble.x, bubble.y + (18.0 - shaped.metrics.height) * 0.5},
                       scope.visual().foreground, width, WidgetTextAlignment::center);
        }
    }
}

/** Moves one thumb, keeping the other thumb `gap` away, and reports the new pair. */
void range_slider_set(WidgetInputScope& scope, const RangeSliderState& state,
                      const std::size_t thumb, const double requested) {
    double start = state.start;
    double end = state.end;
    if (thumb == 0U)
        start = std::min(state.snapped(requested), end - state.gap);
    else
        end = std::max(state.snapped(requested), start + state.gap);
    start = std::clamp(start, state.minimum, state.maximum);
    end = std::clamp(end, state.minimum, state.maximum);
    if (start == state.start && end == state.end)
        return;
    if (property(scope.node(), "start") == nullptr)
        scope.set_retained("$start", runtime::Value(start), DirtyReason::properties);
    if (property(scope.node(), "end") == nullptr)
        scope.set_retained("$end", runtime::Value(end), DirtyReason::properties);
    scope.value_changed("onChange", "range-changed",
                        runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                            {"end", runtime::Value(end)}, {"start", runtime::Value(start)}}));
}

[[nodiscard]] double value_at(const RangeSliderState& state, const Rect track,
                              const double x) noexcept {
    const double fraction = track.width > 0.0 ? std::clamp((x - track.x) / track.width, 0.0, 1.0)
                                              : 0.0;
    return state.minimum + (state.maximum - state.minimum) * fraction;
}

/** A press takes the nearer thumb (the one on the pressed side when they meet) and drags it. */
bool range_slider_pointer(WidgetInputScope& scope) {
    const PointerInputEvent* pointer = scope.pointer();
    const LayoutRecord* layout = scope.layout();
    if (pointer == nullptr || pointer->button != 0 || layout == nullptr)
        return false;
    const RangeSliderState state = range_slider_state(scope.node());
    const Rect track = range_slider_track(layout->bounds);
    const double value = value_at(state, track, pointer->position.x);
    if (pointer->type == PointerEventType::press) {
        const double to_start = std::abs(value - state.start);
        const double to_end = std::abs(value - state.end);
        const std::size_t thumb =
            to_start < to_end ? 0U : to_end < to_start ? 1U : value < state.start ? 0U : 1U;
        scope.set_retained("$thumb", runtime::Value(static_cast<double>(thumb)),
                           DirtyReason::input);
        range_slider_set(scope, state, thumb, value);
        // Left unconsumed, the press still focuses the slider the ordinary way, so the keyboard
        // moves the thumb just taken.
        return false;
    }
    const InputDispatchContext* dispatch = scope.dispatch();
    if (dispatch == nullptr || !dispatch->press_origin().has_value())
        return false;
    if (pointer->type == PointerEventType::move) {
        range_slider_set(scope, state, state.thumb, value);
        return true;
    }
    return pointer->type == PointerEventType::release ||
           pointer->type == PointerEventType::cancel;
}

/** Tab moves from the start thumb to the end thumb (Shift+Tab back) before focus leaves. */
bool range_slider_event(WidgetInputScope& scope) {
    if (scope.kind() != InputEventKind::key || scope.phase() != InputEventPhase::target ||
        scope.key() != "tab" || scope.modifiers().alt || scope.modifiers().control ||
        scope.modifiers().super_key)
        return false;
    const RangeSliderState state = range_slider_state(scope.node());
    const std::size_t next = scope.modifiers().shift ? 0U : 1U;
    if (next == state.thumb)
        return false;
    scope.set_retained("$thumb", runtime::Value(static_cast<double>(next)), DirtyReason::input);
    scope.consume();
    return true;
}

/** Arrows step the active thumb, Page Up and Down by ten steps, Home and End to its limits. */
bool range_slider_key(WidgetInputScope& scope) {
    const RangeSliderState state = range_slider_state(scope.node());
    const std::string_view key = scope.key();
    const double step =
        state.step > 0.0 ? state.step : (state.maximum - state.minimum) / 100.0;
    const double current = state.thumb == 0U ? state.start : state.end;
    double next = current;
    if (key == "right" || key == "up") next = current + step;
    else if (key == "left" || key == "down") next = current - step;
    else if (key == "pageup") next = current + step * 10.0;
    else if (key == "pagedown") next = current - step * 10.0;
    else if (key == "home") next = state.minimum;
    else if (key == "end") next = state.maximum;
    else return false;
    scope.set_event_count(1U);
    range_slider_set(scope, state, state.thumb, next);
    return true;
}

void range_slider_semantics(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    const RangeSliderState state = range_slider_state(scope.node());
    std::string suffix;
    if (const runtime::Value* value = scope.property("valueSuffix");
        value != nullptr && value->string() != nullptr)
        suffix = *value->string();
    scope.default_value_text(number_text(state.start) + suffix + " to " + number_text(state.end) +
                             suffix);
}

std::vector<WidgetSubtarget> range_slider_subtargets(const WidgetSubtargetContext& context) {
    std::vector<WidgetSubtarget> result;
    if (context.record.bounds.empty())
        return result;
    const RangeSliderState state = range_slider_state(context.node);
    const Rect track = range_slider_track(context.record.bounds);
    const runtime::Value* flag = property(context.node, "enabled");
    const bool enabled = flag == nullptr || flag->boolean() == nullptr || *flag->boolean();
    for (std::size_t index = 0U; index < 2U; ++index) {
        const double value = index == 0U ? state.start : state.end;
        result.push_back(WidgetSubtarget{
            context.node.identity(), index == 0U ? "thumb:start" : "thumb:end",
            WidgetSubtargetKind::control, index, thumb_rect(track, state.fraction(value)),
            runtime::Value(value), index == 0U ? "Start" : "End", {}, enabled, false,
            context.record.z_index});
    }
    return result;
}

} // namespace

double RangeSliderState::fraction(const double value) const noexcept {
    return maximum > minimum ? std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0) : 0.0;
}

double RangeSliderState::snapped(const double value) const noexcept {
    const double stepped =
        step > 0.0 ? minimum + std::round((value - minimum) / step) * step : value;
    return std::clamp(stepped, minimum, maximum);
}

RangeSliderState range_slider_state(const RetainedNode& node) {
    RangeSliderState state;
    state.minimum = finite(property(node, "min")).value_or(0.0);
    state.maximum = std::max(state.minimum, finite(property(node, "max")).value_or(1.0));
    state.step = std::max(0.0, finite(property(node, "step")).value_or(0.0));
    state.gap = std::clamp(finite(property(node, "minGap")).value_or(0.0), 0.0,
                           state.maximum - state.minimum);
    state.start = std::clamp(bound(node, "start", "$start", "defaultStart", state.minimum),
                             state.minimum, state.maximum);
    state.end = std::clamp(bound(node, "end", "$end", "defaultEnd", state.maximum), state.start,
                           state.maximum);
    if (const std::optional<double> thumb = finite(node.retained_value("$thumb"));
        thumb.has_value() && *thumb >= 0.5)
        state.thumb = 1U;
    return state;
}

Rect range_slider_track(const Rect bounds) noexcept {
    const double inset = thumb_size * 0.5 + 2.0;
    return Rect{bounds.x + inset, bounds.y + (bounds.height - track_thickness) * 0.5,
                std::max(0.0, bounds.width - inset * 2.0), track_thickness};
}

void register_range_slider_widget(WidgetRegistry& registry) {
    WidgetDescribePhase describe{&range_slider_defaults, nullptr, {}, "RangeSlider", true};
    registry.register_describe_phase("RangeSlider", std::move(describe));
    WidgetInputPhase input;
    input.action_property = "onChange";
    input.fallback_action = "RangeSlider";
    input.focusable = true;
    input.event = &range_slider_event;
    input.pointer = &range_slider_pointer;
    input.key = &range_slider_key;
    registry.register_input_phase("RangeSlider", std::move(input));
    registry.register_semantics_phase(
        "RangeSlider", WidgetSemanticsPhase{"slider", {}, &range_slider_semantics, false, false});
    registry.register_subtargets("RangeSlider", &range_slider_subtargets);
    WidgetPresentPhase present{&range_slider_content, nullptr, nullptr, nullptr, false};
    present.visual = WidgetVisualProfile{true, false, false};
    registry.register_present_phase("RangeSlider", std::move(present));
}

} // namespace strata::ui
