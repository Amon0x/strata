#include "ui/widget/input.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "ui/widget/choice_model.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] DirtyReason choice_cursor_reason(const WidgetInputScope& scope) noexcept {
    return scope.node().description().type == "Select" &&
            (scope.property("popupTemplate") != nullptr ||
             scope.property("itemTemplate") != nullptr)
        ? DirtyReason::properties
        : DirtyReason::input;
}

void set_choice_cursor(WidgetInputScope& scope, const std::size_t index) {
    scope.set_retained(
        "$choiceIndex",
        runtime::Value(static_cast<double>(index)),
        choice_cursor_reason(scope)
    );
}

bool toggle(WidgetInputScope& scope) {
    const bool next = !scope.effective_boolean(
        "checked", "$checked", "defaultChecked", false
    );
    scope.set_retained("$checked", runtime::Value(next), DirtyReason::properties);
    scope.boolean_changed("onChange", next);
    return true;
}

bool activate(WidgetInputScope& scope) {
    scope.activated("onClick");
    return true;
}

bool slider_change_at_pointer(WidgetInputScope& scope) {
    const double minimum = scope.number("min", 0.0);
    const double maximum = scope.number("max", 1.0);
    const LayoutRecord* layout = scope.layout();
    const PointerInputEvent* pointer = scope.pointer();
    const Point position = pointer != nullptr ? pointer->position : Point{};
    const bool vertical = scope.property("axis") != nullptr &&
                          scope.property("axis")->string() != nullptr &&
                          *scope.property("axis")->string() == "VERTICAL";
    const runtime::Value* authored_style = scope.property("$layout");
    const runtime::Value* authored_inset = authored_style != nullptr
        ? authored_style->field("indicatorInset")
        : nullptr;
    const double available = layout == nullptr
        ? 0.0
        : vertical ? layout->bounds.height : layout->bounds.width;
    const double inset = std::clamp(
        authored_inset != nullptr && authored_inset->number() != nullptr
            ? *authored_inset->number()
            : 7.0,
        0.0,
        available * 0.5
    );
    const double track_extent = std::max(available - inset * 2.0, 1.0);
    const double fraction = layout == nullptr
        ? 0.5
        : vertical
            ? 1.0 - (position.y - layout->bounds.y - inset) / track_extent
            : (position.x - layout->bounds.x - inset) / track_extent;
    const double raw = minimum +
        (maximum - minimum) * std::clamp(fraction, 0.0, 1.0);
    const runtime::Value* step_value = scope.property("step");
    const double step = step_value != nullptr && step_value->number() != nullptr
        ? std::max(0.0, *step_value->number())
        : 0.0;
    const double snapped = step > 0.0
        ? minimum + std::round((raw - minimum) / step) * step
        : raw;
    const double next = std::clamp(snapped, minimum, maximum);
    const double current = scope.effective_number(
        "value", "$value", "defaultValue", minimum
    );
    const bool controlled = scope.property("value") != nullptr;
    if (!controlled && (next != current || scope.retained("$value") == nullptr)) {
        scope.set_retained("$value", runtime::Value(next), DirtyReason::properties);
    }
    if (next == current) return true;
    scope.number_changed("onChange", next);
    return true;
}

bool slider_pointer(WidgetInputScope& scope) {
    const PointerInputEvent* pointer = scope.pointer();
    if (pointer == nullptr || pointer->button != 0) return false;
    if (pointer->type == PointerEventType::press) {
        return slider_change_at_pointer(scope);
    }
    const InputDispatchContext* dispatch = scope.dispatch();
    const bool active_press = dispatch != nullptr && dispatch->press_origin().has_value();
    if (!active_press) return false;
    if (pointer->type == PointerEventType::move) {
        return slider_change_at_pointer(scope);
    }
    return pointer->type == PointerEventType::release ||
        pointer->type == PointerEventType::cancel;
}

bool choose(WidgetInputScope& scope) {
    const WidgetSubtarget* target = scope.subtarget();
    if (target == nullptr || target->kind != WidgetSubtargetKind::choice || !target->enabled) {
        return false;
    }
    if (!choice_is_controlled(scope.node())) {
        scope.set_retained("$selectedId", target->value, DirtyReason::properties);
    }
    set_choice_cursor(scope, target->index);
    scope.value_changed("onChange", "selection-changed", target->value);
    return true;
}

