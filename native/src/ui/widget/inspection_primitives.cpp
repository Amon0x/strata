#include "ui/widget/inspection.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace strata::ui {
namespace {

void section(WidgetInspectionScope& scope) {
    const Rect& bounds = scope.layout().bounds;
    scope.hit_bounds(Rect{
        bounds.x,
        bounds.y,
        bounds.width,
        std::min(32.0, bounds.height),
    });
}

void menu(WidgetInspectionScope& scope) {
    if (!scope.effective_boolean("open", "$expanded", "defaultOpen", false)) return;
    const runtime::Value* items = scope.property("items");
    if (items == nullptr || items->list() == nullptr) return;
    const Rect& bounds = scope.layout().bounds;
    scope.hit_bounds(Rect{
        bounds.x,
        bounds.y,
        std::max(bounds.width, scope.number("menuWidth", 180.0)),
        bounds.height + 2.0 +
            scope.number("rowHeight", 26.0) * static_cast<double>(items->list()->values.size()),
    });
}

void add(
    WidgetRegistry& registry,
    std::string type,
    const WidgetInspectionHook derive
) {
    registry.register_inspection_phase(
        std::move(type),
        WidgetInspectionPhase{derive}
    );
}

} // namespace

void register_primitive_widget_inspection(WidgetRegistry& registry) {
    add(registry, "Section", &section);
    add(registry, "Menu", &menu);
}

} // namespace strata::ui
