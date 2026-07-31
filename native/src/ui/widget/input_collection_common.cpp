#include "ui/widget/input_collection_common.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace strata::ui::collection_input {
namespace {

[[nodiscard]] const runtime::Value* source(WidgetInputScope& scope) noexcept {
    if (scope.node().description().type == "TreeView") return scope.property("items");
    if (scope.node().description().type == "Table") return scope.property("rows");
    return scope.property("entries");
}

void emit_selection(
    WidgetInputScope& scope,
    const collection::SelectionTransition& transition
) {
    scope.value_changed(
        "onSelect",
        "collection-selection-changed",
        runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
            {"activeKey", transition.active.has_value()
                ? key_value(*transition.active) : runtime::Value{}},
            {"anchorKey", transition.anchor.has_value()
                ? key_value(*transition.anchor) : runtime::Value{}},
            {"selectedKeys", key_list(transition.selected)},
        })
    );
}

[[nodiscard]] collection::SelectionModifiers selection_modifiers(
    const KeyModifiers modifiers
) noexcept {
    return collection::SelectionModifiers{
        modifiers.control || modifiers.super_key,
        modifiers.shift,
    };
}

[[nodiscard]] std::optional<collection::Navigation> navigation_for(
    const std::string_view key,
    const bool two_dimensional
) noexcept {
    if (key == "up") return collection::Navigation::previous;
    if (key == "down") return collection::Navigation::next;
    if (key == "home") return collection::Navigation::first;
    if (key == "end") return collection::Navigation::last;
    if (key == "pageup") return collection::Navigation::page_previous;
    if (key == "pagedown") return collection::Navigation::page_next;
    if (two_dimensional && key == "left") return collection::Navigation::column_previous;
    if (two_dimensional && key == "right") return collection::Navigation::column_next;
    return std::nullopt;
}

[[nodiscard]] bool controlled_selection(WidgetInputScope& scope) noexcept {
    const runtime::Value* value = scope.property("selectedKeys");
    return value != nullptr && value->list() != nullptr;
}

void apply_transition(
    WidgetInputScope& scope,
    const collection::KeySet& prior,
    const collection::SelectionTransition& transition
) {
    if (transition.active.has_value()) {
        scope.set_retained(
            "strata.collection.active",
            key_value(*transition.active),
            DirtyReason::semantics
        );
    }
    if (transition.anchor.has_value()) {
        scope.set_retained(
            "strata.collection.anchor",
            key_value(*transition.anchor),
            DirtyReason::semantics
        );
    } else {
        scope.set_retained("strata.collection.anchor", runtime::Value{}, DirtyReason::semantics);
    }
    if (!controlled_selection(scope)) {
        scope.set_retained(
            "strata.collection.selected",
            key_list(transition.selected),
            DirtyReason::properties
        );
    }
    if (transition.selected != prior) emit_selection(scope, transition);
}

void reveal(
    WidgetInputScope& scope,
    const Model& model,
    const std::string& key,
    const NavigationConfig& config
) {
    const auto found = std::ranges::find(model.ordered, key);
    if (found == model.ordered.end()) return;
    const LayoutRecord* layout = scope.layout();
    if (layout != nullptr && layout->virtual_item_extents.has_value()) {
        std::optional<std::size_t> virtual_index;
        if (layout->virtual_items != nullptr) {
            virtual_index = layout->virtual_items->index_of_key(key);
        } else if (const auto direct = std::ranges::find(layout->virtual_item_keys, key);
                   direct != layout->virtual_item_keys.end()) {
            virtual_index = layout->virtual_item_key_start +
                static_cast<std::size_t>(direct - layout->virtual_item_keys.begin());
        }
        if (!virtual_index.has_value() && layout->virtual_item_members != nullptr) {
            for (std::size_t index = 0U;
                 index < layout->virtual_item_members->size(); ++index) {
                if (std::ranges::contains((*layout->virtual_item_members)[index], key)) {
                    virtual_index = index;
                    break;
                }
            }
        }
        if (virtual_index.has_value() &&
            *virtual_index < layout->virtual_item_extents->size()) {
            const double start = config.leading_content_inset +
                layout->virtual_item_extents->start(*virtual_index);
            static_cast<void>(scope.reveal_vertical(
                start,
                start + layout->virtual_item_extents->extent(*virtual_index),
                config.sticky_viewport_inset
            ));
            return;
        }
    }
    const std::size_t item_index = static_cast<std::size_t>(found - model.ordered.begin());
    const std::size_t position = config.banded
        ? item_index / std::max<std::size_t>(1U, config.columns)
        : item_index;
    const double start = config.leading_content_inset +
        static_cast<double>(position) * config.item_extent;
    static_cast<void>(scope.reveal_vertical(
        start,
        start + config.item_extent,
        config.sticky_viewport_inset
    ));
}

