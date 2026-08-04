#include "compiler/builtin_catalog.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "compiler/builtin_catalog_internal.hpp"

namespace strata::compiler {
namespace {

using data::JsonValue;

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> fields) {
    return JsonValue(JsonValue::Object(fields));
}

[[nodiscard]] JsonValue array(std::vector<JsonValue> values) {
    return JsonValue(JsonValue::Array(std::move(values)));
}

[[nodiscard]] JsonValue strings(const std::vector<std::string>& values) {
    std::vector<JsonValue> encoded;
    encoded.reserve(values.size());
    for (const std::string& value : values)
        encoded.emplace_back(value);
    return array(std::move(encoded));
}

[[nodiscard]] std::string_view type_kind_name(const DeclaredTypeKind kind) {
    switch (kind) {
    case DeclaredTypeKind::unknown:
        return "unknown";
    case DeclaredTypeKind::any:
        return "any";
    case DeclaredTypeKind::unsafe_component_parameter:
        return "unsafeComponentParameter";
    case DeclaredTypeKind::null_value:
        return "null";
    case DeclaredTypeKind::string:
        return "string";
    case DeclaredTypeKind::string_literal:
        return "stringLiteral";
    case DeclaredTypeKind::number:
        return "number";
    case DeclaredTypeKind::duration:
        return "duration";
    case DeclaredTypeKind::boolean:
        return "boolean";
    case DeclaredTypeKind::color:
        return "color";
    case DeclaredTypeKind::path:
        return "path";
    case DeclaredTypeKind::image:
        return "image";
    case DeclaredTypeKind::key:
        return "key";
    case DeclaredTypeKind::style:
        return "style";
    case DeclaredTypeKind::layout:
        return "layout";
    case DeclaredTypeKind::animation:
        return "animation";
    case DeclaredTypeKind::effect:
        return "effect";
    case DeclaredTypeKind::material:
        return "material";
    case DeclaredTypeKind::action:
        return "action";
    case DeclaredTypeKind::component:
        return "component";
    case DeclaredTypeKind::lambda:
        return "lambda";
    case DeclaredTypeKind::component_template:
        return "componentTemplate";
    case DeclaredTypeKind::enumeration:
        return "enum";
    case DeclaredTypeKind::list:
        return "list";
    case DeclaredTypeKind::map:
        return "map";
    case DeclaredTypeKind::object:
        return "object";
    case DeclaredTypeKind::union_value:
        return "union";
    case DeclaredTypeKind::host_object:
        return "hostObject";
    case DeclaredTypeKind::async_value:
        return "async";
    case DeclaredTypeKind::collection:
        return "collection";
    }
    throw std::logic_error("invalid declared type kind");
}

[[nodiscard]] JsonValue encode_type(const DeclaredTypePtr& type) {
    if (type == nullptr)
        throw std::logic_error("catalog contains a null type");
    if (!type->reference.empty())
        return object({{"ref", JsonValue(type->reference)}});

    JsonValue::Object fields{
        {"kind", JsonValue(std::string(type_kind_name(type->kind)))},
    };
    if (!type->label.empty())
        fields.emplace_back("label", JsonValue(type->label));
    if (type->kind == DeclaredTypeKind::string_literal) {
        fields.emplace_back("value", JsonValue(type->literal));
    }
    if (!type->values.empty())
        fields.emplace_back("values", strings(type->values));
    if (!type->fields.empty()) {
        std::vector<JsonValue> encoded;
        encoded.reserve(type->fields.size());
        for (const DeclaredTypeField& field : type->fields) {
            encoded.push_back(object({
                {"name", JsonValue(field.name)},
                {"nullable", JsonValue(field.nullable)},
                {"required", JsonValue(field.required)},
                {"type", encode_type(field.type)},
            }));
        }
        fields.emplace_back("fields", array(std::move(encoded)));
    }
    if (!type->parameters.empty()) {
        std::vector<JsonValue> encoded;
        encoded.reserve(type->parameters.size());
        for (const DeclaredParameter& parameter : type->parameters) {
            encoded.push_back(object({
                {"name", JsonValue(parameter.name)},
                {"nullable", JsonValue(parameter.nullable)},
                {"required", JsonValue(parameter.required)},
                {"type", encode_type(parameter.type)},
            }));
        }
        fields.emplace_back("parameters", array(std::move(encoded)));
    }
    if (!type->options.empty()) {
        std::vector<JsonValue> encoded;
        encoded.reserve(type->options.size());
        for (const DeclaredTypePtr& option : type->options)
            encoded.push_back(encode_type(option));
        fields.emplace_back("options", array(std::move(encoded)));
    }
    if (type->element != nullptr) {
        fields.emplace_back(type->kind == DeclaredTypeKind::collection ? "item" : "element",
                            encode_type(type->element));
    }
    if (type->value != nullptr)
        fields.emplace_back("value", encode_type(type->value));
    if (type->parameter != nullptr) {
        fields.emplace_back("parameter", encode_type(type->parameter));
    }
    if (type->returns != nullptr)
        fields.emplace_back("returns", encode_type(type->returns));
    if (type->minimum_items.has_value()) {
        fields.emplace_back("minimumItems",
                            JsonValue(static_cast<std::int64_t>(*type->minimum_items)));
    }
    if (type->maximum_items.has_value()) {
        fields.emplace_back("maximumItems",
                            JsonValue(static_cast<std::int64_t>(*type->maximum_items)));
    }
    if (type->element_nullable)
        fields.emplace_back("elementNullable", JsonValue(true));
    if (type->value_nullable)
        fields.emplace_back("valueNullable", JsonValue(true));
    if (type->allow_unknown_fields) {
        fields.emplace_back("allowUnknownFields", JsonValue(true));
    }
    return JsonValue(std::move(fields));
}

[[nodiscard]] JsonValue encode_parameter(const DeclaredParameter& parameter) {
    return object({
        {"aliases", strings(parameter.aliases)},
        {"default", JsonValue()},
        {"name", JsonValue(parameter.name)},
        {"nullable", JsonValue(parameter.nullable)},
        {"required", JsonValue(parameter.required)},
        {"type", encode_type(parameter.type)},
    });
}

[[nodiscard]] JsonValue encode_property(const DeclaredProperty& property,
                                        const bool include_nullable) {
    JsonValue::Object fields{
        {"name", JsonValue(property.name)},
    };
    if (include_nullable)
        fields.emplace_back("nullable", JsonValue(property.nullable));
    fields.emplace_back("type", encode_type(property.type));
    return JsonValue(std::move(fields));
}

template <typename Value, typename Projection>
void require_unique(const std::vector<Value>& values, const std::string_view label,
                    Projection projection) {
    std::set<std::string, std::less<>> names;
    for (const Value& value : values) {
        const std::string_view name = std::invoke(projection, value);
        if (name.empty() || !names.emplace(name).second) {
            throw std::logic_error("built-in catalog contains an empty or duplicate " +
                                   std::string(label));
        }
    }
}

void validate_type(const DeclaredTypePtr& type,
                   const std::set<std::string, std::less<>>& named_types) {
    if (type == nullptr)
        throw std::logic_error("built-in catalog contains a null type");
    if (!type->reference.empty()) {
        if (!named_types.contains(type->reference)) {
            throw std::logic_error("built-in catalog has unresolved type '" + type->reference +
                                   "'");
        }
        return;
    }
    for (const DeclaredTypeField& field : type->fields)
        validate_type(field.type, named_types);
    require_unique(type->parameters, "type parameter", &DeclaredParameter::name);
    for (const DeclaredParameter& parameter : type->parameters)
        validate_type(parameter.type, named_types);
    for (const DeclaredTypePtr& option : type->options)
        validate_type(option, named_types);
    for (const DeclaredTypePtr& child :
         {type->element, type->value, type->parameter, type->returns}) {
        if (child != nullptr)
            validate_type(child, named_types);
    }
}

void validate_catalog(const BuiltinCatalog& catalog) {
    require_unique(catalog.types, "type id", &DeclaredNamedType::id);
    require_unique(catalog.widgets, "widget name", &DeclaredWidget::name);
    require_unique(catalog.actions, "action id", &DeclaredAction::id);
    require_unique(catalog.helpers, "helper name", &DeclaredHelper::name);
    require_unique(catalog.behaviors, "behavior id", &DeclaredBehavior::id);
    require_unique(catalog.materials, "material id", &DeclaredMaterial::id);
    require_unique(catalog.effects, "effect name", &DeclaredEffect::name);

    std::set<std::string, std::less<>> type_ids;
    for (const DeclaredNamedType& type : catalog.types)
        type_ids.emplace(type.id);
    for (const DeclaredNamedType& type : catalog.types) {
        validate_type(type.definition, type_ids);
    }
    const auto validate_properties = [&type_ids](const auto& values) {
        require_unique(values, "property name", &DeclaredProperty::name);
        for (const DeclaredProperty& value : values)
            validate_type(value.type, type_ids);
    };
    validate_properties(catalog.layout_properties);
    validate_properties(catalog.style_properties);
    validate_properties(catalog.animation_properties);
    validate_properties(catalog.animation_timing_properties);
    validate_properties(catalog.component_parameter_types);
    for (const DeclaredWidget& widget : catalog.widgets) {
        const std::vector<DeclaredParameter> parameters =
            composed_widget_parameters(catalog, widget);
        require_unique(parameters, "widget parameter", &DeclaredParameter::name);
        for (const DeclaredParameter& parameter : parameters) {
            validate_type(parameter.type, type_ids);
        }
        for (const DeclaredWidgetBinding& binding : widget.bindings) {
            const auto has = [&parameters](const std::string_view name) {
                return std::ranges::contains(parameters, name, &DeclaredParameter::name);
            };
            if (!has(binding.shorthand_parameter) || !has(binding.value_parameter) ||
                !has(binding.event_parameter)) {
                throw std::logic_error(
                    "built-in widget binding references an undeclared parameter");
            }
        }
    }
    for (const DeclaredAction& action : catalog.actions) {
        require_unique(action.parameters, "action parameter", &DeclaredParameter::name);
        for (const DeclaredParameter& parameter : action.parameters) {
            validate_type(parameter.type, type_ids);
        }
    }
    for (const DeclaredHelper& helper : catalog.helpers) {
        require_unique(helper.parameters, "helper parameter", &DeclaredParameter::name);
        for (const DeclaredParameter& parameter : helper.parameters) {
            validate_type(parameter.type, type_ids);
        }
        validate_type(helper.return_type, type_ids);
        if (helper.vararg_type != nullptr) validate_type(helper.vararg_type, type_ids);
    }
    for (const DeclaredBehavior& behavior : catalog.behaviors) {
        validate_type(behavior.options, type_ids);
    }
    for (const DeclaredMaterial& material : catalog.materials) {
        require_unique(material.parameters, "material parameter", &DeclaredParameter::name);
        for (const DeclaredParameter& parameter : material.parameters) {
            validate_type(parameter.type, type_ids);
        }
    }
    for (const DeclaredEffect& effect : catalog.effects) {
        require_unique(effect.parameters, "effect parameter", &DeclaredProperty::name);
        for (const DeclaredProperty& parameter : effect.parameters) {
            validate_type(parameter.type, type_ids);
        }
        if (effect.input != "BACKDROP" && effect.input != "CONTENT" &&
            effect.input != "SHAPE") {
            throw std::logic_error("built-in effect has an unsupported input");
        }
        for (const DeclaredEffect::Pass& pass : effect.passes) {
            if (pass.kind != "BLUR" && pass.kind != "SHADOW") {
                throw std::logic_error("built-in effect has an unsupported pass");
            }
            const auto has_parameter = [&effect](const std::optional<std::string>& name) {
                return !name.has_value() ||
                    std::ranges::contains(effect.parameters, *name, &DeclaredProperty::name);
            };
            if (!has_parameter(pass.radius_parameter) ||
                !has_parameter(pass.downsample_parameter)) {
                throw std::logic_error(
                    "built-in effect pass references an undeclared parameter"
                );
            }
        }
    }
}

} // namespace

