#include "ui/widget/input_collection_common.hpp"

#include <algorithm>
#include <cmath>

#include "ui/behavior/collection_marquee.hpp"

namespace strata::ui::collection_input {
namespace {

bool click(WidgetInputScope& scope) {
    return common_click(scope, model(scope));
}

bool key(WidgetInputScope& scope) {
    if (scope.key() == "escape" && cancel_collection_marquee(scope)) return true;
    const std::size_t columns = static_cast<std::size_t>(std::max(
        1.0,
        std::floor(scope.number("columns", 4.0))
    ));
    return common_key(scope, model(scope), NavigationConfig{
        .item_extent = scope.number("cellHeight", 88.0) + scope.number("gap", 6.0),
        .columns = columns,
        .two_dimensional = true,
        .banded = true,
    });
}

} // namespace

void register_item_grid_input(WidgetRegistry& registry) {
    WidgetInputPhase phase;
    phase.click = &click;
    phase.key = &key;
    phase.focusable = true;
    registry.register_input_phase("ItemGrid", std::move(phase));
}

} // namespace strata::ui::collection_input
