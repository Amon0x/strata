#include "ui/widget/date_picker_model.hpp"

#include <algorithm>
#include <vector>

#include "runtime/expression.hpp"
#include "ui/input.hpp"
#include "ui/path.hpp"
#include "ui/text.hpp"
#include "ui/theme.hpp"
#include "ui/tree.hpp"
#include "ui/widget/description.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/presentation.hpp"
#include "ui/widget/registry.hpp"
#include "ui/widget/semantics.hpp"
#include "ui/widget/subtarget.hpp"

namespace strata::ui {
namespace {

constexpr double popup_width = 280.0;
constexpr double popup_height = 300.0;
constexpr double popup_inset = 6.0;

[[nodiscard]] const runtime::Value* property(const RetainedNode& node,
                                             const std::string_view name) noexcept {
    const auto found = node.description().properties.find(name);
    return found != node.description().properties.end() ? found->second.value() : nullptr;
}

[[nodiscard]] bool expanded(const RetainedNode& node) noexcept {
    const runtime::Value* value = node.retained_value("$expanded");
    return value != nullptr && value->boolean() != nullptr && *value->boolean();
}

[[nodiscard]] bool enabled(const RetainedNode& node) noexcept {
    const runtime::Value* value = property(node, "enabled");
    return value == nullptr || value->boolean() == nullptr || *value->boolean();
}

[[nodiscard]] Rect root_of(const LayoutResult& layout, const Rect fallback) noexcept {
    const LayoutRecord* root = layout.find(layout.root_identity);
    return root != nullptr ? root->bounds : fallback;
}

void date_picker_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", runtime::Value(34.0));
    scope.set("width", runtime::Value(220.0));
    scope.intrinsic(220.0, 34.0);
}

/** A small calendar: a rounded page with a header bar and two binding rings. */
[[nodiscard]] PathShape calendar_icon(const RenderColor color) {
    Path path = Path::rectangle(Rect{0.1, 0.2, 0.8, 0.7}, 0.12);
    path.move_to(Point{0.1, 0.42});
    path.line_to(Point{0.9, 0.42});
    path.move_to(Point{0.32, 0.08});
    path.line_to(Point{0.32, 0.28});
    path.move_to(Point{0.68, 0.08});
    path.line_to(Point{0.68, 0.28});
    StrokeStyle style;
    style.width = 1.4;
    style.cap = PathCap::round;
    style.join = PathJoin::round;
    return PathShape{std::move(path), std::nullopt, Paint(color), style};
}

void date_picker_content(WidgetRenderScope& scope) {
    const Rect bounds = scope.layout().bounds;
    if (scope.visual().background.has_value())
        scope.rounded_rect(bounds, *scope.visual().background, scope.visual().border);
    scope.interaction(bounds, "$control");
    const CalendarState state = calendar_state(scope.node());
    const std::optional<std::string> shown = date_picker_text(scope.node(), state);
    const std::string placeholder = scope.string("placeholder", "Pick a date");
    const std::string& label = shown.value_or(placeholder);
    const double icon = std::min(14.0, bounds.height - 8.0);
    if (scope.text_engine() != nullptr && !label.empty()) {
        const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), label);
        scope.push_clip(Rect{bounds.x + 10.0, bounds.y,
                             std::max(0.0, bounds.width - icon - 28.0), bounds.height});
        scope.text(label,
                   Point{bounds.x + 10.0, bounds.y + (bounds.height - shaped.metrics.height) * 0.5},
                   shown.has_value() ? scope.visual().foreground : scope.visual().text_hint);
        scope.pop_clip();
    }
    scope.shape(Rect{bounds.right() - icon - 10.0, bounds.y + (bounds.height - icon) * 0.5, icon,
                     icon},
                calendar_icon(shown.has_value() ? scope.visual().foreground
                                                : scope.visual().text_hint));
    scope.focus(bounds);
}

void date_picker_overlay(WidgetRenderScope& scope) {
    if (!expanded(scope.node()))
        return;
    const DatePickerPopup popup = date_picker_popup(scope.layout().bounds, scope.root_bounds());
    scope.shadow(popup.surface, CornerRadii::all(8.0), scope.palette("shadow", std::uint8_t{90U}),
                 14.0, 1.0);
    scope.rounded_rect(popup.surface, Paint(scope.palette("popupSurface")), scope.visual().border,
                       8.0);
    present_calendar_month(scope, calendar_state(scope.node()), popup.month);
}

