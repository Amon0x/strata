#include "ui/widget/description_collection_common.hpp"

namespace strata::ui {

void register_collection_widget_descriptions(WidgetRegistry& registry) {
    collection_description::register_collection_container_descriptions(registry);
    collection_description::register_tree_description(registry);
    collection_description::register_table_description(registry);
    collection_description::register_item_grid_description(registry);
}

} // namespace strata::ui
