#include "ui/widget/menu_model.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

#include "ui/command.hpp"
#include "ui/tree.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] const runtime::Value* property(
    const RetainedNode& node,
    const std::string_view name
) noexcept {
    const auto found = node.description().properties.find(name);
    return found != node.description().properties.end() ? found->second.data_value() : nullptr;
}

[[nodiscard]] const std::string* text(const runtime::Value* value) noexcept {
    if (value == nullptr) return nullptr;
    if (value->string() != nullptr) return value->string();
    return value->key() != nullptr ? &value->key()->value : nullptr;
}

[[nodiscard]] bool boolean(const runtime::Value* value, const bool fallback) noexcept {
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

[[nodiscard]] double number(const runtime::Value* value, const double fallback) noexcept {
    return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
               ? *value->number()
               : fallback;
}

[[nodiscard]] Rect viewport_bounds(const LayoutResult& layout, const Rect fallback) noexcept {
    const LayoutRecord* root = layout.find(layout.root_identity);
    return root != nullptr ? root->bounds : fallback;
}

[[nodiscard]] Rect place_root(
    const Rect anchor,
    const Rect viewport,
    const double width,
    const double height,
    const bool context
) noexcept {
    const double resolved_width = std::min(width, std::max(0.0, viewport.width));
    const double resolved_height = std::min(height, std::max(0.0, viewport.height));
    const double x = std::clamp(
        anchor.x,
        viewport.x,
        std::max(viewport.x, viewport.right() - resolved_width)
    );
    double y = context ? anchor.y : anchor.bottom() + 2.0;
    if (!context && y + resolved_height > viewport.bottom() &&
        anchor.y - 2.0 - resolved_height >= viewport.y) {
        y = anchor.y - 2.0 - resolved_height;
    }
    y = std::clamp(y, viewport.y, std::max(viewport.y, viewport.bottom() - resolved_height));
    return Rect{x, y, resolved_width, resolved_height};
}

[[nodiscard]] Rect place_submenu(
    const Rect row,
    const Rect viewport,
    const double width,
    const double height
) noexcept {
    const double resolved_width = std::min(width, std::max(0.0, viewport.width));
    const double resolved_height = std::min(height, std::max(0.0, viewport.height));
    double x = row.right();
    if (x + resolved_width > viewport.right()) x = row.x - resolved_width;
    x = std::clamp(x, viewport.x, std::max(viewport.x, viewport.right() - resolved_width));
    const double y = std::clamp(
        row.y,
        viewport.y,
        std::max(viewport.y, viewport.bottom() - resolved_height)
    );
    return Rect{x, y, resolved_width, resolved_height};
}

[[nodiscard]] std::vector<MenuItemModel> parse_level(
    const runtime::ValueList& values,
    const CommandIndex* commands
) {
    std::vector<MenuItemModel> result;
    result.reserve(values.values.size());
    for (std::size_t index = 0U; index < values.values.size(); ++index) {
        const runtime::Value& entry = values.values[index];
        const std::string* command_id = text(entry.field("command"));
        const std::string* authored_id = text(entry.field("id"));
        const std::string id = authored_id != nullptr && !authored_id->empty()
            ? *authored_id
            : command_id != nullptr && !command_id->empty()
                ? *command_id
                : std::to_string(index);
        const bool separator = boolean(entry.field("separator"), false);
        const CommandSnapshot* command = command_id != nullptr && commands != nullptr
            ? commands->find(*command_id)
            : nullptr;
        const std::string* authored_label = text(entry.field("label"));
        const runtime::Value* children = entry.field("children");
        MenuItemModel item;
        item.id = id;
        item.label = separator ? std::string{}
            : command != nullptr ? command->label
            : authored_label != nullptr && !authored_label->empty() ? *authored_label
            : id;
        item.command_id = command_id != nullptr ? *command_id : std::string{};
        item.shortcut = command != nullptr
            ? format_command_shortcut(*command)
            : text(entry.field("shortcutHint")) != nullptr
                ? *text(entry.field("shortcutHint"))
                : std::string{};
        item.enabled = !separator && boolean(entry.field("enabled"), true) &&
            (command_id == nullptr || command != nullptr) &&
            (command == nullptr || command->enabled);
        item.separator = separator;
        if (const runtime::Value* checked = entry.field("checked");
            checked != nullptr && checked->boolean() != nullptr) {
            item.checked = *checked->boolean();
            item.has_checked = true;
        } else if (command != nullptr && command->checked.has_value()) {
            item.checked = *command->checked;
            item.has_checked = true;
        }
        if (!separator && children != nullptr && children->list() != nullptr) {
            item.children = parse_level(*children->list(), commands);
        }
        result.push_back(std::move(item));
    }
    return result;
}

[[nodiscard]] bool context_menu(const RetainedNode& node) noexcept {
    const std::string* authoring = text(property(node, "$authoringType"));
    return authoring != nullptr && *authoring == "ContextMenu";
}

[[nodiscard]] Rect menu_anchor(
    const RetainedNode& node,
    const LayoutRecord& record,
    const bool context
) noexcept {
    if (!context) return record.bounds;
    const runtime::Value* x = node.retained_value("$menuAnchorX");
    const runtime::Value* y = node.retained_value("$menuAnchorY");
    return Rect{
        number(x, record.bounds.x),
        number(y, record.bounds.y),
        0.0,
        0.0,
    };
}

} // namespace

