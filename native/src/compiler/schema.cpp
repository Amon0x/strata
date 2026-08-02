#include "compiler/schema.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "compiler/builtin_catalog.hpp"

namespace strata::compiler {
namespace {

[[nodiscard]] const data::JsonValue& required(const data::JsonValue& value,
                                              const std::string_view field) {
    const data::JsonValue* child = value.find(field);
    if (child == nullptr) {
        throw std::runtime_error("schema value is missing field '" + std::string(field) + "'");
    }
    return *child;
}

[[nodiscard]] const std::string& string_field(const data::JsonValue& value,
                                              const std::string_view field) {
    const std::string* text = required(value, field).string();
    if (text == nullptr) {
        throw std::runtime_error("schema field '" + std::string(field) + "' must be a string");
    }
    return *text;
}

[[nodiscard]] bool bool_field(const data::JsonValue& value, const std::string_view field) {
    const bool* boolean = required(value, field).boolean();
    if (boolean == nullptr) {
        throw std::runtime_error("schema field '" + std::string(field) + "' must be a boolean");
    }
    return *boolean;
}

[[nodiscard]] bool optional_bool(const data::JsonValue& value, const std::string_view field,
                                 const bool fallback = false) {
    const data::JsonValue* child = value.find(field);
    if (child == nullptr)
        return fallback;
    const bool* boolean = child->boolean();
    if (boolean == nullptr) {
        throw std::runtime_error("schema field '" + std::string(field) + "' must be a boolean");
    }
    return *boolean;
}

[[nodiscard]] const data::JsonValue::Array& array_field(const data::JsonValue& value,
                                                        const std::string_view field) {
    const data::JsonValue::Array* array = required(value, field).array();
    if (array == nullptr) {
        throw std::runtime_error("schema field '" + std::string(field) + "' must be an array");
    }
    return *array;
}

[[nodiscard]] std::optional<std::size_t> optional_size(const data::JsonValue& value,
                                                       const std::string_view field) {
    const data::JsonValue* child = value.find(field);
    if (child == nullptr || child->is_null())
        return std::nullopt;
    const std::int64_t* integer = child->integer();
    if (integer == nullptr || *integer < 0 ||
        static_cast<std::uint64_t>(*integer) > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("schema field '" + std::string(field) + "' must be a size");
    }
    return static_cast<std::size_t>(*integer);
}

[[nodiscard]] SemanticTypeKind type_kind(const std::string_view kind) {
    if (kind == "unknown")
        return SemanticTypeKind::unknown;
    if (kind == "any")
        return SemanticTypeKind::any;
    if (kind == "unsafeComponentParameter")
        return SemanticTypeKind::unsafe_component_parameter;
    if (kind == "null")
        return SemanticTypeKind::null_value;
    if (kind == "string")
        return SemanticTypeKind::string;
    if (kind == "stringLiteral")
        return SemanticTypeKind::string_literal;
    if (kind == "number")
        return SemanticTypeKind::number;
    if (kind == "duration")
        return SemanticTypeKind::duration;
    if (kind == "boolean")
        return SemanticTypeKind::boolean;
    if (kind == "color")
        return SemanticTypeKind::color;
    if (kind == "path")
        return SemanticTypeKind::path;
    if (kind == "image")
        return SemanticTypeKind::image;
    if (kind == "key")
        return SemanticTypeKind::key;
    if (kind == "style")
        return SemanticTypeKind::style;
    if (kind == "layout")
        return SemanticTypeKind::layout;
    if (kind == "animation")
        return SemanticTypeKind::animation;
    if (kind == "effect")
        return SemanticTypeKind::effect;
    if (kind == "material")
        return SemanticTypeKind::material;
    if (kind == "action")
        return SemanticTypeKind::action;
    if (kind == "component")
        return SemanticTypeKind::component;
    if (kind == "lambda")
        return SemanticTypeKind::lambda;
    if (kind == "componentTemplate")
        return SemanticTypeKind::component_template;
    if (kind == "enum")
        return SemanticTypeKind::enumeration;
    if (kind == "list")
        return SemanticTypeKind::list;
    if (kind == "map" || kind == "object")
        return SemanticTypeKind::map;
    if (kind == "union")
        return SemanticTypeKind::union_value;
    if (kind == "hostObject")
        return SemanticTypeKind::host_object;
    if (kind == "async")
        return SemanticTypeKind::async_value;
    if (kind == "collection")
        return SemanticTypeKind::collection;
    throw std::runtime_error("unsupported schema semantic type kind '" + std::string(kind) + "'");
}

[[nodiscard]] SemanticTypeKind type_kind(const DeclaredTypeKind kind) {
    switch (kind) {
    case DeclaredTypeKind::unknown:
        return SemanticTypeKind::unknown;
    case DeclaredTypeKind::any:
        return SemanticTypeKind::any;
    case DeclaredTypeKind::unsafe_component_parameter:
        return SemanticTypeKind::unsafe_component_parameter;
    case DeclaredTypeKind::null_value:
        return SemanticTypeKind::null_value;
    case DeclaredTypeKind::string:
        return SemanticTypeKind::string;
    case DeclaredTypeKind::string_literal:
        return SemanticTypeKind::string_literal;
    case DeclaredTypeKind::number:
        return SemanticTypeKind::number;
    case DeclaredTypeKind::duration:
        return SemanticTypeKind::duration;
    case DeclaredTypeKind::boolean:
        return SemanticTypeKind::boolean;
    case DeclaredTypeKind::color:
        return SemanticTypeKind::color;
    case DeclaredTypeKind::path:
        return SemanticTypeKind::path;
    case DeclaredTypeKind::image:
        return SemanticTypeKind::image;
    case DeclaredTypeKind::key:
        return SemanticTypeKind::key;
    case DeclaredTypeKind::style:
        return SemanticTypeKind::style;
    case DeclaredTypeKind::layout:
        return SemanticTypeKind::layout;
    case DeclaredTypeKind::animation:
        return SemanticTypeKind::animation;
    case DeclaredTypeKind::effect:
        return SemanticTypeKind::effect;
    case DeclaredTypeKind::material:
        return SemanticTypeKind::material;
    case DeclaredTypeKind::action:
        return SemanticTypeKind::action;
    case DeclaredTypeKind::component:
        return SemanticTypeKind::component;
    case DeclaredTypeKind::lambda:
        return SemanticTypeKind::lambda;
    case DeclaredTypeKind::component_template:
        return SemanticTypeKind::component_template;
    case DeclaredTypeKind::enumeration:
        return SemanticTypeKind::enumeration;
    case DeclaredTypeKind::list:
        return SemanticTypeKind::list;
    case DeclaredTypeKind::map:
    case DeclaredTypeKind::object:
        return SemanticTypeKind::map;
    case DeclaredTypeKind::union_value:
        return SemanticTypeKind::union_value;
    case DeclaredTypeKind::host_object:
        return SemanticTypeKind::host_object;
    case DeclaredTypeKind::async_value:
        return SemanticTypeKind::async_value;
    case DeclaredTypeKind::collection:
        return SemanticTypeKind::collection;
    }
    throw std::logic_error("invalid native catalog type kind");
}

[[nodiscard]] std::string join(const std::vector<std::string>& values) {
    std::string output;
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (index != 0U)
            output += ", ";
        output += values[index];
    }
    return output;
}

[[nodiscard]] std::string normalized_semantic_name(const std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const char byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        if (character == '-' || character == '_')
            continue;
        normalized.push_back(static_cast<char>(std::tolower(character)));
    }
    return normalized;
}

} // namespace