DeclaredTypePtr declared_type(DeclaredType value) {
    return std::make_shared<const DeclaredType>(std::move(value));
}

DeclaredTypePtr declared_type_reference(std::string id) {
    return declared_type(DeclaredType{.reference = std::move(id)});
}

std::vector<DeclaredParameter> composed_widget_parameters(const BuiltinCatalog& catalog,
                                                          const DeclaredWidget& widget) {
    std::vector<DeclaredParameter> available = widget.parameters;
    static constexpr std::array persistent_widgets{
        std::string_view("Scroll"),      std::string_view("List"),
        std::string_view("VirtualList"), std::string_view("Table"),
        std::string_view("TreeView"),    std::string_view("ItemGrid"),
        std::string_view("SplitPane"),   std::string_view("Section"),
    };
    for (const DeclaredParameter& framework : catalog.framework_widget_parameters) {
        if (framework.name == "persistenceKey" &&
            !std::ranges::contains(persistent_widgets, std::string_view(widget.name))) {
            continue;
        }
        if ((framework.name == "undoLabel" || framework.name == "undoCoalesce") &&
            widget.bindings.empty()) {
            continue;
        }
        if (!std::ranges::contains(available, framework.name, &DeclaredParameter::name)) {
            available.push_back(framework);
        }
    }
    if (widget.parameter_order.empty())
        return available;
    std::vector<DeclaredParameter> result;
    result.reserve(available.size());
    for (const std::string& name : widget.parameter_order) {
        const auto found = std::ranges::find(available, name, &DeclaredParameter::name);
        if (found == available.end()) {
            throw std::logic_error(
                "built-in widget parameter order references an unavailable parameter");
        }
        result.push_back(*found);
    }
    for (const DeclaredParameter& parameter : available) {
        if (!std::ranges::contains(result, parameter.name, &DeclaredParameter::name)) {
            result.push_back(parameter);
        }
    }
    return result;
}

