#include "compiler/portable_ir.hpp"

#include <algorithm>
#include <limits>
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
#include <utility>
#include <variant>
#include <vector>

namespace strata::compiler {
namespace {

using data::JsonValue;

[[nodiscard]] JsonValue object(const std::initializer_list<data::JsonEntry> entries) {
    return data::json_object(entries);
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

[[nodiscard]] const std::string& map_key(const MapKey& key) {
    if (const auto* identifier = std::get_if<IdentifierMapKey>(&key)) return identifier->name;
    return std::get<StringMapKey>(key).value;
}

[[nodiscard]] std::string_view assignment_operator(const AssignmentOperator operation) {
    switch (operation) {
    case AssignmentOperator::assign: return "assign";
    case AssignmentOperator::add: return "add";
    case AssignmentOperator::subtract: return "subtract";
    case AssignmentOperator::multiply: return "multiply";
    case AssignmentOperator::divide: return "divide";
    case AssignmentOperator::modulo: return "modulo";
    }
    return "assign";
}

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
        {"stateBinding", JsonValue(
            parameter.type->kind == SemanticTypeKind::state_binding
        )},
        {"type", semantic_type(*parameter.type)},
    });
}

[[nodiscard]] JsonValue type_base(
    std::string kind,
    const std::initializer_list<data::JsonEntry> fields = {}
) {
    JsonValue::Object entries;
    entries.emplace_back("kind", JsonValue(std::move(kind)));
    data::append_json_entries(entries, fields);
    return JsonValue(std::move(entries));
}