std::string SemanticType::diagnostic_name() const {
    switch (kind) {
    case SemanticTypeKind::unknown:
        return "unknown";
    case SemanticTypeKind::any:
        return "any value";
    case SemanticTypeKind::unsafe_component_parameter:
        return "explicit component parameter schema";
    case SemanticTypeKind::null_value:
        return "null";
    case SemanticTypeKind::string:
        return "string";
    case SemanticTypeKind::string_literal:
        return "string literal '" + literal + "'";
    case SemanticTypeKind::number:
        return "number";
    case SemanticTypeKind::duration:
        return "duration";
    case SemanticTypeKind::boolean:
        return "boolean";
    case SemanticTypeKind::color:
        return "color";
    case SemanticTypeKind::path:
        return "path outline";
    case SemanticTypeKind::image:
        return "image id";
    case SemanticTypeKind::key:
        return "key";
    case SemanticTypeKind::style:
        return "style";
    case SemanticTypeKind::layout:
        return "layout";
    case SemanticTypeKind::animation:
        return "animation";
    case SemanticTypeKind::effect:
        return "effect";
    case SemanticTypeKind::material:
        return "material";
    case SemanticTypeKind::action:
        return "action";
    case SemanticTypeKind::component:
        return "component";
    case SemanticTypeKind::lambda:
        return "pure expression (" + parameter->diagnostic_name() + ") -> " +
               returns->diagnostic_name();
    case SemanticTypeKind::component_template:
        return "component template";
    case SemanticTypeKind::enumeration:
        return label + " (" + join(values) + ")";
    case SemanticTypeKind::list: {
        std::string bounds;
        if (minimum_items.has_value() && maximum_items == minimum_items) {
            bounds = " with exactly " + std::to_string(*minimum_items) + " items";
        } else if (minimum_items.has_value() && maximum_items.has_value()) {
            bounds = " with " + std::to_string(*minimum_items) + " to " +
                     std::to_string(*maximum_items) + " items";
        } else if (minimum_items.has_value()) {
            bounds = " with at least " + std::to_string(*minimum_items) + " items";
        } else if (maximum_items.has_value()) {
            bounds = " with at most " + std::to_string(*maximum_items) + " items";
        }
        return "list of " + element->diagnostic_name() + bounds;
    }
    case SemanticTypeKind::map:
        return label.empty() ? "object" : label;
    case SemanticTypeKind::union_value:
        return label;
    case SemanticTypeKind::host_object:
        return "host object '" + label + "'";
    case SemanticTypeKind::async_value:
        return "async " + (value != nullptr ? value->diagnostic_name() : std::string("value"));
    case SemanticTypeKind::collection:
        return maximum_items.has_value()
                   ? "collection up to " + std::to_string(*maximum_items) + " items"
                   : "unbounded collection";
    }
    throw std::runtime_error("invalid semantic type kind");
}

