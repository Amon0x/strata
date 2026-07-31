#include "ui/widget/semantics.hpp"

#include <string>
#include <utility>

#include "ui/collection/table_model.hpp"

namespace strata::ui {
namespace {

void label_or_key(WidgetSemanticsScope& scope) {
    if (const auto label = scope.text(scope.property("label")); label.has_value()) {
        scope.name(*label);
    } else if (scope.node().description().key.has_value()) {
        scope.default_name(*scope.node().description().key);
    }
}

void empty_value(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    scope.default_value_text(std::string{});
}

void table(WidgetSemanticsScope& scope) {
    empty_value(scope);
    const std::vector<collection::TableColumn> columns = collection::resolve_table_columns(
        scope.property("columns"),
        scope.property("columnWidths"),
        scope.retained("strata.table.columnWidths"),
        scope.property("defaultColumnWidths"),
        scope.property("columnOrder"),
        scope.retained("strata.table.columnOrder"),
        scope.property("defaultColumnOrder"),
        0.0
    );
    for (std::size_t index = 0U; index < columns.size(); ++index) {
        scope.virtual_before(scope.virtual_item(
            index,
            2'000'000U,
            "column_header",
            columns[index].header
        ));
    }
}

void grid(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    scope.actions({});
}

void add(
    WidgetRegistry& registry,
    std::string type,
    std::string role,
    const WidgetSemanticsHook derive = nullptr
) {
    registry.register_semantics_phase(
        std::move(type),
        WidgetSemanticsPhase{std::move(role), {}, derive, false, false}
    );
}

} // namespace

void register_collection_widget_semantics(WidgetRegistry& registry) {
    add(registry, "Scroll", "group", &label_or_key);
    add(registry, "VirtualList", "list", &label_or_key);
    add(registry, "Tree", "tree", &empty_value);
    add(registry, "TreeView", "tree", &empty_value);
    add(registry, "Table", "table", &table);
    add(registry, "Grid", "grid", &grid);
    add(registry, "ItemGrid", "grid", &empty_value);
}

} // namespace strata::ui
