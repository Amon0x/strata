#include "ui/widget/description.hpp"

#include <algorithm>
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
    scope.set("gap", runtime::Value(0.0));
    scope.set("height", runtime::Value("content"));
    scope.set("kind", runtime::Value("COLUMN"));
    scope.set("width", widget_fill());
    scope.padding(0.0, 0.0, 0.0, 0.0);
}

void section_expand(WidgetDescriptionScope& scope) {
    WidgetDescriptionExpansion& description = scope.description();
    const double header_height = std::max(24.0, scope.number("headerHeight", 36.0));
    const runtime::Value* authored_padding = scope.property("contentPadding");
    const runtime::Value content_padding = authored_padding != nullptr
        ? *authored_padding
        : widget_object({
              {"bottom", runtime::Value(10.0)},
              {"left", runtime::Value(10.0)},
              {"right", runtime::Value(10.0)},
              {"top", runtime::Value(8.0)},
          });
    const runtime::Value* authored_gap = scope.property("contentGap");
    const runtime::Value content_gap = authored_gap != nullptr
        ? *authored_gap
        : runtime::Value(8.0);

    DescriptionNode::Properties header = widget_transparent_properties();
    header.emplace(
        "$inputTransparent",
        runtime::ExpressionValue(runtime::Value(true))
    );
    header.emplace(
        "semantics",
        runtime::ExpressionValue(widget_object({{"decorative", runtime::Value(true)}}))
    );
    scope.set_layout(header, "background", runtime::Value{});
    scope.set_layout(header, "border", runtime::Value{});
    scope.set_layout(header, "height", runtime::Value(header_height));
    scope.set_layout(header, "kind", runtime::Value("PANEL"));
    scope.set_layout(header, "width", widget_fill());

    DescriptionNode::Properties content = widget_transparent_properties();
    content.emplace(
        "$semanticTransparent",
        runtime::ExpressionValue(runtime::Value(true))
    );
    scope.set_layout(content, "alignItems", runtime::Value("STRETCH"));
    scope.set_layout(content, "background", runtime::Value{});
    scope.set_layout(content, "border", runtime::Value{});
    scope.set_layout(content, "gap", content_gap);
    scope.set_layout(content, "height", runtime::Value("content"));
    scope.set_layout(content, "kind", runtime::Value("COLUMN"));
    scope.set_layout(content, "padding", content_padding);
    scope.set_layout(content, "width", widget_fill());

    std::vector<std::shared_ptr<const DescriptionNode>> authored_children =
        std::move(description.children);
    description.children.clear();
    description.children.push_back(scope.node("Panel", std::nullopt, std::move(header)));
    description.children.push_back(scope.node(
        "Panel",
        std::nullopt,
        std::move(content),
        std::move(authored_children)
    ));
    scope.synthesized(2U);

    description.properties.insert_or_assign(
        "$disclosureDefaults",
        runtime::ExpressionValue(widget_object({
            {"collapsedExtent", runtime::Value(header_height)},
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
    add(registry, "Section", &section_defaults, &section_expand);
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
