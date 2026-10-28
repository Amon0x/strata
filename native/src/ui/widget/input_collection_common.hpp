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

/** How a collection widget's items are authored; each widget declares its own. */
struct Shape final {
    /** List property holding the items. */
    std::string_view source = "entries";
    /** Items nest through parentKey; rows under a collapsed parent are not navigable. */
    bool hierarchical = false;
    /** Only items of kind ITEM are navigable (headers and separators are skipped). */
    bool items_only = false;
    /** Selection mode when selectionMode is not authored. */
    collection::SelectionMode default_selection = collection::SelectionMode::multiple;
};

struct Model final {
    std::vector<std::string> ordered;
    std::vector<collection::Label> labels;
    std::map<std::string, const runtime::Value*, std::less<>> items;
    collection::SelectionMode default_selection = collection::SelectionMode::multiple;
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
[[nodiscard]] Model model(WidgetInputScope& scope, const Shape& shape);
/** The authored selectionMode, else the fallback. */
[[nodiscard]] collection::SelectionMode selection_mode(WidgetInputScope& scope,
                                                       collection::SelectionMode fallback);
[[nodiscard]] collection::SelectionMode selection_mode(WidgetInputScope& scope, const Model& model);
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