[[nodiscard]] bool navigate(
    WidgetInputScope& scope,
    const Model& model,
    const collection::Navigation navigation,
    const NavigationConfig& config
) {
    const LayoutRecord* layout = scope.layout();
    const double viewport_height = layout != nullptr && layout->viewport.has_value()
        ? layout->viewport->height
        : config.item_extent;
    const double usable = std::max(
        config.item_extent,
        viewport_height - config.sticky_viewport_inset
    );
    const std::size_t rows = std::max<std::size_t>(
        1U,
        static_cast<std::size_t>(std::floor(usable / std::max(1.0, config.item_extent)))
    );
    const std::optional<std::string> next = collection::navigate(
        model.ordered,
        retained_key(scope, "strata.collection.active"),
        navigation,
        rows * std::max<std::size_t>(1U, config.columns),
        config.columns
    );
    if (!next.has_value()) return true;
    apply_selection(scope, model, *next, scope.modifiers());
    reveal(scope, model, *next, config);
    return true;
}

} // namespace

const std::string* text(const runtime::Value* const value) noexcept {
    if (value == nullptr) return nullptr;
    if (value->string() != nullptr) return value->string();
    return value->key() != nullptr ? &value->key()->value : nullptr;
}

runtime::Value key_value(const std::string& value) {
    return runtime::Value(runtime::KeyValue{value});
}

runtime::Value key_list(const collection::KeySet& values) {
    std::vector<runtime::Value> encoded;
    encoded.reserve(values.size());
    for (const std::string& value : values.values()) encoded.push_back(key_value(value));
    return runtime::Value(std::move(encoded));
}

collection::KeySet keys(const runtime::Value* value) {
    collection::KeySet result;
    if (value == nullptr || value->list() == nullptr) return result;
    for (const runtime::Value& entry : value->list()->values) {
        if (const std::string* key = text(&entry); key != nullptr) {
            static_cast<void>(result.insert(*key));
        }
    }
    return result;
}

collection::KeySet expanded(WidgetInputScope& scope) {
    const runtime::Value* value = scope.property("expandedKeys");
    if (value == nullptr || value->list() == nullptr) value = scope.retained("strata.tree.expanded");
    if (value == nullptr || value->list() == nullptr) value = scope.property("defaultExpandedKeys");
    return keys(value);
}

Model model(WidgetInputScope& scope) {
    Model result;
    const runtime::Value* values = source(scope);
    if (values == nullptr || values->list() == nullptr) return result;
    const bool tree = scope.node().description().type == "TreeView";
    const bool grid = scope.node().description().type == "ItemGrid";
    const collection::KeySet open = tree ? expanded(scope) : collection::KeySet{};
    std::map<std::string, std::optional<std::string>, std::less<>> parents;
    for (const runtime::Value& item : values->list()->values) {
        const std::string* key = text(item.field("key"));
        if (key == nullptr || key->empty()) continue;
        result.items.insert_or_assign(*key, &item);
        const std::string* parent = text(item.field("parentKey"));
        parents.insert_or_assign(
            *key,
            parent != nullptr ? std::optional<std::string>(*parent) : std::nullopt
        );
    }
    for (const runtime::Value& item : values->list()->values) {
        const std::string* key = text(item.field("key"));
        if (key == nullptr || key->empty()) continue;
        const runtime::Value* enabled = item.field("enabled");
        if (enabled != nullptr && enabled->boolean() != nullptr && !*enabled->boolean()) continue;
        if (grid) {
            const std::string* kind = text(item.field("kind"));
            if (kind != nullptr && *kind != "ITEM") continue;
        }
        bool visible = true;
        if (tree) {
            std::optional<std::string> parent = parents[*key];
            std::set<std::string, std::less<>> visited;
            while (parent.has_value() && visited.insert(*parent).second) {
                if (!open.contains(*parent)) {
                    visible = false;
                    break;
                }
                const auto found = parents.find(*parent);
                parent = found != parents.end() ? found->second : std::nullopt;
            }
        }
        if (!visible) continue;
        result.ordered.push_back(*key);
        const std::string* label = text(item.field("label"));
        result.labels.push_back(collection::Label{*key, label != nullptr ? *label : *key});
    }
    return result;
}

collection::SelectionMode selection_mode(WidgetInputScope& scope) {
    std::string value = scope.node().description().type == "TreeView" ? "SINGLE" : "MULTIPLE";
    if (const std::string* authored = text(scope.property("selectionMode")); authored != nullptr) {
        value = *authored;
    }
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    if (value == "NONE") return collection::SelectionMode::none;
    if (value == "MULTIPLE") return collection::SelectionMode::multiple;
    return collection::SelectionMode::single;
}

collection::KeySet selected(WidgetInputScope& scope) {
    const runtime::Value* value = scope.property("selectedKeys");
    if (value == nullptr || value->list() == nullptr) value = scope.retained("strata.collection.selected");
    if (value == nullptr || value->list() == nullptr) value = scope.property("defaultSelectedKeys");
    return keys(value);
}

