#include "ui/widget/description.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "ui/widget/menu_model.hpp"

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
    if (scope.property("triggerTemplate") != nullptr) {
        scope.set("height", runtime::Value("content"));
        scope.set("width", runtime::Value("content"));
        scope.padding(0.0, 0.0, 0.0, 0.0);
        return;
    }
    scope.set("height", runtime::Value(30.0));
    scope.set("width", runtime::Value("content"));
    scope.padding(10.0, 6.0, 10.0, 6.0);
}

[[nodiscard]] bool retained_boolean(
    WidgetDescriptionScope& scope,
    const std::string_view property,
    const std::string_view retained,
    const std::string_view initial
) {
    if (const runtime::Value* value = scope.property(property);
        value != nullptr && value->boolean() != nullptr) {
        return *value->boolean();
    }
    if (const runtime::Value* value = scope.retained(retained);
        value != nullptr && value->boolean() != nullptr) {
        return *value->boolean();
    }
    return scope.boolean(initial, false);
}

[[nodiscard]] std::vector<std::size_t> retained_menu_path(
    WidgetDescriptionScope& scope
) {
    const runtime::Value* value = scope.retained("$menuPath");
    std::vector<std::size_t> result;
    if (value == nullptr || value->list() == nullptr) return result;
    for (const runtime::Value& entry : value->list()->values) {
        if (entry.number() == nullptr || !std::isfinite(*entry.number()) ||
            *entry.number() < 0.0) {
            break;
        }
        result.push_back(static_cast<std::size_t>(*entry.number()));
    }
    return result;
}

[[nodiscard]] std::shared_ptr<const DescriptionNode> append_children(
    const std::shared_ptr<const DescriptionNode>& source,
    std::vector<std::shared_ptr<const DescriptionNode>> children
) {
    auto result = std::make_shared<DescriptionNode>(*source);
    std::vector<std::shared_ptr<const DescriptionNode>> merged;
    if (source->children != nullptr) {
        merged.reserve(source->children->size() + children.size());
        for (std::size_t index = 0U; index < source->children->size(); ++index) {
            merged.push_back(source->children->at(index));
        }
    }
    merged.insert(
        merged.end(),
        std::make_move_iterator(children.begin()),
        std::make_move_iterator(children.end())
    );
    result->children = std::make_shared<const EagerDescriptionChildren>(std::move(merged));
    return result;
}

