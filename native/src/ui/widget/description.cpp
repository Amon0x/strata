#include "ui/widget/description.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

#include "runtime/expression.hpp"
#include "runtime/registry.hpp"
#include "runtime/value.hpp"
#include "ui/tree.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] runtime::Value fields_value(WidgetLayoutFields fields) {
    std::vector<std::pair<std::string, runtime::Value>> values;
    values.reserve(fields.size());
    for (auto& [name, value] : fields) values.emplace_back(std::move(name), std::move(value));
    return runtime::Value(std::move(values));
}

[[nodiscard]] const runtime::Value* property_value(
    const DescriptionNode::Properties& properties,
    const std::string_view name
) noexcept {
    const auto found = properties.find(name);
    return found != properties.end() ? found->second.data_value() : nullptr;
}

void set_layout_property(
    DescriptionNode::Properties& properties,
    std::string name,
    runtime::Value value
) {
    WidgetLayoutFields layout;
    const runtime::Value* existing = property_value(properties, "$layout");
    if (existing != nullptr && existing->object() != nullptr) {
        for (const auto& [field_name, field_value] : existing->object()->fields) {
            layout.insert_or_assign(field_name, field_value);
        }
    }
    layout.insert_or_assign(std::move(name), std::move(value));
    properties.insert_or_assign(
        "$layout",
        runtime::ExpressionValue(fields_value(std::move(layout)))
    );
}

} // namespace

runtime::Value widget_object(
    std::initializer_list<std::pair<std::string, runtime::Value>> fields
) {
    return runtime::Value(std::vector<std::pair<std::string, runtime::Value>>(fields));
}

runtime::Value widget_fill() {
    return widget_object({{"weight", runtime::Value(1.0)}});
}

const std::string* widget_description_string(const runtime::Value* value) noexcept {
    if (value == nullptr) return nullptr;
    if (value->string() != nullptr) return value->string();
    if (value->key() != nullptr) return &value->key()->value;
    return nullptr;
}

DescriptionNode::Properties widget_transparent_properties() {
    return {
        {"background", runtime::ExpressionValue(runtime::Value{})},
        {"border", runtime::ExpressionValue(runtime::Value{})},
    };
}

void widget_mark_native_presentation(DescriptionNode::Properties& properties) {
    properties.insert_or_assign(
        "$nativePresentation",
        runtime::ExpressionValue(runtime::Value(true))
    );
    properties.insert_or_assign(
        "semantics",
        runtime::ExpressionValue(widget_object({
            {"decorative", runtime::Value(true)},
        }))
    );
}

std::shared_ptr<const DescriptionNode> widget_native_presentation(
    const std::shared_ptr<const DescriptionNode>& node
) {
    if (node == nullptr) return nullptr;
    auto result = std::make_shared<DescriptionNode>(*node);
    widget_mark_native_presentation(result->properties);
    return result;
}

bool widget_native_presentation_root(const RetainedNode& node) noexcept {
    const auto marker = node.description().properties.find("$nativePresentation");
    const runtime::Value* value = marker != node.description().properties.end()
        ? marker->second.value()
        : nullptr;
    return value != nullptr && value->boolean() != nullptr && *value->boolean();
}

bool widget_inside_native_presentation(const RetainedNode& node) noexcept {
    for (const RetainedNode* current = &node;
         current != nullptr;
         current = current->parent()) {
        if (widget_native_presentation_root(*current)) return true;
    }
    return false;
}

const RetainedNode* widget_native_input_owner(const RetainedNode* node) noexcept {
    const RetainedNode* owner = node;
    for (const RetainedNode* current = node;
         current != nullptr;
         current = current->parent()) {
        if (widget_native_presentation_root(*current)) owner = current->parent();
    }
    return owner;
}

RetainedNode* widget_native_input_owner(RetainedNode* node) noexcept {
    return const_cast<RetainedNode*>(
        widget_native_input_owner(static_cast<const RetainedNode*>(node))
    );
}

