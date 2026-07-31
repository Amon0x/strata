#include "ui/widget/semantics.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ui/widget/menu_model.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] std::optional<std::string> text(
    WidgetSemanticsScope& scope,
    const std::string_view property
) {
    return scope.text(scope.property(property));
}

void label_or_key(WidgetSemanticsScope& scope) {
    if (const auto label = text(scope, "label"); label.has_value()) {
        scope.name(*label);
    } else if (scope.node().description().key.has_value()) {
        scope.default_name(*scope.node().description().key);
    }
}

void panel(WidgetSemanticsScope& scope) {
    if (scope.explicit_field("role") == nullptr && scope.node().parent() != nullptr &&
        scope.node().parent()->description().type == "Table") {
        scope.role("row");
        scope.selected(false);
    }
    label_or_key(scope);
}

void text_node(WidgetSemanticsScope& scope) {
    if (scope.explicit_field("role") == nullptr &&
        (scope.has_ancestor("Table") || scope.has_ancestor("ItemGrid"))) {
        scope.role("grid_cell");
        scope.selected(false);
    }
    if (const auto label = text(scope, "label"); label.has_value()) scope.name(*label);
    else if (const auto value = text(scope, "text"); value.has_value()) scope.name(*value);
    else if (scope.node().description().key.has_value()) {
        scope.default_name(*scope.node().description().key);
    }
}

void rich_text(WidgetSemanticsScope& scope) {
    text_node(scope);
    const runtime::ExpressionValue* expression = scope.expression_property("spans");
    const runtime::Value* spans = scope.property("spans");
    if (spans == nullptr || spans->list() == nullptr) return;
    for (std::size_t index = 0U; index < spans->list()->values.size(); ++index) {
        const runtime::Value& span = spans->list()->values[index];
        bool has_action = false;
        if (expression != nullptr && expression->list() != nullptr &&
            index < (**expression->list()).values.size()) {
            const runtime::ExpressionValue& entry = (**expression->list()).values[index];
            if (entry.object() != nullptr) {
                const runtime::ExpressionValue* action = (**entry.object()).field("action");
                has_action = action != nullptr && action->action() != nullptr &&
                             *action->action() != nullptr;
            }
        } else {
            has_action = span.field("action") != nullptr;
        }
        if (!has_action) continue;
        scope.virtual_before(scope.virtual_item(
            index,
            1'000'000U,
            "link",
            scope.text(span.field("text")).value_or(std::string{}),
            {"activate", "focus"},
            std::nullopt,
            std::nullopt,
            false,
            index
        ));
    }
}

void list(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    scope.actions({"activate", "focus"});
}

[[nodiscard]] bool menu_path_prefix(
    const std::vector<std::size_t>& prefix,
    const std::vector<std::size_t>& active
) {
    return prefix.size() <= active.size() &&
           std::equal(prefix.begin(), prefix.end(), active.begin());
}

[[nodiscard]] data::JsonValue menu_semantic_item(
    WidgetSemanticsScope& scope,
    const MenuItemModel& item,
    const std::size_t index,
    const std::vector<std::size_t>& parent_path,
    const std::string& structural_parent,
    const bool root,
    const bool menu_open,
    const std::vector<std::size_t>& active_path
) {
    std::vector<std::size_t> path = parent_path;
    path.push_back(index);
    const std::string structural_path = structural_parent + "/" + std::to_string(
        (root ? 2'100'000U : 0U) + index
    );
    data::JsonValue::Array children;
    children.reserve(item.children.size());
    for (std::size_t child = 0U; child < item.children.size(); ++child) {
        children.push_back(menu_semantic_item(
            scope,
            item.children[child],
            child,
            path,
            structural_path,
            false,
            menu_open,
            active_path
        ));
    }
    const bool branch_expanded = menu_open && !item.children.empty() &&
        menu_path_prefix(path, active_path);
    std::vector<std::string> actions;
    if (item.enabled && !item.separator) {
        actions.push_back(item.children.empty()
            ? "activate"
            : branch_expanded ? "collapse" : "expand");
    }
    return scope.virtual_item(
        index,
        root ? 2'100'000U : 0U,
        item.separator ? "separator" : "menu_item",
        item.label,
        std::move(actions),
        item.has_checked ? std::optional<bool>(item.checked) : std::nullopt,
        std::nullopt,
        !item.enabled,
        std::nullopt,
        item.command_id.empty()
            ? std::nullopt
            : std::optional<std::string>(item.command_id),
        std::move(children),
        structural_parent,
        !item.children.empty() ? std::optional<bool>(branch_expanded) : std::nullopt
    );
}

void menu(WidgetSemanticsScope& scope) {
    if (const auto label = text(scope, "label"); label.has_value()) scope.name(*label);
    const bool expanded = scope.effective_boolean("open", "$expanded", "defaultOpen", false);
    scope.expanded(expanded);
    scope.actions({expanded ? "collapse" : "expand", "focus"});

    const std::vector<MenuItemModel> items = parse_menu_items(
        scope.property("items"), scope.command_index()
    );
    const std::vector<std::size_t> active = menu_path(scope.node());
    for (std::size_t index = 0U; index < items.size(); ++index) {
        scope.virtual_before(menu_semantic_item(
            scope,
            items[index],
            index,
            {},
            std::string(scope.node().structural_path()),
            true,
            expanded,
            active
        ));
    }
}

void section(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    const bool expanded = scope.effective_boolean(
        "expanded", "$expanded", "defaultExpanded", false
    );
    scope.expanded(expanded);
    scope.actions({expanded ? "collapse" : "expand", "focus"});
}

void status_bar(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    scope.live_region("polite");
}

void add(
    WidgetRegistry& registry,
    std::string type,
    std::string role,
    const WidgetSemanticsHook derive = nullptr,
    const bool hidden = false,
    const bool transparent = false
) {
    registry.register_semantics_phase(
        std::move(type),
        WidgetSemanticsPhase{std::move(role), {}, derive, hidden, transparent}
    );
}

} // namespace

void register_primitive_widget_semantics(WidgetRegistry& registry) {
    add(registry, "Panel", "group", &panel, false, true);
    add(registry, "Slot", "group", &label_or_key);
    add(registry, "Text", "text", &text_node);
    add(registry, "RichText", "text", &rich_text);
    add(registry, "Button", "button", &label_or_key);
    add(registry, "Image", "image", &label_or_key);
    add(registry, "Draw", "image", &label_or_key);
    add(registry, "Link", "link", &label_or_key);
    add(registry, "List", "list", &list);
    add(registry, "Menu", "menu", &menu);
    add(registry, "Section", "group", &section);
    add(registry, "StatusBar", "status", &status_bar);
}

} // namespace strata::ui