[[nodiscard]] runtime::Value popup_portal_layout(
    const DescriptionNode& popup,
    const std::string& anchor,
    const bool submenu,
    const std::optional<Point> anchor_point = std::nullopt
) {
    std::vector<std::pair<std::string, runtime::Value>> fields{
        {"anchorAlign", runtime::Value("START")},
        {"anchorFlip", runtime::Value(true)},
        {"anchorGap", runtime::Value(submenu ? 2.0 : 4.0)},
        {"anchorShift", runtime::Value(true)},
        {"anchorSide", runtime::Value(submenu ? "RIGHT" : "BOTTOM")},
        {"anchorTarget", runtime::Value(anchor)},
        {"detachFromParentClip", runtime::Value(true)},
        {"height", runtime::Value("content")},
        {"kind", runtime::Value("PORTAL")},
        {"portalTarget", runtime::Value("root")},
        {"width", runtime::Value("content")},
        {"zIndex", runtime::Value(20'000.0)},
    };
    if (anchor_point.has_value()) {
        fields.emplace_back("anchorPoint", widget_object({
            {"x", runtime::Value(anchor_point->x)},
            {"y", runtime::Value(anchor_point->y)},
        }));
    }
    const auto authored = popup.properties.find("$layout");
    const runtime::Value* layout = authored != popup.properties.end()
        ? authored->second.data_value()
        : nullptr;
    for (const std::string_view name : {
             "anchorAlign",
             "anchorFlip",
             "anchorGap",
             "anchorShift",
             "anchorSide",
             "matchAnchorWidth",
         }) {
        const runtime::Value* value = layout != nullptr ? layout->field(name) : nullptr;
        if (value == nullptr) continue;
        const auto found = std::ranges::find(fields, name, &std::pair<std::string, runtime::Value>::first);
        if (found != fields.end()) found->second = *value;
        else fields.emplace_back(name, *value);
    }
    return runtime::Value(std::move(fields));
}

void menu_expand(WidgetDescriptionScope& scope) {
    const bool context = scope.string("$authoringType") == "ContextMenu";
    const std::string* trigger_component =
        widget_description_string(scope.property("triggerTemplate"));
    const std::string* popup_component =
        widget_description_string(scope.property("popupTemplate"));
    const std::string* item_component =
        widget_description_string(scope.property("itemTemplate"));
    const bool authored_popup = popup_component != nullptr && item_component != nullptr;
    if ((context && !authored_popup) ||
        (!context && trigger_component == nullptr && !authored_popup)) {
        return;
    }
    WidgetDescriptionExpansion& description = scope.description();
    const std::string key = description.key.value_or("$menu");
    const bool expanded = retained_boolean(
        scope, "open", "$expanded", "defaultOpen"
    );
    if (!context) {
        if (trigger_component != nullptr) {
            std::shared_ptr<const DescriptionNode> trigger = scope.instantiate_component(
                *trigger_component,
                key + ".trigger",
                WidgetTemplateArguments{
                    {"key", runtime::Value(runtime::KeyValue{key + ".trigger"})},
                    {"label", runtime::Value(scope.string("label"))},
                    {"value", runtime::Value{}},
                    {"enabled", runtime::Value(true)},
                    {"expanded", runtime::Value(expanded)},
                }
            );
            if (trigger == nullptr) return;
            trigger = widget_native_presentation(trigger);
            description.children = {std::move(trigger)};
        }
    }
    if (!expanded || !authored_popup) return;

    const std::vector<MenuItemModel> items = parse_menu_items(scope.property("items"), nullptr);
    const std::vector<std::size_t> active_path = retained_menu_path(scope);
    const std::vector<MenuItemModel>* level_items = &items;
    std::vector<std::size_t> parent_path;
    std::string anchor = trigger_component != nullptr ? key + ".trigger" : key;
    std::optional<Point> pointer_anchor;
    if (context) {
        const runtime::Value* x = scope.retained("$menuAnchorX");
        const runtime::Value* y = scope.retained("$menuAnchorY");
        if (x != nullptr && x->number() != nullptr &&
            y != nullptr && y->number() != nullptr) {
            pointer_anchor = Point{*x->number(), *y->number()};
        }
    }
    for (std::size_t level = 0U; !level_items->empty(); ++level) {
        std::vector<std::shared_ptr<const DescriptionNode>> rows;
        rows.reserve(level_items->size());
        for (std::size_t index = 0U; index < level_items->size(); ++index) {
            const MenuItemModel& item = (*level_items)[index];
            std::vector<std::size_t> path = parent_path;
            path.push_back(index);
            const std::string row_key = menu_row_key(key, path);
            const bool active = active_path.size() >= path.size() &&
                std::equal(path.begin(), path.end(), active_path.begin());
            std::shared_ptr<const DescriptionNode> row = scope.instantiate_component(
                *item_component,
                row_key,
                WidgetTemplateArguments{
                    {"key", runtime::Value(runtime::KeyValue{row_key})},
                    {"id", runtime::Value(item.id)},
                    {"label", runtime::Value(item.label)},
                    {"value", runtime::Value(item.id)},
                    {"index", runtime::Value(static_cast<double>(index))},
                    {"level", runtime::Value(static_cast<double>(level))},
                    {"enabled", runtime::Value(item.enabled)},
                    {"selected", runtime::Value(active)},
                    {"active", runtime::Value(active)},
                    {"checked", runtime::Value(item.has_checked && item.checked)},
                    {"separator", runtime::Value(item.separator)},
                    {"hasChildren", runtime::Value(!item.children.empty())},
                    {"shortcut", runtime::Value(item.shortcut)},
                }
            );
            if (row != nullptr) rows.push_back(std::move(row));
        }
        std::shared_ptr<const DescriptionNode> popup = scope.instantiate_component(
            *popup_component,
            key + ".popup.surface." + std::to_string(level),
            WidgetTemplateArguments{
                {"key", runtime::Value(runtime::KeyValue{
                    key + ".popup.surface." + std::to_string(level)
                })},
                {"level", runtime::Value(static_cast<double>(level))},
                {"expanded", runtime::Value(true)},
            }
        );
        if (popup == nullptr) break;
        popup = append_children(popup, std::move(rows));
        DescriptionNode::Properties portal_properties = widget_transparent_properties();
        widget_mark_native_presentation(portal_properties);
        portal_properties.insert_or_assign(
            "$layout",
            runtime::ExpressionValue(popup_portal_layout(
                *popup,
                anchor,
                level != 0U,
                level == 0U ? pointer_anchor : std::nullopt
            ))
        );
        description.children.push_back(scope.node(
            "Panel",
            key + ".popup." + std::to_string(level),
            std::move(portal_properties),
            {std::move(popup)}
        ));
        scope.synthesized();
        if (level >= active_path.size()) break;
        const std::size_t selected = active_path[level];
        if (selected >= level_items->size() ||
            (*level_items)[selected].children.empty()) {
            break;
        }
        parent_path.push_back(selected);
        anchor = menu_row_key(key, parent_path);
        level_items = &(*level_items)[selected].children;
    }
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
    add(registry, "Menu", &menu_defaults, &menu_expand, {}, "Menu");
    // ContextMenu is an authoring alias with container-authored geometry; unlike Menu it does not
    // inherit trigger padding before canonicalization.
    add(registry, "ContextMenu", nullptr, &menu_expand, "Menu", "ContextMenu");
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
