#include "compiler/portable_ir.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace strata::compiler {
namespace {

using data::JsonValue;

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> entries) {
    return JsonValue(JsonValue::Object(entries));
}

[[nodiscard]] JsonValue array(std::vector<JsonValue> values) {
    return JsonValue(JsonValue::Array(std::move(values)));
}

[[nodiscard]] JsonValue null() {
    return JsonValue(JsonValue::Null{});
}

[[nodiscard]] JsonValue repeater_identity_from_block(const JsonValue& block) {
    std::vector<JsonValue> identities;
    const JsonValue* statements_value = block.find("statements");
    const JsonValue::Array* statements =
        statements_value != nullptr ? statements_value->array() : nullptr;
    if (statements == nullptr) return null();
    for (const JsonValue& statement : *statements) {
        const JsonValue* kind_value = statement.find("kind");
        const std::string* kind = kind_value != nullptr ? kind_value->string() : nullptr;
        if (kind == nullptr) return null();
        if (*kind == "node") {
            const JsonValue* call = statement.find("call");
            const JsonValue* arguments = call != nullptr ? call->find("arguments") : nullptr;
            const JsonValue* key = arguments != nullptr ? arguments->find("key") : nullptr;
            if (key == nullptr && arguments != nullptr) key = arguments->find("rowKey");
            if (key == nullptr) return null();
            identities.push_back(object({
                {"expression", *key},
                {"kind", JsonValue("key")},
            }));
        } else if (*kind == "if") {
            const JsonValue* condition = statement.find("condition");
            const JsonValue* then_block = statement.find("then");
            const JsonValue* else_block = statement.find("else");
            if (condition == nullptr || then_block == nullptr || else_block == nullptr ||
                else_block->is_null()) return null();
            JsonValue then_identity = repeater_identity_from_block(*then_block);
            JsonValue else_identity = repeater_identity_from_block(*else_block);
            if (then_identity.is_null() || else_identity.is_null()) return null();
            identities.push_back(object({
                {"condition", *condition},
                {"else", std::move(else_identity)},
                {"kind", JsonValue("if")},
                {"then", std::move(then_identity)},
            }));
        } else if (*kind == "when") {
            const JsonValue* subject = statement.find("subject");
            const JsonValue* branches_value = statement.find("branches");
            const JsonValue::Array* branches =
                branches_value != nullptr ? branches_value->array() : nullptr;
            if (subject == nullptr || branches == nullptr) return null();
            std::vector<JsonValue> identity_branches;
            identity_branches.reserve(branches->size());
            for (const JsonValue& branch : *branches) {
                const JsonValue* branch_block = branch.find("block");
                const JsonValue* match = branch.find("match");
                if (branch_block == nullptr || match == nullptr) return null();
                JsonValue branch_identity = repeater_identity_from_block(*branch_block);
                if (branch_identity.is_null()) return null();
                identity_branches.push_back(object({
                    {"identity", std::move(branch_identity)},
                    {"match", *match},
                }));
            }
            identities.push_back(object({
                {"branches", array(std::move(identity_branches))},
                {"kind", JsonValue("when")},
                {"subject", *subject},
            }));
        } else return null();
    }
    return object({
        {"kind", JsonValue("block")},
        {"statements", array(std::move(identities))},
    });
}

[[nodiscard]] JsonValue nullable_string(const std::optional<std::string>& value) {
    return value.has_value() ? JsonValue(*value) : null();
}

[[nodiscard]] JsonValue position(const SourcePosition& value) {
    return object({
        {"column", JsonValue(static_cast<std::int64_t>(value.column))},
        {"line", JsonValue(static_cast<std::int64_t>(value.line))},
        {"offset", JsonValue(static_cast<std::int64_t>(value.offset))},
    });
}

[[nodiscard]] JsonValue span(const SourceSpan& value) {
    return object({
        {"end", position(value.end)},
        {"length", JsonValue(static_cast<std::int64_t>(value.length))},
        {"sourceId", JsonValue(value.source_id)},
        {"start", position(value.start)},
    });
}

[[nodiscard]] JsonValue range(const SourceSpan& value) {
    return object({
        {"end", position(value.end)},
        {"sourceId", JsonValue(value.source_id)},
        {"start", position(value.start)},
    });
}

[[nodiscard]] std::string child(const std::string_view path, const std::string_view segment) {
    return std::string(path) + "/" + std::string(segment);
}

[[nodiscard]] double parse_number(std::string raw) {
    raw.erase(std::remove(raw.begin(), raw.end(), '_'), raw.end());
    double value = 0.0;
    const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (result.ec != std::errc{} || result.ptr != raw.data() + raw.size() || !std::isfinite(value)) {
        return 0.0;
    }
    return value;
}

[[nodiscard]] std::string color_rgba(std::string raw) {
    if (!raw.empty() && raw.front() == '#') raw.erase(raw.begin());
    std::ranges::transform(raw, raw.begin(), [](const unsigned char character) {
        if (character >= 'A' && character <= 'F') {
            return static_cast<char>(character - 'A' + 'a');
        }
        return static_cast<char>(character);
    });
    if (raw.size() == 6U) raw += "ff";
    return raw;
}

[[nodiscard]] JsonValue semantic_type(const SemanticType& type);

[[nodiscard]] JsonValue schema_parameter(const SchemaParameter& parameter) {
    std::vector<JsonValue> aliases;
    aliases.reserve(parameter.aliases.size());
    for (const std::string& alias : parameter.aliases) aliases.emplace_back(alias);
    std::ranges::sort(aliases, {}, [](const JsonValue& value) { return *value.string(); });
    return object({
        {"aliases", array(std::move(aliases))},
        {"default", null()},
        {"name", JsonValue(parameter.name)},
        {"nullable", JsonValue(parameter.nullable)},
        {"required", JsonValue(parameter.required)},
        {"type", semantic_type(*parameter.type)},
    });
}

[[nodiscard]] JsonValue type_base(
    std::string kind,
    std::initializer_list<JsonValue::ObjectEntry> fields = {}
) {
    JsonValue::Object entries;
    entries.emplace_back("kind", JsonValue(std::move(kind)));
    entries.insert(entries.end(), fields.begin(), fields.end());
    return JsonValue(std::move(entries));
}

