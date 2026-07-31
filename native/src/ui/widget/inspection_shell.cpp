#include "ui/widget/inspection.hpp"

#include <algorithm>

#include "ui/surface.hpp"

namespace strata::ui {
namespace {

void tooltip(WidgetInspectionScope& scope) {
    const runtime::Value* value = scope.property("text");
    if (scope.surface().text_engine() == nullptr || value == nullptr ||
        value->string() == nullptr) {
        return;
    }
    const font::ShapedText shaped = scope.surface().text_engine()->shape(
        scope.node(), *value->string()
    );
    const Rect& bounds = scope.layout().bounds;
    const double popup_width = shaped.metrics.width + 16.0;
    const double popup_height = shaped.metrics.height + 10.0;
    const double popup_x = bounds.x + (bounds.width - popup_width) * 0.5;
    scope.hit_bounds(Rect{
        std::min(bounds.x, popup_x),
        bounds.y,
        std::max(bounds.right(), popup_x + popup_width) - std::min(bounds.x, popup_x),
        bounds.height + 6.0 + popup_height,
    });
}

} // namespace

void register_shell_widget_inspection(WidgetRegistry& registry) {
    registry.register_inspection_phase("Tooltip", WidgetInspectionPhase{&tooltip});
}

} // namespace strata::ui
