#include "ui/widget/input_collection_common.hpp"

#include <algorithm>
#include <set>

namespace strata::ui::collection_input {
namespace {

constexpr std::string_view drag_hover_key = "strata.tree.dragHover";
constexpr std::int64_t default_hover_expand_delay = 650'000'000;

[[nodiscard]] bool authored_expandable(const runtime::Value* item) noexcept {
    const runtime::Value* value = item != nullptr ? item->field("mayHaveChildren") : nullptr;
    return value != nullptr && value->boolean() != nullptr && *value->boolean();
}

[[nodiscard]] bool expandable(const Model& model, const std::string& key) noexcept {
    const auto found = model.items.find(key);
    if (found != model.items.end() && authored_expandable(found->second)) return true;
    return std::ranges::any_of(model.items, [&key](const auto& entry) {
        const std::string* parent = text(entry.second->field("parentKey"));
        return parent != nullptr && *parent == key;
    });
}

[[nodiscard]] std::size_t depth(const Model& model, const std::string& key) {
    std::size_t result = 0U;
    auto found = model.items.find(key);
    const std::string* parent = found != model.items.end()
        ? text(found->second->field("parentKey"))
        : nullptr;
    std::set<std::string, std::less<>> visited;
    while (parent != nullptr && visited.insert(*parent).second) {
        ++result;
        found = model.items.find(*parent);
        parent = found != model.items.end() ? text(found->second->field("parentKey")) : nullptr;
    }
    return result;
}

void set_expanded(
    WidgetInputScope& scope,
    const Model& model,
    const std::string& key,
    const bool next_expanded
) {
    const auto found = model.items.find(key);
    if (found == model.items.end() || !expandable(model, key)) return;
    collection::KeySet current = expanded(scope);
    if (current.contains(key) == next_expanded) return;
    if (next_expanded) static_cast<void>(current.insert(key));
    else static_cast<void>(current.erase(key));
    const runtime::Value* controlled = scope.property("expandedKeys");
    if (controlled == nullptr || controlled->list() == nullptr) {
        scope.set_retained("strata.tree.expanded", key_list(current), DirtyReason::structure);
    }
    scope.value_changed(
        "onExpand",
        "tree-expansion-changed",
        runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
            {"expanded", runtime::Value(next_expanded)},
            {"expandedKeys", key_list(current)},
            {"key", key_value(key)},
        })
    );
    const runtime::Value* loaded = found->second->field("childrenLoaded");
    if (next_expanded && loaded != nullptr && loaded->boolean() != nullptr && !*loaded->boolean()) {
        collection::KeySet loading = keys(scope.retained("strata.tree.loading"));
        static_cast<void>(loading.insert(key));
        scope.set_retained("strata.tree.loading", key_list(loading), DirtyReason::properties);
        scope.value_changed("onLoadChildren", "tree-children-requested", key_value(key));
    }
}

void toggle(WidgetInputScope& scope, const Model& model, const std::string& key) {
    set_expanded(scope, model, key, !expanded(scope).contains(key));
}

[[nodiscard]] runtime::Value drag_hover(
    const std::string& target,
    const std::int64_t started,
    const bool expansion_requested
) {
    return runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
        {"expansionRequested", runtime::Value(expansion_requested)},
        {"startedAt", runtime::Value(static_cast<double>(started))},
        {"targetKey", key_value(target)},
    });
}

bool advance(WidgetInputScope& scope) {
    const std::optional<WidgetDragInteraction> interaction = scope.active_drag();
    const runtime::Value* prior = scope.retained(drag_hover_key);
    const Model collection_model = model(scope);
    const std::string* target = interaction.has_value() && interaction->target_key.has_value()
        ? &*interaction->target_key
        : nullptr;
    const bool eligible = target != nullptr && interaction->placement == "on" &&
        expandable(collection_model, *target) && !expanded(scope).contains(*target);
    if (!eligible) {
        if (prior != nullptr && prior->object() != nullptr) {
            scope.set_retained(std::string(drag_hover_key), runtime::Value{}, DirtyReason::input);
            return true;
        }
        return false;
    }
    const std::string* prior_target = prior != nullptr ? text(prior->field("targetKey")) : nullptr;
    const std::int64_t now = scope.frame_time_nanos();
    if (prior_target == nullptr || *prior_target != *target) {
        scope.set_retained(
            std::string(drag_hover_key),
            drag_hover(*target, now, false),
            DirtyReason::input
        );
        return true;
    }
    const runtime::Value* requested = prior->field("expansionRequested");
    if (requested != nullptr && requested->boolean() != nullptr && *requested->boolean()) {
        return true;
    }
    const runtime::Value* started_value = prior->field("startedAt");
    const std::int64_t started = started_value != nullptr && started_value->number() != nullptr
        ? static_cast<std::int64_t>(*started_value->number())
        : now;
    const std::int64_t delay = std::max<std::int64_t>(
        0,
        scope.duration_nanos("hoverExpandDelay", default_hover_expand_delay)
    );
    if (now < started || now - started < delay) return true;
    scope.set_retained(
        std::string(drag_hover_key),
        drag_hover(*target, started, true),
        DirtyReason::input
    );
    set_expanded(scope, collection_model, *target, true);
    return true;
}

