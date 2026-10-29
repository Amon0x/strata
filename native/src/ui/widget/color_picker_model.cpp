#include "ui/widget/color_picker_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>

#include "ui/input.hpp"
#include "ui/path.hpp"
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

constexpr double inset = 8.0;
constexpr double strip = 12.0;
constexpr double gap = 10.0;
constexpr double swatch_size = 28.0;
constexpr double minimum_swatch = 18.0;
constexpr double swatch_gap = 6.0;
constexpr double preview_size = 28.0;

[[nodiscard]] const runtime::Value* property(const RetainedNode& node,
                                             const std::string_view name) noexcept {
    const auto found = node.description().properties.find(name);
    return found != node.description().properties.end() ? found->second.value() : nullptr;
}

[[nodiscard]] std::uint8_t channel(const double unit) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(unit, 0.0, 1.0) * 255.0));
}

[[nodiscard]] bool enabled(const RetainedNode& node) noexcept {
    const runtime::Value* value = property(node, "enabled");
    return value == nullptr || value->boolean() == nullptr || *value->boolean();
}

void color_picker_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", runtime::Value(260.0));
    scope.set("width", runtime::Value(240.0));
    scope.intrinsic(240.0, 260.0);
}

[[nodiscard]] Paint linear(const Point start, const Point end,
                           std::vector<GradientStop> stops) {
    Gradient gradient;
    gradient.start = start;
    gradient.end = end;
    gradient.stops = std::move(stops);
    return Paint(std::move(gradient));
}

/** A small grey checkerboard, under anything translucent. */
void checker(WidgetRenderScope& scope, const Rect bounds, const double radius) {
    scope.rounded_rect(bounds, Paint(RenderColor{200U, 200U, 200U, 255U}), std::nullopt, radius);
    constexpr double cell = 6.0;
    scope.push_clip(bounds, CornerRadii::all(radius));
    for (double y = bounds.y; y < bounds.bottom(); y += cell) {
        const bool odd_row = static_cast<int>((y - bounds.y) / cell) % 2 == 1;
        for (double x = bounds.x + (odd_row ? cell : 0.0); x < bounds.right(); x += cell * 2.0)
            scope.solid_rect(Rect{x, y, std::min(cell, bounds.right() - x),
                                  std::min(cell, bounds.bottom() - y)},
                             Paint(RenderColor{150U, 150U, 150U, 255U}));
    }
    scope.pop_clip();
}

/** A round handle with a white ring and a dark outline, legible on any colour. */
void handle(WidgetRenderScope& scope, const Point center, const double radius,
            const RenderColor fill) {
    const Rect outer{center.x - radius - 1.0, center.y - radius - 1.0, (radius + 1.0) * 2.0,
                     (radius + 1.0) * 2.0};
    scope.rounded_rect(outer, Paint(RenderColor{0U, 0U, 0U, 110U}), std::nullopt,
                       outer.width * 0.5);
    const Rect inner{center.x - radius, center.y - radius, radius * 2.0, radius * 2.0};
    scope.rounded_rect(inner, Paint(fill), RenderBorder{2.5, RenderColor{255U, 255U, 255U, 255U}, true},
                       radius);
}

