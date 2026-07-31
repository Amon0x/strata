#include "ui/widget/input_collection_common.hpp"

namespace strata::ui {

void register_collection_widget_inputs(WidgetRegistry& registry) {
    collection_input::register_tree_input(registry);
    collection_input::register_table_input(registry);
    collection_input::register_item_grid_input(registry);
}

} // namespace strata::ui