[[nodiscard]] bool disclosure_hit(
    WidgetInputScope& scope,
    const Model& model,
    const std::string& key,
    RetainedNode* item
) {
    const PointerInputEvent* pointer = scope.pointer();
    const LayoutRecord* layout = item != nullptr ? scope.layout(*item) : nullptr;
    const auto found = model.items.find(key);
    if (pointer == nullptr || layout == nullptr || found == model.items.end() ||
        !expandable(model, key)) {
        return false;
    }
    return pointer->position.x <= layout->bounds.x +
        static_cast<double>(depth(model, key)) * scope.number("indent", 18.0) +
        scope.number("disclosureSize", 10.0) + 7.0;
}

bool click(WidgetInputScope& scope) {
    const PointerInputEvent* pointer = scope.pointer();
    const Model collection_model = model(scope);
    const auto [key, item] = pointer_item(scope, collection_model);
    if (key.empty()) return false;
    if (pointer != nullptr && pointer->button == 1) {
        context_item(scope, key);
        return true;
    }
    if (pointer != nullptr && pointer->button != 0) return false;
    apply_selection(
        scope,
        collection_model,
        key,
        pointer != nullptr ? pointer->modifiers : scope.modifiers()
    );
    if (disclosure_hit(scope, collection_model, key, item) || scope.click_count() == 2U) {
        toggle(scope, collection_model, key);
    }
    if (scope.click_count() == 2U) activate_item(scope, key);
    return true;
}

bool key(WidgetInputScope& scope) {
    const Model collection_model = model(scope);
    const std::optional<std::string> active = retained_key(scope, "strata.collection.active");
    if (scope.key() == "enter" && active.has_value()) {
        toggle(scope, collection_model, *active);
        activate_item(scope, *active);
        return true;
    }
    if (active.has_value() && (scope.key() == "left" || scope.key() == "right")) {
        const auto found = collection_model.items.find(*active);
        const bool open = expanded(scope).contains(*active);
        if (scope.key() == "right" && found != collection_model.items.end() &&
            expandable(collection_model, *active)) {
            if (!open) {
                toggle(scope, collection_model, *active);
            } else {
                const auto child = std::ranges::find_if(
                    collection_model.ordered,
                    [&collection_model, &active](const std::string& candidate) {
                        const auto item = collection_model.items.find(candidate);
                        const std::string* parent = item != collection_model.items.end()
                            ? text(item->second->field("parentKey"))
                            : nullptr;
                        return parent != nullptr && *parent == *active;
                    }
                );
                if (child != collection_model.ordered.end()) {
                    apply_selection(scope, collection_model, *child, scope.modifiers());
                }
            }
            return true;
        }
        if (scope.key() == "left") {
            if (open) {
                toggle(scope, collection_model, *active);
            } else if (found != collection_model.items.end()) {
                if (const std::string* parent = text(found->second->field("parentKey"));
                    parent != nullptr) {
                    apply_selection(scope, collection_model, *parent, scope.modifiers());
                }
            }
            return true;
        }
    }
    return common_key(scope, collection_model, NavigationConfig{
        .item_extent = scope.number("rowHeight", 28.0),
    });
}

} // namespace

void register_tree_input(WidgetRegistry& registry) {
    WidgetInputPhase phase;
    phase.advance = &advance;
    phase.click = &click;
    phase.key = &key;
    phase.focusable = true;
    registry.register_input_phase("TreeView", std::move(phase));
}

} // namespace strata::ui::collection_input