bool SemanticType::accepts(const SemanticType& actual) const {
    if (kind == SemanticTypeKind::any || kind == SemanticTypeKind::unknown ||
        actual.kind == SemanticTypeKind::unknown) {
        return true;
    }
    if (kind == SemanticTypeKind::union_value) {
        return std::ranges::any_of(
            options, [&actual](const SemanticTypePtr& option) { return option->accepts(actual); });
    }
    if (kind == SemanticTypeKind::string && actual.kind == SemanticTypeKind::string_literal)
        return true;
    if (kind == SemanticTypeKind::path && (actual.kind == SemanticTypeKind::string ||
                                           actual.kind == SemanticTypeKind::string_literal)) {
        return true;
    }
    if ((kind == SemanticTypeKind::key || kind == SemanticTypeKind::image) &&
        (actual.kind == SemanticTypeKind::string ||
         actual.kind == SemanticTypeKind::string_literal)) {
        return true;
    }
    if (kind == SemanticTypeKind::enumeration && actual.kind == SemanticTypeKind::string_literal) {
        return std::ranges::find(values, actual.literal) != values.end();
    }
    if (kind == SemanticTypeKind::list &&
        (actual.kind == SemanticTypeKind::list || actual.kind == SemanticTypeKind::collection)) {
        return element != nullptr && actual.element != nullptr && element->accepts(*actual.element);
    }
    if (kind == SemanticTypeKind::map && actual.kind == SemanticTypeKind::map)
        return true;
    if (kind == SemanticTypeKind::async_value && actual.kind == SemanticTypeKind::async_value) {
        return value == nullptr || actual.value == nullptr || value->accepts(*actual.value);
    }
    if (kind == SemanticTypeKind::collection &&
        (actual.kind == SemanticTypeKind::collection || actual.kind == SemanticTypeKind::list)) {
        return true;
    }
    return kind == actual.kind;
}

const ObjectField* SemanticType::find_field(const std::string_view name) const noexcept {
    const auto found = std::ranges::find(fields, name, &ObjectField::name);
    return found == fields.end() ? nullptr : &*found;
}

bool SchemaParameter::accepts_name(const std::string_view candidate) const noexcept {
    return candidate == name || std::ranges::find(aliases, candidate) != aliases.end();
}

std::size_t material_parameter_width(const std::optional<std::string>& material_type) noexcept {
    if (!material_type.has_value())
        return 1U;
    if (*material_type == "FLOAT2")
        return 2U;
    if (*material_type == "FLOAT4" || *material_type == "COLOR")
        return 4U;
    return 1U;
}

const SchemaParameter* MaterialSchema::find_parameter(const std::string_view name) const noexcept {
    const auto found = std::ranges::find(parameters, name, &SchemaParameter::name);
    return found == parameters.end() ? nullptr : &*found;
}

std::optional<std::size_t>
MaterialSchema::packing_slot(const std::string_view name) const noexcept {
    std::size_t slot = first_material_slot;
    for (const SchemaParameter& parameter : parameters) {
        const std::size_t width = material_parameter_width(parameter.material_type);
        if (parameter.name == name) {
            return slot + width <= first_material_slot + maximum_material_slots
                       ? std::optional<std::size_t>(slot)
                       : std::nullopt;
        }
        slot += width;
    }
    return std::nullopt;
}

const SchemaParameter*
EffectSchema::find_parameter(const std::string_view parameter_name) const noexcept {
    const auto found = std::ranges::find(parameters, parameter_name, &SchemaParameter::name);
    return found == parameters.end() ? nullptr : &*found;
}

const SchemaParameter*
WidgetSchema::find_parameter(const std::string_view candidate_name) const noexcept {
    const auto found =
        std::ranges::find_if(parameters, [candidate_name](const SchemaParameter& parameter) {
            return parameter.accepts_name(candidate_name);
        });
    return found == parameters.end() ? nullptr : &*found;
}

const WidgetBindingSchema*
WidgetSchema::find_binding(const std::string_view shorthand) const noexcept {
    const auto found =
        std::ranges::find(bindings, shorthand, &WidgetBindingSchema::shorthand_parameter);
    return found == bindings.end() ? nullptr : &*found;
}

SchemaRegistry SchemaRegistry::builtins() {
    static const SchemaRegistry registry = from_catalog(builtin_catalog());
    return registry;
}