void color_picker_content(WidgetRenderScope& scope) {
    const ColorPickerState state = color_picker_state(scope.node());
    const ColorPickerProjection projection = color_picker_projection(state, scope.layout().bounds);
    if (scope.visual().background.has_value())
        scope.rounded_rect(scope.layout().bounds, *scope.visual().background,
                           scope.visual().border);
    const ColorPickerHsv pure{state.hsv.hue, 1.0, 1.0, 1.0};
    const RenderColor opaque{state.color.red, state.color.green, state.color.blue, 255U};

    // Saturation runs left to right from white, value top to bottom into black.
    const Rect field = projection.field;
    scope.rounded_rect(field, Paint(pure.rgb()), std::nullopt, 6.0);
    scope.rounded_rect(field,
                       linear(Point{0.0, 0.5}, Point{1.0, 0.5},
                              {{0.0, {255U, 255U, 255U, 255U}}, {1.0, {255U, 255U, 255U, 0U}}}),
                       std::nullopt, 6.0);
    scope.rounded_rect(field,
                       linear(Point{0.5, 0.0}, Point{0.5, 1.0},
                              {{0.0, {0U, 0U, 0U, 0U}}, {1.0, {0U, 0U, 0U, 255U}}}),
                       std::nullopt, 6.0);
    handle(scope,
           Point{field.x + field.width * state.hsv.saturation,
                 field.y + field.height * (1.0 - state.hsv.value)},
           7.0, opaque);

    std::vector<GradientStop> hues;
    for (int step = 0; step <= 6; ++step)
        hues.push_back(GradientStop{step / 6.0, ColorPickerHsv{step * 60.0, 1.0, 1.0, 1.0}.rgb()});
    scope.rounded_rect(projection.hue, linear(Point{0.0, 0.5}, Point{1.0, 0.5}, std::move(hues)),
                       std::nullopt, strip * 0.5);
    handle(scope,
           Point{projection.hue.x + projection.hue.width * state.hsv.hue / 360.0,
                 projection.hue.y + strip * 0.5},
           strip * 0.5 + 1.0, pure.rgb());

    if (projection.alpha.has_value()) {
        const Rect alpha = *projection.alpha;
        checker(scope, alpha, strip * 0.5);
        RenderColor clear = opaque;
        clear.alpha = 0U;
        scope.rounded_rect(alpha, linear(Point{0.0, 0.5}, Point{1.0, 0.5}, {{0.0, clear}, {1.0, opaque}}),
                           std::nullopt, strip * 0.5);
        handle(scope,
               Point{alpha.x + alpha.width * state.hsv.alpha, alpha.y + strip * 0.5},
               strip * 0.5 + 1.0, state.color);
    }

    checker(scope, projection.preview, 6.0);
    scope.rounded_rect(projection.preview, Paint(state.color),
                       RenderBorder{1.0, RenderColor{128U, 128U, 128U, 90U}, true}, 6.0);
    if (scope.text_engine() != nullptr) {
        const std::string hex = state.hex();
        const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), hex);
        scope.text(hex,
                   Point{projection.label.x,
                         projection.label.y + (projection.label.height - shaped.metrics.height) * 0.5},
                   scope.visual().foreground);
    }
    for (std::size_t index = 0U; index < projection.swatches.size(); ++index) {
        const Rect swatch = projection.swatches[index];
        const RenderColor color = state.swatches[index];
        if (color.alpha < 255U)
            checker(scope, swatch, 5.0);
        const bool chosen = color == state.color;
        scope.rounded_rect(swatch, Paint(color),
                           RenderBorder{chosen ? 2.0 : 1.0,
                                        chosen ? scope.visual().foreground
                                               : RenderColor{128U, 128U, 128U, 90U},
                                        true},
                           5.0);
        scope.interaction(swatch, "swatch:" + std::to_string(index));
    }
    scope.focus(field);
}

/** Which part a point is in: the field, a strip, or a swatch ("swatch:<index>"). */
[[nodiscard]] std::string region_at(const ColorPickerProjection& projection, const Point point) {
    const auto inside = [point](const Rect rect, const double slop) {
        return point.x >= rect.x - slop && point.x <= rect.right() + slop &&
               point.y >= rect.y - slop && point.y <= rect.bottom() + slop;
    };
    if (inside(projection.field, 0.0)) return "field";
    if (inside(projection.hue, 4.0)) return "hue";
    if (projection.alpha.has_value() && inside(*projection.alpha, 4.0)) return "alpha";
    for (std::size_t index = 0U; index < projection.swatches.size(); ++index) {
        if (inside(projection.swatches[index], 0.0))
            return "swatch:" + std::to_string(index);
    }
    return {};
}

