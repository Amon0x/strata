#include "ui/widget/description.hpp"

#include <utility>

namespace strata::ui {
namespace {

void button_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", runtime::Value(32.0));
    scope.set("width", runtime::Value("content"));
    scope.set("alignItems", runtime::Value("CENTER"));
    scope.padding(10.0, 6.0, 10.0, 6.0);
}

void image_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", runtime::Value(64.0));
    scope.set("width", runtime::Value(64.0));
    scope.intrinsic(64.0, 64.0);
}

void draw_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", runtime::Value(64.0));
    scope.set("width", runtime::Value(64.0));
    scope.intrinsic(64.0, 64.0);
}

void menu_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("height", runtime::Value(30.0));
    scope.set("width", runtime::Value("content"));
    scope.padding(10.0, 6.0, 10.0, 6.0);
}

void status_bar_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("alignItems", runtime::Value("CENTER"));
    scope.set("height", runtime::Value(28.0));
    scope.set("kind", runtime::Value("ROW"));
    scope.set("width", widget_fill());
}

void section_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("clip", runtime::Value(true));
    scope.set("gap", runtime::Value(6.0));
    scope.set("height", runtime::Value("content"));
    scope.set("kind", runtime::Value("COLUMN"));
    scope.set("width", widget_fill());
    scope.padding(0.0, 32.0, 0.0, 0.0);
}

void section_motion(WidgetDescriptionScope& scope) {
    WidgetDescriptionExpansion& description = scope.description();
    description.properties.insert_or_assign(
        "$disclosureDefaults",
        runtime::ExpressionValue(widget_object({
            {"collapsedExtent", runtime::Value(32.0)},
            {"controlled", runtime::Value("expanded")},
            {"durationNanos", runtime::Value(180'000'000.0)},
            {"initial", runtime::Value("defaultExpanded")},
            {"retained", runtime::Value("$expanded")},
        }))
    );
    if (!description.properties.contains("animateContentSize")) {
        description.properties.emplace(
            "$contentSizeMotionDefaults",
            runtime::ExpressionValue(widget_object({
                {"height", runtime::Value(true)},
                {"width", runtime::Value(false)},
                {"clip", runtime::Value(true)},
                {"durationNanos", runtime::Value(180'000'000.0)},
            }))
        );
    }
}

void list_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("gap", runtime::Value(2.0));
    scope.set("kind", runtime::Value("COLUMN"));
}

void rich_text_expand(WidgetDescriptionScope& scope) {
    std::string text;
    for (const runtime::Value* span : scope.list("spans")) {
        if (const std::string* value = widget_description_string(span->field("text")); value != nullptr) {
            text += *value;
        }
    }
    scope.description().properties.insert_or_assign(
        "text",
        runtime::ExpressionValue(runtime::Value(std::move(text)))
    );
}

void add(
    WidgetRegistry& registry,
    std::string type,
    const WidgetLayoutDefaultsHook defaults = nullptr,
    const WidgetDescriptionHook expand = nullptr,
    std::string canonical_type = {},
    std::string default_action = {}
) {
    registry.register_describe_phase(
        std::move(type),
        WidgetDescribePhase{
            defaults,
            expand,
            std::move(canonical_type),
            std::move(default_action),
            true,
        }
    );
}

} // namespace

void register_primitive_widget_descriptions(WidgetRegistry& registry) {
    add(registry, "Button", &button_defaults);
    add(registry, "Image", &image_defaults);
    add(registry, "Draw", &draw_defaults);
    add(registry, "Menu", &menu_defaults, nullptr, {}, "Menu");
    // ContextMenu is an authoring alias with container-authored geometry; unlike Menu it does not
    // inherit trigger padding before canonicalization.
    add(registry, "ContextMenu", nullptr, nullptr, "Menu", "ContextMenu");
    add(registry, "StatusBar", &status_bar_defaults);
    add(registry, "Section", &section_defaults, &section_motion);
    add(registry, "List", &list_defaults, nullptr, {}, "List");
    add(registry, "RichText", nullptr, &rich_text_expand);

    WidgetCommandPhase menu_commands;
    menu_commands.references_property.clear();
    menu_commands.item_collection_property = "items";
    menu_commands.item_reference_property = "command";
    registry.register_command_phase("Menu", std::move(menu_commands));

    WidgetCommandPhase button_command;
    button_command.references_property = "command";
    button_command.activation_reference_property = "command";
    registry.register_command_phase("Button", std::move(button_command));
}

} // namespace strata::ui