SchemaRegistry SchemaRegistry::from_catalog(const BuiltinCatalog& catalog) {
    SchemaRegistry registry;
    for (const DeclaredNamedType& entry : catalog.types) {
        if (!registry.declared_type_definitions_.emplace(entry.id, entry.definition).second) {
            throw std::logic_error("duplicate native catalog type id '" + entry.id + "'");
        }
    }
    const auto property_parameter = [&registry](const DeclaredProperty& value) {
        SchemaParameter parameter;
        parameter.name = value.name;
        parameter.type = registry.parse_type(value.type);
        parameter.nullable = value.nullable;
        return parameter;
    };
    for (const DeclaredProperty& value : catalog.layout_properties) {
        registry.layout_properties_.push_back(property_parameter(value));
    }
    for (const DeclaredProperty& value : catalog.style_properties) {
        registry.style_properties_.push_back(property_parameter(value));
    }
    for (const DeclaredProperty& value : catalog.animation_properties) {
        registry.animation_properties_.push_back(property_parameter(value));
    }
    for (const DeclaredProperty& value : catalog.animation_timing_properties) {
        registry.animation_timing_properties_.push_back(property_parameter(value));
    }
    for (const DeclaredWidget& value : catalog.widgets) {
        WidgetSchema schema;
        schema.name = value.name;
        schema.allows_children = value.allows_children;
        for (const DeclaredParameter& parameter : composed_widget_parameters(catalog, value)) {
            schema.parameters.push_back(registry.parse_parameter(parameter));
        }
        for (const DeclaredWidgetBinding& binding : value.bindings) {
            WidgetBindingSchema parsed{
                binding.shorthand_parameter,
                binding.value_parameter,
                binding.event_parameter,
            };
            if (schema.find_binding(parsed.shorthand_parameter) != nullptr) {
                throw std::logic_error("duplicate native widget binding shorthand '" +
                                       parsed.shorthand_parameter + "'");
            }
            const SchemaParameter* event = schema.find_parameter(parsed.event_parameter);
            if (event == nullptr || event->type == nullptr ||
                event->type->kind != SemanticTypeKind::action) {
                throw std::logic_error("native widget binding event parameter must be an action");
            }
            schema.bindings.push_back(std::move(parsed));
        }
        if (!registry.widgets_.emplace(schema.name, std::move(schema)).second) {
            throw std::logic_error("duplicate native catalog widget");
        }
    }
    for (const DeclaredParameter& value : catalog.framework_widget_parameters) {
        registry.framework_widget_parameters_.push_back(registry.parse_parameter(value));
    }
    for (const DeclaredAction& value : catalog.actions) {
        ActionSchema schema;
        schema.id = value.id;
        schema.dispatch_policy = value.dispatch_policy;
        schema.payload_contract = value.payload_contract;
        schema.summary = value.summary;
        for (const DeclaredParameter& parameter : value.parameters) {
            schema.parameters.push_back(registry.parse_parameter(parameter));
        }
        if (!registry.actions_.emplace(schema.id, std::move(schema)).second) {
            throw std::logic_error("duplicate native catalog action");
        }
    }
    for (const DeclaredHelper& value : catalog.helpers) {
        HelperSchema schema;
        schema.name = value.name;
        schema.implementation = value.implementation;
        schema.return_type = registry.parse_type(value.return_type);
        schema.vararg_type = value.vararg_type != nullptr ? registry.parse_type(value.vararg_type)
                                                          : SemanticTypePtr{};
        schema.allow_named_varargs = value.allow_named_varargs;
        for (const DeclaredParameter& parameter : value.parameters) {
            schema.parameters.push_back(registry.parse_parameter(parameter));
        }
        if (!registry.helpers_.emplace(schema.name, std::move(schema)).second) {
            throw std::logic_error("duplicate native catalog helper");
        }
    }
    for (const DeclaredProperty& value : catalog.component_parameter_types) {
        const auto [entry, inserted] = registry.component_parameter_types_.emplace(
            normalized_semantic_name(value.name), registry.parse_type(value.type));
        static_cast<void>(entry);
        if (!inserted) {
            throw std::logic_error("duplicate normalized native catalog component parameter type");
        }
    }
    for (const DeclaredMaterial& value : catalog.materials) {
        MaterialSchema schema;
        schema.id = value.id;
        for (const DeclaredParameter& declared : value.parameters) {
            schema.parameters.push_back(registry.parse_parameter(declared));
        }
        registry.material_ids_.push_back(schema.id);
        if (!registry.materials_.emplace(schema.id, std::move(schema)).second) {
            throw std::logic_error("duplicate native catalog material");
        }
    }
    for (const DeclaredEffect& value : catalog.effects) {
        EffectSchema schema;
        schema.name = value.name;
        schema.input = value.input;
        for (const DeclaredProperty& parameter : value.parameters) {
            schema.parameters.push_back(property_parameter(parameter));
        }
        for (const DeclaredEffect::Pass& pass : value.passes) {
            schema.passes.push_back(EffectSchema::Pass{
                pass.kind,
                pass.radius_parameter,
                pass.radius,
                pass.downsample_parameter,
                pass.downsample,
                {},
            });
        }
        if (!registry.effects_.emplace(schema.name, std::move(schema)).second) {
            throw std::logic_error("duplicate native catalog effect");
        }
    }
    std::ranges::sort(registry.material_ids_);
    return registry;
}

