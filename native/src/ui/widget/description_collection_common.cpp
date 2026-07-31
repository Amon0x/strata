#include "ui/widget/description_collection_common.hpp"

#include <algorithm>
#include <cmath>

namespace strata::ui::collection_description {

std::set<std::string, std::less<>> effective_keys(
    WidgetDescriptionScope& scope,
    const std::string_view controlled,
    const std::string_view retained_name,
    const std::string_view defaults
) {
    const runtime::Value* value = scope.property(controlled);
    if (value == nullptr || value->list() == nullptr) value = scope.retained(retained_name);
    if (value == nullptr || value->list() == nullptr) value = scope.property(defaults);
    std::set<std::string, std::less<>> result;
    if (value == nullptr || value->list() == nullptr) return result;
    for (const runtime::Value& entry : value->list()->values) {
        if (const std::string* key = widget_description_string(&entry); key != nullptr) {
            result.insert(*key);
        }
    }
    return result;
}

bool boolean(const runtime::Value* value, const bool fallback) noexcept {
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

std::shared_ptr<const DescriptionNode> with_layout_fields(
    const std::shared_ptr<const DescriptionNode>& node,
    const std::initializer_list<std::pair<std::string, runtime::Value>> fields
) {
    if (node == nullptr) return nullptr;
    auto result = std::make_shared<DescriptionNode>(*node);
    std::vector<std::pair<std::string, runtime::Value>> layout_fields;
    const auto current = result->properties.find("$layout");
    if (current != result->properties.end() && current->second.value() != nullptr &&
        current->second.value()->object() != nullptr) {
        layout_fields = current->second.value()->object()->fields;
    }
    for (const auto& [name, value] : fields) {
        const auto found = std::ranges::find(layout_fields, name, &std::pair<std::string, runtime::Value>::first);
        if (found != layout_fields.end()) found->second = value;
        else layout_fields.emplace_back(name, value);
    }
    result->properties.insert_or_assign(
        "$layout",
        runtime::ExpressionValue(runtime::Value(std::move(layout_fields)))
    );
    return result;
}

std::shared_ptr<const DescriptionNode> with_layout_padding(
    const std::shared_ptr<const DescriptionNode>& node,
    const std::initializer_list<std::pair<std::string_view, double>> edges
) {
    if (node == nullptr) return nullptr;
    auto result = std::make_shared<DescriptionNode>(*node);
    std::vector<std::pair<std::string, runtime::Value>> layout_fields;
    const auto current = result->properties.find("$layout");
    if (current != result->properties.end() && current->second.value() != nullptr &&
        current->second.value()->object() != nullptr) {
        layout_fields = current->second.value()->object()->fields;
    }
    std::vector<std::pair<std::string, runtime::Value>> padding_fields;
    const auto current_padding = std::ranges::find(
        layout_fields,
        std::string("padding"),
        &std::pair<std::string, runtime::Value>::first
    );
    if (current_padding != layout_fields.end() && current_padding->second.object() != nullptr) {
        padding_fields = current_padding->second.object()->fields;
    }
    for (const auto& [name, value] : edges) {
        const auto found = std::ranges::find(
            padding_fields,
            name,
            &std::pair<std::string, runtime::Value>::first
        );
        if (found != padding_fields.end()) found->second = runtime::Value(value);
        else padding_fields.emplace_back(name, runtime::Value(value));
    }
    const runtime::Value padding(std::move(padding_fields));
    if (current_padding != layout_fields.end()) current_padding->second = padding;
    else layout_fields.emplace_back("padding", padding);
    result->properties.insert_or_assign(
        "$layout",
        runtime::ExpressionValue(runtime::Value(std::move(layout_fields)))
    );
    return result;
}

std::shared_ptr<const DescriptionNode> with_semantics(
    const std::shared_ptr<const DescriptionNode>& node,
    runtime::Value semantics
) {
    if (node == nullptr) return nullptr;
    auto result = std::make_shared<DescriptionNode>(*node);
    result->properties.insert_or_assign(
        "semantics",
        runtime::ExpressionValue(std::move(semantics))
    );
    return result;
}

std::shared_ptr<const DescriptionNode> with_behaviors(
    const std::shared_ptr<const DescriptionNode>& node,
    std::vector<DescriptionBehavior> behaviors
) {
    if (node == nullptr) return nullptr;
    auto result = std::make_shared<DescriptionNode>(*node);
    for (DescriptionBehavior& behavior : behaviors) {
        const auto duplicate = std::ranges::find(
            result->behaviors,
            behavior.id,
            &DescriptionBehavior::id
        );
        if (duplicate == result->behaviors.end()) result->behaviors.push_back(std::move(behavior));
    }
    return result;
}

namespace {

void scroll_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("clip", runtime::Value(true));
    scope.set("height", widget_fill());
    scope.set("kind", runtime::Value("SCROLL"));
    scope.set("width", widget_fill());
    scope.set("viewportInsets", widget_object({
        {"bottom", runtime::Value(1.0)},
        {"left", runtime::Value(1.0)},
        {"right", runtime::Value(1.0)},
        {"top", runtime::Value(1.0)},
    }));
    scope.set("contentPadding", widget_object({
        {"bottom", runtime::Value(8.0)},
        {"left", runtime::Value(8.0)},
        {"right", runtime::Value(8.0)},
        {"top", runtime::Value(8.0)},
    }));
    scope.set("scrollbarGutter", runtime::Value(8.0));
}

void virtual_list_defaults(WidgetLayoutDefaultsScope& scope) {
    scroll_defaults(scope);
    if (const runtime::Value* count = scope.property("itemCount"); count != nullptr) {
        scope.set("virtualItemCount", *count);
    }
    if (const runtime::Value* extent = scope.property("itemExtent"); extent != nullptr) {
        scope.set("virtualItemExtent", *extent);
    }
}

void virtual_list_expand(WidgetDescriptionScope& scope) {
    const runtime::Value* source = scope.property("items");
    if (source == nullptr) return;
    const std::string collection_key = scope.description().key.value_or("$virtual-list");
    bool async_source = false;
    const auto placeholder = [&scope, &collection_key](
                                 std::string text,
                                 const std::string_view key
                             ) {
        scope.description().children.clear();
        scope.description().generated_widget_children.reset();
        if (text.empty()) return;
        scope.description().children.push_back(scope.node(
            "Text", collection_key + "." + std::string(key),
            widget_text_properties(std::move(text))
        ));
        scope.synthesized();
    };
    if (source->object() != nullptr && source->field("status") != nullptr) {
        async_source = true;
        const std::string* status = widget_description_string(source->field("status"));
        if (status == nullptr || *status == "IDLE" || *status == "LOADING") {
            placeholder(scope.string("loadingText", "Loading…"), "$async-loading");
            return;
        }
        if (*status == "FAILED") {
            const runtime::Value* error = source->field("error");
            const std::string* message = error != nullptr
                ? widget_description_string(error->field("message")) : nullptr;
            placeholder(
                scope.string("errorText", message != nullptr ? *message : "Unable to load data"),
                "$async-error"
            );
            return;
        }
        source = source->field("value");
    }
    if (source == nullptr || source->list() == nullptr) {
        placeholder(scope.string("errorText", "Invalid list source"), "$async-error");
        return;
    }
    if (source->list()->values.empty() && async_source) {
        placeholder(scope.string("emptyText"), "$async-empty");
        return;
    }
    const std::string item_template = scope.string("itemTemplate");
    std::vector<runtime::Value> items = source->list()->values;
    const std::size_t item_count = items.size();
    scope.set_layout("virtualItemCount", runtime::Value(static_cast<double>(item_count)));
    scope.set_generated_children(
        item_count,
        [items = std::move(items), collection_key, item_template](
            WidgetDescriptionScope& item_scope,
            const std::size_t index
        ) {
            const runtime::Value& item = items.at(index);
            const std::string* authored_key = widget_description_string(item.field("key"));
            if (authored_key == nullptr) authored_key = widget_description_string(&item);
            const std::string item_key = authored_key != nullptr && !authored_key->empty()
                ? *authored_key : std::to_string(index);
            const std::string key = collection_key + ".$item." + item_key;
            std::shared_ptr<const DescriptionNode> node = item_template.empty()
                ? nullptr : item_scope.instantiate_component(
                item_template,
                key,
                WidgetTemplateArguments{
                    {"index", runtime::Value(static_cast<double>(index))},
                    {"item", item},
                    {"key", runtime::Value(runtime::KeyValue{key})},
                }
            );
            if (node != nullptr) return node;
            const std::string* label = widget_description_string(item.field("label"));
            if (label == nullptr) label = widget_description_string(&item);
            return item_scope.node(
                "Text",
                key,
                widget_text_properties(label != nullptr ? *label : key)
            );
        }
    );
}

void repeater_expand(WidgetDescriptionScope& scope) {
    const std::shared_ptr<const DescriptionChildren>& source =
        scope.description().generated_children;
    scope.set_layout(
        "virtualItemCount",
        runtime::Value(static_cast<double>(
            source != nullptr ? source->size() : scope.description().children.size()
        ))
    );
    scope.set_layout("virtualItemExtent", runtime::Value(scope.number("estimatedItemExtent", 24.0)));
    scope.set_layout("virtualOverscan", runtime::Value(scope.number("overscan", 1.0)));
}

} // namespace

void register_collection_container_descriptions(WidgetRegistry& registry) {
    WidgetDescribePhase scroll;
    scroll.layout_defaults = &scroll_defaults;
    registry.register_describe_phase("Scroll", std::move(scroll));

    WidgetDescribePhase virtual_list;
    virtual_list.layout_defaults = &virtual_list_defaults;
    virtual_list.expand = &virtual_list_expand;
    virtual_list.starts_unmaterialized = true;
    registry.register_describe_phase("VirtualList", std::move(virtual_list));

    WidgetDescribePhase repeater;
    repeater.layout_defaults = &virtual_list_defaults;
    repeater.expand = &repeater_expand;
    repeater.canonical_type = "VirtualList";
    repeater.starts_unmaterialized = true;
    registry.register_describe_phase("Repeater", std::move(repeater));
}

} // namespace strata::ui::collection_description