void color_picker_set(WidgetInputScope& scope, const ColorPickerState& state,
                      const ColorPickerHsv& next) {
    // The hue is kept even where a colour cannot show it (greys), so it does not jump to red.
    scope.set_retained("$hue", runtime::Value(next.hue), DirtyReason::properties);
    const RenderColor color = next.rgb();
    if (color == state.color)
        return;
    if (property(scope.node(), "value") == nullptr)
        scope.set_retained("$value", runtime::Value(color), DirtyReason::properties);
    scope.value_changed("onChange", "color-changed", runtime::Value(color));
}

void color_picker_drag(WidgetInputScope& scope, const ColorPickerState& state,
                       const ColorPickerProjection& projection, const std::string_view region,
                       const Point point) {
    const auto along = [point](const Rect rect) {
        return rect.width > 0.0 ? std::clamp((point.x - rect.x) / rect.width, 0.0, 1.0) : 0.0;
    };
    ColorPickerHsv next = state.hsv;
    if (region == "field") {
        next.saturation = along(projection.field);
        next.value = projection.field.height > 0.0
                         ? 1.0 - std::clamp((point.y - projection.field.y) / projection.field.height,
                                            0.0, 1.0)
                         : 0.0;
    } else if (region == "hue") {
        next.hue = std::min(359.999, along(projection.hue) * 360.0);
    } else if (region == "alpha" && projection.alpha.has_value()) {
        next.alpha = along(*projection.alpha);
    } else {
        return;
    }
    color_picker_set(scope, state, next);
}

/** A press picks in the part it lands on, and dragging keeps adjusting that part. */
bool color_picker_pointer(WidgetInputScope& scope) {
    const PointerInputEvent* pointer = scope.pointer();
    const LayoutRecord* layout = scope.layout();
    if (pointer == nullptr || pointer->button != 0 || layout == nullptr || !enabled(scope.node()))
        return false;
    const ColorPickerState state = color_picker_state(scope.node());
    const ColorPickerProjection projection = color_picker_projection(state, layout->bounds);
    if (pointer->type == PointerEventType::press) {
        const std::string region = region_at(projection, pointer->position);
        scope.set_retained("$drag", runtime::Value(region), DirtyReason::input);
        if (region.starts_with("swatch:")) {
            const std::size_t index = static_cast<std::size_t>(std::stoul(region.substr(7U)));
            color_picker_set(scope, state, ColorPickerHsv::from(state.swatches[index], state.hsv.hue));
        } else {
            color_picker_drag(scope, state, projection, region, pointer->position);
        }
        // Left unconsumed, the press still focuses the picker for its keys.
        return false;
    }
    const InputDispatchContext* dispatch = scope.dispatch();
    if (dispatch == nullptr || !dispatch->press_origin().has_value())
        return false;
    const runtime::Value* region = scope.retained("$drag");
    if (pointer->type == PointerEventType::move && region != nullptr && region->string() != nullptr) {
        color_picker_drag(scope, state, projection, *region->string(), pointer->position);
        return true;
    }
    return pointer->type == PointerEventType::release ||
           pointer->type == PointerEventType::cancel;
}

/**
 * Left and Right change saturation, Up and Down value, by 1% (10% with Shift); Page Up and Page
 * Down turn the hue by 10 degrees.
 */
bool color_picker_key(WidgetInputScope& scope) {
    if (!enabled(scope.node()))
        return false;
    const ColorPickerState state = color_picker_state(scope.node());
    const std::string_view key = scope.key();
    const double step = scope.modifiers().shift ? 0.1 : 0.01;
    ColorPickerHsv next = state.hsv;
    if (key == "left") next.saturation = std::max(0.0, next.saturation - step);
    else if (key == "right") next.saturation = std::min(1.0, next.saturation + step);
    else if (key == "down") next.value = std::max(0.0, next.value - step);
    else if (key == "up") next.value = std::min(1.0, next.value + step);
    else if (key == "pageup") next.hue = std::fmod(next.hue + 10.0, 360.0);
    else if (key == "pagedown") next.hue = std::fmod(next.hue + 350.0, 360.0);
    else return false;
    scope.set_event_count(1U);
    color_picker_set(scope, state, next);
    return true;
}

void color_picker_semantics(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    scope.default_value_text(color_picker_state(scope.node()).hex());
}