SemanticTypePtr SchemaRegistry::parse_type(const data::JsonValue& value) {
    if (const data::JsonValue* reference = value.find("ref"); reference != nullptr) {
        if (reference->string() == nullptr)
            throw std::runtime_error("schema type ref must be a string");
        const std::string& id = *reference->string();
        if (const auto cached = resolved_types_.find(id); cached != resolved_types_.end()) {
            return cached->second;
        }
        if (const auto definition = declared_type_definitions_.find(id);
            definition != declared_type_definitions_.end()) {
            SemanticTypePtr parsed = parse_type(definition->second);
            resolved_types_.emplace(id, parsed);
            return parsed;
        }
        throw std::runtime_error("unknown schema type ref '" + id + "'");
    }

    auto parsed = std::make_shared<SemanticType>();
    const std::string& kind_name = string_field(value, "kind");
    parsed->kind = type_kind(kind_name);
    if (const data::JsonValue* label = value.find("label");
        label != nullptr && label->string() != nullptr) {
        parsed->label = *label->string();
    }
    if (const data::JsonValue* literal = value.find("value");
        parsed->kind == SemanticTypeKind::string_literal && literal != nullptr &&
        literal->string() != nullptr) {
        parsed->literal = *literal->string();
    }
    if (const data::JsonValue* values = value.find("values");
        values != nullptr && values->array() != nullptr) {
        for (const data::JsonValue& item : *values->array()) {
            if (item.string() == nullptr)
                throw std::runtime_error("schema enum value must be a string");
            parsed->values.push_back(*item.string());
        }
    }
    if (const data::JsonValue* fields = value.find("fields"); fields != nullptr) {
        if (fields->array() == nullptr)
            throw std::runtime_error("schema type fields must be an array");
        for (const data::JsonValue& field : *fields->array()) {
            parsed->fields.push_back(ObjectField{
                string_field(field, "name"),
                parse_type(required(field, "type")),
                optional_bool(field, "required"),
                optional_bool(field, "nullable"),
            });
        }
    }
    if (const data::JsonValue* options = value.find("options"); options != nullptr) {
        if (options->array() == nullptr)
            throw std::runtime_error("schema type options must be an array");
        for (const data::JsonValue& option : *options->array()) {
            parsed->options.push_back(parse_type(option));
        }
    }
    if (const data::JsonValue* child = value.find("element"); child != nullptr)
        parsed->element = parse_type(*child);
    if (const data::JsonValue* child = value.find("item"); child != nullptr)
        parsed->element = parse_type(*child);
    if (const data::JsonValue* child = value.find("value");
        child != nullptr && parsed->kind != SemanticTypeKind::string_literal) {
        parsed->value = parse_type(*child);
    }
    if (const data::JsonValue* child = value.find("parameter"); child != nullptr)
        parsed->parameter = parse_type(*child);
    if (const data::JsonValue* child = value.find("returns"); child != nullptr)
        parsed->returns = parse_type(*child);
    parsed->minimum_items = optional_size(value, "minimumItems");
    parsed->maximum_items = optional_size(value, "maximumItems");
    parsed->element_nullable = optional_bool(value, "elementNullable");
    parsed->value_nullable = optional_bool(value, "valueNullable");
    parsed->allow_unknown_fields = optional_bool(value, "allowUnknownFields");
    if (parsed->kind == SemanticTypeKind::async_value) {
        if (parsed->value == nullptr) {
            throw std::runtime_error("schema async type requires a value schema");
        }
        auto status = std::make_shared<SemanticType>();
        status->kind = SemanticTypeKind::enumeration;
        status->label = "async status";
        status->values = {"IDLE", "LOADING", "READY", "FAILED"};
        auto string = std::make_shared<SemanticType>();
        string->kind = SemanticTypeKind::string;
        auto number = std::make_shared<SemanticType>();
        number->kind = SemanticTypeKind::number;
        auto progress = std::make_shared<SemanticType>();
        progress->kind = SemanticTypeKind::host_object;
        progress->label = "async progress";
        progress->fields = {
            {"completed", number, true, false},
            {"message", string, true, false},
            {"total", number, true, true},
        };
        auto error = std::make_shared<SemanticType>();
        error->kind = SemanticTypeKind::host_object;
        error->label = "async failure";
        error->fields = {
            {"code", string, true, false},
            {"message", string, true, false},
        };
        parsed->fields = {
            {"error", error, true, true},
            {"progress", progress, true, true},
            {"status", status, true, false},
            {"value", parsed->value, true, true},
        };
        parsed->label = parsed->label.empty() ? "async value" : parsed->label;
        parsed->allow_unknown_fields = false;
    }
    return parsed;
}

SemanticTypePtr SchemaRegistry::parse_type(const std::shared_ptr<const DeclaredType>& value) {
    if (value == nullptr)
        throw std::logic_error("native catalog contains a null type");
    if (!value->reference.empty()) {
        if (const auto cached = resolved_types_.find(value->reference);
            cached != resolved_types_.end()) {
            return cached->second;
        }
        if (const auto definition = declared_type_definitions_.find(value->reference);
            definition != declared_type_definitions_.end()) {
            SemanticTypePtr parsed = parse_type(definition->second);
            resolved_types_.emplace(value->reference, parsed);
            return parsed;
        }
        throw std::logic_error("unknown native catalog type ref '" + value->reference + "'");
    }

    auto parsed = std::make_shared<SemanticType>();
    parsed->kind = type_kind(value->kind);
    parsed->label = value->label;
    parsed->literal = value->literal;
    parsed->values = value->values;
    parsed->fields.reserve(value->fields.size());
    for (const DeclaredTypeField& field : value->fields) {
        parsed->fields.push_back(ObjectField{
            field.name,
            parse_type(field.type),
            field.required,
            field.nullable,
        });
    }
    parsed->options.reserve(value->options.size());
    for (const DeclaredTypePtr& option : value->options) {
        parsed->options.push_back(parse_type(option));
    }
    if (value->element != nullptr)
        parsed->element = parse_type(value->element);
    if (value->value != nullptr)
        parsed->value = parse_type(value->value);
    if (value->parameter != nullptr)
        parsed->parameter = parse_type(value->parameter);
    if (value->returns != nullptr)
        parsed->returns = parse_type(value->returns);
    parsed->minimum_items = value->minimum_items;
    parsed->maximum_items = value->maximum_items;
    parsed->element_nullable = value->element_nullable;
    parsed->value_nullable = value->value_nullable;
    parsed->allow_unknown_fields = value->allow_unknown_fields;
    if (parsed->kind == SemanticTypeKind::async_value) {
        if (parsed->value == nullptr) {
            throw std::logic_error("native catalog async type requires a value schema");
        }
        auto status = std::make_shared<SemanticType>();
        status->kind = SemanticTypeKind::enumeration;
        status->label = "async status";
        status->values = {"IDLE", "LOADING", "READY", "FAILED"};
        auto string = std::make_shared<SemanticType>();
        string->kind = SemanticTypeKind::string;
        auto number = std::make_shared<SemanticType>();
        number->kind = SemanticTypeKind::number;
        auto progress = std::make_shared<SemanticType>();
        progress->kind = SemanticTypeKind::host_object;
        progress->label = "async progress";
        progress->fields = {
            {"completed", number, true, false},
            {"message", string, true, false},
            {"total", number, true, true},
        };
        auto error = std::make_shared<SemanticType>();
        error->kind = SemanticTypeKind::host_object;
        error->label = "async failure";
        error->fields = {
            {"code", string, true, false},
            {"message", string, true, false},
        };
        parsed->fields = {
            {"error", error, true, true},
            {"progress", progress, true, true},
            {"status", status, true, false},
            {"value", parsed->value, true, true},
        };
        parsed->label = parsed->label.empty() ? "async value" : parsed->label;
        parsed->allow_unknown_fields = false;
    }
    return parsed;
}

