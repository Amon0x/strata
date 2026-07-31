#include "ui/widget/semantics.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ui/widget/choice_model.hpp"

namespace strata::ui {
namespace {

using data::JsonValue;

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> entries) {
    return JsonValue(JsonValue::Object(entries));
}

[[nodiscard]] std::optional<std::string> text(
    WidgetSemanticsScope& scope,
    const std::string_view property
) {
    return scope.text(scope.property(property));
}

void label_or_key(WidgetSemanticsScope& scope) {
    if (const auto label = text(scope, "label"); label.has_value()) scope.name(*label);
    else if (scope.node().description().key.has_value()) {
        scope.default_name(*scope.node().description().key);
    }
}

void icon_button(WidgetSemanticsScope& scope) {
    if (scope.node().description().key.has_value()) scope.name(*scope.node().description().key);
    else label_or_key(scope);
}

void text_field(WidgetSemanticsScope& scope) {
    if (const auto hint = text(scope, "hint"); hint.has_value()) scope.name(*hint);
    else label_or_key(scope);
    std::optional<std::string> value;
    if (const std::string* edited = scope.edited_text(); edited != nullptr) value = *edited;
    if (!value.has_value()) value = text(scope, "text");
    if (!value.has_value()) value = scope.text(scope.retained("$text"));
    scope.default_value_text(std::move(value));
    scope.read_only(scope.boolean(scope.property("readOnly")).value_or(false));
}

[[nodiscard]] const runtime::Value* effective_number_value(WidgetSemanticsScope& scope) {
    const runtime::Value* value = scope.property("value");
    if (value == nullptr || value->number() == nullptr) value = scope.retained("$value");
    if (value == nullptr || value->number() == nullptr) value = scope.property("defaultValue");
    return value;
}

void numeric_range(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    const runtime::Value* current = effective_number_value(scope);
    const runtime::Value* minimum = scope.property("min");
    const runtime::Value* maximum = scope.property("max");
    scope.value_range(object({
        {"current", current != nullptr && current->number() != nullptr
                        ? JsonValue(*current->number()) : JsonValue(0.0)},
        {"maximum", maximum != nullptr && maximum->number() != nullptr
                        ? JsonValue(*maximum->number()) : JsonValue(1.0)},
        {"minimum", minimum != nullptr && minimum->number() != nullptr
                        ? JsonValue(*minimum->number()) : JsonValue(0.0)},
        {"text", JsonValue{}},
    }));
}

void tabs(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    const std::optional<EffectiveChoice> selection = effective_choice(scope.node());
    const std::string selected = selection.has_value() ? selection->id : std::string{};
    scope.default_value_text(selected);
    const runtime::Value* entries = scope.property("tabs");
    if (entries == nullptr || entries->list() == nullptr) return;
    for (std::size_t index = 0U; index < entries->list()->values.size(); ++index) {
        const runtime::Value& entry = entries->list()->values[index];
        const std::string id = scope.text(entry.field("id")).value_or(std::string{});
        const std::string label = scope.text(entry.field("label")).value_or(id);
        const bool enabled = scope.boolean(entry.field("enabled")).value_or(true);
        scope.virtual_after(scope.virtual_item(
            index,
            2'000'000U,
            "tab",
            label,
            {"activate", "focus"},
            std::nullopt,
            std::optional<bool>(selection.has_value() && selection->index == index),
            !enabled,
            index
        ));
    }
}

void selectable_options(
    WidgetSemanticsScope& scope,
    const std::string_view item_role,
    const bool checked
) {
    const std::optional<EffectiveChoice> selection = effective_choice(scope.node());
    const runtime::Value* entries = scope.property("options");
    if (entries == nullptr || entries->list() == nullptr) return;
    for (std::size_t index = 0U; index < entries->list()->values.size(); ++index) {
        const runtime::Value& entry = entries->list()->values[index];
        const std::string id = scope.text(entry.field("id")).value_or(std::string{});
        const std::string label = scope.text(entry.field("label")).value_or(id);
        const bool enabled = scope.boolean(entry.field("enabled")).value_or(true);
        scope.virtual_before(scope.virtual_item(
            index,
            2'000'000U,
            std::string(item_role),
            label,
            {"activate", "focus"},
            checked
                ? std::optional<bool>(selection.has_value() && selection->index == index)
                : std::nullopt,
            checked
                ? std::nullopt
                : std::optional<bool>(selection.has_value() && selection->index == index),
            !enabled,
            index
        ));
    }
}

void select(WidgetSemanticsScope& scope) {
    if (scope.node().description().key.has_value()) scope.name(*scope.node().description().key);
    else label_or_key(scope);
    selectable_options(scope, "list_item", false);
    const std::optional<EffectiveChoice> selection = effective_choice(scope.node());
    scope.default_value_text(selection.has_value() ? selection->id : std::string{});
    scope.expanded(scope.effective_boolean(
        "expanded", "$expanded", "defaultExpanded", false
    ));
}

void radio_group(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    selectable_options(scope, "radio", true);
    const std::optional<EffectiveChoice> selection = effective_choice(scope.node());
    scope.default_value_text(selection.has_value() ? selection->id : std::string{});
}

void combo_box(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    scope.actions({});
}

void add(
    WidgetRegistry& registry,
    std::string type,
    std::string role,
    const WidgetSemanticsHook derive = nullptr
) {
    registry.register_semantics_phase(
        std::move(type),
        WidgetSemanticsPhase{std::move(role), {}, derive, false, false}
    );
}

} // namespace

void register_control_widget_semantics(WidgetRegistry& registry) {
    add(registry, "IconButton", "button", &icon_button);
    add(registry, "Checkbox", "checkbox", &label_or_key);
    add(registry, "Toggle", "switch", &label_or_key);
    add(registry, "Switch", "switch", &label_or_key);
    add(registry, "TextBox", "text_field", &text_field);
    add(registry, "TextArea", "text_field", &text_field);
    add(registry, "NumberField", "slider", &numeric_range);
    add(registry, "Slider", "slider", &numeric_range);
    add(registry, "Progress", "progress_bar", &numeric_range);
    add(registry, "Tabs", "tab_list", &tabs);
    add(registry, "Select", "combo_box", &select);
    add(registry, "RadioGroup", "radio_group", &radio_group);
    add(registry, "ComboBox", "combo_box", &combo_box);
}

} // namespace strata::ui
