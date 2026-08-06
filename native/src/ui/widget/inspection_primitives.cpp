#include "ui/widget/inspection.hpp"

#include <algorithm>

#include "ui/surface.hpp"
#include <string>
#include <utility>

namespace strata::ui {
namespace {

void panel(WidgetInspectionScope& scope) {
    if (scope.boolean(scope.property("$inputTransparent")).value_or(false)) {
        scope.hit_bounds(Rect{});
    }
}

void section(WidgetInspectionScope& scope) {
    for (const auto& child : scope.node().children()) {
        const auto found = child->description().properties.find("$inputTransparent");
        const runtime::Value* marker = found != child->description().properties.end()
                                           ? found->second.value()
                                           : nullptr;
        if (marker == nullptr || marker->boolean() == nullptr || !*marker->boolean()) continue;
        if (const LayoutRecord* header = scope.surface().layout().find(child->identity());
            header != nullptr) {
            scope.hit_bounds(header->bounds);
            return;
        }
    }
    const Rect& bounds = scope.layout().bounds;
    scope.hit_bounds(Rect{
        bounds.x,
        bounds.y,
        bounds.width,
        std::min(std::max(24.0, scope.number("headerHeight", 36.0)), bounds.height),
    });
}

void menu(WidgetInspectionScope& scope) {
    // The retained Menu semantic represents its control. Popup rows have independently projected
    // subtargets, so folding their detached extent into the owner made key-based automation click
    // somewhere other than a custom trigger and obscured its measured hit geometry.
    const std::vector<WidgetSubtarget> targets =
        scope.surface().input().subtargets(scope.node().identity());
    const auto control = std::ranges::find(
        targets,
        std::string_view("$control"),
        &WidgetSubtarget::id
    );
    if (control != targets.end()) scope.hit_bounds(control->bounds);
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
    add(registry, "Panel", &panel);
    add(registry, "Section", &section);
    add(registry, "Menu", &menu);
}

} // namespace strata::ui