SchemaParameter SchemaRegistry::parse_parameter(const data::JsonValue& value) {
    SchemaParameter parameter;
    parameter.name = string_field(value, "name");
    parameter.type = parse_type(required(value, "type"));
    parameter.required = bool_field(value, "required");
    parameter.nullable = bool_field(value, "nullable");
    for (const data::JsonValue& alias : array_field(value, "aliases")) {
        if (alias.string() == nullptr)
            throw std::runtime_error("schema alias must be a string");
        parameter.aliases.push_back(*alias.string());
    }
    return parameter;
}

SchemaParameter SchemaRegistry::parse_parameter(const DeclaredParameter& value) {
    return SchemaParameter{
        value.name,     parse_type(value.type), value.required,
        value.nullable, value.aliases,          value.material_type,
    };
}

const WidgetSchema* SchemaRegistry::widget(const std::string_view name) const noexcept {
    const auto found = widgets_.find(std::string(name));
    return found == widgets_.end() ? nullptr : &found->second;
}

const ActionSchema* SchemaRegistry::action(const std::string_view id) const noexcept {
    const auto found = actions_.find(std::string(id));
    return found == actions_.end() ? nullptr : &found->second;
}

const HelperSchema* SchemaRegistry::helper(const std::string_view name) const noexcept {
    const auto found = helpers_.find(std::string(name));
    return found == helpers_.end() ? nullptr : &found->second;
}

const MaterialSchema* SchemaRegistry::material(const std::string_view id) const noexcept {
    const auto found = materials_.find(std::string(id));
    return found == materials_.end() ? nullptr : &found->second;
}

const EffectSchema* SchemaRegistry::effect(const std::string_view name) const noexcept {
    const auto found = effects_.find(std::string(name));
    return found == effects_.end() ? nullptr : &found->second;
}

const SemanticType*
SchemaRegistry::component_parameter_type(const std::string_view name) const noexcept {
    const auto found = component_parameter_types_.find(normalized_semantic_name(name));
    return found == component_parameter_types_.end() ? nullptr : found->second.get();
}

const SchemaParameter* SchemaRegistry::layout_property(const std::string_view name) const noexcept {
    const auto found = std::ranges::find(layout_properties_, name, &SchemaParameter::name);
    return found != layout_properties_.end() ? &*found : nullptr;
}

const SchemaParameter* SchemaRegistry::style_property(const std::string_view name) const noexcept {
    const auto found = std::ranges::find(style_properties_, name, &SchemaParameter::name);
    return found != style_properties_.end() ? &*found : nullptr;
}

const SchemaParameter*
SchemaRegistry::animation_property(const std::string_view name) const noexcept {
    const auto found = std::ranges::find(animation_properties_, name, &SchemaParameter::name);
    return found != animation_properties_.end() ? &*found : nullptr;
}

const SchemaParameter*
SchemaRegistry::animation_timing_property(const std::string_view name) const noexcept {
    const auto found =
        std::ranges::find(animation_timing_properties_, name, &SchemaParameter::name);
    return found != animation_timing_properties_.end() ? &*found : nullptr;
}

std::vector<std::string> SchemaRegistry::layout_property_names() const {
    std::vector<std::string> names;
    names.reserve(layout_properties_.size());
    for (const SchemaParameter& property : layout_properties_)
        names.push_back(property.name);
    std::ranges::sort(names);
    return names;
}

namespace {

[[nodiscard]] std::vector<std::string>
parameter_names(const std::vector<SchemaParameter>& parameters) {
    std::vector<std::string> names;
    names.reserve(parameters.size());
    for (const SchemaParameter& parameter : parameters)
        names.push_back(parameter.name);
    std::ranges::sort(names);
    return names;
}

} // namespace

std::vector<std::string> SchemaRegistry::style_property_names() const {
    return parameter_names(style_properties_);
}