bool select_click(WidgetInputScope& scope) {
    const WidgetSubtarget* target = scope.subtarget();
    if (target == nullptr || !target->enabled) return false;
    if (target->kind == WidgetSubtargetKind::control) {
        const bool next = !scope.effective_boolean(
            "expanded", "$expanded", "defaultExpanded", false
        );
        if (next) {
            if (const std::optional<EffectiveChoice> selected =
                    effective_choice(scope.node());
                selected.has_value()) {
                set_choice_cursor(scope, selected->index);
            }
        }
        scope.set_retained("$expanded", runtime::Value(next), DirtyReason::properties);
        return true;
    }
    if (!choose(scope)) return false;
    scope.set_retained("$expanded", runtime::Value(false), DirtyReason::properties);
    return true;
}

bool select_pointer(WidgetInputScope& scope) {
    const PointerInputEvent* pointer = scope.pointer();
    const WidgetSubtarget* target = scope.subtarget();
    if (pointer == nullptr || pointer->type != PointerEventType::move ||
        target == nullptr || target->kind != WidgetSubtargetKind::choice ||
        !target->enabled) {
        return false;
    }
    set_choice_cursor(scope, target->index);
    return true;
}

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] bool select_typeahead(
    WidgetInputScope& scope,
    const runtime::ValueList& options,
    const std::size_t current,
    const bool expanded
) {
    if (scope.key().size() != 1U ||
        !std::isalnum(static_cast<unsigned char>(scope.key().front()))) {
        return false;
    }
    constexpr std::int64_t timeout = 700'000'000;
    std::string query;
    const runtime::Value* deadline = scope.retained("$choiceTypeaheadDeadline");
    if (deadline != nullptr && deadline->number() != nullptr &&
        static_cast<double>(scope.frame_time_nanos()) <= *deadline->number()) {
        const runtime::Value* retained = scope.retained("$choiceTypeahead");
        if (retained != nullptr && retained->string() != nullptr) {
            query = *retained->string();
        }
    }
    query += lower_ascii(std::string(scope.key()));
    const auto match = [&options, current](const std::string& prefix) {
        for (std::size_t offset = 1U; offset <= options.values.size(); ++offset) {
            const std::size_t index = (current + offset) % options.values.size();
            const runtime::Value& option = options.values[index];
            const std::string* label = choice_id(option.field("label"));
            if (label != nullptr && choice_option_enabled(option) &&
                lower_ascii(*label).starts_with(prefix)) {
                return std::optional<std::size_t>(index);
            }
        }
        return std::optional<std::size_t>{};
    };
    std::optional<std::size_t> selected = match(query);
    if (!selected.has_value() && query.size() > 1U) {
        query = lower_ascii(std::string(scope.key()));
        selected = match(query);
    }
    scope.set_retained("$choiceTypeahead", runtime::Value(query), DirtyReason::input);
    scope.set_retained(
        "$choiceTypeaheadDeadline",
        runtime::Value(static_cast<double>(scope.frame_time_nanos() + timeout)),
        DirtyReason::input
    );
    if (!selected.has_value()) return true;
    const std::size_t index = *selected;
    set_choice_cursor(scope, index);
    if (expanded) return true;
    const std::string* id = choice_option_id(options.values[index]);
    if (id == nullptr) return true;
    if (!choice_is_controlled(scope.node())) {
        scope.set_retained("$selectedId", runtime::Value(*id), DirtyReason::properties);
    }
    scope.value_changed("onChange", "selection-changed", runtime::Value(*id));
    return true;
}