[[nodiscard]] bool date_picker_overlay_shown(const RetainedNode& node, const InputRouter&) {
    return expanded(node);
}

/** The field, and while open the dropdown's surface, month buttons and days. */
std::vector<WidgetSubtarget> date_picker_subtargets(const WidgetSubtargetContext& context) {
    std::vector<WidgetSubtarget> result;
    if (context.record.bounds.empty())
        return result;
    const RetainedNode& node = context.node;
    result.push_back(WidgetSubtarget{node.identity(), "$control", WidgetSubtargetKind::control, 0U,
                                     context.record.bounds, runtime::Value{}, {}, {},
                                     enabled(node), false, context.record.z_index});
    if (!expanded(node))
        return result;
    const DatePickerPopup popup =
        date_picker_popup(context.record.bounds, root_of(context.layout, context.record.bounds));
    // The surface comes first so the days and buttons above it are hit before it.
    WidgetSubtarget surface{node.identity(), "$popup", WidgetSubtargetKind::separator, 1U,
                            popup.surface, runtime::Value{}, {}, {}, false, true,
                            detached_overlay_menu_z};
    surface.separator = true;
    result.push_back(std::move(surface));
    const CalendarState state = calendar_state(node);
    const CalendarProjection projection = calendar_projection(state, popup.month);
    result.push_back(WidgetSubtarget{node.identity(), "previous", WidgetSubtargetKind::control, 2U,
                                     projection.previous, runtime::Value("previous"),
                                     "Previous month", {}, true, true, detached_overlay_menu_z});
    result.push_back(WidgetSubtarget{node.identity(), "next", WidgetSubtargetKind::control, 3U,
                                     projection.next, runtime::Value("next"), "Next month", {},
                                     true, true, detached_overlay_menu_z});
    for (std::size_t index = 0U; index < projection.cells.size(); ++index) {
        const CalendarCell& cell = projection.cells[index];
        const std::string text = cell.date.text();
        result.push_back(WidgetSubtarget{
            node.identity(), "day:" + text, WidgetSubtargetKind::choice, index + 4U, cell.bounds,
            runtime::Value(text),
            state.month_name(cell.date.month) + " " + std::to_string(cell.date.day) + ", " +
                std::to_string(cell.date.year),
            {}, cell.enabled, true, detached_overlay_menu_z});
    }
    return result;
}

/** Opening shows the picked day's month with the keyboard on it. */
void date_picker_open(WidgetInputScope& scope, const bool open) {
    if (open) {
        scope.set_retained("$cursor", runtime::Value{}, DirtyReason::properties);
        scope.set_retained("$month", runtime::Value{}, DirtyReason::properties);
    }
    scope.set_retained("$expanded", runtime::Value(open), DirtyReason::properties);
}

void date_picker_move(WidgetInputScope& scope, const CalendarDate& to) {
    scope.set_retained("$cursor", runtime::Value(to.text()), DirtyReason::properties);
    scope.set_retained("$month", runtime::Value(CalendarDate{to.year, to.month, 1U}.text()),
                       DirtyReason::properties);
}

void date_picker_pick(WidgetInputScope& scope, const CalendarState& state,
                      const CalendarDate& day) {
    if (!state.allows(day))
        return;
    if (scope.property("value") == nullptr)
        scope.set_retained("$value", runtime::Value(day.text()), DirtyReason::properties);
    scope.value_changed("onChange", "date-changed", runtime::Value(day.text()));
    date_picker_open(scope, false);
}

bool date_picker_click(WidgetInputScope& scope) {
    const WidgetSubtarget* target = scope.subtarget();
    if (target == nullptr || !target->enabled)
        return false;
    if (target->id == "$control") {
        date_picker_open(scope, !expanded(scope.node()));
        return true;
    }
    const CalendarState state = calendar_state(scope.node());
    if (target->id == "previous" || target->id == "next") {
        date_picker_move(scope, state.cursor.add_months(target->id == "previous" ? -1 : 1));
        return true;
    }
    const std::string* text = target->value.string();
    const std::optional<CalendarDate> day =
        text != nullptr ? CalendarDate::parse(*text) : std::nullopt;
    if (!day.has_value())
        return false;
    date_picker_pick(scope, state, *day);
    return true;
}