std::vector<MenuRowModel> MenuProjection::rows() const {
    std::vector<MenuRowModel> result;
    for (const MenuPanelModel& panel : panels) {
        const std::vector<MenuItemModel>* level = level_at(panel.parent_path);
        if (level == nullptr) continue;
        result.reserve(result.size() + level->size());
        for (std::size_t index = 0U; index < level->size(); ++index) {
            std::vector<std::size_t> path = panel.parent_path;
            path.push_back(index);
            result.push_back(MenuRowModel{
                &(*level)[index],
                std::move(path),
                panel.level,
                Rect{
                    panel.bounds.x,
                    panel.bounds.y + row_height * static_cast<double>(index),
                    panel.bounds.width,
                    row_height,
                },
            });
        }
    }
    return result;
}

const MenuItemModel* MenuProjection::item_at(
    const std::vector<std::size_t>& path
) const {
    if (path.empty()) return nullptr;
    const std::vector<MenuItemModel>* level = &items;
    const MenuItemModel* item = nullptr;
    for (const std::size_t index : path) {
        if (index >= level->size()) return nullptr;
        item = &(*level)[index];
        level = &item->children;
    }
    return item;
}

const std::vector<MenuItemModel>* MenuProjection::level_at(
    const std::vector<std::size_t>& parent_path
) const {
    const std::vector<MenuItemModel>* level = &items;
    for (const std::size_t index : parent_path) {
        if (index >= level->size()) return nullptr;
        level = &(*level)[index].children;
    }
    return level;
}

std::vector<MenuItemModel> parse_menu_items(
    const runtime::Value* source,
    const CommandIndex* commands
) {
    return source != nullptr && source->list() != nullptr
        ? parse_level(*source->list(), commands)
        : std::vector<MenuItemModel>{};
}

MenuProjection project_menu(
    const RetainedNode& node,
    const LayoutResult& layout,
    const CommandIndex* commands
) {
    MenuProjection result;
    const LayoutRecord* record = layout.find(node.identity());
    if (record == nullptr) return result;
    result.items = parse_menu_items(property(node, "items"), commands);
    result.active_path = menu_path(node);
    result.row_height = std::max(18.0, number(property(node, "rowHeight"), 26.0));
    result.menu_width = std::max(80.0, number(property(node, "menuWidth"), 180.0));
    result.context = context_menu(node);
    if (result.items.empty()) return result;

    const Rect viewport = viewport_bounds(layout, record->bounds);
    Rect panel = place_root(
        menu_anchor(node, *record, result.context),
        viewport,
        result.menu_width,
        result.row_height * static_cast<double>(result.items.size()),
        result.context
    );
    const std::vector<MenuItemModel>* level = &result.items;
    std::vector<std::size_t> parent_path;
    for (std::size_t depth = 0U; !level->empty(); ++depth) {
        result.panels.push_back(MenuPanelModel{depth, parent_path, panel});
        if (depth >= result.active_path.size()) break;
        const std::size_t selected = result.active_path[depth];
        if (selected >= level->size() || (*level)[selected].children.empty()) break;
        const Rect row{
            panel.x,
            panel.y + result.row_height * static_cast<double>(selected),
            panel.width,
            result.row_height,
        };
        parent_path.push_back(selected);
        level = &(*level)[selected].children;
        panel = place_submenu(
            row,
            viewport,
            result.menu_width,
            result.row_height * static_cast<double>(level->size())
        );
    }
    return result;
}

std::vector<std::size_t> menu_path(const RetainedNode& node) {
    const runtime::Value* value = node.retained_value("$menuPath");
    std::vector<std::size_t> result;
    if (value == nullptr || value->list() == nullptr) return result;
    result.reserve(value->list()->values.size());
    for (const runtime::Value& entry : value->list()->values) {
        if (entry.number() == nullptr || !std::isfinite(*entry.number()) ||
            *entry.number() < 0.0 ||
            *entry.number() > 1'000'000.0) {
            break;
        }
        result.push_back(static_cast<std::size_t>(*entry.number()));
    }
    return result;
}

runtime::Value menu_path_value(const std::vector<std::size_t>& path) {
    std::vector<runtime::Value> values;
    values.reserve(path.size());
    for (const std::size_t index : path) {
        values.emplace_back(static_cast<double>(index));
    }
    return runtime::Value(std::move(values));
}

std::size_t first_enabled_menu_item(const std::vector<MenuItemModel>& items) {
    const auto found = std::ranges::find_if(items, [](const MenuItemModel& item) {
        return item.enabled && !item.separator;
    });
    return found != items.end() ? static_cast<std::size_t>(found - items.begin()) : 0U;
}

std::string menu_row_identity(const std::vector<std::size_t>& path) {
    std::string result = "$menu";
    for (const std::size_t index : path) result += "/" + std::to_string(index);
    return result;
}

} // namespace strata::ui