std::vector<std::string> SchemaRegistry::animation_property_names() const {
    return parameter_names(animation_properties_);
}

std::vector<std::string> SchemaRegistry::animation_timing_property_names() const {
    return parameter_names(animation_timing_properties_);
}

bool SchemaRegistry::has_material(const std::string_view id) const noexcept {
    return std::ranges::binary_search(material_ids_, id);
}

std::vector<std::string> SchemaRegistry::widget_names() const {
    std::vector<std::string> names;
    names.reserve(widgets_.size());
    for (const auto& [name, schema] : widgets_) {
        static_cast<void>(schema);
        names.push_back(name);
    }
    std::ranges::sort(names);
    return names;
}

std::vector<std::string> SchemaRegistry::material_ids() const {
    return material_ids_;
}

std::vector<std::string> SchemaRegistry::effect_names() const {
    std::vector<std::string> names;
    names.reserve(effects_.size());
    for (const auto& [name, schema] : effects_) {
        static_cast<void>(schema);
        names.push_back(name);
    }
    std::ranges::sort(names);
    return names;
}

std::vector<std::string> SchemaRegistry::action_names() const {
    std::vector<std::string> names;
    names.reserve(actions_.size());
    for (const auto& [name, schema] : actions_) {
        static_cast<void>(schema);
        names.push_back(name);
    }
    std::ranges::sort(names);
    return names;
}