bool choice_key(WidgetInputScope& scope) {
    const runtime::ValueList* options = choice_options(scope.node());
    const std::optional<EffectiveChoice> selected = effective_choice(scope.node());
    if (options == nullptr || selected == std::nullopt) return false;
    const auto& values = options->values;
    std::size_t current = selected->index;
    const bool select_widget = scope.node().description().type == "Select";
    const bool expanded = select_widget && scope.effective_boolean(
        "expanded", "$expanded", "defaultExpanded", false
    );
    if (expanded) {
        const runtime::Value* cursor = scope.retained("$choiceIndex");
        if (cursor != nullptr && cursor->number() != nullptr && *cursor->number() >= 0.0 &&
            *cursor->number() < static_cast<double>(values.size())) {
            const std::size_t index = static_cast<std::size_t>(*cursor->number());
            if (choice_option_id(values[index]) != nullptr && choice_option_enabled(values[index])) {
                current = index;
            }
        }
    }
    if (scope.key() == "escape" && select_widget) {
        set_choice_cursor(scope, selected->index);
        scope.set_retained("$expanded", runtime::Value(false), DirtyReason::properties);
        return true;
    }
    if ((scope.key() == "enter" || scope.key() == "space") && select_widget) {
        if (!expanded) {
            set_choice_cursor(scope, selected->index);
            scope.set_retained("$expanded", runtime::Value(true), DirtyReason::properties);
            return true;
        }
        const std::string* id = choice_option_id(values[current]);
        if (id != nullptr && choice_option_enabled(values[current])) {
            if (!choice_is_controlled(scope.node())) {
                scope.set_retained("$selectedId", runtime::Value(*id), DirtyReason::properties);
            }
            scope.value_changed("onChange", "selection-changed", runtime::Value(*id));
        }
        scope.set_retained("$expanded", runtime::Value(false), DirtyReason::properties);
        return true;
    }
    if (select_widget && select_typeahead(scope, *options, current, expanded)) {
        return true;
    }
    int direction = 0;
    if (scope.key() == "right" || scope.key() == "down") direction = 1;
    else if (scope.key() == "left" || scope.key() == "up") direction = -1;
    else if (scope.key() == "home") current = values.size() - 1U, direction = 1;
    else if (scope.key() == "end") current = 0U, direction = -1;
    else return false;
    std::optional<std::size_t> next;
    for (std::size_t attempt = 0U; attempt < values.size(); ++attempt) {
        current = direction > 0 ? (current + 1U) % values.size()
                                : (current + values.size() - 1U) % values.size();
        if (choice_option_id(values[current]) != nullptr && choice_option_enabled(values[current])) {
            next = current;
            break;
        }
    }
    if (!next.has_value()) return true;
    current = *next;
    set_choice_cursor(scope, current);
    if (select_widget) {
        scope.set_retained("$expanded", runtime::Value(true), DirtyReason::properties);
        return true;
    }
    const std::string* id = choice_option_id(values[current]);
    if (id == nullptr) return true;
    if (!choice_is_controlled(scope.node())) {
        scope.set_retained("$selectedId", runtime::Value(*id), DirtyReason::properties);
    }
    scope.value_changed("onChange", "selection-changed", runtime::Value(*id));
    return true;
}

bool slider_key(WidgetInputScope& scope) {
    double direction = 0.0;
    if (scope.key() == "right" || scope.key() == "up") direction = 1.0;
    else if (scope.key() == "left" || scope.key() == "down") direction = -1.0;
    else return false;

    const double minimum = scope.number("min", 0.0);
    const double maximum = scope.number("max", 1.0);
    const double step = std::max(0.0, scope.number("step", 0.1));
    const double current = scope.effective_number(
        "value", "$value", "defaultValue", minimum
    );
    const double raw = current + direction * step;
    const double snapped = step > 0.0
                               ? minimum + std::round((raw - minimum) / step) * step
                               : raw;
    const double next = std::clamp(snapped, minimum, maximum);
    scope.set_event_count(1U);
    if (next == current) return true;
    scope.set_retained("$value", runtime::Value(next), DirtyReason::properties);
    scope.number_changed("onChange", next);
    return true;
}

