#include "ui/widget/inspection.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace strata::ui {
namespace {

using data::JsonValue;

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> entries) {
    return JsonValue(JsonValue::Object(entries));
}

[[nodiscard]] JsonValue array(std::vector<JsonValue> values = {}) {
    return JsonValue(JsonValue::Array(std::move(values)));
}

[[nodiscard]] JsonValue rectangle(const Rect value) {
    return object({
        {"height", JsonValue(value.height)},
        {"width", JsonValue(value.width)},
        {"x", JsonValue(value.x)},
        {"y", JsonValue(value.y)},
    });
}

[[nodiscard]] JsonValue visible_range(const VisibleRange& value) {
    return object({
        {"endIndexExclusive", JsonValue(static_cast<std::int64_t>(value.end_exclusive))},
        {"startIndex", JsonValue(static_cast<std::int64_t>(value.start))},
    });
}

void source_derived(
    WidgetInspectionScope& scope,
    const std::size_t total,
    JsonValue active_anchor = JsonValue{}
) {
    if (scope.has_derived_collection()) return;
    const VisibleRange materialized = scope.layout().visible_range.value_or(
        VisibleRange{0U, scope.node().children().size()}
    );
    scope.derived_collection(object({
        {"activeAnchor", std::move(active_anchor)},
        {"cacheHits", JsonValue(std::int64_t{0})},
        {"derivedRange", visible_range(VisibleRange{0U, total})},
        {"matchCount", JsonValue(static_cast<std::int64_t>(total))},
        {"materializedRange", visible_range(materialized)},
        {"operation", JsonValue("source")},
        {"rebuilds", JsonValue(std::int64_t{0})},
        {"sourceCount", JsonValue(static_cast<std::int64_t>(total))},
    }));
}

void virtual_list(WidgetInspectionScope& scope) {
    std::size_t total = scope.node().children().size();
    const runtime::Value* count = scope.property("itemCount");
    if (count != nullptr && count->number() != nullptr && *count->number() >= 0.0) {
        total = static_cast<std::size_t>(*count->number());
    }
    const VisibleRange materialized = scope.layout().visible_range.value_or(
        VisibleRange{0U, scope.node().children().size()}
    );
    JsonValue active_anchor;
    if (materialized.start < scope.node().description().children->size()) {
        const DescriptionNode& item =
            *scope.node().description().children->at(materialized.start);
        if (item.materialization_key.has_value()) {
            active_anchor = JsonValue(*item.materialization_key);
        } else if (item.key.has_value()) {
            active_anchor = JsonValue(*item.key);
        }
    }
    source_derived(scope, total, std::move(active_anchor));
}

[[nodiscard]] std::size_t source_count(
    WidgetInspectionScope& scope,
    const std::string_view property
) {
    const runtime::Value* source = scope.property(property);
    return source != nullptr && source->list() != nullptr ? source->list()->values.size() : 0U;
}

void collection(
    WidgetInspectionScope& scope,
    std::string selection_mode,
    std::vector<JsonValue> regions = {}
) {
    const std::optional<std::string> active = scope.text(
        scope.retained("strata.collection.active")
    );
    const std::optional<std::string> anchor = scope.text(
        scope.retained("strata.collection.anchor")
    );
    const std::optional<std::string> active_column = scope.text(
        scope.retained("strata.table.activeColumn")
    );
    const runtime::Value* selected = scope.property("selectedKeys");
    if (selected == nullptr || selected->list() == nullptr) {
        selected = scope.retained("strata.collection.selected");
    }
    if (selected == nullptr || selected->list() == nullptr) {
        selected = scope.property("defaultSelectedKeys");
    }
    std::vector<JsonValue> selected_keys;
    if (selected != nullptr && selected->list() != nullptr) {
        selected_keys.reserve(selected->list()->values.size());
        for (const runtime::Value& value : selected->list()->values) {
            if (const std::optional<std::string> key = scope.text(&value); key.has_value()) {
                selected_keys.emplace_back(*key);
            }
        }
    }
    if (const auto authored = scope.text(scope.property("selectionMode")); authored.has_value()) {
        selection_mode = *authored;
        std::ranges::transform(
            selection_mode,
            selection_mode.begin(),
            [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            }
        );
    }
    scope.collection(object({
        {"activeColumnId", active_column.has_value() ? JsonValue(*active_column) : JsonValue{}},
        {"activeKey", active.has_value() ? JsonValue(*active) : JsonValue{}},
        {"anchorKey", anchor.has_value() ? JsonValue(*anchor) : JsonValue{}},
        {"regions", array(std::move(regions))},
        {"selectedKeys", array(std::move(selected_keys))},
        {"selectionMode", JsonValue(std::move(selection_mode))},
    }));
}

void tree_view(WidgetInspectionScope& scope) {
    const runtime::Value* source = scope.property("items");
    if (!scope.has_derived_collection() && (source == nullptr || source->list() == nullptr)) return;
    source_derived(scope, source_count(scope, "items"));
    collection(scope, "single");
}

void table(WidgetInspectionScope& scope) {
    const runtime::Value* source = scope.property("rows");
    if (!scope.has_derived_collection() && (source == nullptr || source->list() == nullptr)) return;
    source_derived(scope, source_count(scope, "rows"));
    const Rect& bounds = scope.layout().bounds;
    std::vector<JsonValue> regions{
        object({
            {"bounds", rectangle(Rect{
                bounds.x,
                bounds.y,
                bounds.width,
                scope.number("headerHeight", 32.0),
            })},
            {"name", JsonValue("sticky-header")},
        }),
    };
    double pinned_width = 0.0;
    const runtime::Value* columns = scope.property("columns");
    if (columns != nullptr && columns->list() != nullptr) {
        for (const runtime::Value& column : columns->list()->values) {
            if (!scope.boolean(column.field("pinned")).value_or(false)) continue;
            const runtime::Value* width = column.field("width");
            pinned_width += width != nullptr && width->number() != nullptr
                                ? *width->number() : 120.0;
        }
    }
    if (pinned_width > 0.0) {
        regions.push_back(object({
            {"bounds", rectangle(Rect{bounds.x, bounds.y, pinned_width, bounds.height})},
            {"name", JsonValue("pinned-leading")},
        }));
    }
    collection(scope, "multiple", std::move(regions));
}

void item_grid(WidgetInspectionScope& scope) {
    const runtime::Value* source = scope.property("entries");
    if (!scope.has_derived_collection() && (source == nullptr || source->list() == nullptr)) return;
    source_derived(scope, source_count(scope, "entries"));
    collection(scope, "multiple");
}

} // namespace

void register_collection_widget_inspection(WidgetRegistry& registry) {
    registry.register_inspection_phase("VirtualList", WidgetInspectionPhase{&virtual_list});
    registry.register_inspection_phase("TreeView", WidgetInspectionPhase{&tree_view});
    registry.register_inspection_phase("Table", WidgetInspectionPhase{&table});
    registry.register_inspection_phase("ItemGrid", WidgetInspectionPhase{&item_grid});
}

} // namespace strata::ui