std::optional<std::string> retained_key(
    WidgetInputScope& scope,
    const std::string_view name
) {
    const std::string* value = text(scope.retained(name));
    return value != nullptr ? std::optional<std::string>(*value) : std::nullopt;
}

std::pair<std::string, RetainedNode*> pointer_item(
    WidgetInputScope& scope,
    const Model& collection_model
) {
    for (RetainedNode* current = scope.pointer_target();
         current != nullptr && current != &scope.node();
         current = current->parent()) {
        if (current->description().key.has_value() &&
            collection_model.items.contains(*current->description().key) &&
            std::ranges::contains(collection_model.ordered, *current->description().key)) {
            return {*current->description().key, current};
        }
    }
    return {};
}

void apply_selection(
    WidgetInputScope& scope,
    const Model& collection_model,
    const std::string& key,
    const KeyModifiers modifiers
) {
    const collection::KeySet prior = selected(scope);
    const collection::SelectionTransition transition = collection::select(
        collection_model.ordered,
        prior,
        retained_key(scope, "strata.collection.anchor"),
        key,
        selection_mode(scope),
        selection_modifiers(modifiers)
    );
    apply_transition(scope, prior, transition);
}

void commit_selection(
    WidgetInputScope& scope,
    const collection::KeySet& prior,
    const collection::SelectionTransition& transition
) {
    apply_transition(scope, prior, transition);
}

void activate_item(WidgetInputScope& scope, const std::string& key) {
    scope.value_changed(
        "onActivate",
        "collection-activated",
        runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
            {"key", key_value(key)},
        })
    );
}

void context_item(WidgetInputScope& scope, const std::string& key) {
    const PointerInputEvent* pointer = scope.pointer();
    scope.value_changed(
        "onContext",
        "collection-context-requested",
        runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
            {"key", key_value(key)},
            {"position", pointer != nullptr
                ? runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                      {"x", runtime::Value(pointer->position.x)},
                      {"y", runtime::Value(pointer->position.y)},
                  })
                : runtime::Value{}},
        })
    );
}

bool common_click(WidgetInputScope& scope, const Model& collection_model) {
    const PointerInputEvent* pointer = scope.pointer();
    const auto [key, item] = pointer_item(scope, collection_model);
    static_cast<void>(item);
    if (key.empty()) return false;
    if (pointer != nullptr && pointer->button == 1) {
        context_item(scope, key);
        return true;
    }
    if (pointer != nullptr && pointer->button != 0) return false;
    apply_selection(scope, collection_model, key, pointer != nullptr ? pointer->modifiers : scope.modifiers());
    if (scope.click_count() == 2U) activate_item(scope, key);
    return true;
}

bool common_key(
    WidgetInputScope& scope,
    const Model& collection_model,
    const NavigationConfig& config
) {
    if (scope.key() == "a" && (scope.modifiers().control || scope.modifiers().super_key) &&
        selection_mode(scope) == collection::SelectionMode::multiple) {
        const collection::KeySet prior = selected(scope);
        const collection::SelectionTransition transition = collection::select_all(
            collection_model.ordered,
            collection::SelectionMode::multiple
        );
        apply_transition(scope, prior, transition);
        return true;
    }
    const std::optional<std::string> active = retained_key(scope, "strata.collection.active");
    if (scope.key() == "enter" && active.has_value()) {
        activate_item(scope, *active);
        return true;
    }
    if (scope.key() == "menu" && active.has_value()) {
        context_item(scope, *active);
        return true;
    }
    if (const auto navigation = navigation_for(scope.key(), config.two_dimensional);
        navigation.has_value()) {
        return navigate(scope, collection_model, *navigation, config);
    }
    const bool character = scope.key().size() == 1U &&
        std::isalnum(static_cast<unsigned char>(scope.key().front())) != 0;
    if (!character || scope.modifiers().control || scope.modifiers().super_key || scope.modifiers().alt) {
        return false;
    }
    const runtime::Value* prior_time = scope.retained("strata.collection.typeahead.time");
    const std::int64_t last = prior_time != nullptr && prior_time->number() != nullptr
        ? static_cast<std::int64_t>(*prior_time->number())
        : 0;
    const bool continues = scope.frame_time_nanos() >= last &&
        scope.frame_time_nanos() - last <= 750'000'000;
    const std::string* prior_query = text(scope.retained("strata.collection.typeahead"));
    const std::string query = (continues && prior_query != nullptr ? *prior_query : std::string{}) +
        std::string(scope.key());
    scope.set_retained("strata.collection.typeahead", runtime::Value(query), DirtyReason::input);
    scope.set_retained(
        "strata.collection.typeahead.time",
        runtime::Value(static_cast<double>(scope.frame_time_nanos())),
        DirtyReason::input
    );
    const std::optional<std::string> match = collection::typeahead(
        collection_model.labels,
        active,
        query
    );
    if (!match.has_value()) return true;
    apply_selection(scope, collection_model, *match, scope.modifiers());
    reveal(scope, collection_model, *match, config);
    return true;
}

} // namespace strata::ui::collection_input