DescriptionNode::Properties widget_text_properties(std::string text, runtime::Value layout) {
    DescriptionNode::Properties result{
        {"text", runtime::ExpressionValue(runtime::Value(std::move(text)))},
    };
    if (layout.kind() != runtime::ValueKind::null_value) {
        result.emplace("$layout", runtime::ExpressionValue(std::move(layout)));
    }
    return result;
}

WidgetLayoutDefaultsScope::WidgetLayoutDefaultsScope(
    const DescriptionNode::Properties& properties
) noexcept : properties_(properties) {}

const runtime::Value* WidgetLayoutDefaultsScope::property(const std::string_view name) const noexcept {
    return property_value(properties_, name);
}

double WidgetLayoutDefaultsScope::number(
    const std::string_view name,
    const double fallback
) const noexcept {
    const runtime::Value* value = property(name);
    return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
               ? *value->number()
               : fallback;
}

bool WidgetLayoutDefaultsScope::boolean(
    const std::string_view name,
    const bool fallback
) const noexcept {
    const runtime::Value* value = property(name);
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

std::string WidgetLayoutDefaultsScope::string(
    const std::string_view name,
    std::string fallback
) const {
    const std::string* value = widget_description_string(property(name));
    return value != nullptr ? *value : std::move(fallback);
}

void WidgetLayoutDefaultsScope::set(std::string name, runtime::Value value) {
    fields_.insert_or_assign(std::move(name), std::move(value));
}

void WidgetLayoutDefaultsScope::padding(
    const double left,
    const double top,
    const double right,
    const double bottom
) {
    set("padding", widget_object({
        {"bottom", runtime::Value(bottom)},
        {"left", runtime::Value(left)},
        {"right", runtime::Value(right)},
        {"top", runtime::Value(top)},
    }));
}

void WidgetLayoutDefaultsScope::intrinsic(const double width, const double height) {
    set("intrinsicSize", widget_object({
        {"height", runtime::Value(height)},
        {"width", runtime::Value(width)},
    }));
}

WidgetLayoutFields WidgetLayoutDefaultsScope::take() { return std::move(fields_); }

WidgetDescriptionScope::WidgetDescriptionScope(
    WidgetDescriptionExpansion& description,
    const std::string_view state_scope,
    const runtime::RuntimeActionRegistry& actions,
    const WidgetRegistry& registry,
    const RetainedDescriptionSnapshot::Node* const retained,
    WidgetTemplateInstantiator instantiate_template,
    WidgetRetainedDependencyObserver observe_retained
) noexcept : description_(description),
    state_scope_(state_scope),
    actions_(actions),
    registry_(registry),
    retained_(retained),
    instantiate_template_(std::move(instantiate_template)),
    observe_retained_(std::move(observe_retained)) {}

WidgetDescriptionExpansion& WidgetDescriptionScope::description() noexcept { return description_; }
const WidgetDescriptionExpansion& WidgetDescriptionScope::description() const noexcept { return description_; }
std::string_view WidgetDescriptionScope::state_scope() const noexcept { return state_scope_; }

const runtime::Value* WidgetDescriptionScope::property(const std::string_view name) const noexcept {
    return property_value(description_.properties, name);
}

const runtime::Value* WidgetDescriptionScope::retained(const std::string_view name) const {
    const runtime::Value* const value = retained_ != nullptr
                                            ? retained_->retained_value(name)
                                            : nullptr;
    if (observe_retained_) observe_retained_(name, value);
    return value;
}

double WidgetDescriptionScope::number(const std::string_view name, const double fallback) const noexcept {
    const runtime::Value* value = property(name);
    return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
               ? *value->number()
               : fallback;
}

bool WidgetDescriptionScope::boolean(const std::string_view name, const bool fallback) const noexcept {
    const runtime::Value* value = property(name);
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

std::string WidgetDescriptionScope::string(const std::string_view name, std::string fallback) const {
    const std::string* value = widget_description_string(property(name));
    return value != nullptr ? *value : std::move(fallback);
}

std::vector<const runtime::Value*> WidgetDescriptionScope::list(const std::string_view name) const {
    const runtime::Value* value = property(name);
    if (value == nullptr || value->list() == nullptr) return {};
    std::vector<const runtime::Value*> result;
    result.reserve(value->list()->values.size());
    for (const runtime::Value& entry : value->list()->values) result.push_back(&entry);
    return result;
}

runtime::ExpressionValue WidgetDescriptionScope::action(const std::string_view id) const {
    const std::shared_ptr<const runtime::Action> resolved = action_contract(id);
    if (resolved == nullptr) return runtime::ExpressionValue(runtime::Value{});
    return runtime::ExpressionValue(std::make_shared<const runtime::ActionValue>(runtime::ActionValue{
        resolved,
        std::nullopt,
        {},
    }));
}

std::shared_ptr<const runtime::Action> WidgetDescriptionScope::action_contract(
    const std::string_view id
) const {
    const std::shared_ptr<const runtime::ActionContract> contract = actions_.contract(id);
    return contract != nullptr ? std::make_shared<const runtime::Action>(contract) : nullptr;
}

std::shared_ptr<const runtime::ActionValue> WidgetDescriptionScope::bound_action(
    const std::string_view property_name
) const {
    const auto found = description_.properties.find(property_name);
    return found != description_.properties.end() && found->second.action() != nullptr
        ? *found->second.action()
        : nullptr;
}

std::shared_ptr<const DescriptionNode> WidgetDescriptionScope::instantiate(
    const std::string_view template_property,
    std::string key,
    WidgetTemplateArguments arguments
) const {
    const std::string* component = widget_description_string(property(template_property));
    return component != nullptr
        ? instantiate_component(*component, std::move(key), std::move(arguments))
        : nullptr;
}

runtime::Value WidgetDescriptionScope::presentation_state(
    WidgetLayoutFields fields
) const {
    const runtime::Value* interaction = retained("$presentationState");
    const auto flag = [interaction](const std::string_view name) {
        const runtime::Value* value = interaction != nullptr
            ? interaction->field(name)
            : nullptr;
        return value != nullptr && value->boolean() != nullptr && *value->boolean();
    };
    fields.try_emplace("enabled", runtime::Value(boolean("enabled", true)));
    fields.insert_or_assign("hovered", runtime::Value(flag("hovered")));
    fields.insert_or_assign("pressed", runtime::Value(flag("pressed")));
    fields.insert_or_assign("focused", runtime::Value(flag("focused")));
    fields.insert_or_assign("focusVisible", runtime::Value(flag("focusVisible")));
    return fields_value(std::move(fields));
}

bool WidgetDescriptionScope::install_presentation(
    const std::string_view template_property,
    WidgetTemplateArguments arguments
) {
    if (property(template_property) == nullptr) return false;
    if (!description_.key.has_value()) {
        throw std::logic_error(
            "validated authored presentation owner must have an explicit key"
        );
    }
    const std::string& owner_key = *description_.key;
    const std::string presentation_key = owner_key + ".presentation";
    arguments.insert_or_assign(
        "key",
        runtime::Value(runtime::KeyValue{presentation_key})
    );
    std::shared_ptr<const DescriptionNode> presentation = instantiate(
        template_property,
        presentation_key,
        std::move(arguments)
    );
    if (presentation == nullptr) return false;
    description_.children = {widget_native_presentation(presentation)};
    synthesized();
    return true;
}

std::shared_ptr<const DescriptionNode> WidgetDescriptionScope::instantiate_component(
    const std::string_view component,
    std::string key,
    WidgetTemplateArguments arguments
) const {
    if (component.empty() || !instantiate_template_) return nullptr;
    return instantiate_template_(component, std::move(key), std::move(arguments));
}

void WidgetDescriptionScope::set_layout(
    DescriptionNode::Properties& properties,
    std::string name,
    runtime::Value value
) const {
    set_layout_property(properties, std::move(name), std::move(value));
}

void WidgetDescriptionScope::set_layout(std::string name, runtime::Value value) {
    set_layout_property(description_.properties, std::move(name), std::move(value));
}

void WidgetDescriptionScope::apply_layout_defaults(
    const std::string_view type,
    DescriptionNode::Properties& properties
) const {
    registry_.apply_layout_defaults(type, properties);
}

std::shared_ptr<const DescriptionNode> WidgetDescriptionScope::with_layout(
    const std::shared_ptr<const DescriptionNode>& source,
    std::string name,
    runtime::Value value
) const {
    auto result = std::make_shared<DescriptionNode>(*source);
    set_layout_property(result->properties, std::move(name), std::move(value));
    return result;
}

std::shared_ptr<const DescriptionNode> WidgetDescriptionScope::node(
    std::string type,
    std::optional<std::string> key,
    DescriptionNode::Properties properties,
    std::vector<std::shared_ptr<const DescriptionNode>> children,
    std::vector<DescriptionBehavior> behaviors
) const {
    return DescriptionNode::create(
        std::move(type),
        std::move(key),
        {},
        std::string(state_scope_),
        std::move(properties),
        std::make_shared<const EagerDescriptionChildren>(std::move(children)),
        std::move(behaviors)
    );
}

void WidgetDescriptionScope::set_generated_children(
    const std::size_t count,
    WidgetGeneratedChildHook factory,
    WidgetGeneratedVirtualization virtualization
) {
    if (!factory) throw std::invalid_argument("generated collection children require a factory");
    if (virtualization.sequence != nullptr && virtualization.sequence->count() != count) {
        throw std::invalid_argument("generated collection sequence count must match its rows");
    }
    if (virtualization.item_members != nullptr &&
        virtualization.item_members->size() != count) {
        throw std::invalid_argument("generated collection member count must match its rows");
    }
    if (virtualization.item_extents != nullptr &&
        virtualization.item_extents->size() != count) {
        throw std::invalid_argument("generated collection extent count must match its rows");
    }
    description_.children.clear();
    description_.generated_children.reset();
    description_.generated_widget_children = std::make_shared<const WidgetGeneratedChildren>(
        WidgetGeneratedChildren{count, std::move(factory), std::move(virtualization)}
    );
}

void WidgetDescriptionScope::synthesized(const std::size_t count) noexcept {
    description_.synthesized_nodes += count;
}

void WidgetRegistry::apply_layout_defaults(
    const std::string_view type,
    DescriptionNode::Properties& properties
) const {
    const WidgetLifecycle* widget = find(type);
    if (widget == nullptr) return;
    if (!widget->describe.layout_participates) {
        properties.insert_or_assign(
            "$layoutParticipates",
            runtime::ExpressionValue(runtime::Value(false))
        );
    }
    if (widget->describe.layout_defaults == nullptr) return;
    WidgetLayoutDefaultsScope scope(properties);
    widget->describe.layout_defaults(scope);
    WidgetLayoutFields merged = scope.take();
    const runtime::Value* authored = property_value(properties, "$layout");
    if (authored != nullptr && authored->object() != nullptr) {
        for (const auto& [name, value] : authored->object()->fields) {
            merged.insert_or_assign(name, value);
        }
    }
    properties.insert_or_assign(
        "$layout",
        runtime::ExpressionValue(fields_value(std::move(merged)))
    );
}

WidgetDescriptionExpansion WidgetRegistry::expand_description(
    WidgetDescriptionExpansion description,
    const std::string_view state_scope,
    const runtime::RuntimeActionRegistry& actions,
    const RetainedDescriptionSnapshot::Node* const retained,
    WidgetTemplateInstantiator instantiate_template,
    WidgetRetainedDependencyObserver observe_retained
) const {
    const std::string authoring_type = description.type;
    const WidgetLifecycle* widget = find(authoring_type);
    if (widget == nullptr) return description;
    if (!widget->describe.canonical_type.empty()) {
        description.type = widget->describe.canonical_type;
        description.properties.insert_or_assign(
            "$authoringType",
            runtime::ExpressionValue(runtime::Value(authoring_type))
        );
    }
    if (!widget->describe.default_action.empty()) {
        description.properties.insert_or_assign(
            "$defaultAction",
            runtime::ExpressionValue(runtime::Value(widget->describe.default_action))
        );
    }
    if (widget->describe.expand != nullptr) {
        WidgetDescriptionScope scope(
            description,
            state_scope,
            actions,
            *this,
            retained,
            std::move(instantiate_template),
            std::move(observe_retained)
        );
        widget->describe.expand(scope);
    }
    return description;
}

} // namespace strata::ui