std::vector<WidgetSubtarget> color_picker_subtargets(const WidgetSubtargetContext& context) {
    std::vector<WidgetSubtarget> result;
    if (context.record.bounds.empty())
        return result;
    const ColorPickerState state = color_picker_state(context.node);
    const ColorPickerProjection projection = color_picker_projection(state, context.record.bounds);
    const bool on = enabled(context.node);
    const int z = context.record.z_index;
    const std::uint64_t owner = context.node.identity();
    result.push_back(WidgetSubtarget{owner, "field", WidgetSubtargetKind::control, 0U,
                                     projection.field, runtime::Value{}, "Saturation and value",
                                     {}, on, false, z});
    result.push_back(WidgetSubtarget{owner, "hue", WidgetSubtargetKind::control, 1U,
                                     projection.hue, runtime::Value(state.hsv.hue), "Hue", {}, on,
                                     false, z});
    if (projection.alpha.has_value()) {
        result.push_back(WidgetSubtarget{owner, "alpha", WidgetSubtargetKind::control, 2U,
                                         *projection.alpha, runtime::Value(state.hsv.alpha),
                                         "Opacity", {}, on, false, z});
    }
    for (std::size_t index = 0U; index < projection.swatches.size(); ++index) {
        ColorPickerState swatch = state;
        swatch.color = state.swatches[index];
        result.push_back(WidgetSubtarget{owner, "swatch:" + std::to_string(index),
                                         WidgetSubtargetKind::choice, index + 3U,
                                         projection.swatches[index],
                                         runtime::Value(state.swatches[index]), swatch.hex(), {},
                                         on, false, z});
    }
    return result;
}

} // namespace

RenderColor ColorPickerHsv::rgb() const noexcept {
    const double h = std::fmod(std::fmod(hue, 360.0) + 360.0, 360.0) / 60.0;
    const double c = value * saturation;
    const double x = c * (1.0 - std::abs(std::fmod(h, 2.0) - 1.0));
    const double m = value - c;
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    switch (static_cast<int>(h)) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    return RenderColor{channel(r + m), channel(g + m), channel(b + m), channel(alpha)};
}

ColorPickerHsv ColorPickerHsv::from(const RenderColor color, const double fallback_hue) noexcept {
    const double r = color.red / 255.0;
    const double g = color.green / 255.0;
    const double b = color.blue / 255.0;
    const double high = std::max({r, g, b});
    const double low = std::min({r, g, b});
    const double delta = high - low;
    ColorPickerHsv result;
    result.value = high;
    result.saturation = high > 0.0 ? delta / high : 0.0;
    result.alpha = color.alpha / 255.0;
    if (delta <= 1.0e-9) {
        result.hue = fallback_hue;
    } else if (high == r) {
        result.hue = 60.0 * std::fmod((g - b) / delta + 6.0, 6.0);
    } else if (high == g) {
        result.hue = 60.0 * ((b - r) / delta + 2.0);
    } else {
        result.hue = 60.0 * ((r - g) / delta + 4.0);
    }
    return result;
}

std::string ColorPickerState::hex() const {
    char buffer[16];
    if (alpha)
        std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X", color.red, color.green,
                      color.blue, color.alpha);
    else
        std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", color.red, color.green, color.blue);
    return buffer;
}