bool number_field_key(WidgetInputScope& scope) {
    double direction = 0.0;
    if (scope.key() == "up") direction = 1.0;
    else if (scope.key() == "down") direction = -1.0;
    else return false;

    const runtime::Value* read_only = scope.property("readOnly");
    if (read_only != nullptr && read_only->boolean() != nullptr && *read_only->boolean()) {
        return true;
    }
    const double step = std::max(0.0, scope.number("step", 1.0));
    const double current = scope.effective_number("value", "$value", "defaultValue", 0.0);
    const runtime::Value* minimum_value = scope.property("min");
    const runtime::Value* maximum_value = scope.property("max");
    const double minimum = minimum_value != nullptr && minimum_value->number() != nullptr
        ? *minimum_value->number()
        : -std::numeric_limits<double>::infinity();
    const double maximum = maximum_value != nullptr && maximum_value->number() != nullptr
        ? *maximum_value->number()
        : std::numeric_limits<double>::infinity();
    const double anchor = std::isfinite(minimum) ? minimum : 0.0;
    const double raw = current + direction * step;
    const double snapped = step > 0.0
        ? anchor + std::round((raw - anchor) / step) * step
        : raw;
    const double next = std::clamp(snapped, minimum, maximum);
    scope.set_event_count(1U);
    if (next == current) return true;
    scope.set_retained("$value", runtime::Value(next), DirtyReason::properties);
    scope.synchronize_editor_text(runtime::display_string(runtime::Value(next)), true);
    scope.number_changed("onChange", next);
    return true;
}

void add(
    WidgetRegistry& registry,
    std::string type,
    const bool focusable,
    const WidgetInputHook click = nullptr,
    const WidgetInputHook key = nullptr,
    std::string action_property = {},
    std::string fallback_action = {},
    const WidgetTextEditMode text_edit_mode = WidgetTextEditMode::none
) {
    WidgetInputPhase phase;
    phase.click = click;
    phase.key = key;
    phase.action_property = std::move(action_property);
    phase.fallback_action = std::move(fallback_action);
    phase.focusable = focusable;
    phase.text_edit_mode = text_edit_mode;
    registry.register_input_phase(std::move(type), std::move(phase));
}

} // namespace

void register_control_widget_inputs(WidgetRegistry& registry) {
    add(registry, "IconButton", true, &activate, nullptr, "onClick", "IconButton");
    add(registry, "Checkbox", true, &toggle, nullptr, "onChange", "Checkbox");
    add(registry, "Toggle", true, &toggle, nullptr, "onChange", "Toggle");
    add(registry, "Switch", true, &toggle, nullptr, "onChange", "Switch");
    add(
        registry, "TextBox", true, nullptr, nullptr, "onChange", "text-edit",
        WidgetTextEditMode::single_line
    );
    add(
        registry, "TextArea", true, nullptr, nullptr, "onChange", "TextArea",
        WidgetTextEditMode::multi_line
    );
    add(
        registry, "NumberField", true, nullptr, nullptr, "onChange", "NumberField",
        WidgetTextEditMode::numeric
    );
    const WidgetLifecycle* number_field_lifecycle = registry.find("NumberField");
    WidgetInputPhase number_field = number_field_lifecycle != nullptr
        ? number_field_lifecycle->input
        : WidgetInputPhase{};
    number_field.editor_key = &number_field_key;
    registry.register_input_phase("NumberField", std::move(number_field));
    add(registry, "Slider", true, nullptr, &slider_key, "onChange", "Slider");
    const WidgetLifecycle* slider_lifecycle = registry.find("Slider");
    WidgetInputPhase slider = slider_lifecycle != nullptr
        ? slider_lifecycle->input
        : WidgetInputPhase{};
    slider.pointer = &slider_pointer;
    registry.register_input_phase("Slider", std::move(slider));
    add(registry, "Tabs", true, &choose, &choice_key, "onChange", "Tabs");
    add(registry, "RadioGroup", true, &choose, &choice_key, "onChange", "RadioGroup");

    WidgetInputPhase select;
    select.action_property = "onChange";
    select.fallback_action = "Select";
    select.focusable = true;
    select.popup_controlled = "expanded";
    select.popup_retained = "$expanded";
    select.popup_initial = "defaultExpanded";
    select.click = &select_click;
    select.pointer = &select_pointer;
    select.key = &choice_key;
    registry.register_input_phase("Select", std::move(select));
}

} // namespace strata::ui
