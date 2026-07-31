#include "ui/widget/input.hpp"
#include "ui/widget/menu_model.hpp"

#include <algorithm>
#include <ranges>
#include <utility>
#include <vector>

namespace strata::ui {
namespace {

bool text_selection_click(WidgetInputScope&) { return false; }

bool rich_text_click(WidgetInputScope& scope) {
    const std::vector<RichTextLink> links = rich_text_links(scope.node());
    if (links.empty()) return false;
    std::size_t selected = 0U;
    if (scope.pointer() != nullptr) {
        const WidgetSubtarget* target = scope.subtarget();
        if (target == nullptr || target->kind != WidgetSubtargetKind::link ||
            target->index >= links.size()) {
            return false;
        }
        selected = target->index;
    } else if (const runtime::Value* active = scope.retained("$activeLink");
               active != nullptr && active->number() != nullptr && *active->number() >= 0.0) {
        selected = std::min(
            static_cast<std::size_t>(*active->number()), links.size() - 1U
        );
    }
    scope.set_retained(
        "$activeLink", runtime::Value(static_cast<double>(selected)), DirtyReason::input
    );
    scope.dispatch_action(links[selected].action, "rich-text-link-activated");
    return true;
}

bool rich_text_key(WidgetInputScope& scope) {
    const std::vector<RichTextLink> links = rich_text_links(scope.node());
    if (links.empty()) return false;
    std::size_t selected = 0U;
    if (const runtime::Value* active = scope.retained("$activeLink");
        active != nullptr && active->number() != nullptr && *active->number() >= 0.0) {
        selected = std::min(
            static_cast<std::size_t>(*active->number()), links.size() - 1U
        );
    }
    if (scope.key() == "left" || scope.key() == "up") {
        selected = selected == 0U ? links.size() - 1U : selected - 1U;
    } else if (scope.key() == "right" || scope.key() == "down") {
        selected = (selected + 1U) % links.size();
    } else if (scope.key() == "enter" || scope.key() == "space") {
        scope.dispatch_action(links[selected].action, "rich-text-link-activated");
        return true;
    } else {
        return false;
    }
    scope.set_retained(
        "$activeLink", runtime::Value(static_cast<double>(selected)), DirtyReason::input
    );
    return true;
}

bool activate(WidgetInputScope& scope) {
    scope.activated("onClick");
    return true;
}

bool section_toggle(WidgetInputScope& scope) {
    if (scope.pointer() != nullptr && scope.pointer_target() != &scope.node()) return false;
    const bool next = !scope.effective_boolean(
        "expanded", "$expanded", "defaultExpanded", false
    );
    scope.set_retained("$expanded", runtime::Value(next), DirtyReason::properties);
    scope.boolean_changed({}, next);
    return true;
}

bool list_select(WidgetInputScope& scope) {
    const WidgetSubtarget* target = scope.subtarget();
    if (target == nullptr || target->kind != WidgetSubtargetKind::choice || !target->enabled) {
        return false;
    }
    if (scope.property("selectedKey") == nullptr) {
        scope.set_retained("$selectedKey", target->value, DirtyReason::properties);
    }
    scope.value_changed("onSelect", "selection-changed", target->value);
    return true;
}

[[nodiscard]] bool context_menu(const WidgetInputScope& scope) {
    return scope.string("$authoringType") == "ContextMenu";
}

void set_menu_path(WidgetInputScope& scope, const std::vector<std::size_t>& path) {
    scope.set_retained("$menuPath", menu_path_value(path), DirtyReason::input);
}

void close_menu(WidgetInputScope& scope) {
    scope.set_retained("$expanded", runtime::Value(false), DirtyReason::properties);
    set_menu_path(scope, {});
}

[[nodiscard]] std::optional<MenuProjection> menu_projection(WidgetInputScope& scope) {
    const LayoutResult* layout = scope.layout_result();
    if (layout == nullptr) return std::nullopt;
    return project_menu(scope.node(), *layout, scope.command_index());
}

[[nodiscard]] std::size_t adjacent_menu_item(
    const std::vector<MenuItemModel>& items,
    const std::size_t current,
    const int direction
) {
    if (items.empty()) return 0U;
    std::size_t index = std::min(current, items.size() - 1U);
    for (std::size_t attempt = 0U; attempt < items.size(); ++attempt) {
        index = direction < 0
            ? index == 0U ? items.size() - 1U : index - 1U
            : (index + 1U) % items.size();
        if (items[index].enabled && !items[index].separator) return index;
    }
    return current;
}

[[nodiscard]] bool activate_menu_target(
    WidgetInputScope& scope,
    const WidgetSubtarget& target
) {
    if (!target.enabled || target.separator) return false;
    if (target.has_children) {
        std::optional<MenuProjection> projection = menu_projection(scope);
        std::vector<std::size_t> next = target.path;
        if (projection.has_value()) {
            const MenuItemModel* item = projection->item_at(target.path);
            if (item != nullptr && !item->children.empty()) {
                next.push_back(first_enabled_menu_item(item->children));
            }
        }
        set_menu_path(scope, next);
        return true;
    }
    bool handled = false;
    if (target.kind == WidgetSubtargetKind::command && !target.command_id.empty()) {
        handled = scope.invoke_command(target.command_id);
    } else {
        scope.value_changed("onSelect", "selection-changed", target.value);
        handled = true;
    }
    close_menu(scope);
    return handled;
}

bool menu_pointer(WidgetInputScope& scope) {
    const PointerInputEvent* pointer = scope.pointer();
    if (pointer == nullptr) return false;
    if (context_menu(scope) && pointer->type == PointerEventType::press && pointer->button == 1) {
        scope.set_retained("$menuAnchorX", runtime::Value(pointer->position.x), DirtyReason::input);
        scope.set_retained("$menuAnchorY", runtime::Value(pointer->position.y), DirtyReason::input);
        scope.set_retained("$expanded", runtime::Value(true), DirtyReason::properties);
        if (std::optional<MenuProjection> projection = menu_projection(scope);
            projection.has_value() && !projection->items.empty()) {
            set_menu_path(scope, {first_enabled_menu_item(projection->items)});
        }
        scope.consume();
        return true;
    }
    const WidgetSubtarget* target = scope.subtarget();
    if (pointer->type == PointerEventType::move &&
        scope.effective_boolean("open", "$expanded", "defaultOpen", false) &&
        target != nullptr && !target->path.empty() && target->enabled && !target->separator) {
        set_menu_path(scope, target->path);
        return true;
    }
    return false;
}

bool menu_activate(WidgetInputScope& scope) {
    const WidgetSubtarget* target = scope.subtarget();
    if (target == nullptr || !target->enabled) return false;
    if (target->kind == WidgetSubtargetKind::control) {
        if (context_menu(scope)) return false;
        const bool next = !scope.effective_boolean("open", "$expanded", "defaultOpen", false);
        scope.set_retained("$expanded", runtime::Value(next), DirtyReason::properties);
        if (next) {
            if (std::optional<MenuProjection> projection = menu_projection(scope);
                projection.has_value() && !projection->items.empty()) {
                set_menu_path(scope, {first_enabled_menu_item(projection->items)});
            }
        } else {
            set_menu_path(scope, {});
        }
        return true;
    }
    return activate_menu_target(scope, *target);
}

bool menu_key(WidgetInputScope& scope) {
    const bool open = scope.effective_boolean("open", "$expanded", "defaultOpen", false);
    if (scope.key() == "escape") {
        close_menu(scope);
        return true;
    }
    std::optional<MenuProjection> projection = menu_projection(scope);
    if (!projection.has_value() || projection->items.empty()) return false;
    if (!open) {
        if (scope.key() != "enter" && scope.key() != "space" && scope.key() != "down") {
            return false;
        }
        scope.set_retained("$expanded", runtime::Value(true), DirtyReason::properties);
        set_menu_path(scope, {first_enabled_menu_item(projection->items)});
        return true;
    }
    std::vector<std::size_t> path = projection->active_path;
    if (path.empty()) path.push_back(first_enabled_menu_item(projection->items));
    std::vector<std::size_t> parent(path.begin(), path.end() - 1);
    const std::vector<MenuItemModel>* level = projection->level_at(parent);
    if (level == nullptr || level->empty()) return false;
    path.back() = std::min(path.back(), level->size() - 1U);
    if (scope.key() == "up" || scope.key() == "down") {
        path.back() = adjacent_menu_item(*level, path.back(), scope.key() == "up" ? -1 : 1);
        set_menu_path(scope, path);
        return true;
    }
    if (scope.key() == "home" || scope.key() == "end") {
        std::size_t selected = path.back();
        for (std::size_t attempt = 0U; attempt < level->size(); ++attempt) {
            const std::size_t candidate = scope.key() == "home"
                ? attempt
                : level->size() - 1U - attempt;
            if ((*level)[candidate].enabled && !(*level)[candidate].separator) {
                selected = candidate;
                break;
            }
        }
        path.back() = selected;
        set_menu_path(scope, path);
        return true;
    }
    const MenuItemModel& item = (*level)[path.back()];
    if (scope.key() == "right" && !item.children.empty()) {
        path.push_back(first_enabled_menu_item(item.children));
        set_menu_path(scope, path);
        return true;
    }
    if (scope.key() == "left") {
        if (path.size() > 1U) {
            path.pop_back();
            set_menu_path(scope, path);
        }
        return true;
    }
    if (scope.key() == "enter" || scope.key() == "space") {
        const auto targets = scope.subtargets();
        const std::string id = menu_row_identity(path);
        const auto target = std::ranges::find(targets, id, &WidgetSubtarget::id);
        return target != targets.end() && activate_menu_target(scope, *target);
    }
    return false;
}

void add(
    WidgetRegistry& registry,
    std::string type,
    const bool focusable,
    const WidgetInputHook click = nullptr,
    std::string action_property = {},
    std::string fallback_action = {}
) {
    WidgetInputPhase phase;
    phase.click = click;
    phase.action_property = std::move(action_property);
    phase.fallback_action = std::move(fallback_action);
    phase.focusable = focusable;
    registry.register_input_phase(std::move(type), std::move(phase));
}

} // namespace

void register_primitive_widget_inputs(WidgetRegistry& registry) {
    add(registry, "Button", true, &activate, "onClick", "Button");
    add(registry, "Link", true, &activate, "onClick", "Link");
    add(registry, "List", true, &list_select, "onSelect", "List");
    add(registry, "Section", true, &section_toggle);

    WidgetInputPhase text;
    text.click = &text_selection_click;
    text.focusable = true;
    text.tabbable = false;
    text.text_edit_mode = WidgetTextEditMode::static_text;
    registry.register_input_phase("Text", std::move(text));

    WidgetInputPhase rich_text;
    rich_text.click = &rich_text_click;
    rich_text.key = &rich_text_key;
    rich_text.focusable = true;
    rich_text.tabbable = true;
    rich_text.text_edit_mode = WidgetTextEditMode::static_text;
    registry.register_input_phase("RichText", std::move(rich_text));

    WidgetInputPhase menu;
    menu.focusable = true;
    menu.click = &menu_activate;
    menu.pointer = &menu_pointer;
    menu.key = &menu_key;
    menu.action_property = "onSelect";
    menu.popup_controlled = "open";
    menu.popup_retained = "$expanded";
    menu.popup_initial = "defaultOpen";
    registry.register_input_phase("Menu", std::move(menu));
}

} // namespace strata::ui
