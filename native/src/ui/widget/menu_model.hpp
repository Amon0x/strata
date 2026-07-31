#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/value.hpp"
#include "ui/layout.hpp"

namespace strata::ui {

class CommandIndex;
class RetainedNode;

struct MenuItemModel final {
    std::string id;
    std::string label;
    std::string command_id;
    std::string shortcut;
    bool enabled = true;
    bool separator = false;
    bool checked = false;
    bool has_checked = false;
    std::vector<MenuItemModel> children;
};

struct MenuPanelModel final {
    std::size_t level = 0U;
    std::vector<std::size_t> parent_path;
    Rect bounds;
};

struct MenuRowModel final {
    const MenuItemModel* item = nullptr;
    std::vector<std::size_t> path;
    std::size_t level = 0U;
    Rect bounds;
};

struct MenuProjection final {
    std::vector<MenuItemModel> items;
    std::vector<std::size_t> active_path;
    std::vector<MenuPanelModel> panels;
    double row_height = 26.0;
    double menu_width = 180.0;
    bool context = false;

    [[nodiscard]] std::vector<MenuRowModel> rows() const;
    [[nodiscard]] const MenuItemModel* item_at(const std::vector<std::size_t>& path) const;
    [[nodiscard]] const std::vector<MenuItemModel>* level_at(
        const std::vector<std::size_t>& parent_path
    ) const;
};

[[nodiscard]] std::vector<MenuItemModel> parse_menu_items(
    const runtime::Value* source,
    const CommandIndex* commands
);

[[nodiscard]] MenuProjection project_menu(
    const RetainedNode& node,
    const LayoutResult& layout,
    const CommandIndex* commands
);

[[nodiscard]] std::vector<std::size_t> menu_path(const RetainedNode& node);
[[nodiscard]] runtime::Value menu_path_value(const std::vector<std::size_t>& path);
[[nodiscard]] std::size_t first_enabled_menu_item(const std::vector<MenuItemModel>& items);
[[nodiscard]] std::string menu_row_identity(const std::vector<std::size_t>& path);

} // namespace strata::ui
