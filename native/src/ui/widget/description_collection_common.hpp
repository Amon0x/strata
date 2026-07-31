#pragma once

#include <set>
#include <string>
#include <string_view>

#include "ui/widget/description.hpp"

namespace strata::ui::collection_description {

[[nodiscard]] std::set<std::string, std::less<>> effective_keys(
    WidgetDescriptionScope& scope,
    std::string_view controlled,
    std::string_view retained,
    std::string_view defaults
);
[[nodiscard]] bool boolean(const runtime::Value* value, bool fallback = false) noexcept;
[[nodiscard]] std::shared_ptr<const DescriptionNode> with_layout_fields(
    const std::shared_ptr<const DescriptionNode>& node,
    std::initializer_list<std::pair<std::string, runtime::Value>> fields
);
[[nodiscard]] std::shared_ptr<const DescriptionNode> with_layout_padding(
    const std::shared_ptr<const DescriptionNode>& node,
    std::initializer_list<std::pair<std::string_view, double>> edges
);
[[nodiscard]] std::shared_ptr<const DescriptionNode> with_semantics(
    const std::shared_ptr<const DescriptionNode>& node,
    runtime::Value semantics
);
[[nodiscard]] std::shared_ptr<const DescriptionNode> with_behaviors(
    const std::shared_ptr<const DescriptionNode>& node,
    std::vector<DescriptionBehavior> behaviors
);

void register_collection_container_descriptions(WidgetRegistry& registry);
void register_tree_description(WidgetRegistry& registry);
void register_table_description(WidgetRegistry& registry);
void register_item_grid_description(WidgetRegistry& registry);

} // namespace strata::ui::collection_description