[[nodiscard]] JsonValue semantic_type(const SemanticType& type) {
    switch (type.kind) {
    case SemanticTypeKind::unknown: return type_base("unknown");
    case SemanticTypeKind::any: return type_base("any");
    case SemanticTypeKind::unsafe_component_parameter: return type_base("unsafeComponentParameter");
    case SemanticTypeKind::null_value: return type_base("null");
    case SemanticTypeKind::string: return type_base("string");
    case SemanticTypeKind::string_literal:
        return type_base("stringLiteral", {{"value", JsonValue(type.literal)}});
    case SemanticTypeKind::number: return type_base("number");
    case SemanticTypeKind::duration: return type_base("duration");
    case SemanticTypeKind::boolean: return type_base("boolean");
    case SemanticTypeKind::color: return type_base("color");
    case SemanticTypeKind::path: return type_base("path");
    case SemanticTypeKind::texture: return type_base("texture");
    case SemanticTypeKind::key: return type_base("key");
    case SemanticTypeKind::style: return type_base("style");
    case SemanticTypeKind::layout: return type_base("layout");
    case SemanticTypeKind::animation: return type_base("animation");
    case SemanticTypeKind::effect: return type_base("effect");
    case SemanticTypeKind::material: return type_base("material");
    case SemanticTypeKind::action: return type_base("action");
    case SemanticTypeKind::component: return type_base("component");
    case SemanticTypeKind::lambda:
        return type_base("lambda", {
            {"parameter", semantic_type(*type.parameter)},
            {"returns", semantic_type(*type.returns)},
        });
    case SemanticTypeKind::component_template: {
        std::vector<JsonValue> parameters;
        for (const ObjectField& field : type.fields) {
            parameters.push_back(object({
                {"name", JsonValue(field.name)},
                {"type", semantic_type(*field.type)},
            }));
        }
        return type_base("componentTemplate", {{"parameters", array(std::move(parameters))}});
    }
    case SemanticTypeKind::enumeration: {
        std::vector<JsonValue> values;
        for (const std::string& value : type.values) values.emplace_back(value);
        std::ranges::sort(values, {}, [](const JsonValue& value) { return *value.string(); });
        return type_base("enum", {
            {"label", JsonValue(type.label)},
            {"values", array(std::move(values))},
        });
    }
    case SemanticTypeKind::list:
        return type_base("list", {
            {"element", semantic_type(*type.element)},
            {"elementNullable", JsonValue(type.element_nullable)},
            {"maximumItems", type.maximum_items.has_value()
                                 ? JsonValue(static_cast<std::int64_t>(*type.maximum_items))
                                 : null()},
            {"minimumItems", type.minimum_items.has_value()
                                 ? JsonValue(static_cast<std::int64_t>(*type.minimum_items))
                                 : null()},
        });
    case SemanticTypeKind::map: {
        std::vector<JsonValue> fields;
        for (const ObjectField& field : type.fields) {
            fields.push_back(object({
                {"name", JsonValue(field.name)},
                {"nullable", JsonValue(field.nullable)},
                {"required", JsonValue(field.required)},
                {"type", semantic_type(*field.type)},
            }));
        }
        std::ranges::sort(fields, {}, [](const JsonValue& value) {
            return *value.find("name")->string();
        });
        const SemanticType fallback{SemanticTypeKind::any};
        return type_base("map", {
            {"allowUnknownFields", JsonValue(type.allow_unknown_fields)},
            {"fields", array(std::move(fields))},
            {"label", JsonValue(type.label.empty() ? "object" : type.label)},
            {"value", semantic_type(type.value != nullptr ? *type.value : fallback)},
            {"valueNullable", JsonValue(type.value_nullable)},
        });
    }
    case SemanticTypeKind::union_value: {
        std::vector<JsonValue> options;
        for (const SemanticTypePtr& option : type.options) options.push_back(semantic_type(*option));
        return type_base("union", {
            {"label", JsonValue(type.label)},
            {"options", array(std::move(options))},
        });
    }
    case SemanticTypeKind::host_object: {
        std::vector<JsonValue> fields;
        for (const ObjectField& field : type.fields) {
            fields.push_back(object({
                {"name", JsonValue(field.name)},
                {"type", semantic_type(*field.type)},
            }));
        }
        return type_base("hostObject", {
            {"fields", array(std::move(fields))},
            {"path", JsonValue(type.label)},
        });
    }
    case SemanticTypeKind::async_value:
        return type_base("async", {
            {"label", JsonValue(type.label)},
            {"value", semantic_type(*type.value)},
        });
    case SemanticTypeKind::collection:
        return type_base("collection", {
            {"hostPath", null()},
            {"item", semantic_type(*type.element)},
            {"maximumItems", type.maximum_items.has_value()
                                 ? JsonValue(static_cast<std::int64_t>(*type.maximum_items))
                                 : null()},
        });
    }
    throw std::runtime_error("invalid semantic type");
}

enum class BindingKind { local, parameter, host, style, animation, component, unknown };

struct Binding final {
    BindingKind kind = BindingKind::unknown;
    SemanticTypePtr type;
    bool retained_state = false;
};

using Scope = std::unordered_map<std::string, Binding>;

[[nodiscard]] std::string binding_name(const BindingKind kind) {
    switch (kind) {
    case BindingKind::local: return "local";
    case BindingKind::parameter: return "parameter";
    case BindingKind::host: return "host";
    case BindingKind::style: return "style";
    case BindingKind::animation: return "animation";
    case BindingKind::component: return "component";
    case BindingKind::unknown: return "unknown";
    }
    throw std::runtime_error("invalid binding kind");
}

struct ComponentSchema final {
    std::vector<SchemaParameter> parameters;
};

struct ActionReference final {
    std::string component_path;
    const ActionSchema* schema;
    SourceSpan span;
};

class Lowerer final {
public:
    Lowerer(
        const File& file,
        const SchemaRegistry& registry,
        const std::map<std::string, ValidatedAnimation, std::less<>>& validated_animations
    ) : file_(file), registry_(registry), validated_animations_(validated_animations) {
        for (const Declaration& declaration : file_.declarations) {
            if (const auto* component = std::get_if<ComponentDeclaration>(&declaration.node)) {
                ComponentSchema schema;
                for (const Parameter& parameter : component->parameters) {
                    SemanticTypePtr type = parameter.type_reference.has_value()
                                               ? resolve_type(*parameter.type_reference)
                                               : type_of(SemanticTypeKind::unknown);
                    schema.parameters.push_back(SchemaParameter{
                        parameter.name,
                        std::move(type),
                        parameter.default_value == nullptr,
                        parameter.type_reference.has_value() && parameter.type_reference->nullable,
                        {},
                    });
                }
                components_.emplace(component->name, std::move(schema));
            } else if (const auto* style = std::get_if<StyleDeclaration>(&declaration.node)) {
                styles_.insert(style->name);
            } else if (const auto* animation = std::get_if<AnimationDeclaration>(&declaration.node)) {
                animations_.insert(animation->name);
            }
        }
    }

    [[nodiscard]] PortableIrResult run() {
        std::vector<std::pair<std::string, JsonValue>> screens;
        std::vector<std::pair<std::string, JsonValue>> overlays;
        std::vector<std::pair<std::string, JsonValue>> components;
        std::vector<std::pair<std::string, JsonValue>> styles;
        std::vector<std::pair<std::string, JsonValue>> animations;
        for (const Declaration& declaration : file_.declarations) {
            if (const auto* screen = std::get_if<ScreenDeclaration>(&declaration.node)) {
                screens.emplace_back(screen->name, compile_screen(*screen, declaration.span));
            } else if (const auto* overlay = std::get_if<OverlayDeclaration>(&declaration.node)) {
                overlays.emplace_back(overlay->name, compile_overlay(*overlay, declaration.span));
            } else if (const auto* component = std::get_if<ComponentDeclaration>(&declaration.node)) {
                components.emplace_back(component->name, compile_component(*component, declaration.span));
            } else if (const auto* style = std::get_if<StyleDeclaration>(&declaration.node)) {
                styles.emplace_back(style->name, compile_style(*style, declaration.span));
            } else if (const auto* animation = std::get_if<AnimationDeclaration>(&declaration.node)) {
                const auto validated = validated_animations_.find(animation->name);
                if (validated == validated_animations_.end()) {
                    throw std::logic_error(
                        "portable animation lowering received an unvalidated declaration"
                    );
                }
                animations.emplace_back(animation->name, compile_animation(validated->second));
            }
        }
        const auto sorted_values = [](std::vector<std::pair<std::string, JsonValue>>& entries) {
            std::ranges::sort(entries, {}, &std::pair<std::string, JsonValue>::first);
            std::vector<JsonValue> values;
            values.reserve(entries.size());
            for (auto& [name, value] : entries) {
                static_cast<void>(name);
                values.push_back(std::move(value));
            }
            return values;
        };

        std::ranges::stable_sort(action_references_, {}, &ActionReference::component_path);
        std::vector<JsonValue> action_references;
        for (const ActionReference& reference : action_references_) {
            action_references.push_back(object({
                {"componentPath", JsonValue(reference.component_path)},
                {"dispatchPolicy", JsonValue(reference.schema->dispatch_policy)},
                {"id", JsonValue(reference.schema->id)},
                {"payloadContract", JsonValue(reference.schema->payload_contract)},
                {"range", range(reference.span)},
            }));
        }
        std::vector<JsonValue> materials;
        for (const std::string& id : material_ids_) {
            materials.push_back(object({
                {"id", JsonValue(id)},
                {"name", JsonValue(id)},
                {"parameters", array({})},
            }));
        }
        JsonValue unit = object({
            {"actionReferences", array(std::move(action_references))},
            {"animations", array(sorted_values(animations))},
            {"components", array(sorted_values(components))},
            {"materials", array(std::move(materials))},
            {"overlays", array(sorted_values(overlays))},
            {"screens", array(sorted_values(screens))},
            {"sourceId", JsonValue(file_.source_id)},
            {"styles", array(sorted_values(styles))},
        });
        if (!diagnostics_.empty()) return PortableIrResult{std::nullopt, std::move(diagnostics_)};
        validate_portable_ir(unit);
        return PortableIrResult{std::move(unit), {}};
    }

private:
    [[nodiscard]] SemanticTypePtr type_of(const SemanticTypeKind kind) const {
        auto type = std::make_shared<SemanticType>();
        type->kind = kind;
        return type;
    }