/** Closed: Enter, Space, Down or Alt+Down opens it. Open: the Calendar's keys, Enter picks. */
bool date_picker_key(WidgetInputScope& scope) {
    if (!enabled(scope.node()))
        return false;
    const std::string_view key = scope.key();
    if (!expanded(scope.node())) {
        if (key == "enter" || key == "space" || key == "down") {
            date_picker_open(scope, true);
            return true;
        }
        return false;
    }
    const CalendarState state = calendar_state(scope.node());
    if (key == "enter" || key == "space") {
        date_picker_pick(scope, state, state.cursor);
        return true;
    }
    const std::optional<CalendarDate> next =
        calendar_key_target(state, key, scope.modifiers().shift);
    if (!next.has_value())
        return false;
    date_picker_move(scope, *next);
    return true;
}

void date_picker_semantics(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    const CalendarState state = calendar_state(scope.node());
    scope.default_value_text(date_picker_text(scope.node(), state).value_or(std::string()));
    scope.expanded(expanded(scope.node()));
}

} // namespace

DatePickerPopup date_picker_popup(const Rect anchor, const Rect viewport) {
    const double width = std::min(popup_width, viewport.width);
    const double x =
        std::clamp(anchor.x, viewport.x, std::max(viewport.x, viewport.right() - width));
    const double below = std::max(0.0, viewport.bottom() - anchor.bottom() - 4.0);
    const double above = std::max(0.0, anchor.y - viewport.y - 4.0);
    const bool flip = popup_height > below && above > below;
    const double height = std::min(popup_height, flip ? above : below);
    const double y = flip ? anchor.y - 4.0 - height : anchor.bottom() + 4.0;
    const Rect surface{x, y, width, height};
    return DatePickerPopup{surface,
                           Rect{surface.x + popup_inset, surface.y + popup_inset,
                                std::max(0.0, surface.width - 2.0 * popup_inset),
                                std::max(0.0, surface.height - 2.0 * popup_inset)}};
}

std::optional<std::string> date_picker_text(const RetainedNode& node, const CalendarState& state) {
    if (!state.selected.has_value())
        return std::nullopt;
    const runtime::Value* format = property(node, "format");
    if (format != nullptr && format->string() != nullptr && !format->string()->empty()) {
        return runtime::format_date_pattern(
            static_cast<double>(state.selected->days()) * 86'400'000.0, *format->string(), 0.0,
            state.month_names, {});
    }
    const std::string& month = state.month_name(state.selected->month);
    return month.substr(0U, std::min<std::size_t>(3U, month.size())) + " " +
           std::to_string(state.selected->day) + ", " + std::to_string(state.selected->year);
}

void register_date_picker_widget(WidgetRegistry& registry) {
    WidgetDescribePhase describe{&date_picker_defaults, nullptr, {}, "DatePicker", true};
    registry.register_describe_phase("DatePicker", std::move(describe));
    WidgetInputPhase input;
    input.action_property = "onChange";
    input.fallback_action = "DatePicker";
    input.focusable = true;
    input.popup_controlled = "expanded";
    input.popup_retained = "$expanded";
    input.popup_initial = "defaultExpanded";
    input.click = &date_picker_click;
    input.key = &date_picker_key;
    registry.register_input_phase("DatePicker", std::move(input));
    registry.register_semantics_phase(
        "DatePicker", WidgetSemanticsPhase{"combobox", {}, &date_picker_semantics, false, false});
    registry.register_subtargets("DatePicker", &date_picker_subtargets, true);
    WidgetPresentPhase present{&date_picker_content, nullptr, &date_picker_overlay, nullptr, true};
    present.visual = WidgetVisualProfile{false, true, false};
    present.overlay_shown = &date_picker_overlay_shown;
    registry.register_present_phase("DatePicker", std::move(present));
}

} // namespace strata::ui
