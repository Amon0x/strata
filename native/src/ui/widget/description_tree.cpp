#include "ui/widget/description_collection_common.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace strata::ui::collection_description {
namespace {

struct TreeItem final {
    std::size_t source_index = 0U;
    std::string key;
    std::optional<std::string> parent;
};

struct TreeTopology final {
    runtime::Value source;
    std::vector<TreeItem> items;
    std::map<std::string, std::size_t, std::less<>> indices;
    std::map<std::string, std::vector<std::size_t>, std::less<>> children;
    std::vector<std::size_t> roots;
    std::vector<std::size_t> disconnected;
};

[[nodiscard]] std::shared_ptr<const TreeTopology> tree_topology(
    const runtime::Value& source
) {
    static std::mutex cache_mutex;
    static std::map<const void*, std::weak_ptr<const TreeTopology>> cache;
    const void* const identity = source.composite_identity();
    if (identity != nullptr) {
        const std::lock_guard lock(cache_mutex);
        const auto found = cache.find(identity);
        if (found != cache.end()) {
            if (std::shared_ptr<const TreeTopology> retained = found->second.lock()) {
                return retained;
            }
            cache.erase(found);
        }
    }

    auto result = std::make_shared<TreeTopology>();
    result->source = source;
    const runtime::ValueList* values = result->source.list();
    if (values == nullptr) return result;
    result->items.reserve(values->values.size());
    for (std::size_t source_index = 0U; source_index < values->values.size(); ++source_index) {
        const runtime::Value& item = values->values[source_index];
        const std::string* key = widget_description_string(item.field("key"));
        const std::string* parent = widget_description_string(item.field("parentKey"));
        if (key == nullptr || key->empty() || result->indices.contains(*key)) continue;
        result->indices.emplace(*key, result->items.size());
        result->items.push_back(TreeItem{
            source_index,
            *key,
            parent != nullptr && !parent->empty()
                ? std::optional<std::string>(*parent)
                : std::nullopt,
        });
    }
    for (std::size_t index = 0U; index < result->items.size(); ++index) {
        const TreeItem& item = result->items[index];
        if (item.parent.has_value() && result->indices.contains(*item.parent)) {
            result->children[*item.parent].push_back(index);
        } else {
            result->roots.push_back(index);
        }
    }

    std::set<std::string, std::less<>> reachable;
    std::vector<std::size_t> pending(result->roots.rbegin(), result->roots.rend());
    while (!pending.empty()) {
        const std::size_t index = pending.back();
        pending.pop_back();
        const TreeItem& item = result->items[index];
        if (!reachable.insert(item.key).second) continue;
        if (const auto found = result->children.find(item.key);
            found != result->children.end()) {
            pending.insert(pending.end(), found->second.rbegin(), found->second.rend());
        }
    }
    for (std::size_t index = 0U; index < result->items.size(); ++index) {
        if (!reachable.contains(result->items[index].key)) {
            result->disconnected.push_back(index);
        }
    }

    if (identity != nullptr) {
        const std::lock_guard lock(cache_mutex);
        if (cache.size() >= 64U) {
            std::erase_if(cache, [](const auto& entry) { return entry.second.expired(); });
        }
        cache.insert_or_assign(identity, result);
    }
    return result;
}

[[nodiscard]] std::shared_ptr<const DescriptionNode> tree_row_layout(
    const std::shared_ptr<const DescriptionNode>& node,
    const double row_height,
    const double left_padding,
    const std::size_t depth,
    const bool expanded,
    const bool expandable,
    const bool selected
) {
    if (node == nullptr) return nullptr;
    auto result = std::make_shared<DescriptionNode>(*node);
    std::vector<std::pair<std::string, runtime::Value>> layout;
    const auto current = result->properties.find("$layout");
    if (current != result->properties.end() && current->second.value() != nullptr &&
        current->second.value()->object() != nullptr) {
        layout = current->second.value()->object()->fields;
    }
    std::vector<std::pair<std::string, runtime::Value>> padding;
    const auto padding_field = std::ranges::find(
        layout,
        std::string("padding"),
        &std::pair<std::string, runtime::Value>::first
    );
    if (padding_field != layout.end() && padding_field->second.object() != nullptr) {
        padding = padding_field->second.object()->fields;
    }
    const auto authored_left = std::ranges::find(
        padding,
        std::string("left"),
        &std::pair<std::string, runtime::Value>::first
    );
    const double existing = authored_left != padding.end() && authored_left->second.number() != nullptr
        ? *authored_left->second.number()
        : 0.0;
    if (authored_left != padding.end()) authored_left->second = runtime::Value(existing + left_padding);
    else padding.emplace_back("left", runtime::Value(left_padding));
    const runtime::Value merged_padding(std::move(padding));
    const std::initializer_list<std::pair<std::string, runtime::Value>> overrides{
        {"height", runtime::Value(row_height)},
        {"padding", merged_padding},
        {"width", widget_fill()},
    };
    for (const auto& [name, value] : overrides) {
        const auto found = std::ranges::find(
            layout,
            name,
            &std::pair<std::string, runtime::Value>::first
        );
        if (found != layout.end()) found->second = value;
        else layout.emplace_back(name, value);
    }
    result->properties.insert_or_assign(
        "$layout",
        runtime::ExpressionValue(runtime::Value(std::move(layout)))
    );
    result->properties.insert_or_assign(
        "$treeDepth", runtime::ExpressionValue(runtime::Value(static_cast<double>(depth)))
    );
    result->properties.insert_or_assign(
        "$treeExpanded", runtime::ExpressionValue(runtime::Value(expanded))
    );
    result->properties.insert_or_assign(
        "$treeExpandable", runtime::ExpressionValue(runtime::Value(expandable))
    );
    result->properties.insert_or_assign(
        "$treeSelected", runtime::ExpressionValue(runtime::Value(selected))
    );
    return result;
}

void expand(WidgetDescriptionScope& scope) {
    WidgetDescriptionExpansion& description = scope.description();
    description.children.clear();
    if (std::ranges::none_of(description.behaviors, [](const DescriptionBehavior& behavior) {
            return behavior.id == "strata.drag-source";
        })) {
        description.behaviors.push_back(DescriptionBehavior{
            "strata.drag-source",
            true,
            widget_object({
                {"allowedOperations", runtime::Value(std::vector<runtime::Value>{runtime::Value("MOVE")})},
                {"descendantKeyPayloadType", runtime::Value("strata.tree-row")},
            }),
            nullptr,
        });
    }
    const runtime::Value* items = scope.property("items");
    const std::string collection_key = description.key.value_or("$tree-view");
    const auto placeholder = [&scope, &description, &collection_key](
                                 std::string text,
                                 const std::string_view key
                             ) {
        if (text.empty()) return;
        description.children.push_back(scope.node(
            "Text",
            collection_key + "." + std::string(key),
            widget_text_properties(std::move(text))
        ));
        description.synthesized_nodes += 1U;
    };
    if (items != nullptr && items->object() != nullptr && items->field("status") != nullptr) {
        const std::string* status = widget_description_string(items->field("status"));
        if (status == nullptr || *status == "IDLE" || *status == "LOADING") {
            placeholder(scope.string("loadingText", "Loading…"), "$async-loading");
            return;
        }
        if (*status == "FAILED") {
            const runtime::Value* error = items->field("error");
            const std::string* message = error != nullptr
                ? widget_description_string(error->field("message")) : nullptr;
            placeholder(
                scope.string("errorText", message != nullptr ? *message : "Unable to load data"),
                "$async-error"
            );
            return;
        }
        items = items->field("value");
    }
    if (items == nullptr || items->list() == nullptr) return;
    if (items->list()->values.empty()) {
        placeholder(scope.string("emptyText"), "$async-empty");
        return;
    }
    const std::set<std::string, std::less<>> open = effective_keys(
        scope, "expandedKeys", "strata.tree.expanded", "defaultExpandedKeys"
    );
    const std::set<std::string, std::less<>> selected = effective_keys(
        scope, "selectedKeys", "strata.collection.selected", "defaultSelectedKeys"
    );
    const std::set<std::string, std::less<>> loading = effective_keys(
        scope, "$none", "strata.tree.loading", "$none"
    );
    const double row_height = scope.number("rowHeight", 28.0);
    const double indent = scope.number("indent", 18.0);
    const std::string row_template = scope.string("rowTemplate");
    const double disclosure = scope.number("disclosureSize", 10.0);
    const std::shared_ptr<const runtime::ActionValue> drop_action = scope.bound_action("onDrop");
    struct Row final {
        runtime::Value item;
        std::string key;
        std::string label;
        std::size_t depth = 0U;
        bool expanded = false;
        bool expandable = false;
        bool selected = false;
        bool loading = false;
        bool enabled = true;
    };
    std::shared_ptr<const TreeTopology> topology = tree_topology(*items);
    const runtime::ValueList* source_items = topology->source.list();
    if (source_items == nullptr) return;
    std::vector<Row> rows;
    rows.reserve(topology->roots.size() + topology->disconnected.size());
    std::set<std::string, std::less<>> emitted;
    std::set<std::string, std::less<>> visiting;
    const auto append = [&](auto&& self, const std::size_t index, const std::size_t depth) -> void {
        const TreeItem& indexed_item = topology->items[index];
        if (emitted.contains(indexed_item.key) || !visiting.insert(indexed_item.key).second) {
            return;
        }
        const runtime::Value& item = source_items->values[indexed_item.source_index];
        const std::string* label_value = widget_description_string(item.field("label"));
        const std::string label = label_value != nullptr ? *label_value : indexed_item.key;
        const auto child_items = topology->children.find(indexed_item.key);
        const bool expandable = boolean(item.field("mayHaveChildren")) ||
            (child_items != topology->children.end() && !child_items->second.empty());
        const bool children_loaded = boolean(item.field("childrenLoaded"), true);
        const bool is_loading = loading.contains(indexed_item.key) && !children_loaded;
        rows.push_back(Row{
            item,
            indexed_item.key,
            label,
            depth,
            open.contains(indexed_item.key),
            expandable,
            selected.contains(indexed_item.key),
            is_loading,
            boolean(item.field("enabled"), true),
        });
        emitted.insert(indexed_item.key);
        if (open.contains(indexed_item.key) && children_loaded &&
            child_items != topology->children.end()) {
            for (const std::size_t child : child_items->second) self(self, child, depth + 1U);
        }
        visiting.erase(indexed_item.key);
    };
    for (const std::size_t root : topology->roots) append(append, root, 0U);
    // Malformed cycles have no root. Retain each record once instead of silently dropping data.
    for (const std::size_t index : topology->disconnected) append(append, index, 0U);
    std::vector<runtime::Value> keys;
    keys.reserve(rows.size());
    for (const Row& row : rows) keys.emplace_back(runtime::KeyValue{row.key});
    scope.set_layout("virtualItemCount", runtime::Value(static_cast<double>(rows.size())));
    scope.set_layout("virtualItemExtent", runtime::Value(row_height));
    scope.set_layout("virtualItemKeys", runtime::Value(std::move(keys)));
    scope.set_layout("virtualOverscan", runtime::Value(scope.number("overscan", 3.0)));
    const std::size_t row_count = rows.size();
    scope.set_generated_children(
        row_count,
        [rows = std::move(rows), topology = std::move(topology), row_template, row_height,
         indent, disclosure, drop_action](
            WidgetDescriptionScope& item_scope,
            const std::size_t index
        ) {
        static_cast<void>(topology);
        const Row& row_source = rows.at(index);
        WidgetTemplateArguments arguments{
            {"depth", runtime::Value(static_cast<double>(row_source.depth))},
            {"expanded", runtime::Value(row_source.expanded)},
            {"key", runtime::Value(runtime::KeyValue{row_source.key})},
            {"label", runtime::Value(row_source.label)},
            {"loading", runtime::Value(row_source.loading)},
            {"mayHaveChildren", runtime::Value(row_source.expandable)},
            {"selected", runtime::Value(row_source.selected)},
            {"value", row_source.item.field("value") != nullptr
                          ? *row_source.item.field("value") : runtime::Value{}},
        };
        std::shared_ptr<const DescriptionNode> row = row_template.empty()
            ? nullptr
            : item_scope.instantiate_component(
                  row_template, row_source.key, std::move(arguments)
              );
        if (row == nullptr) {
            row = item_scope.node(
                "Text", row_source.key, widget_text_properties(row_source.label)
            );
            item_scope.synthesized();
        }
        row = tree_row_layout(
            row,
            row_height,
            indent * static_cast<double>(row_source.depth) + disclosure + 8.0,
            row_source.depth,
            row_source.expanded,
            row_source.expandable,
            row_source.selected
        );
        row = with_semantics(row, widget_object({
            {"expanded", runtime::Value(row_source.expanded)},
            {"label", runtime::Value(row_source.label)},
            {"role", runtime::Value("TREE_ITEM")},
            {"selected", runtime::Value(row_source.selected)},
        }));
        if (row_source.enabled) {
            runtime::Value drop_options = row_source.expandable
                ? widget_object({
                    {"acceptedTypes", runtime::Value(std::vector<runtime::Value>{runtime::Value("strata.tree-row")})},
                    {"allowedOperations", runtime::Value(std::vector<runtime::Value>{runtime::Value("MOVE")})},
                    {"placementMode", runtime::Value("TREE")},
                })
                : widget_object({
                    {"acceptedTypes", runtime::Value(std::vector<runtime::Value>{runtime::Value("strata.tree-row")})},
                    {"allowedOperations", runtime::Value(std::vector<runtime::Value>{runtime::Value("MOVE")})},
                    {"placementMode", runtime::Value("TREE")},
                    {"insertionAxis", runtime::Value("VERTICAL")},
                });
            row = with_behaviors(row, {
                DescriptionBehavior{
                    "strata.drop-target",
                    true,
                    std::move(drop_options),
                    drop_action,
                },
            });
        }
        return row;
    });
}

} // namespace

void register_tree_description(WidgetRegistry& registry) {
    WidgetDescribePhase phase;
    phase.expand = &expand;
    phase.starts_unmaterialized = true;
    registry.register_describe_phase("TreeView", std::move(phase));
}

} // namespace strata::ui::collection_description