    [[nodiscard]] SemanticTypePtr resolve_type(const TypeReference& reference) const {
        std::string normalized = reference.name;
        std::ranges::transform(normalized, normalized.begin(), [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if (normalized == "list") {
            auto type = std::make_shared<SemanticType>();
            type->kind = SemanticTypeKind::list;
            type->element = reference.arguments.size() == 1U
                                ? resolve_type(reference.arguments.front())
                                : type_of(SemanticTypeKind::any);
            type->maximum_items = 1000U;
            return type;
        }
        if (normalized == "map" || normalized == "record") {
            auto type = std::make_shared<SemanticType>();
            type->kind = SemanticTypeKind::map;
            type->label = normalized;
            type->allow_unknown_fields = true;
            type->value = reference.arguments.empty()
                              ? type_of(SemanticTypeKind::any)
                              : resolve_type(reference.arguments.front());
            return type;
        }
        if (const SemanticType* type = registry_.component_parameter_type(normalized); type != nullptr) {
            return std::make_shared<SemanticType>(*type);
        }
        return type_of(SemanticTypeKind::unknown);
    }

    [[nodiscard]] JsonValue compile_screen(const ScreenDeclaration& screen, const SourceSpan& source_span) {
        const std::string path = "/screen/" + screen.name;
        return object({
            {"body", compile_block(*screen.body, child(path, "body"), "screen " + screen.name, {})},
            {"name", JsonValue(screen.name)},
            {"path", JsonValue(path)},
            {"span", span(source_span)},
        });
    }

    [[nodiscard]] JsonValue compile_overlay(const OverlayDeclaration& overlay, const SourceSpan& source_span) {
        const std::string path = "/overlay/" + overlay.name;
        return object({
            {"body", compile_block(*overlay.body, child(path, "body"), "overlay " + overlay.name, {})},
            {"name", JsonValue(overlay.name)},
            {"path", JsonValue(path)},
            {"span", span(source_span)},
        });
    }

    [[nodiscard]] JsonValue compile_component(
        const ComponentDeclaration& component,
        const SourceSpan& source_span
    ) {
        const std::string path = "/component/" + component.name;
        const ComponentSchema& schema = components_.at(component.name);
        Scope scope;
        std::vector<JsonValue> parameters;
        for (std::size_t index = 0U; index < component.parameters.size(); ++index) {
            const Parameter& parameter = component.parameters[index];
            const SchemaParameter& parameter_schema = schema.parameters[index];
            scope.insert_or_assign(parameter.name, Binding{BindingKind::parameter, parameter_schema.type});
            parameters.push_back(object({
                {"default", parameter.default_value != nullptr
                                ? compile_expression(
                                      *parameter.default_value,
                                      child(child(child(path, "parameter"), parameter.name), "default"),
                                      "component " + component.name + "(" + parameter.name + ")",
                                      scope,
                                      parameter_schema.type.get()
                                  )
                                : null()},
                {"schema", schema_parameter(parameter_schema)},
                {"span", span(parameter.span)},
            }));
        }
        std::vector<std::pair<std::string, JsonValue>> widget_defaults;
        for (const StatementPtr& statement : component.body->statements) {
            const auto* property = std::get_if<PropertyStatement>(&statement->node);
            if (property == nullptr || property->property.name != "defaults") continue;
            const auto* defaults = std::get_if<MapExpression>(&property->property.value->node);
            if (defaults == nullptr) continue;
            for (const MapEntry& widget_entry : defaults->entries) {
                const std::optional<std::string> widget_name = static_map_key(widget_entry.key);
                const auto* values = std::get_if<MapExpression>(&widget_entry.value->node);
                if (!widget_name.has_value() || values == nullptr ||
                    registry_.widget(*widget_name) == nullptr) {
                    continue;
                }
                const Expression* style = nullptr;
                const Expression* variant = nullptr;
                for (const MapEntry& value : values->entries) {
                    const std::optional<std::string> name = static_map_key(value.key);
                    if (name == "style") style = value.value.get();
                    else if (name == "variant") variant = value.value.get();
                }
                widget_defaults.emplace_back(
                    *widget_name,
                    object({
                        {"name", JsonValue(*widget_name)},
                        {"style", style != nullptr
                                      ? compile_expression(
                                            *style,
                                            child(child(child(path, "defaults"), *widget_name), "style"),
                                            "component " + component.name + "/defaults/" +
                                                *widget_name + ".style",
                                            scope,
                                            type_of(SemanticTypeKind::style).get()
                                        )
                                      : null()},
                        {"variant", variant != nullptr
                                        ? compile_expression(
                                              *variant,
                                              child(child(child(path, "defaults"), *widget_name), "variant"),
                                              "component " + component.name + "/defaults/" +
                                                  *widget_name + ".variant",
                                              scope,
                                              type_of(SemanticTypeKind::string).get()
                                          )
                                        : null()},
                    })
                );
            }
            break;
        }
        std::ranges::sort(widget_defaults, {}, &decltype(widget_defaults)::value_type::first);
        std::vector<JsonValue> compiled_defaults;
        compiled_defaults.reserve(widget_defaults.size());
        for (auto& [name, value] : widget_defaults) {
            static_cast<void>(name);
            compiled_defaults.push_back(std::move(value));
        }
        return object({
            {"body", compile_block(
                         *component.body,
                         child(path, "body"),
                         "component " + component.name,
                         std::move(scope)
                     )},
            {"name", JsonValue(component.name)},
            {"parameters", array(std::move(parameters))},
            {"path", JsonValue(path)},
            {"span", span(source_span)},
            {"widgetDefaults", array(std::move(compiled_defaults))},
        });
    }

    [[nodiscard]] JsonValue compile_style(const StyleDeclaration& style, const SourceSpan& source_span) {
        const std::string path = "/style/" + style.name;
        JsonValue::Object properties;
        for (const Property& property : style.properties) {
            properties.emplace_back(
                property.name,
                compile_expression(
                    *property.value,
                    child(path, property.name),
                    "style " + style.name + "." + property.name,
                    {},
                    nullptr
                )
            );
        }
        std::vector<JsonValue> bases;
        for (const StyleBase& base : style.bases) bases.emplace_back(base.name);
        return object({
            {"bases", array(std::move(bases))},
            {"name", JsonValue(style.name)},
            {"path", JsonValue(path)},
            {"properties", JsonValue(std::move(properties))},
            {"span", span(source_span)},
        });
    }

    [[nodiscard]] JsonValue compile_block(
        const Block& block,
        const std::string& path,
        const std::string& component_path,
        Scope scope,
        const bool repeater_body = false
    ) {
        for (const StatementPtr& statement : block.statements) {
            if (const auto* derived = std::get_if<DerivedStatement>(&statement->node)) {
                scope.insert_or_assign(derived->name, Binding{BindingKind::local, type_of(SemanticTypeKind::unknown)});
            }
        }
        std::vector<JsonValue> statements;
        for (std::size_t index = 0U; index < block.statements.size(); ++index) {
            const Statement& statement = *block.statements[index];
            const std::string statement_path = child(path, std::to_string(index));
            if (const auto* state = std::get_if<StateStatement>(&statement.node)) {
                SemanticTypePtr declared = state->type_reference.has_value()
                                               ? resolve_type(*state->type_reference)
                                               : infer_structured_type(state->initializer.get());
                JsonValue declared_schema = declared != nullptr ? semantic_type(*declared) : null();
                if (declared != nullptr && state->type_reference.has_value() &&
                    state->type_reference->nullable) {
                    declared_schema = type_base("union", {
                        {"label", JsonValue("nullable state")},
                        {"options", array({type_base("null"), std::move(declared_schema)})},
                    });
                }
                statements.push_back(object({
                    {"declaredSchema", std::move(declared_schema)},
                    {"declaredType", declared != nullptr ? JsonValue(declared->diagnostic_name()) : null()},
                    {"initializer", state->initializer != nullptr
                                         ? compile_expression(
                                               *state->initializer,
                                               child(statement_path, "initializer"),
                                               component_path + "/" + state->name,
                                               scope,
                                               nullptr
                                           )
                                         : null()},
                    {"kind", JsonValue("state")},
                    {"name", JsonValue(state->name)},
                    {"path", JsonValue(statement_path)},
                    {"span", span(statement.span)},
                }));
                scope.insert_or_assign(
                    state->name,
                    Binding{BindingKind::local, type_of(SemanticTypeKind::unknown), true}
                );
            } else if (const auto* derived = std::get_if<DerivedStatement>(&statement.node)) {
                statements.push_back(object({
                    {"expression", compile_expression(
                                       *derived->expression,
                                       child(statement_path, "expression"),
                                       component_path + "/" + derived->name,
                                       scope,
                                       nullptr
                                   )},
                    {"kind", JsonValue("derived")},
                    {"name", JsonValue(derived->name)},
                    {"path", JsonValue(statement_path)},
                    {"span", span(statement.span)},
                }));
                scope.insert_or_assign(derived->name, Binding{BindingKind::local, type_of(SemanticTypeKind::unknown)});
            } else if (const auto* widget = std::get_if<WidgetStatement>(&statement.node)) {
                statements.push_back(node_statement(
                    widget->call,
                    statement_path,
                    component_path + "/" + widget->call.name,
                    scope,
                    false,
                    statement.span
                ));
            } else if (const auto* root = std::get_if<RootStatement>(&statement.node)) {
                statements.push_back(node_statement(
                    root->call,
                    child(statement_path, "root"),
                    component_path + "/root/" + root->call.name,
                    scope,
                    true,
                    statement.span,
                    statement_path
                ));
            } else if (const auto* conditional = std::get_if<IfStatement>(&statement.node)) {
                statements.push_back(object({
                    {"condition", compile_expression(
                                      *conditional->condition,
                                      child(statement_path, "condition"),
                                      component_path + "/if",
                                      scope,
                                      nullptr
                                  )},
                    {"else", conditional->else_block != nullptr
                                 ? compile_block(
                                       *conditional->else_block,
                                       child(statement_path, "else"),
                                       component_path + "/else",
                                       scope
                                   )
                                 : null()},
                    {"kind", JsonValue("if")},
                    {"path", JsonValue(statement_path)},
                    {"span", span(statement.span)},
                    {"then", compile_block(
                                 *conditional->then_block,
                                 child(statement_path, "then"),
                                 component_path + "/then",
                                 scope
                             )},
                }));
            } else if (const auto* when = std::get_if<WhenStatement>(&statement.node)) {
                std::vector<JsonValue> branches;
                for (std::size_t branch_index = 0U; branch_index < when->branches.size(); ++branch_index) {
                    const WhenBranch& branch = when->branches[branch_index];
                    const std::string branch_path = child(
                        child(statement_path, "branch"), std::to_string(branch_index)
                    );
                    const std::string branch_component = component_path + "/when[" +
                                                         std::to_string(branch_index) + "]";
                    branches.push_back(object({
                        {"block", compile_block(*branch.block, branch_path, branch_component, scope)},
                        {"match", branch.match != nullptr
                                      ? compile_expression(
                                            *branch.match,
                                            child(branch_path, "match"),
                                            branch_component,
                                            scope,
                                            nullptr
                                        )
                                      : null()},
                        {"span", span(branch.span)},
                    }));
                }
                statements.push_back(object({
                    {"branches", array(std::move(branches))},
                    {"kind", JsonValue("when")},
                    {"path", JsonValue(statement_path)},
                    {"span", span(statement.span)},
                    {"subject", compile_expression(
                                    *when->subject,
                                    child(statement_path, "subject"),
                                    component_path + "/when",
                                    scope,
                                    nullptr
                                )},
                }));
            } else if (const auto* loop = std::get_if<ForStatement>(&statement.node)) {
                Scope loop_scope = scope;
                loop_scope.insert_or_assign(loop->item_name, Binding{BindingKind::local, type_of(SemanticTypeKind::unknown)});
                if (loop->index_name.has_value()) {
                    loop_scope.insert_or_assign(
                        *loop->index_name,
                        Binding{BindingKind::local, type_of(SemanticTypeKind::number)}
                    );
                }
                JsonValue compiled_body = compile_block(
                    *loop->block,
                    child(statement_path, "body"),
                    component_path + "/" + loop->item_name,
                    loop_scope
                );
                JsonValue::Object loop_fields{
                    {"block", std::move(compiled_body)},
                    {"collection", compile_expression(
                                       *loop->collection,
                                       child(statement_path, "collection"),
                                       component_path + "/for",
                                       scope,
                                       nullptr
                                   )},
                    {"filter", loop->filter != nullptr
                                   ? compile_expression(
                                         *loop->filter,
                                         child(statement_path, "filter"),
                                         component_path + "/for/filter",
                                         loop_scope,
                                         nullptr
                                     )
                                   : null()},
                };
                if (repeater_body) {
                    loop_fields.emplace_back(
                        "identity",
                        repeater_identity_from_block(loop_fields.front().second)
                    );
                }
                loop_fields.emplace_back("indexName", nullable_string(loop->index_name));
                loop_fields.emplace_back("itemName", JsonValue(loop->item_name));
                loop_fields.emplace_back("kind", JsonValue("for"));
                loop_fields.emplace_back("path", JsonValue(statement_path));
                loop_fields.emplace_back("span", span(statement.span));
                statements.emplace_back(std::move(loop_fields));
            }
        }
        return object({
            {"path", JsonValue(path)},
            {"span", span(block.span)},
            {"statements", array(std::move(statements))},
        });
    }

    [[nodiscard]] JsonValue node_statement(
        const WidgetCall& call,
        const std::string& call_path,
        const std::string& component_path,
        const Scope& scope,
        const bool root,
        const SourceSpan& statement_span,
        std::optional<std::string> statement_path = std::nullopt
    ) {
        return object({
            {"call", compile_call(call, call_path, component_path, scope)},
            {"kind", JsonValue("node")},
            {"path", JsonValue(statement_path.value_or(call_path))},
            {"root", JsonValue(root)},
            {"span", span(statement_span)},
        });
    }

    [[nodiscard]] JsonValue compile_call(
        const WidgetCall& call,
        const std::string& path,
        const std::string& component_path,
        const Scope& scope
    ) {
        const WidgetSchema* widget = registry_.widget(call.name);
        const auto component = components_.find(call.name);
        const std::vector<SchemaParameter>* parameters = nullptr;
        if (widget != nullptr) parameters = &widget->parameters;
        if (component != components_.end()) parameters = &component->second.parameters;
        const std::vector<SchemaParameter> empty;
        if (parameters == nullptr) parameters = &empty;

        struct ResolvedArgument final {
            const Argument* argument = nullptr;
            const SchemaParameter* parameter = nullptr;
            std::string name;
        };
        std::vector<ResolvedArgument> resolved;
        resolved.reserve(call.arguments.size());
        std::set<std::string, std::less<>> supplied;
        std::size_t positional_index = 0U;
        bool seen_named = false;
        for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
            const Argument& argument = call.arguments[index];
            const SchemaParameter* parameter = nullptr;
            if (!argument.name.has_value()) {
                if (!seen_named && positional_index < parameters->size()) {
                    parameter = &(*parameters)[positional_index++];
                }
            } else {
                seen_named = true;
                const auto found = std::ranges::find_if(*parameters, [&argument](const SchemaParameter& candidate) {
                    return candidate.accepts_name(*argument.name);
                });
                if (found != parameters->end()) parameter = &*found;
            }
            const std::string name = parameter != nullptr
                                         ? parameter->name
                                         : argument.name.value_or("#" + std::to_string(index));
            resolved.push_back(ResolvedArgument{&argument, parameter, name});
            if (parameter != nullptr) supplied.insert(parameter->name);
        }

        JsonValue::Object arguments;
        for (const ResolvedArgument& resolved_argument : resolved) {
            const Argument& argument = *resolved_argument.argument;
            const SchemaParameter* parameter = resolved_argument.parameter;
            if (widget != nullptr && parameter != nullptr &&
                widget->find_binding(parameter->name) != nullptr) {
                continue;
            }
            arguments.emplace_back(
                resolved_argument.name,
                compile_expression(
                    *argument.value,
                    child(child(path, "arguments"), resolved_argument.name),
                    component_path + "." + resolved_argument.name,
                    scope,
                    parameter != nullptr ? parameter->type.get() : nullptr
                )
            );
        }
        if (widget != nullptr) {
            for (const WidgetBindingSchema& binding : widget->bindings) {
                const auto bound = std::ranges::find_if(
                    resolved,
                    [&binding](const ResolvedArgument& argument) {
                        return argument.parameter != nullptr &&
                               argument.parameter->name == binding.shorthand_parameter;
                    }
                );
                if (bound == resolved.end() ||
                    supplied.contains(binding.value_parameter) ||
                    supplied.contains(binding.event_parameter)) {
                    continue;
                }
                const auto* identifier = std::get_if<IdentifierExpression>(
                    &bound->argument->value->node
                );
                const auto scoped = identifier != nullptr
                                        ? scope.find(identifier->name)
                                        : scope.end();
                if (identifier == nullptr || scoped == scope.end() ||
                    !scoped->second.retained_state) {
                    continue;
                }
                const SchemaParameter* controlled = widget->find_parameter(
                    binding.value_parameter
                );
                if (controlled == nullptr) {
                    throw std::logic_error("validated widget binding lost its controlled parameter");
                }
                const std::string value_path = child(
                    child(path, "arguments"), binding.value_parameter
                );
                arguments.emplace_back(
                    binding.value_parameter,
                    compile_expression(
                        *bound->argument->value,
                        value_path,
                        component_path + "." + binding.value_parameter,
                        scope,
                        controlled->type.get()
                    )
                );
                const std::string event_path = child(
                    child(path, "arguments"), binding.event_parameter
                );
                const auto undo_label = std::ranges::find(
                    resolved, std::string("undoLabel"), &ResolvedArgument::name
                );
                const auto undo_coalesce = std::ranges::find(
                    resolved, std::string("undoCoalesce"), &ResolvedArgument::name
                );
                arguments.emplace_back(
                    binding.event_parameter,
                    compile_binding_action(
                        identifier->name,
                        bound->argument->value->span,
                        event_path,
                        component_path + "." + binding.event_parameter,
                        undo_label != resolved.end() ? undo_label->argument : nullptr,
                        undo_coalesce != resolved.end() ? undo_coalesce->argument : nullptr,
                        scope
                    )
                );
            }
        }
        return object({
            {"arguments", JsonValue(std::move(arguments))},
            {"children", call.body != nullptr
                             ? compile_block(
                                   *call.body,
                                   child(path, "children"),
                                   component_path,
                                   scope,
                                   call.name == "Repeater"
                               )
                             : null()},
            {"kind", JsonValue(widget != nullptr ? "widget" : "component")},
            {"name", JsonValue(call.name)},
            {"path", JsonValue(path)},
            {"span", span(call.span)},
        });
    }

    [[nodiscard]] const SemanticType* nested_expected(
        const SemanticType* expected,
        const std::string_view field
    ) const {
        if (expected == nullptr) return nullptr;
        if (expected->kind == SemanticTypeKind::map) {
            if (const ObjectField* object_field = expected->find_field(field); object_field != nullptr) {
                return object_field->type.get();
            }
            return expected->value.get();
        }
        if (expected->kind == SemanticTypeKind::union_value) {
            for (const SemanticTypePtr& option : expected->options) {
                if (const SemanticType* nested = nested_expected(option.get(), field); nested != nullptr) {
                    return nested;
                }
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool accepts_kind(const SemanticType* expected, const SemanticTypeKind kind) const {
        if (expected == nullptr) return false;
        if (expected->kind == kind) return true;
        if (expected->kind == SemanticTypeKind::union_value) {
            return std::ranges::any_of(expected->options, [this, kind](const SemanticTypePtr& option) {
                return accepts_kind(option.get(), kind);
            });
        }
        return false;
    }

    [[nodiscard]] JsonValue compile_expression(
        const Expression& expression,
        const std::string& path,
        const std::string& component_path,
        const Scope& scope,
        const SemanticType* expected
    ) {
        if (const auto* property = std::get_if<PropertyAccessExpression>(&expression.node)) {
            if (const auto* receiver = std::get_if<IdentifierExpression>(&property->receiver->node);
                receiver != nullptr && receiver->name == "theme") {
                return expression_base(path, expression.span, {
                    {"kind", JsonValue("literal")},
                    {"value", object({
                        {"kind", JsonValue("themeToken")},
                        {"name", JsonValue(property->property_name)},
                    })},
                });
            }
        }
        if (const auto* grouping = std::get_if<GroupingExpression>(&expression.node)) {
            return compile_expression(
                *grouping->expression,
                child(path, "grouped"),
                component_path,
                scope,
                expected
            );
        }
        if (accepts_kind(expected, SemanticTypeKind::material)) {
            if (JsonValue special = compile_material(expression, path, component_path, scope);
                !special.is_null()) {
                return special;
            }
        }
        if (accepts_kind(expected, SemanticTypeKind::action)) {
            if (JsonValue special = compile_action(expression, path, component_path, scope);
                !special.is_null()) {
                return special;
            }
        }
        if (accepts_kind(expected, SemanticTypeKind::animation)) {
            std::optional<std::string> name;
            if (const std::string* literal = literal_string(expression); literal != nullptr) name = *literal;
            if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.node)) name = identifier->name;
            if (name.has_value()) {
                return expression_base(path, expression.span, {
                    {"kind", JsonValue("literal")},
                    {"value", object({
                        {"kind", JsonValue("animation")},
                        {"name", JsonValue(*name)},
                    })},
                });
            }
        }
        if (accepts_kind(expected, SemanticTypeKind::style)) {
            if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.node);
                identifier != nullptr && scope.find(identifier->name) == scope.end()) {
                return expression_base(path, expression.span, {
                    {"kind", JsonValue("literal")},
                    {"value", object({
                        {"kind", JsonValue("styleReference")},
                        {"name", JsonValue(identifier->name)},
                    })},
                });
            }
            if (const auto* call = std::get_if<CallExpression>(&expression.node);
                call != nullptr && call->target.qualified_name() == "style") {
                return compile_helper(*call, expression.span, path, component_path, scope);
            }
        }

        if (const auto* literal = std::get_if<LiteralExpression>(&expression.node)) {
            return expression_base(path, expression.span, {
                {"kind", JsonValue("literal")},
                {"value", literal_value(literal->value, expected)},
            });
        }
        if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.node)) {
            const Binding binding = binding_for(identifier->name, scope);
            return expression_base(path, expression.span, {
                {"binding", JsonValue(binding_name(binding.kind))},
                {"kind", JsonValue("variable")},
                {"name", JsonValue(identifier->name)},
                {"type", JsonValue(binding.type->diagnostic_name())},
            });
        }
        if (const auto* list = std::get_if<ListExpression>(&expression.node)) {
            std::vector<JsonValue> elements;
            const SemanticType* element_expected = expected != nullptr ? expected->element.get() : nullptr;
            for (std::size_t index = 0U; index < list->elements.size(); ++index) {
                elements.push_back(compile_expression(
                    *list->elements[index],
                    child(path, std::to_string(index)),
                    component_path + "[" + std::to_string(index) + "]",
                    scope,
                    element_expected
                ));
            }
            return expression_base(path, expression.span, {
                {"elements", array(std::move(elements))},
                {"kind", JsonValue("list")},
            });
        }
        if (const auto* map = std::get_if<MapExpression>(&expression.node)) {
            JsonValue::Object entries;
            for (std::size_t index = 0U; index < map->entries.size(); ++index) {
                const MapEntry& entry = map->entries[index];
                const std::string key = static_map_key(entry.key).value_or(
                    "dynamic_" + std::to_string(index)
                );
                entries.emplace_back(
                    key,
                    compile_expression(
                        *entry.value,
                        child(path, key),
                        component_path + "." + key,
                        scope,
                        nested_expected(expected, key)
                    )
                );
            }
            return expression_base(path, expression.span, {
                {"entries", JsonValue(std::move(entries))},
                {"kind", JsonValue("map")},
            });
        }
        if (const auto* unary = std::get_if<UnaryExpression>(&expression.node)) {
            return expression_base(path, expression.span, {
                {"kind", JsonValue("unary")},
                {"operand", compile_expression(
                                *unary->operand,
                                child(path, "operand"),
                                component_path,
                                scope,
                                nullptr
                            )},
                {"operator", JsonValue(unary->operation == UnaryOperator::negate ? "negate" : "not")},
            });
        }
        if (const auto* binary = std::get_if<BinaryExpression>(&expression.node)) {
            return expression_base(path, expression.span, {
                {"kind", JsonValue("binary")},
                {"left", compile_expression(
                             *binary->left,
                             child(path, "left"),
                             component_path + ".left",
                             scope,
                             nullptr
                         )},
                {"operator", JsonValue(binary_operator(binary->operation))},
                {"right", compile_expression(
                              *binary->right,
                              child(path, "right"),
                              component_path + ".right",
                              scope,
                              nullptr
                          )},
            });
        }
        if (const auto* conditional = std::get_if<ConditionalExpression>(&expression.node)) {
            return expression_base(path, expression.span, {
                {"condition", compile_expression(
                                  *conditional->condition,
                                  child(path, "condition"),
                                  component_path + ".condition",
                                  scope,
                                  nullptr
                              )},
                {"else", compile_expression(
                             *conditional->else_expression,
                             child(path, "else"),
                             component_path + ".else",
                             scope,
                             expected
                         )},
                {"kind", JsonValue("conditional")},
                {"then", compile_expression(
                             *conditional->then_expression,
                             child(path, "then"),
                             component_path + ".then",
                             scope,
                             expected
                         )},
            });
        }
        if (const auto* property = std::get_if<PropertyAccessExpression>(&expression.node)) {
            return expression_base(path, expression.span, {
                {"kind", JsonValue("property")},
                {"name", JsonValue(property->property_name)},
                {"receiver", compile_expression(
                                 *property->receiver,
                                 child(path, "receiver"),
                                 component_path + ".receiver",
                                 scope,
                                 nullptr
                             )},
            });
        }
        if (const auto* indexed = std::get_if<IndexExpression>(&expression.node)) {
            return expression_base(path, expression.span, {
                {"index", compile_expression(
                              *indexed->index,
                              child(path, "index"),
                              component_path + ".index",
                              scope,
                              nullptr
                          )},
                {"kind", JsonValue("index")},
                {"receiver", compile_expression(
                                 *indexed->receiver,
                                 child(path, "receiver"),
                                 component_path + ".receiver",
                                 scope,
                                 nullptr
                             )},
            });
        }
        if (const auto* call = std::get_if<CallExpression>(&expression.node)) {
            if (call->target.qualified_name() == "action") {
                return compile_action(expression, path, component_path, scope);
            }
            if (call->target.qualified_name() == "material") {
                return compile_material(expression, path, component_path, scope);
            }
            return compile_helper(*call, expression.span, path, component_path, scope);
        }
        if (const auto* lambda = std::get_if<LambdaExpression>(&expression.node)) {
            Scope lambda_scope = scope;
            lambda_scope.insert_or_assign(
                lambda->parameter_name,
                Binding{BindingKind::local, type_of(SemanticTypeKind::unknown)}
            );
            return expression_base(path, expression.span, {
                {"body", compile_expression(
                             *lambda->body,
                             child(path, "body"),
                             component_path + "/" + lambda->parameter_name,
                             lambda_scope,
                             nullptr
                         )},
                {"kind", JsonValue("lambda")},
                {"parameter", JsonValue(lambda->parameter_name)},
            });
        }
        return expression_base(path, expression.span, {{"kind", JsonValue("error")}});
    }

    [[nodiscard]] JsonValue compile_helper(
        const CallExpression& call,
        const SourceSpan& source_span,
        const std::string& path,
        const std::string& component_path,
        const Scope& scope
    ) {
        const std::string name = call.target.qualified_name();
        const HelperSchema* helper = registry_.helper(name);
        std::vector<JsonValue> arguments;
        for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
            const Argument& argument = call.arguments[index];
            const SchemaParameter* parameter = nullptr;
            if (helper != nullptr) {
                if (argument.name.has_value()) {
                    const auto found = std::ranges::find_if(
                        helper->parameters,
                        [&argument](const SchemaParameter& candidate) {
                            return candidate.accepts_name(*argument.name);
                        }
                    );
                    if (found != helper->parameters.end()) parameter = &*found;
                } else if (index < helper->parameters.size()) {
                    parameter = &helper->parameters[index];
                }
            }
            const SemanticType* expected = parameter != nullptr
                                               ? parameter->type.get()
                                               : helper != nullptr ? helper->vararg_type.get() : nullptr;
            const std::string argument_path = argument.name.value_or(std::to_string(index));
            arguments.push_back(object({
                {"name", nullable_string(argument.name)},
                {"span", span(argument.span)},
                {"value", compile_expression(
                              *argument.value,
                              child(path, argument_path),
                              component_path + "." + argument_path,
                              scope,
                              expected
                          )},
            }));
        }
        return expression_base(path, source_span, {
            {"arguments", array(std::move(arguments))},
            {"kind", JsonValue("helper")},
            {"name", JsonValue(name)},
        });
    }

    [[nodiscard]] JsonValue compile_action(
        const Expression& expression,
        const std::string& path,
        const std::string& component_path,
        const Scope& scope
    ) {
        const auto* call = std::get_if<CallExpression>(&expression.node);
        const std::string* id = call != nullptr && !call->arguments.empty()
                                    ? literal_string(*call->arguments.front().value)
                                    : literal_string(expression);
        if (id == nullptr) return null();
        const ActionSchema* action = registry_.action(*id);
        std::vector<JsonValue> arguments;
        if (call != nullptr && action != nullptr) {
            for (const Argument& argument : call->arguments | std::views::drop(1)) {
                if (!argument.name.has_value()) continue;
                const auto parameter = std::ranges::find_if(
                    action->parameters,
                    [&argument](const SchemaParameter& candidate) {
                        return candidate.accepts_name(*argument.name);
                    }
                );
                if (parameter == action->parameters.end()) continue;
                arguments.push_back(object({
                    {"name", JsonValue(parameter->name)},
                    {"span", span(argument.span)},
                    {"value", compile_expression(
                                  *argument.value,
                                  child(path, parameter->name),
                                  component_path + "." + parameter->name,
                                  scope,
                                  parameter->type.get()
                              )},
                }));
            }
            action_references_.push_back(ActionReference{component_path, action, expression.span});
        }
        return expression_base(path, expression.span, {
            {"arguments", array(std::move(arguments))},
            {"id", JsonValue(*id)},
            {"kind", JsonValue("action")},
        });
    }

    [[nodiscard]] JsonValue compile_binding_action(
        const std::string& state_name,
        const SourceSpan& source_span,
        const std::string& path,
        const std::string& component_path,
        const Argument* const undo_label,
        const Argument* const undo_coalesce,
        const Scope& scope
    ) {
        const ActionSchema* action = registry_.action("state.setFromEvent");
        if (action == nullptr) {
            throw std::logic_error("widget binding requires the state.setFromEvent action contract");
        }
        action_references_.push_back(ActionReference{component_path, action, source_span});
        const std::string name_path = child(path, "name");
        std::vector<JsonValue> arguments;
        arguments.push_back(object({
            {"name", JsonValue("name")},
            {"span", span(source_span)},
            {"value", expression_base(name_path, source_span, {
                {"kind", JsonValue("literal")},
                {"value", object({
                    {"kind", JsonValue("string")},
                    {"value", JsonValue(state_name)},
                })},
            })},
        }));
        const auto append_undo_argument = [this, &arguments, &path, &component_path, &scope](
                                              const std::string_view name,
                                              const Argument* const argument
                                          ) {
            if (argument == nullptr) return;
            arguments.push_back(object({
                {"name", JsonValue(std::string(name))},
                {"span", span(argument->span)},
                {"value", compile_expression(
                    *argument->value,
                    child(path, name),
                    component_path + "." + std::string(name),
                    scope,
                    nullptr
                )},
            }));
        };
        append_undo_argument("undoLabel", undo_label);
        append_undo_argument("undoCoalesce", undo_coalesce);
        return expression_base(path, source_span, {
            {"arguments", array(std::move(arguments))},
            {"id", JsonValue("state.setFromEvent")},
            {"kind", JsonValue("action")},
        });
    }

    [[nodiscard]] JsonValue compile_material(
        const Expression& expression,
        const std::string& path,
        const std::string& component_path,
        const Scope& scope
    ) {
        const auto* call = std::get_if<CallExpression>(&expression.node);
        const std::string* id = call != nullptr && !call->arguments.empty()
                                    ? literal_string(*call->arguments.front().value)
                                    : literal_string(expression);
        if (id == nullptr) return null();
        material_ids_.insert(*id);
        if (call != nullptr) {
            std::vector<JsonValue> parameters;
            const MaterialSchema* material = registry_.material(*id);
            for (std::size_t index = 1U; index < call->arguments.size(); ++index) {
                const Argument& argument = call->arguments[index];
                if (!argument.name.has_value() || material == nullptr) continue;
                const SchemaParameter* parameter = material->find_parameter(*argument.name);
                if (parameter == nullptr) continue;
                parameters.push_back(object({
                    {"name", JsonValue(parameter->name)},
                    {"span", span(argument.span)},
                    {"value", compile_expression(
                                  *argument.value,
                                  child(path, parameter->name),
                                  component_path + "." + parameter->name,
                                  scope,
                                  parameter->type.get()
                              )},
                }));
            }
            return expression_base(path, expression.span, {
                {"id", JsonValue(*id)},
                {"kind", JsonValue("materialCall")},
                {"parameters", array(std::move(parameters))},
            });
        }
        return expression_base(path, expression.span, {
            {"id", JsonValue(*id)},
            {"kind", JsonValue("materialReference")},
        });
    }

    [[nodiscard]] JsonValue expression_base(
        const std::string& path,
        const SourceSpan& source_span,
        std::initializer_list<JsonValue::ObjectEntry> fields
    ) const {
        JsonValue::Object entries;
        entries.emplace_back("path", JsonValue(path));
        entries.emplace_back("span", span(source_span));
        entries.insert(entries.end(), fields.begin(), fields.end());
        return JsonValue(std::move(entries));
    }

    [[nodiscard]] JsonValue literal_value(
        const LiteralValue& literal,
        const SemanticType* expected
    ) const {
        if (const auto* value = std::get_if<StringLiteral>(&literal)) {
            if (accepts_kind(expected, SemanticTypeKind::key)) {
                return object({{"kind", JsonValue("key")}, {"value", JsonValue(value->value)}});
            }
            if (accepts_kind(expected, SemanticTypeKind::texture)) {
                return object({{"kind", JsonValue("texture")}, {"value", JsonValue(value->value)}});
            }
            return object({{"kind", JsonValue("string")}, {"value", JsonValue(value->value)}});
        }
        if (const auto* value = std::get_if<NumberLiteral>(&literal)) {
            const double number = parse_number(value->raw);
            if (value->unit.has_value()) {
                const double multiplier = *value->unit == "ms" ? 1'000'000.0 : 1'000'000'000.0;
                return object({
                    {"kind", JsonValue("duration")},
                    {"nanos", JsonValue(static_cast<std::int64_t>(number * multiplier))},
                });
            }
            return object({{"kind", JsonValue("number")}, {"value", JsonValue(number)}});
        }
        if (const auto* value = std::get_if<ColorLiteral>(&literal)) {
            return object({
                {"kind", JsonValue("color")},
                {"rgba", JsonValue(color_rgba(value->raw))},
            });
        }
        if (const auto* value = std::get_if<BooleanLiteral>(&literal)) {
            return object({{"kind", JsonValue("boolean")}, {"value", JsonValue(value->value)}});
        }
        return object({{"kind", JsonValue("null")}});
    }

    [[nodiscard]] Binding binding_for(const std::string_view name, const Scope& scope) const {
        const auto scoped = scope.find(std::string(name));
        if (scoped != scope.end()) return scoped->second;
        if (styles_.contains(std::string(name))) {
            return Binding{BindingKind::style, type_of(SemanticTypeKind::style)};
        }
        if (animations_.contains(std::string(name))) {
            return Binding{BindingKind::animation, type_of(SemanticTypeKind::animation)};
        }
        if (components_.contains(std::string(name))) {
            return Binding{BindingKind::component, type_of(SemanticTypeKind::component)};
        }
        return Binding{BindingKind::host, type_of(SemanticTypeKind::unknown)};
    }

    [[nodiscard]] SemanticTypePtr infer_structured_type(const Expression* expression) const {
        if (expression == nullptr) return nullptr;
        if (const auto* literal = std::get_if<LiteralExpression>(&expression->node)) {
            if (std::holds_alternative<StringLiteral>(literal->value)) return type_of(SemanticTypeKind::string);
            if (std::holds_alternative<NumberLiteral>(literal->value)) return type_of(SemanticTypeKind::number);
            if (std::holds_alternative<ColorLiteral>(literal->value)) return type_of(SemanticTypeKind::color);
            if (std::holds_alternative<BooleanLiteral>(literal->value)) return type_of(SemanticTypeKind::boolean);
            return type_of(SemanticTypeKind::null_value);
        }
        if (const auto* list = std::get_if<ListExpression>(&expression->node)) {
            auto type = std::make_shared<SemanticType>();
            type->kind = SemanticTypeKind::list;
            type->element = list->elements.empty()
                                ? type_of(SemanticTypeKind::any)
                                : infer_structured_type(list->elements.front().get());
            type->maximum_items = 1000U;
            return type;
        }
        if (const auto* map = std::get_if<MapExpression>(&expression->node)) {
            auto type = std::make_shared<SemanticType>();
            type->kind = SemanticTypeKind::map;
            type->label = "record";
            for (const MapEntry& entry : map->entries) {
                const std::optional<std::string> key = static_map_key(entry.key);
                if (!key.has_value()) {
                    type->allow_unknown_fields = true;
                    type->value = type_of(SemanticTypeKind::any);
                    continue;
                }
                SemanticTypePtr field = infer_structured_type(entry.value.get());
                type->fields.push_back(ObjectField{
                    *key,
                    field != nullptr ? std::move(field) : type_of(SemanticTypeKind::any),
                    true,
                    false,
                });
            }
            return type;
        }
        if (const auto* call = std::get_if<CallExpression>(&expression->node);
            call != nullptr && call->target.qualified_name() == "persisted") {
            std::size_t positional = 0U;
            for (const Argument& argument : call->arguments) {
                const bool initial = argument.name.has_value()
                    ? *argument.name == "initial" : positional++ == 1U;
                if (initial) return infer_structured_type(argument.value.get());
            }
        }
        return nullptr;
    }

    [[nodiscard]] static const std::string* literal_string(const Expression& expression) {
        const auto* literal = std::get_if<LiteralExpression>(&expression.node);
        if (literal == nullptr) return nullptr;
        const auto* string = std::get_if<StringLiteral>(&literal->value);
        return string == nullptr ? nullptr : &string->value;
    }

    [[nodiscard]] static std::optional<std::string> static_map_key(const MapKey& key) {
        if (const auto* identifier = std::get_if<IdentifierMapKey>(&key)) return identifier->name;
        if (const auto* string = std::get_if<StringMapKey>(&key)) return string->value;
        return std::nullopt;
    }

    [[nodiscard]] static std::string binary_operator(const BinaryOperator operation) {
        switch (operation) {
        case BinaryOperator::add: return "add";
        case BinaryOperator::subtract: return "subtract";
        case BinaryOperator::multiply: return "multiply";
        case BinaryOperator::divide: return "divide";
        case BinaryOperator::modulo: return "modulo";
        case BinaryOperator::equal: return "equal";
        case BinaryOperator::not_equal: return "not_equal";
        case BinaryOperator::less: return "less";
        case BinaryOperator::less_equal: return "less_equal";
        case BinaryOperator::greater: return "greater";
        case BinaryOperator::greater_equal: return "greater_equal";
        case BinaryOperator::logical_and: return "and";
        case BinaryOperator::logical_or: return "or";
        case BinaryOperator::coalesce: return "coalesce";
        }
        throw std::runtime_error("invalid binary operator");
    }

    [[nodiscard]] JsonValue compile_animation(const ValidatedAnimation& animation) const {
        std::vector<std::string> names;
        names.reserve(animation.tracks.size());
        for (const ValidatedAnimationTrack& track : animation.tracks) {
            names.push_back(track.property);
        }
        std::vector<JsonValue> authored_track_order;
        authored_track_order.reserve(names.size());
        for (const std::string& name : names) authored_track_order.emplace_back(name);
        // The executable sidecar retains authoring order while the public v1 IR projection keeps
        // its canonical, name-sorted track array for stable artifacts and cache keys.
        std::ranges::sort(names);
        std::vector<JsonValue> tracks;
        for (const std::string& name : names) {
            const auto track = std::ranges::find(animation.tracks, name, &ValidatedAnimationTrack::property);
            if (track == animation.tracks.end()) {
                throw std::logic_error("validated animation track disappeared during lowering");
            }
            tracks.push_back(object({
                {"keyframes", array({
                    object({
                        {"easing", null()},
                        {"offset", JsonValue(0.0)},
                        {"value", animation_value(track->from)},
                    }),
                    object({
                        {"easing", null()},
                        {"offset", JsonValue(1.0)},
                        {"value", animation_value(track->to)},
                    }),
                })},
                {"property", JsonValue(name)},
            }));
        }
        const std::set<std::string> layout_properties = {
            "width", "height", "minWidth", "minHeight", "maxWidth", "maxHeight",
            "marginLeft", "marginTop", "marginRight", "marginBottom",
            "paddingLeft", "paddingTop", "paddingRight", "paddingBottom",
        };
        const bool affects_layout = std::ranges::any_of(names, [&layout_properties](const std::string& name) {
            return layout_properties.contains(name);
        });
        return object({
            {"animation", object({
                {"$authoredTrackOrder", array(std::move(authored_track_order))},
                {"affectsLayout", JsonValue(affects_layout)},
                {"name", JsonValue(animation.name)},
                {"timing", object({
                    {"delayNanos", JsonValue(animation.delay_nanos)},
                    {"durationNanos", JsonValue(animation.duration_nanos)},
                    {"easing", object({{"kind", JsonValue(animation.easing)}})},
                    {"fillMode", JsonValue(animation.fill_mode)},
                    {"repeat", animation_repeat(animation.repeat)},
                    {"reverse", JsonValue(animation.reverse)},
                })},
                {"tracks", array(std::move(tracks))},
            })},
            {"name", JsonValue(animation.name)},
            // Trigger ownership is executable program data. Omitting it made portable IR
            // impossible to run without retaining the compiler's in-memory declaration table.
            {"trigger", JsonValue(animation.trigger)},
        });
    }

    [[nodiscard]] static JsonValue animation_value(const ValidatedAnimationValue& value) {
        switch (value.kind) {
        case ValidatedAnimationValueKind::number:
            return object({
                {"kind", JsonValue("number")},
                {"value", JsonValue(value.number)},
            });
        case ValidatedAnimationValueKind::boolean:
            return object({
                {"kind", JsonValue("boolean")},
                {"value", JsonValue(value.boolean)},
            });
        case ValidatedAnimationValueKind::color:
            return object({
                {"kind", JsonValue("color")},
                {"value", JsonValue(value.color_rgba)},
            });
        }
        throw std::logic_error("validated animation value has an unknown kind");
    }

    [[nodiscard]] static JsonValue animation_repeat(const ValidatedAnimationRepeat& repeat) {
        switch (repeat.kind) {
        case ValidatedAnimationRepeatKind::none:
            return object({{"kind", JsonValue("none")}});
        case ValidatedAnimationRepeatKind::forever:
            return object({{"kind", JsonValue("forever")}});
        case ValidatedAnimationRepeatKind::count:
            return object({
                {"count", JsonValue(static_cast<std::int64_t>(repeat.count))},
                {"kind", JsonValue("count")},
            });
        }
        throw std::logic_error("validated animation repeat has an unknown kind");
    }

    const File& file_;
    const SchemaRegistry& registry_;
    const std::map<std::string, ValidatedAnimation, std::less<>>& validated_animations_;
    std::unordered_map<std::string, ComponentSchema> components_;
    std::set<std::string> styles_;
    std::set<std::string> animations_;
    std::set<std::string> material_ids_;
    std::vector<ActionReference> action_references_;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace

PortableIrResult lower_portable_ir(
    const File& file,
    const SchemaRegistry& registry,
    const std::map<std::string, ValidatedAnimation, std::less<>>& animations
) {
    return Lowerer(file, registry, animations).run();
}

} // namespace strata::compiler
