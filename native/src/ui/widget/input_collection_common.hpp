#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui/collection/model.hpp"
#include "ui/widget/input.hpp"

namespace strata::ui::collection_input {

struct Model final {
    std::vector<std::string> ordered;
    std::vector<collection::Label> labels;
    std::map<std::string, const runtime::Value*, std::less<>> items;
};

struct NavigationConfig final {
    double item_extent = 1.0;
    double leading_content_inset = 0.0;
    double sticky_viewport_inset = 0.0;
    std::size_t columns = 1U;
    bool two_dimensional = false;
    bool banded = false;
};

[[nodiscard]] const std::string* text(const runtime::Value* value) noexcept;
[[nodiscard]] runtime::Value key_value(const std::string& value);
[[nodiscard]] runtime::Value key_list(const collection::KeySet& values);
[[nodiscard]] collection::KeySet keys(const runtime::Value* value);
[[nodiscard]] collection::KeySet expanded(WidgetInputScope& scope);
[[nodiscard]] Model model(WidgetInputScope& scope);
[[nodiscard]] collection::SelectionMode selection_mode(WidgetInputScope& scope);
[[nodiscard]] collection::KeySet selected(WidgetInputScope& scope);
[[nodiscard]] std::optional<std::string> retained_key(
    WidgetInputScope& scope,
    std::string_view name
);
[[nodiscard]] std::pair<std::string, RetainedNode*> pointer_item(
    WidgetInputScope& scope,
    const Model& model
);

void apply_selection(
    WidgetInputScope& scope,
    const Model& model,
    const std::string& key,
    KeyModifiers modifiers
);
void commit_selection(
    WidgetInputScope& scope,
    const collection::KeySet& prior,
    const collection::SelectionTransition& transition
);
void activate_item(WidgetInputScope& scope, const std::string& key);
void context_item(WidgetInputScope& scope, const std::string& key);
[[nodiscard]] bool common_click(WidgetInputScope& scope, const Model& model);
[[nodiscard]] bool common_key(
    WidgetInputScope& scope,
    const Model& model,
    const NavigationConfig& config
);

void register_tree_input(WidgetRegistry& registry);
void register_table_input(WidgetRegistry& registry);
void register_item_grid_input(WidgetRegistry& registry);

} // namespace strata::ui::collection_input