const BuiltinCatalog& builtin_catalog() {
    static const BuiltinCatalog catalog = [] {
        BuiltinCatalog result;
        add_builtin_types(result);
        add_builtin_properties(result);
        add_builtin_actions(result);
        add_builtin_helpers(result);
        add_builtin_primitive_widgets(result);
        add_builtin_control_widgets(result);
        add_builtin_shell_widgets(result);
        add_builtin_collection_widgets(result);
        std::ranges::sort(result.widgets, {}, &DeclaredWidget::name);
        validate_catalog(result);
        return result;
    }();
    return catalog;
}

data::JsonValue export_builtin_registry(const BuiltinCatalog& catalog) {
    const auto properties = [](const std::vector<DeclaredProperty>& declarations,
                               const bool include_nullable) {
        std::vector<JsonValue> result;
        result.reserve(declarations.size());
        for (const DeclaredProperty& property : declarations) {
            result.push_back(encode_property(property, include_nullable));
        }
        return array(std::move(result));
    };

    std::vector<JsonValue> types;
    types.reserve(catalog.types.size());
    for (const DeclaredNamedType& type : catalog.types) {
        types.push_back(object({
            {"definition", encode_type(type.definition)},
            {"id", JsonValue(type.id)},
        }));
    }
    std::vector<JsonValue> framework;
    framework.reserve(catalog.framework_widget_parameters.size());
    for (const DeclaredParameter& parameter : catalog.framework_widget_parameters) {
        framework.push_back(encode_parameter(parameter));
    }
    std::vector<JsonValue> widgets;
    widgets.reserve(catalog.widgets.size());
    for (const DeclaredWidget& widget : catalog.widgets) {
        std::vector<JsonValue> parameters;
        for (const DeclaredParameter& parameter : composed_widget_parameters(catalog, widget)) {
            if (!widget.parameter_order.empty() &&
                !std::ranges::contains(widget.parameter_order, parameter.name)) {
                continue;
            }
            parameters.push_back(encode_parameter(parameter));
        }
        std::vector<JsonValue> bindings;
        for (const DeclaredWidgetBinding& binding : widget.bindings) {
            bindings.push_back(object({
                {"eventParameter", JsonValue(binding.event_parameter)},
                {"shorthandParameter", JsonValue(binding.shorthand_parameter)},
                {"valueParameter", JsonValue(binding.value_parameter)},
            }));
        }
        std::vector<JsonValue> events;
        for (const DeclaredWidgetEvent& event : widget.events) {
            events.push_back(object({
                {"callbackParameter", JsonValue(event.callback_parameter)},
                {"description", JsonValue(event.description)},
                {"name", JsonValue(event.name)},
            }));
        }
        std::vector<JsonValue> retained;
        for (const DeclaredRetainedState& state : widget.retained_state) {
            retained.push_back(object({
                {"description", JsonValue(state.description)},
                {"name", JsonValue(state.name)},
            }));
        }
        widgets.push_back(object({
            {"allowsChildren", JsonValue(widget.allows_children)},
            {"bindings", array(std::move(bindings))},
            {"events", array(std::move(events))},
            {"name", JsonValue(widget.name)},
            {"parameters", array(std::move(parameters))},
            {"retainedState", array(std::move(retained))},
        }));
    }
    std::vector<JsonValue> actions;
    for (const DeclaredAction& action : catalog.actions) {
        std::vector<JsonValue> parameters;
        for (const DeclaredParameter& parameter : action.parameters) {
            parameters.push_back(encode_parameter(parameter));
        }
        actions.push_back(object({
            {"dispatchPolicy", JsonValue(action.dispatch_policy)},
            {"id", JsonValue(action.id)},
            {"parameters", array(std::move(parameters))},
            {"payloadContract", JsonValue(action.payload_contract)},
            {"summary", JsonValue(action.summary)},
        }));
    }
    std::vector<JsonValue> helpers;
    for (const DeclaredHelper& helper : catalog.helpers) {
        std::vector<JsonValue> parameters;
        for (const DeclaredParameter& parameter : helper.parameters) {
            parameters.push_back(encode_parameter(parameter));
        }
        helpers.push_back(object({
            {"allowNamedVarargs", JsonValue(helper.allow_named_varargs)},
            {"capabilities", strings(helper.capabilities)},
            {"implementation", JsonValue(helper.implementation)},
            {"name", JsonValue(helper.name)},
            {"parameters", array(std::move(parameters))},
            {"returnType", encode_type(helper.return_type)},
            {"varargType",
             helper.vararg_type != nullptr ? encode_type(helper.vararg_type) : JsonValue()},
        }));
    }
    std::vector<JsonValue> behaviors;
    for (const DeclaredBehavior& behavior : catalog.behaviors) {
        behaviors.push_back(object({
            {"id", JsonValue(behavior.id)},
            {"options", encode_type(behavior.options)},
        }));
    }
    std::vector<JsonValue> materials;
    for (const DeclaredMaterial& material : catalog.materials) {
        std::vector<JsonValue> parameters;
        for (const DeclaredParameter& parameter : material.parameters) {
            JsonValue::Object encoded{
                {"name", JsonValue(parameter.name)},
                {"type", encode_type(parameter.type)},
            };
            if (parameter.material_type.has_value()) {
                encoded.emplace_back("materialType", JsonValue(*parameter.material_type));
            }
            parameters.emplace_back(std::move(encoded));
        }
        materials.push_back(object({
            {"id", JsonValue(material.id)},
            {"parameters", array(std::move(parameters))},
        }));
    }
    std::vector<JsonValue> effects;
    for (const DeclaredEffect& effect : catalog.effects) {
        std::vector<JsonValue> parameters;
        for (const DeclaredProperty& parameter : effect.parameters) {
            parameters.push_back(encode_property(parameter, true));
        }
        std::vector<JsonValue> passes;
        for (const DeclaredEffect::Pass& pass : effect.passes) {
            JsonValue::Object encoded{
                {"downsample", JsonValue(static_cast<std::int64_t>(pass.downsample))},
                {"kind", JsonValue(pass.kind)},
                {"radius", JsonValue(pass.radius)},
            };
            if (pass.downsample_parameter.has_value()) {
                encoded.emplace_back(
                    "downsampleParameter", JsonValue(*pass.downsample_parameter)
                );
            }
            if (pass.radius_parameter.has_value()) {
                encoded.emplace_back(
                    "radiusParameter", JsonValue(*pass.radius_parameter)
                );
            }
            passes.emplace_back(std::move(encoded));
        }
        effects.push_back(object({
            {"input", JsonValue(effect.input)},
            {"name", JsonValue(effect.name)},
            {"parameters", array(std::move(parameters))},
            {"passes", array(std::move(passes))},
        }));
    }
    return object({
        {"actions", array(std::move(actions))},
        {"animationProperties", properties(catalog.animation_properties, false)},
        {"animationTimingProperties", properties(catalog.animation_timing_properties, false)},
        {"behaviors", array(std::move(behaviors))},
        {"componentParameterTypes", properties(catalog.component_parameter_types, false)},
        {"effects", array(std::move(effects))},
        {"format", JsonValue("strata.registry")},
        {"frameworkWidgetParameters", array(std::move(framework))},
        {"helpers", array(std::move(helpers))},
        {"id", JsonValue(catalog.id)},
        {"layoutProperties", properties(catalog.layout_properties, true)},
        {"materials", array(std::move(materials))},
        {"styleProperties", properties(catalog.style_properties, true)},
        {"types", array(std::move(types))},
        {"version", JsonValue(std::int64_t{1})},
        {"widgets", array(std::move(widgets))},
    });
}

std::string export_builtin_registry_json() {
    return data::encode_json_line(export_builtin_registry(builtin_catalog()));
}

} // namespace strata::compiler