ColorPickerState color_picker_state(const RetainedNode& node) {
    ColorPickerState state;
    for (const runtime::Value* candidate :
         {property(node, "value"), node.retained_value("$value"), property(node, "defaultValue")}) {
        if (candidate != nullptr && candidate->color() != nullptr) {
            state.color = *candidate->color();
            break;
        }
        if (candidate != nullptr && candidate == property(node, "value"))
            break;
    }
    const runtime::Value* alpha = property(node, "showAlpha");
    state.alpha = alpha != nullptr && alpha->boolean() != nullptr && *alpha->boolean();
    if (!state.alpha)
        state.color.alpha = 255U;
    const runtime::Value* hue = node.retained_value("$hue");
    const double kept = hue != nullptr && hue->number() != nullptr ? *hue->number() : 0.0;
    state.hsv = ColorPickerHsv::from(state.color, kept);
    // A retained hue that still produces this colour wins over the recomputed one, so dragging
    // through greys and blacks keeps the hue the user chose.
    if (hue != nullptr && hue->number() != nullptr &&
        ColorPickerHsv{kept, state.hsv.saturation, state.hsv.value, state.hsv.alpha}.rgb() ==
            state.color)
        state.hsv.hue = kept;
    if (const runtime::Value* swatches = property(node, "swatches");
        swatches != nullptr && swatches->list() != nullptr) {
        for (const runtime::Value& entry : swatches->list()->values) {
            if (entry.color() != nullptr)
                state.swatches.push_back(*entry.color());
        }
    }
    return state;
}

ColorPickerProjection color_picker_projection(const ColorPickerState& state, const Rect bounds) {
    ColorPickerProjection projection;
    const Rect area{bounds.x + inset, bounds.y + inset, std::max(0.0, bounds.width - inset * 2.0),
                    std::max(0.0, bounds.height - inset * 2.0)};
    double bottom = area.bottom();
    if (!state.swatches.empty()) {
        // As few rows as keep each swatch at least the minimum size, the rows balanced so none
        // is left with a straggler, and swatches grown to fill the width up to the largest size.
        const std::size_t count = state.swatches.size();
        std::size_t rows = 1U;
        std::size_t per_row = count;
        double size = 0.0;
        for (; rows <= count; ++rows) {
            per_row = (count + rows - 1U) / rows;
            size = (area.width - static_cast<double>(per_row - 1U) * swatch_gap) /
                   static_cast<double>(per_row);
            if (size >= minimum_swatch)
                break;
        }
        size = std::clamp(size, 1.0, swatch_size);
        const double height =
            static_cast<double>(rows) * size + static_cast<double>(rows - 1U) * swatch_gap;
        const double top = bottom - height;
        for (std::size_t index = 0U; index < count; ++index) {
            const double column = static_cast<double>(index % per_row);
            const double row = static_cast<double>(index / per_row);
            projection.swatches.push_back(Rect{area.x + column * (size + swatch_gap),
                                               top + row * (size + swatch_gap), size, size});
        }
        bottom = top - gap;
    }
    projection.preview = Rect{area.x, bottom - preview_size, preview_size, preview_size};
    projection.label = Rect{area.x + preview_size + 10.0, projection.preview.y,
                            std::max(0.0, area.width - preview_size - 10.0), preview_size};
    bottom = projection.preview.y - gap;
    if (state.alpha) {
        projection.alpha = Rect{area.x, bottom - strip, area.width, strip};
        bottom = projection.alpha->y - gap;
    }
    // The strips span the field's width; their handles may overhang into the inset.
    projection.hue = Rect{area.x, bottom - strip, area.width, strip};
    bottom = projection.hue.y - gap;
    projection.field = Rect{area.x, area.y, area.width, std::max(0.0, bottom - area.y)};
    return projection;
}

void register_color_picker_widget(WidgetRegistry& registry) {
    WidgetDescribePhase describe{&color_picker_defaults, nullptr, {}, "ColorPicker", true};
    registry.register_describe_phase("ColorPicker", std::move(describe));
    WidgetInputPhase input;
    input.action_property = "onChange";
    input.fallback_action = "ColorPicker";
    input.focusable = true;
    input.pointer = &color_picker_pointer;
    input.key = &color_picker_key;
    registry.register_input_phase("ColorPicker", std::move(input));
    registry.register_semantics_phase(
        "ColorPicker", WidgetSemanticsPhase{"group", {}, &color_picker_semantics, false, false});
    registry.register_subtargets("ColorPicker", &color_picker_subtargets);
    WidgetPresentPhase present{&color_picker_content, nullptr, nullptr, nullptr, false};
    present.visual = WidgetVisualProfile{true, false, false};
    registry.register_present_phase("ColorPicker", std::move(present));
}

} // namespace strata::ui