void SchemaRegistry::apply_scenario_declarations(const data::JsonValue& schemas) {
    const data::JsonValue& widgets = required(schemas, "widgets");
    for (const data::JsonValue& value : array_field(widgets, "definitions")) {
        WidgetSchema schema;
        schema.name = string_field(value, "name");
        schema.allows_children = bool_field(value, "allowsChildren");
        for (const data::JsonValue& parameter : array_field(value, "parameters")) {
            schema.parameters.push_back(parse_parameter(parameter));
        }
        for (const SchemaParameter& framework : framework_widget_parameters_) {
            const bool declared = std::ranges::any_of(
                schema.parameters, [&framework](const SchemaParameter& parameter) {
                    return parameter.name == framework.name;
                });
            if (!declared || framework.name == "behaviors")
                schema.parameters.push_back(framework);
        }
        widgets_.insert_or_assign(schema.name, std::move(schema));
    }
    const data::JsonValue& actions = required(schemas, "actions");
    for (const data::JsonValue& value : array_field(actions, "definitions")) {
        ActionSchema schema;
        schema.id = string_field(value, "id");
        schema.dispatch_policy = string_field(value, "dispatchPolicy");
        schema.payload_contract = string_field(value, "payloadContract");
        schema.summary = string_field(value, "summary");
        for (const data::JsonValue& parameter : array_field(value, "arguments")) {
            schema.parameters.push_back(parse_parameter(parameter));
        }
        actions_.insert_or_assign(schema.id, std::move(schema));
    }
    if (const data::JsonValue* materials = schemas.find("materials"); materials != nullptr) {
        for (const data::JsonValue& value : array_field(*materials, "definitions")) {
            MaterialSchema schema;
            schema.id = string_field(value, "id");
            if (const data::JsonValue* fallback = value.find("fallback");
                fallback != nullptr && fallback->string() != nullptr) {
                schema.fallback = *fallback->string();
            }
            if (const data::JsonValue* blend = value.find("blendMode");
                blend != nullptr && blend->string() != nullptr) {
                schema.blend_mode = *blend->string();
            }
            std::size_t slots = 0U;
            for (const data::JsonValue& parameter : array_field(value, "parameters")) {
                SchemaParameter declared = parse_parameter(parameter);
                if (const data::JsonValue* material_type = parameter.find("materialType");
                    material_type != nullptr && material_type->string() != nullptr) {
                    declared.material_type = *material_type->string();
                }
                slots += material_parameter_width(declared.material_type);
                schema.parameters.push_back(std::move(declared));
            }
            if (slots > maximum_material_slots) {
                throw std::runtime_error("material '" + schema.id + "' declares more than " +
                                         std::to_string(maximum_material_slots) +
                                         " parameter floats");
            }
            if (const data::JsonValue* shaders = value.find("shaders"); shaders != nullptr) {
                if (shaders->object() == nullptr) {
                    throw std::runtime_error("material shaders must be a backend object");
                }
                for (const auto& [backend, source] : *shaders->object()) {
                    if (source.string() == nullptr) {
                        throw std::runtime_error("material shader source must be a string");
                    }
                    schema.shaders.emplace_back(backend, *source.string());
                }
            }
            if (materials_.find(schema.id) == materials_.end()) {
                // material_ids_ stays sorted; has_material() answers from a binary search.
                material_ids_.insert(std::ranges::upper_bound(material_ids_, schema.id), schema.id);
            }
            materials_.insert_or_assign(schema.id, std::move(schema));
        }
    }
    if (const data::JsonValue* effects = schemas.find("effects"); effects != nullptr) {
        for (const data::JsonValue& value : array_field(*effects, "definitions")) {
            EffectSchema schema;
            schema.name = string_field(value, "id");
            if (const data::JsonValue* input = value.find("input");
                input != nullptr && input->string() != nullptr) {
                schema.input = *input->string();
            }
            if (schema.input != "BACKDROP" && schema.input != "CONTENT") {
                throw std::runtime_error("effect '" + schema.name + "' has an unsupported input");
            }
            std::size_t slots = 0U;
            for (const data::JsonValue& parameter : array_field(value, "parameters")) {
                SchemaParameter declared = parse_parameter(parameter);
                if (declared.name == "refreshRate") {
                    throw std::runtime_error("effect '" + schema.name +
                                             "' uses reserved parameter name 'refreshRate'");
                }
                if (const data::JsonValue* material_type = parameter.find("effectType");
                    material_type != nullptr && material_type->string() != nullptr) {
                    declared.material_type = *material_type->string();
                }
                if (!declared.material_type.has_value() ||
                    (*declared.material_type != "FLOAT" && *declared.material_type != "INT" &&
                     *declared.material_type != "FLOAT2" && *declared.material_type != "FLOAT4" &&
                     *declared.material_type != "COLOR")) {
                    throw std::runtime_error(
                        "effect '" + schema.name +
                        "' parameter requires FLOAT, INT, FLOAT2, FLOAT4, or COLOR effectType");
                }
                slots += material_parameter_width(declared.material_type);
                schema.parameters.push_back(std::move(declared));
            }
            if (slots > 16U) {
                throw std::runtime_error("effect '" + schema.name +
                                         "' declares more than 16 parameter floats");
            }
            const data::JsonValue::Array& passes = array_field(value, "passes");
            if (passes.size() > 16U) {
                throw std::runtime_error("effect '" + schema.name +
                                         "' declares more than 16 passes");
            }
            for (const data::JsonValue& pass_value : passes) {
                EffectSchema::Pass pass;
                pass.kind = string_field(pass_value, "kind");
                if (pass.kind != "BLUR" && pass.kind != "SHADER") {
                    throw std::runtime_error("effect '" + schema.name +
                                             "' has an unsupported pass");
                }
                if (const data::JsonValue* radius = pass_value.find("radius"); radius != nullptr) {
                    if (radius->number() == nullptr || !std::isfinite(*radius->number()) ||
                        *radius->number() < 0.0) {
                        throw std::runtime_error(
                            "effect blur radius must be a finite non-negative number");
                    }
                    pass.radius = *radius->number();
                }
                if (const data::JsonValue* parameter = pass_value.find("radiusParameter");
                    parameter != nullptr && parameter->string() != nullptr) {
                    pass.radius_parameter = *parameter->string();
                }
                if (const data::JsonValue* downsample = pass_value.find("downsample");
                    downsample != nullptr) {
                    if (downsample->integer() == nullptr || *downsample->integer() < 1 ||
                        *downsample->integer() > 8) {
                        throw std::runtime_error(
                            "effect blur downsample must be an integer from 1 through 8");
                    }
                    pass.downsample = static_cast<std::uint32_t>(*downsample->integer());
                }
                if (const data::JsonValue* parameter = pass_value.find("downsampleParameter");
                    parameter != nullptr && parameter->string() != nullptr) {
                    pass.downsample_parameter = *parameter->string();
                }
                if (const data::JsonValue* shaders = pass_value.find("shaders");
                    shaders != nullptr) {
                    if (shaders->object() == nullptr) {
                        throw std::runtime_error("effect pass shaders must be a backend object");
                    }
                    for (const auto& [backend, source] : *shaders->object()) {
                        if (source.string() == nullptr) {
                            throw std::runtime_error("effect shader source must be a string");
                        }
                        pass.shaders.emplace_back(backend, *source.string());
                    }
                }
                const auto declared_parameter = [&schema](const std::optional<std::string>& name) {
                    return !name.has_value() ||
                           std::ranges::contains(schema.parameters, *name, &SchemaParameter::name);
                };
                if (!declared_parameter(pass.radius_parameter) ||
                    !declared_parameter(pass.downsample_parameter)) {
                    throw std::runtime_error("effect pass references an undeclared parameter");
                }
                const auto scalar_parameter = [&schema](const std::optional<std::string>& name) {
                    if (!name.has_value())
                        return true;
                    const SchemaParameter* parameter = schema.find_parameter(*name);
                    return parameter != nullptr && parameter->material_type.has_value() &&
                           (*parameter->material_type == "FLOAT" ||
                            *parameter->material_type == "INT");
                };
                if (!scalar_parameter(pass.radius_parameter) ||
                    !scalar_parameter(pass.downsample_parameter)) {
                    throw std::runtime_error(
                        "effect blur controls must reference FLOAT or INT parameters");
                }
                if (pass.kind == "SHADER" && pass.shaders.empty()) {
                    throw std::runtime_error(
                        "shader effect pass requires at least one backend source");
                }
                schema.passes.push_back(std::move(pass));
            }
            if (schema.passes.empty()) {
                throw std::runtime_error("effect '" + schema.name + "' requires at least one pass");
            }
            if (!effects_.emplace(schema.name, std::move(schema)).second) {
                throw std::runtime_error("effect id is declared more than once");
            }
        }
    }
    for (const data::JsonValue& host : array_field(schemas, "host")) {
        host_types_.insert_or_assign(string_field(host, "path"),
                                     parse_type(required(host, "type")));
    }
}

const std::unordered_map<std::string, SemanticTypePtr>&
SchemaRegistry::host_types() const noexcept {
    return host_types_;
}

} // namespace strata::compiler
