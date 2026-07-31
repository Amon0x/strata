#include "ui/widget/inspection.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "ui/surface.hpp"

namespace strata::ui {
namespace {

using data::JsonValue;

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> entries) {
    return JsonValue(JsonValue::Object(entries));
}

[[nodiscard]] JsonValue visible_range(const VisibleRange& value) {
    return object({
        {"endIndexExclusive", JsonValue(static_cast<std::int64_t>(value.end_exclusive))},
        {"startIndex", JsonValue(static_cast<std::int64_t>(value.start))},
    });
}

} // namespace

WidgetInspectionScope::WidgetInspectionScope(const Surface& surface, const RetainedNode& node,
                                             const LayoutRecord& layout)
    : surface_(surface), node_(node), layout_(layout), hit_bounds_(layout.hit_bounds) {
    const runtime::CollectionViewValue* view = nullptr;
    for (const auto& [name, property_value] : node.description().properties) {
        static_cast<void>(name);
        if (property_value.collection() != nullptr) {
            view = property_value.collection()->get();
            break;
        }
    }
    if (view == nullptr)
        return;
    const VisibleRange derived{view->range_start, view->range_end_exclusive};
    const VisibleRange materialized = layout.visible_range.value_or(derived);
    derived_collection_ = object({
        {"activeAnchor", JsonValue{}},
        {"cacheHits", JsonValue(static_cast<std::int64_t>(view->cache_hits.load()))},
        {"derivedRange", visible_range(derived)},
        {"matchCount", JsonValue(static_cast<std::int64_t>(view->matched))},
        {"materializedRange", visible_range(materialized)},
        {"operation", JsonValue(view->operation)},
        {"rebuilds", JsonValue(static_cast<std::int64_t>(view->rebuilds))},
        {"sourceCount", JsonValue(static_cast<std::int64_t>(view->total))},
    });
}

const Surface& WidgetInspectionScope::surface() const noexcept {
    return surface_;
}
const RetainedNode& WidgetInspectionScope::node() const noexcept {
    return node_;
}
const LayoutRecord& WidgetInspectionScope::layout() const noexcept {
    return layout_;
}

const runtime::Value* WidgetInspectionScope::property(const std::string_view name) const noexcept {
    const auto found = node_.description().properties.find(name);
    return found != node_.description().properties.end() ? found->second.value() : nullptr;
}

const runtime::Value* WidgetInspectionScope::retained(const std::string_view name) const noexcept {
    return node_.retained_value(name);
}

std::optional<std::string> WidgetInspectionScope::text(const runtime::Value* value) const {
    if (value == nullptr)
        return std::nullopt;
    if (value->string() != nullptr)
        return *value->string();
    if (value->key() != nullptr)
        return value->key()->value;
    return std::nullopt;
}

std::optional<bool> WidgetInspectionScope::boolean(const runtime::Value* value) const noexcept {
    return value != nullptr && value->boolean() != nullptr ? std::optional<bool>(*value->boolean())
                                                           : std::nullopt;
}

double WidgetInspectionScope::number(const std::string_view name,
                                     const double fallback) const noexcept {
    const runtime::Value* value = property(name);
    return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
               ? *value->number()
               : fallback;
}

bool WidgetInspectionScope::effective_boolean(const std::string_view controlled,
                                              const std::string_view retained_name,
                                              const std::string_view initial,
                                              const bool fallback) const noexcept {
    const runtime::Value* value = property(controlled);
    if (value == nullptr || value->boolean() == nullptr)
        value = retained(retained_name);
    if (value == nullptr || value->boolean() == nullptr)
        value = property(initial);
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

void WidgetInspectionScope::hit_bounds(const Rect value) noexcept {
    hit_bounds_ = value;
}
void WidgetInspectionScope::collection(JsonValue value) {
    collection_ = std::move(value);
}
void WidgetInspectionScope::derived_collection(JsonValue value) {
    derived_collection_ = std::move(value);
}

const Rect& WidgetInspectionScope::hit_bounds() const noexcept {
    return hit_bounds_;
}
const JsonValue& WidgetInspectionScope::collection() const noexcept {
    return collection_;
}
const JsonValue& WidgetInspectionScope::derived_collection() const noexcept {
    return derived_collection_;
}
bool WidgetInspectionScope::has_derived_collection() const noexcept {
    return !derived_collection_.is_null();
}

Rect widget_hit_bounds(const Surface& surface, const RetainedNode& node,
                       const LayoutRecord& layout) {
    WidgetInspectionScope scope(surface, node, layout);
    const WidgetLifecycle* lifecycle = surface.widget_registry().find(node.description().type);
    if (lifecycle != nullptr && lifecycle->inspection.derive != nullptr) {
        lifecycle->inspection.derive(scope);
    }
    return scope.hit_bounds();
}

} // namespace strata::ui