[[nodiscard]] JsonValue semantic_type(const SemanticType& type) {
    switch (type.kind) {
    case SemanticTypeKind::unknown: return type_base("unknown");
    case SemanticTypeKind::any: return type_base("any");
    case SemanticTypeKind::unsafe_component_parameter: return type_base("unsafeComponentParameter");
    case SemanticTypeKind::state_binding:
        return type.value != nullptr
            ? semantic_type(*type.value)
            : type_base("unsafeComponentParameter");
    case SemanticTypeKind::null_value: return type_base("null");
    case SemanticTypeKind::string: return type_base("string");
    case SemanticTypeKind::string_literal:
        return type_base("stringLiteral", {{"value", JsonValue(type.literal)}});
    case SemanticTypeKind::number: return type_base("number");
    case SemanticTypeKind::duration: return type_base("duration");
    case SemanticTypeKind::boolean: return type_base("boolean");
    case SemanticTypeKind::color: return type_base("color");
    case SemanticTypeKind::path: return type_base("path");
    case SemanticTypeKind::image: return type_base("image");
    case SemanticTypeKind::key: return type_base("key");
    case SemanticTypeKind::style: return type_base("style");
    case SemanticTypeKind::layout: return type_base("layout");
    case SemanticTypeKind::animation: return type_base("animation");
    case SemanticTypeKind::effect: return type_base("effect");
    case SemanticTypeKind::material: return type_base("material");
    case SemanticTypeKind::action: return type_base("action");
    case SemanticTypeKind::component: return type_base("component");
    case SemanticTypeKind::function: return type_base("function");
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
                {"nullable", JsonValue(field.nullable)},
                {"required", JsonValue(field.required)},
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
        SemanticType fallback;
        fallback.kind = SemanticTypeKind::any;
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

struct ActionReference final {
    std::string component_path;
    const ActionSchema* schema;
    SourceSpan span;
};

/** How a variable's binding and static type appear in the IR, from the symbol it resolved to. */
struct VariableBinding final {
    std::string_view binding;
    std::string type;
};

[[nodiscard]] VariableBinding variable_binding(const Symbol& symbol) {
    switch (symbol.kind) {
    case SymbolKind::state:
    case SymbolKind::derived:
    case SymbolKind::loop_item:
    case SymbolKind::lambda_parameter:
        return {"local", "unknown"};
    case SymbolKind::loop_index:
        return {"local", "number"};
    case SymbolKind::local:
    case SymbolKind::function_parameter:
        return {"local", "unknown"};
    case SymbolKind::function:
        // A function declared in a block is bound there; a top-level one is the unit's.
        return {symbol.statement != nullptr ? "local" : "function", "function"};
    case SymbolKind::parameter:
        return {"parameter",
                symbol.component != nullptr
                    ? symbol.component->parameters[symbol.parameter_index].type->diagnostic_name()
                    : std::string("unknown")};
    case SymbolKind::component:
        return {"component", "component"};
    case SymbolKind::style:
        return {"style", "style"};
    case SymbolKind::animation:
        return {"animation", "animation"};
    case SymbolKind::host:
    case SymbolKind::unresolved:
        break;
    }
    return {"host", "unknown"};
}

/**
 * Projects an analysed module into portable IR. Every name, call, type and IR position was decided
 * by semantic analysis; this only writes those facts out in the IR's shape.
 */
class Lowerer final {
public:
    Lowerer(const File& file, const SchemaRegistry& registry, const SemanticResult& semantic)
        : file_(file), registry_(registry), model_(semantic.model),
          validated_animations_(semantic.animations) {}

    [[nodiscard]] PortableIrResult run() {
        std::vector<std::pair<std::string, JsonValue>> screens;
        std::vector<std::pair<std::string, JsonValue>> overlays;
        std::vector<std::pair<std::string, JsonValue>> components;
        std::vector<std::pair<std::string, JsonValue>> styles;
        std::vector<std::pair<std::string, JsonValue>> animations;
        std::vector<std::pair<std::string, JsonValue>> functions;
        for (std::size_t index = 0U; index < file_.declarations.size(); ++index) {
            const Declaration& declaration = file_.declarations[index];
            const Placement& placement = model_.declaration_placements.at(index);
            if (const auto* screen = std::get_if<ScreenDeclaration>(&declaration.node)) {
                screens.emplace_back(screen->name,
                                     layer(screen->name, *screen->body, placement, declaration.span));
            } else if (const auto* overlay = std::get_if<OverlayDeclaration>(&declaration.node)) {
                overlays.emplace_back(
                    overlay->name, layer(overlay->name, *overlay->body, placement, declaration.span));
            } else if (const auto* component = std::get_if<ComponentDeclaration>(&declaration.node)) {
                components.emplace_back(component->name,
                                        compile_component(*component, placement, declaration.span));
            } else if (const auto* style = std::get_if<StyleDeclaration>(&declaration.node)) {
                styles.emplace_back(style->name, compile_style(*style, placement, declaration.span));
            } else if (const auto* function = std::get_if<FunctionDeclaration>(&declaration.node)) {
                functions.emplace_back(function->name,
                                       compile_function(*function, placement.path, declaration.span));
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
        JsonValue::Object fields;
        fields.emplace_back("actionReferences", array(std::move(action_references)));
        fields.emplace_back("animations", array(sorted_values(animations)));
        fields.emplace_back("components", array(sorted_values(components)));
        // A unit without functions keeps the shape it had before scripts existed.
        if (!functions.empty()) fields.emplace_back("functions", array(sorted_values(functions)));
        fields.emplace_back("materials", array(std::move(materials)));
        fields.emplace_back("overlays", array(sorted_values(overlays)));
        fields.emplace_back("screens", array(sorted_values(screens)));
        fields.emplace_back("sourceId", JsonValue(file_.source_id));
        fields.emplace_back("styles", array(sorted_values(styles)));
        JsonValue unit(std::move(fields));
        validate_portable_ir(unit);
        return PortableIrResult{std::move(unit), {}};
    }

private:
    /**
     * Where an expression lowers. An expression lowered again at a synthesized node (a binding's
     * undo arguments under its event action) keeps its subtree's shape below the new position.
     */
    [[nodiscard]] Placement placed(const NodeId id) const {
        Placement placement = model_.placement(id);
        if (rebase_.has_value()) {
            const auto move = [](std::string& value, const std::string& from, const std::string& to) {
                if (value.starts_with(from)) value = to + value.substr(from.size());
            };
            move(placement.path, rebase_->first.path, rebase_->second.path);
            move(placement.component_path, rebase_->first.component_path,
                 rebase_->second.component_path);
        }
        return placement;
    }

    /** Lowers an expression at a synthesized position instead of the one analysis placed. */
    [[nodiscard]] JsonValue compile_at(const Expression& expression, Placement at) {
        rebase_.emplace(model_.placement(expression.id), std::move(at));
        JsonValue lowered = compile_expression(expression);
        rebase_.reset();
        return lowered;
    }

    [[nodiscard]] JsonValue layer(const std::string& name, const Block& body,
                                  const Placement& placement, const SourceSpan& source_span) {
        return object({
            {"body", compile_block(body)},
            {"name", JsonValue(name)},
            {"path", JsonValue(placement.path)},
            {"span", span(source_span)},
        });
    }

    [[nodiscard]] JsonValue compile_component(const ComponentDeclaration& component,
                                              const Placement& placement,
                                              const SourceSpan& source_span) {
        const ComponentInfo& info = model_.components.at(component.name);
        std::vector<JsonValue> parameters;
        for (std::size_t index = 0U; index < component.parameters.size(); ++index) {
            const Parameter& parameter = component.parameters[index];
            parameters.push_back(object({
                {"default", parameter.default_value != nullptr
                                ? compile_expression(*parameter.default_value)
                                : null()},
                {"schema", schema_parameter(info.parameters[index])},
                {"span", span(parameter.span)},
            }));
        }
        std::vector<JsonValue> widget_defaults;
        widget_defaults.reserve(info.widget_defaults.size());
        for (const ComponentWidgetDefault& entry : info.widget_defaults) {
            widget_defaults.push_back(object({
                {"name", JsonValue(entry.widget)},
                {"style", entry.style != nullptr ? compile_expression(*entry.style) : null()},
                {"variant", entry.variant != nullptr ? compile_expression(*entry.variant) : null()},
            }));
        }
        return object({
            {"body", compile_block(*component.body)},
            {"name", JsonValue(component.name)},
            {"parameters", array(std::move(parameters))},
            {"path", JsonValue(placement.path)},
            {"span", span(source_span)},
            {"widgetDefaults", array(std::move(widget_defaults))},
        });
    }

    [[nodiscard]] JsonValue compile_function(const FunctionDeclaration& function,
                                             const std::string& path,
                                             const SourceSpan& source_span) {
        std::vector<JsonValue> parameters;
        for (const Parameter& parameter : function.parameters) {
            parameters.push_back(object({
                {"default", parameter.default_value != nullptr
                                ? compile_expression(*parameter.default_value)
                                : null()},
                {"name", JsonValue(parameter.name)},
                {"span", span(parameter.span)},
            }));
        }
        return object({
            {"async", JsonValue(function.asynchronous)},
            {"block", function.block_body != nullptr ? compile_script_block(*function.block_body)
                                                     : null()},
            {"body", function.expression_body != nullptr
                         ? compile_expression(*function.expression_body)
                         : null()},
            {"name", JsonValue(function.name)},
            {"parameters", array(std::move(parameters))},
            {"path", JsonValue(path)},
            {"span", span(source_span)},
        });
    }

    /** A StrataScript block: statements a handler or function runs, in order. */
    [[nodiscard]] JsonValue compile_script_block(const Block& block) {
        std::vector<JsonValue> statements;
        for (const StatementPtr& pointer : block.statements) {
            const Statement& statement = *pointer;
            const std::string& statement_path = model_.placement(statement.id).path;
            const auto base = [&](const std::string_view kind,
                                  std::initializer_list<data::JsonEntry> fields) {
                JsonValue::Object entries{
                    {"kind", JsonValue(std::string(kind))},
                    {"path", JsonValue(statement_path)},
                    {"span", span(statement.span)},
                };
                for (const data::JsonEntry& field : fields)
                    entries.emplace_back(std::move(field.key), std::move(field.value));
                std::ranges::sort(entries, {}, &JsonValue::Object::value_type::first);
                return JsonValue(std::move(entries));
            };
            if (const auto* let = std::get_if<LetStatement>(&statement.node)) {
                statements.push_back(base("let", {
                    {"initializer", compile_expression(*let->initializer)},
                    {"mutable", JsonValue(let->mutable_binding)},
                    {"name", JsonValue(let->name)},
                }));
            } else if (const auto* assignment = std::get_if<AssignmentStatement>(&statement.node)) {
                statements.push_back(base("assign", {
                    {"operator", JsonValue(std::string(assignment_operator(assignment->operation)))},
                    {"target", compile_expression(*assignment->target)},
                    {"value", compile_expression(*assignment->value)},
                }));
            } else if (const auto* effect = std::get_if<ExpressionStatement>(&statement.node)) {
                statements.push_back(base("expression", {
                    {"expression", compile_expression(*effect->expression)},
                }));
            } else if (const auto* repeat = std::get_if<WhileStatement>(&statement.node)) {
                statements.push_back(base("while", {
                    {"block", compile_script_block(*repeat->block)},
                    {"condition", compile_expression(*repeat->condition)},
                }));
            } else if (std::holds_alternative<BreakStatement>(statement.node)) {
                statements.push_back(base("break", {}));
            } else if (std::holds_alternative<ContinueStatement>(statement.node)) {
                statements.push_back(base("continue", {}));
            } else if (const auto* returned = std::get_if<ReturnStatement>(&statement.node)) {
                statements.push_back(base("return", {
                    {"value", returned->value != nullptr ? compile_expression(*returned->value)
                                                         : null()},
                }));
            } else if (const auto* thrown = std::get_if<ThrowStatement>(&statement.node)) {
                statements.push_back(base("throw", {
                    {"value", compile_expression(*thrown->value)},
                }));
            } else if (const auto* attempt = std::get_if<TryStatement>(&statement.node)) {
                statements.push_back(base("try", {
                    {"block", compile_script_block(*attempt->block)},
                    {"catch", compile_script_block(*attempt->catch_block)},
                    {"catchName", JsonValue(attempt->catch_name)},
                }));
            } else if (const auto* conditional = std::get_if<IfStatement>(&statement.node)) {
                statements.push_back(base("if", {
                    {"condition", compile_expression(*conditional->condition)},
                    {"else", conditional->else_block != nullptr
                                 ? compile_script_block(*conditional->else_block)
                                 : null()},
                    {"then", compile_script_block(*conditional->then_block)},
                }));
            } else if (const auto* when = std::get_if<WhenStatement>(&statement.node)) {
                std::vector<JsonValue> branches;
                for (const WhenBranch& branch : when->branches) {
                    branches.push_back(object({
                        {"block", compile_script_block(*branch.block)},
                        {"match", branch.match != nullptr ? compile_expression(*branch.match)
                                                          : null()},
                        {"span", span(branch.span)},
                    }));
                }
                statements.push_back(base("when", {
                    {"branches", array(std::move(branches))},
                    {"subject", compile_expression(*when->subject)},
                }));
            } else if (const auto* loop = std::get_if<ForStatement>(&statement.node)) {
                statements.push_back(base("for", {
                    {"block", compile_script_block(*loop->block)},
                    {"collection", compile_expression(*loop->collection)},
                    {"filter", loop->filter != nullptr ? compile_expression(*loop->filter)
                                                       : null()},
                    {"indexName", nullable_string(loop->index_name)},
                    {"itemName", JsonValue(loop->item_name)},
                }));
            } else if (const auto* function = std::get_if<FunctionStatement>(&statement.node)) {
                statements.push_back(base("function", {
                    {"function", compile_function(function->function, statement_path,
                                                  statement.span)},
                }));
            }
        }
        return object({
            {"path", JsonValue(model_.placement(block.id).path)},
            {"span", span(block.span)},
            {"statements", array(std::move(statements))},
        });
    }

    [[nodiscard]] JsonValue compile_style(const StyleDeclaration& style, const Placement& placement,
                                          const SourceSpan& source_span) {
        JsonValue::Object properties;
        for (const Property& property : style.properties)
            properties.emplace_back(property.name, compile_expression(*property.value));
        std::vector<JsonValue> bases;
        for (const StyleBase& base : style.bases) bases.emplace_back(base.name);
        return object({
            {"bases", array(std::move(bases))},
            {"name", JsonValue(style.name)},
            {"path", JsonValue(placement.path)},
            {"properties", JsonValue(std::move(properties))},
            {"span", span(source_span)},
        });
    }

    [[nodiscard]] JsonValue compile_block(const Block& block, const bool repeater_body = false) {
        std::vector<JsonValue> statements;
        for (const StatementPtr& pointer : block.statements) {
            const Statement& statement = *pointer;
            const std::string& statement_path = model_.placement(statement.id).path;
            if (const auto* state = std::get_if<StateStatement>(&statement.node)) {
                const auto schema = model_.state_schemas.find(statement.id);
                const SemanticType* declared =
                    schema != model_.state_schemas.end() ? schema->second.get() : nullptr;
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
                    {"declaredType",
                     declared != nullptr ? JsonValue(declared->diagnostic_name()) : null()},
                    {"initializer", state->initializer != nullptr
                                        ? compile_expression(*state->initializer)
                                        : null()},
                    {"kind", JsonValue("state")},
                    {"name", JsonValue(state->name)},
                    {"path", JsonValue(statement_path)},
                    {"span", span(statement.span)},
                }));
            } else if (const auto* derived = std::get_if<DerivedStatement>(&statement.node)) {
                statements.push_back(object({
                    {"expression", compile_expression(*derived->expression)},
                    {"kind", JsonValue("derived")},
                    {"name", JsonValue(derived->name)},
                    {"path", JsonValue(statement_path)},
                    {"span", span(statement.span)},
                }));
            } else if (const auto* widget = std::get_if<WidgetStatement>(&statement.node)) {
                statements.push_back(object({
                    {"call", compile_call(widget->call)},
                    {"kind", JsonValue("node")},
                    {"path", JsonValue(model_.placement(widget->call.id).path)},
                    {"root", JsonValue(false)},
                    {"span", span(statement.span)},
                }));
            } else if (const auto* root = std::get_if<RootStatement>(&statement.node)) {
                statements.push_back(object({
                    {"call", compile_call(root->call)},
                    {"kind", JsonValue("node")},
                    {"path", JsonValue(statement_path)},
                    {"root", JsonValue(true)},
                    {"span", span(statement.span)},
                }));
            } else if (const auto* conditional = std::get_if<IfStatement>(&statement.node)) {
                statements.push_back(object({
                    {"condition", compile_expression(*conditional->condition)},
                    {"else", conditional->else_block != nullptr
                                 ? compile_block(*conditional->else_block)
                                 : null()},
                    {"kind", JsonValue("if")},
                    {"path", JsonValue(statement_path)},
                    {"span", span(statement.span)},
                    {"then", compile_block(*conditional->then_block)},
                }));
            } else if (const auto* when = std::get_if<WhenStatement>(&statement.node)) {
                std::vector<JsonValue> branches;
                for (const WhenBranch& branch : when->branches) {
                    branches.push_back(object({
                        {"block", compile_block(*branch.block)},
                        {"match", branch.match != nullptr ? compile_expression(*branch.match)
                                                          : null()},
                        {"span", span(branch.span)},
                    }));
                }
                statements.push_back(object({
                    {"branches", array(std::move(branches))},
                    {"kind", JsonValue("when")},
                    {"path", JsonValue(statement_path)},
                    {"span", span(statement.span)},
                    {"subject", compile_expression(*when->subject)},
                }));
            } else if (const auto* loop = std::get_if<ForStatement>(&statement.node)) {
                JsonValue compiled_body = compile_block(*loop->block);
                JsonValue::Object loop_fields{
                    {"block", std::move(compiled_body)},
                    {"collection", compile_expression(*loop->collection)},
                    {"filter", loop->filter != nullptr ? compile_expression(*loop->filter) : null()},
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
            } else if (const auto* function = std::get_if<FunctionStatement>(&statement.node)) {
                statements.push_back(object({
                    {"function", compile_function(function->function, statement_path,
                                                  statement.span)},
                    {"kind", JsonValue("function")},
                    {"path", JsonValue(statement_path)},
                    {"span", span(statement.span)},
                }));
            }
        }
        return object({
            {"path", JsonValue(model_.placement(block.id).path)},
            {"span", span(block.span)},
            {"statements", array(std::move(statements))},
        });
    }

    [[nodiscard]] JsonValue compile_call(const WidgetCall& call) {
        const ResolvedWidgetCall& resolved = *model_.widget_call(call);
        const Placement& placement = model_.placement(call.id);
        JsonValue::Object arguments;
        for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
            const SchemaParameter* parameter = resolved.arguments[index].parameter;
            // A `bind:` shorthand lowers only through its expansion below.
            if (resolved.widget != nullptr && parameter != nullptr &&
                resolved.widget->find_binding(parameter->name) != nullptr) {
                continue;
            }
            const Argument& argument = call.arguments[index];
            arguments.emplace_back(
                parameter != nullptr ? parameter->name
                                     : argument.name.value_or("#" + std::to_string(index)),
                compile_expression(*argument.value)
            );
        }
        for (const BindingExpansion& expansion : resolved.bindings) {
            const Argument& shorthand = call.arguments[expansion.shorthand];
            // Analysis placed the shorthand at the controlled value it lowers as.
            arguments.emplace_back(expansion.binding->value_parameter,
                                   compile_expression(*shorthand.value));
            const Placement event{
                child(child(placement.path, "arguments"), expansion.binding->event_parameter),
                placement.component_path + "." + expansion.binding->event_parameter,
            };
            arguments.emplace_back(
                expansion.binding->event_parameter,
                compile_binding_action(
                    std::get<IdentifierExpression>(shorthand.value->node).name,
                    shorthand.value->span, event,
                    expansion.undo_label.has_value() ? &call.arguments[*expansion.undo_label]
                                                     : nullptr,
                    expansion.undo_coalesce.has_value() ? &call.arguments[*expansion.undo_coalesce]
                                                        : nullptr
                )
            );
        }
        return object({
            {"arguments", JsonValue(std::move(arguments))},
            {"children", call.body != nullptr ? compile_block(*call.body, resolved.lazy_rows)
                                              : null()},
            {"kind", JsonValue(resolved.widget != nullptr ? "widget" : "component")},
            {"name", JsonValue(call.name)},
            {"path", JsonValue(placement.path)},
            {"span", span(call.span)},
        });
    }

    [[nodiscard]] JsonValue compile_expression(const Expression& expression) {
        const Placement placement = placed(expression.id);
        const ExpressionFacts& facts = model_.facts(expression);
        if (facts.handler && unwrapped_handler_ != expression.id) {
            // The function itself, run with the event wherever the action would have been.
            unwrapped_handler_ = expression.id;
            JsonValue function = compile_expression(expression);
            return expression_base(placement.path, expression.span, {
                {"kind", JsonValue("handler")},
                {"operand", std::move(function)},
            });
        }
        if (unwrapped_handler_ == expression.id) unwrapped_handler_ = std::numeric_limits<NodeId>::max();
        const auto literal = [&](const std::initializer_list<data::JsonEntry> value) {
            return expression_base(placement.path, expression.span, {
                {"kind", JsonValue("literal")},
                {"value", object(value)},
            });
        };
        switch (facts.form) {
        case ExpressionForm::theme_token:
            return literal({
                {"kind", JsonValue("themeToken")},
                {"name", JsonValue(std::get<PropertyAccessExpression>(expression.node).property_name)},
            });
        case ExpressionForm::material:
            return compile_material(expression, placement);
        case ExpressionForm::action:
            return compile_action(expression, placement);
        case ExpressionForm::animation_name: {
            const auto* identifier = std::get_if<IdentifierExpression>(&expression.node);
            return literal({
                {"kind", JsonValue("animation")},
                {"name", JsonValue(identifier != nullptr
                                       ? identifier->name
                                       : std::get<StringLiteral>(
                                             std::get<LiteralExpression>(expression.node).value)
                                             .value)},
            });
        }
        case ExpressionForm::style_reference:
            return literal({
                {"kind", JsonValue("styleReference")},
                {"name", JsonValue(std::get<IdentifierExpression>(expression.node).name)},
            });
        case ExpressionForm::component_template:
            return compile_component_template(expression, placement);
        case ExpressionForm::key_literal:
        case ExpressionForm::image_literal:
        case ExpressionForm::value:
            break;
        }
        if (const auto* grouping = std::get_if<GroupingExpression>(&expression.node))
            return compile_expression(*grouping->expression);
        if (const auto* value = std::get_if<LiteralExpression>(&expression.node)) {
            return expression_base(placement.path, expression.span, {
                {"kind", JsonValue("literal")},
                {"value", literal_value(value->value, facts.form)},
            });
        }
        if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.node)) {
            const VariableBinding binding = variable_binding(model_.origin(facts.symbol));
            return expression_base(placement.path, expression.span, {
                {"binding", JsonValue(std::string(binding.binding))},
                {"kind", JsonValue("variable")},
                {"name", JsonValue(identifier->name)},
                {"stateBinding", JsonValue(facts.state_binding)},
                {"type", JsonValue(binding.type)},
            });
        }
        if (const auto* list = std::get_if<ListExpression>(&expression.node)) {
            std::vector<JsonValue> elements;
            elements.reserve(list->elements.size());
            for (const ExpressionPtr& element : list->elements)
                elements.push_back(compile_expression(*element));
            return expression_base(placement.path, expression.span, {
                {"elements", array(std::move(elements))},
                {"kind", JsonValue("list")},
            });
        }
        if (const auto* map = std::get_if<MapExpression>(&expression.node);
            map != nullptr && std::ranges::any_of(map->entries, [](const MapEntry& entry) {
                return std::holds_alternative<SpreadMapKey>(entry.key);
            })) {
            // A record built from spreads and entries, applied in order.
            std::vector<JsonValue> parts;
            for (const MapEntry& entry : map->entries) {
                if (std::holds_alternative<SpreadMapKey>(entry.key)) {
                    parts.push_back(object({{"spread", compile_expression(*entry.value)}}));
                } else {
                    parts.push_back(object({
                        {"key", JsonValue(map_key(entry.key))},
                        {"value", compile_expression(*entry.value)},
                    }));
                }
            }
            return expression_base(placement.path, expression.span, {
                {"kind", JsonValue("record")},
                {"parts", array(std::move(parts))},
            });
        }
        if (const auto* map = std::get_if<MapExpression>(&expression.node)) {
            JsonValue::Object entries;
            for (const MapEntry& entry : map->entries)
                entries.emplace_back(map_key(entry.key),
                                     compile_expression(*entry.value));
            return expression_base(placement.path, expression.span, {
                {"entries", JsonValue(std::move(entries))},
                {"kind", JsonValue("map")},
            });
        }
        if (const auto* unary = std::get_if<UnaryExpression>(&expression.node)) {
            return expression_base(placement.path, expression.span, {
                {"kind", JsonValue("unary")},
                {"operand", compile_expression(*unary->operand)},
                {"operator",
                 JsonValue(unary->operation == UnaryOperator::negate ? "negate" : "not")},
            });
        }
        if (const auto* binary = std::get_if<BinaryExpression>(&expression.node)) {
            return expression_base(placement.path, expression.span, {
                {"kind", JsonValue("binary")},
                {"left", compile_expression(*binary->left)},
                {"operator", JsonValue(binary_operator(binary->operation))},
                {"right", compile_expression(*binary->right)},
            });
        }
        if (const auto* conditional = std::get_if<ConditionalExpression>(&expression.node)) {
            return expression_base(placement.path, expression.span, {
                {"condition", compile_expression(*conditional->condition)},
                {"else", compile_expression(*conditional->else_expression)},
                {"kind", JsonValue("conditional")},
                {"then", compile_expression(*conditional->then_expression)},
            });
        }
        if (const auto* property = std::get_if<PropertyAccessExpression>(&expression.node)) {
            return expression_base(placement.path, expression.span, {
                {"kind", JsonValue("property")},
                {"name", JsonValue(property->property_name)},
                {"receiver", compile_expression(*property->receiver)},
            });
        }
        if (const auto* indexed = std::get_if<IndexExpression>(&expression.node)) {
            return expression_base(placement.path, expression.span, {
                {"index", compile_expression(*indexed->index)},
                {"kind", JsonValue("index")},
                {"receiver", compile_expression(*indexed->receiver)},
            });
        }
        if (const auto* call = std::get_if<CallExpression>(&expression.node)) {
            const ResolvedCall* resolved = model_.call(expression);
            if (resolved != nullptr && resolved->kind == CallKind::function)
                return compile_function_call(expression, *call, *resolved, placement);
            return compile_helper(*call, expression.span, placement);
        }
        if (const auto* lambda = std::get_if<LambdaExpression>(&expression.node)) {
            std::vector<JsonValue> parameters;
            for (const LambdaParameter& parameter : lambda->parameters)
                parameters.emplace_back(parameter.name);
            return expression_base(placement.path, expression.span, {
                {"block", lambda->block != nullptr ? compile_script_block(*lambda->block) : null()},
                {"body", lambda->body != nullptr ? compile_expression(*lambda->body) : null()},
                {"kind", JsonValue("lambda")},
                {"parameters", array(std::move(parameters))},
                {"script", JsonValue(lambda->script)},
            });
        }
        if (const auto* spread = std::get_if<SpreadExpression>(&expression.node)) {
            return expression_base(placement.path, expression.span, {
                {"kind", JsonValue("spread")},
                {"operand", compile_expression(*spread->operand)},
            });
        }
        if (const auto* awaited = std::get_if<AwaitExpression>(&expression.node)) {
            return expression_base(placement.path, expression.span, {
                {"kind", JsonValue("await")},
                {"operand", compile_expression(*awaited->operand)},
            });
        }
        return expression_base(placement.path, expression.span, {{"kind", JsonValue("error")}});
    }

    /**
     * A call to a script function. A declared function's arguments are named by the parameter
     * they bind; a function value's stay positional, as written.
     */
    [[nodiscard]] JsonValue compile_function_call(const Expression& expression,
                                                  const CallExpression& call,
                                                  const ResolvedCall& resolved,
                                                  const Placement& placement) {
        std::vector<JsonValue> arguments;
        arguments.reserve(call.arguments.size());
        for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
            const Argument& argument = call.arguments[index];
            const SchemaParameter* parameter = resolved.arguments[index].parameter;
            arguments.push_back(object({
                {"name", parameter != nullptr ? JsonValue(parameter->name)
                                              : nullable_string(argument.name)},
                {"span", span(argument.span)},
                {"value", compile_expression(*argument.value)},
            }));
        }
        const Symbol& callee = model_.origin(resolved.callee);
        const VariableBinding binding = variable_binding(callee);
        return expression_base(placement.path, expression.span, {
            {"arguments", array(std::move(arguments))},
            {"callee", expression_base(placement.path + "/callee", call.target.span, {
                {"binding", JsonValue(std::string(binding.binding))},
                {"kind", JsonValue("variable")},
                {"name", JsonValue(call.target.qualified_name())},
                {"stateBinding", JsonValue(false)},
                {"type", JsonValue(binding.type)},
            })},
            {"kind", JsonValue("call")},
        });
    }

    [[nodiscard]] JsonValue compile_helper(const CallExpression& call, const SourceSpan& source_span,
                                           const Placement& placement) {
        std::vector<JsonValue> arguments;
        arguments.reserve(call.arguments.size());
        for (const Argument& argument : call.arguments) {
            arguments.push_back(object({
                {"name", nullable_string(argument.name)},
                {"span", span(argument.span)},
                {"value", compile_expression(*argument.value)},
            }));
        }
        return expression_base(placement.path, source_span, {
            {"arguments", array(std::move(arguments))},
            {"kind", JsonValue("helper")},
            {"name", JsonValue(call.target.qualified_name())},
        });
    }

    [[nodiscard]] JsonValue compile_component_template(const Expression& expression,
                                                       const Placement& placement) {
        const auto& call = std::get<CallExpression>(expression.node);
        const ResolvedCall& resolved = *model_.call(expression);
        std::vector<JsonValue> arguments;
        arguments.reserve(call.arguments.size());
        for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
            const Argument& argument = call.arguments[index];
            const SchemaParameter* parameter = resolved.arguments[index].parameter;
            arguments.push_back(object({
                {"name", JsonValue(parameter != nullptr
                                       ? parameter->name
                                       : argument.name.value_or("#" + std::to_string(index)))},
                {"span", span(argument.span)},
                {"value", compile_expression(*argument.value)},
            }));
        }
        return expression_base(placement.path, expression.span, {
            {"arguments", array(std::move(arguments))},
            {"component", JsonValue(call.target.qualified_name())},
            {"kind", JsonValue("componentTemplate")},
        });
    }

    /** The named arguments an action or material call lowers, under the parameter each fills. */
    [[nodiscard]] std::vector<JsonValue> identified_arguments(const CallExpression& call,
                                                              const IdentifiedCall& identified) {
        std::vector<JsonValue> arguments;
        arguments.reserve(identified.arguments.size());
        for (const auto& [index, parameter] : identified.arguments) {
            const Argument& argument = call.arguments[index];
            arguments.push_back(object({
                {"name", JsonValue(parameter->name)},
                {"span", span(argument.span)},
                {"value", compile_expression(*argument.value)},
            }));
        }
        return arguments;
    }

    [[nodiscard]] JsonValue compile_action(const Expression& expression,
                                           const Placement& placement) {
        const IdentifiedCall& identified = model_.identified_calls.at(expression.id);
        const auto* call = std::get_if<CallExpression>(&expression.node);
        std::vector<JsonValue> arguments;
        if (call != nullptr && identified.action != nullptr) {
            arguments = identified_arguments(*call, identified);
            action_references_.push_back(
                ActionReference{placement.component_path, identified.action, expression.span});
        }
        return expression_base(placement.path, expression.span, {
            {"arguments", array(std::move(arguments))},
            {"id", JsonValue(identified.id)},
            {"kind", JsonValue("action")},
        });
    }

    /** `bind: state` writes the widget's event value back into the state it names. */
    [[nodiscard]] JsonValue compile_binding_action(
        const std::string& state_name,
        const SourceSpan& source_span,
        const Placement& event,
        const Argument* const undo_label,
        const Argument* const undo_coalesce
    ) {
        const ActionSchema* action = registry_.action("state.setFromEvent");
        if (action == nullptr) {
            throw std::logic_error("widget binding requires the state.setFromEvent action contract");
        }
        action_references_.push_back(ActionReference{event.component_path, action, source_span});
        std::vector<JsonValue> arguments;
        arguments.push_back(object({
            {"name", JsonValue("name")},
            {"span", span(source_span)},
            {"value", expression_base(child(event.path, "name"), source_span, {
                {"kind", JsonValue("literal")},
                {"value", object({
                    {"kind", JsonValue("string")},
                    {"value", JsonValue(state_name)},
                })},
            })},
        }));
        const auto append_undo_argument = [this, &arguments, &event](
                                              const std::string_view name,
                                              const Argument* const argument
                                          ) {
            if (argument == nullptr) return;
            // The argument already lowered in its own slot; here it lowers again under the event.
            arguments.push_back(object({
                {"name", JsonValue(std::string(name))},
                {"span", span(argument->span)},
                {"value", compile_at(*argument->value,
                                     Placement{child(event.path, name),
                                               event.component_path + "." + std::string(name)})},
            }));
        };
        append_undo_argument("undoLabel", undo_label);
        append_undo_argument("undoCoalesce", undo_coalesce);
        return expression_base(event.path, source_span, {
            {"arguments", array(std::move(arguments))},
            {"id", JsonValue("state.setFromEvent")},
            {"kind", JsonValue("action")},
        });
    }

    [[nodiscard]] JsonValue compile_material(const Expression& expression,
                                             const Placement& placement) {
        const IdentifiedCall& identified = model_.identified_calls.at(expression.id);
        material_ids_.insert(identified.id);
        if (const auto* call = std::get_if<CallExpression>(&expression.node)) {
            return expression_base(placement.path, expression.span, {
                {"id", JsonValue(identified.id)},
                {"kind", JsonValue("materialCall")},
                {"parameters", array(identified_arguments(*call, identified))},
            });
        }
        return expression_base(placement.path, expression.span, {
            {"id", JsonValue(identified.id)},
            {"kind", JsonValue("materialReference")},
        });
    }

    [[nodiscard]] static JsonValue expression_base(
        const std::string& path,
        const SourceSpan& source_span,
        const std::initializer_list<data::JsonEntry> fields
    ) {
        JsonValue::Object entries;
        entries.emplace_back("path", JsonValue(path));
        entries.emplace_back("span", span(source_span));
        data::append_json_entries(entries, fields);
        return JsonValue(std::move(entries));
    }

    [[nodiscard]] static JsonValue literal_value(const LiteralValue& literal,
                                                 const ExpressionForm form) {
        if (const auto* value = std::get_if<StringLiteral>(&literal)) {
            const std::string_view kind = form == ExpressionForm::key_literal     ? "key"
                                          : form == ExpressionForm::image_literal ? "image"
                                                                                  : "string";
            return object({{"kind", JsonValue(std::string(kind))}, {"value", JsonValue(value->value)}});
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
        if (const auto* value = std::get_if<BooleanLiteral>(&literal))
            return object({{"kind", JsonValue("boolean")}, {"value", JsonValue(value->value)}});
        return object({{"kind", JsonValue("null")}});
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
            std::vector<JsonValue> keyframes;
            keyframes.push_back(object({
                {"easing", null()},
                {"offset", JsonValue(0.0)},
                {"value", animation_value(track->from)},
            }));
            for (const auto& [offset, value] : track->stops) {
                keyframes.push_back(object({
                    {"easing", null()},
                    {"offset", JsonValue(offset)},
                    {"value", animation_value(value)},
                }));
            }
            keyframes.push_back(object({
                {"easing", null()},
                {"offset", JsonValue(1.0)},
                {"value", animation_value(track->to)},
            }));
            tracks.push_back(object({
                {"keyframes", array(std::move(keyframes))},
                {"property", JsonValue(name)},
            }));
        }
        const std::set<std::string> layout_properties = {
            "width", "height", "minWidth", "minHeight", "maxWidth", "maxHeight",
            "marginLeft", "marginTop", "marginRight", "marginBottom",
            "paddingLeft", "paddingTop", "paddingRight", "paddingBottom",
            "placementX", "placementY", "radialRadius", "radialStart", "radialSweep",
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

    /** The handler being lowered as the function it wraps. */
    NodeId unwrapped_handler_ = std::numeric_limits<NodeId>::max();
    const File& file_;
    const SchemaRegistry& registry_;
    const SemanticModel& model_;
    const std::map<std::string, ValidatedAnimation, std::less<>>& validated_animations_;
    std::set<std::string> material_ids_;
    std::vector<ActionReference> action_references_;
    /** While an expression lowers a second time: its own placement and the one it lowers at. */
    std::optional<std::pair<Placement, Placement>> rebase_;
};

} // namespace

PortableIrResult lower_portable_ir(
    const File& file,
    const SchemaRegistry& registry,
    const SemanticResult& semantic
) {
    return Lowerer(file, registry, semantic).run();
}

} // namespace strata::compiler
