#include "runtime/program.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace strata::runtime {
namespace {

using JsonValue = data::JsonView;
using JsonArray = data::JsonArrayView;
using JsonObject = data::JsonObjectView;

[[nodiscard]] JsonValue required(const JsonValue value, const std::string_view field) {
    const JsonValue found = value.find(field);
    if (!found) {
        throw std::logic_error("portable IR is missing field '" + std::string(field) + "'");
    }
    return found;
}

[[nodiscard]] std::string_view string_field(const JsonValue value, const std::string_view field) {
    const std::optional<std::string_view> text = required(value, field).string();
    if (!text.has_value()) {
        throw std::logic_error("portable IR field '" + std::string(field) + "' must be a string");
    }
    return *text;
}

[[nodiscard]] std::optional<std::string_view> optional_string(const JsonValue value,
                                                              const std::string_view field) {
    return value.find(field).string();
}

[[nodiscard]] JsonArray array_field(const JsonValue value, const std::string_view field) {
    const std::optional<JsonArray> array = required(value, field).array();
    if (!array.has_value()) {
        throw std::logic_error("portable IR field '" + std::string(field) + "' must be an array");
    }
    return *array;
}

[[nodiscard]] JsonObject object_field(const JsonValue value, const std::string_view field) {
    const std::optional<JsonObject> object = required(value, field).object();
    if (!object.has_value()) {
        throw std::logic_error("portable IR field '" + std::string(field) + "' must be an object");
    }
    return *object;
}

[[nodiscard]] double json_number(const JsonValue value) {
    if (const std::optional<double> number = value.number(); number.has_value())
        return *number;
    if (const std::optional<std::int64_t> integer = value.integer(); integer.has_value())
        return static_cast<double>(*integer);
    throw std::logic_error("portable IR number is invalid");
}

[[nodiscard]] int hexadecimal(const char value) noexcept {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

[[nodiscard]] ColorValue parse_color(const std::string_view rgba) {
    if (rgba.size() != 8U)
        throw std::runtime_error("portable IR color must contain RGBA bytes");
    std::uint8_t channels[4]{};
    for (std::size_t index = 0U; index < 4U; ++index) {
        const int high = hexadecimal(rgba[index * 2U]);
        const int low = hexadecimal(rgba[index * 2U + 1U]);
        if (high < 0 || low < 0)
            throw std::runtime_error("portable IR color is not hexadecimal");
        channels[index] = static_cast<std::uint8_t>(high * 16 + low);
    }
    return ColorValue{channels[0], channels[1], channels[2], channels[3]};
}

[[nodiscard]] std::optional<std::size_t> bounded_index(const Value& value) {
    const double* number = value.number();
    if (number == nullptr || *number < 0.0 ||
        *number > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::trunc(*number));
}

[[nodiscard]] HelperKind helper_kind(const std::string_view name) noexcept {
    static constexpr std::pair<std::string_view, HelperKind> helpers[] = {
        {"filter", HelperKind::filter},
        {"map", HelperKind::map},
        {"sortBy", HelperKind::sort_by},
        {"distinctBy", HelperKind::distinct_by},
        {"groupBy", HelperKind::group_by},
        {"flatten", HelperKind::flatten},
        {"takeWhile", HelperKind::take_while},
        {"window", HelperKind::window},
        {"page", HelperKind::page},
        {"persisted", HelperKind::persisted},
        {"sequence", HelperKind::sequence},
        {"parallel", HelperKind::parallel},
        {"choose", HelperKind::choose},
        {"chooseAction", HelperKind::choose},
        {"count", HelperKind::count},
        {"any", HelperKind::any},
        {"all", HelperKind::all},
        {"min", HelperKind::min},
        {"max", HelperKind::max},
        {"clamp", HelperKind::clamp},
        {"abs", HelperKind::abs},
        {"floor", HelperKind::floor},
        {"ceil", HelperKind::ceil},
        {"round", HelperKind::round},
        {"length", HelperKind::length},
        {"size", HelperKind::size},
        {"isEmpty", HelperKind::is_empty},
        {"join", HelperKind::join},
        {"lower", HelperKind::lower},
        {"upper", HelperKind::upper},
        {"trim", HelperKind::trim},
        {"title", HelperKind::title},
        {"contains", HelperKind::contains},
        {"startsWith", HelperKind::starts_with},
        {"endsWith", HelperKind::ends_with},
        {"format", HelperKind::format},
        {"formatNumber", HelperKind::format_number},
        {"rgb", HelperKind::rgb},
        {"rgba", HelperKind::rgba},
        {"animation", HelperKind::animation},
        {"style", HelperKind::style},
        {"whenStyle", HelperKind::when_style},
        {"effect", HelperKind::effect},
    };
    for (const auto& [helper_name, kind] : helpers) {
        if (helper_name == name)
            return kind;
    }
    return HelperKind::unknown;
}

[[nodiscard]] std::optional<BinaryOperator> binary_operator(const std::string_view name) noexcept {
    static constexpr std::pair<std::string_view, BinaryOperator> operators[] = {
        {"add", BinaryOperator::add},
        {"subtract", BinaryOperator::subtract},
        {"multiply", BinaryOperator::multiply},
        {"divide", BinaryOperator::divide},
        {"modulo", BinaryOperator::modulo},
        {"equal", BinaryOperator::equal},
        {"not_equal", BinaryOperator::not_equal},
        {"less", BinaryOperator::less},
        {"less_equal", BinaryOperator::less_equal},
        {"greater", BinaryOperator::greater},
        {"greater_equal", BinaryOperator::greater_equal},
        {"and", BinaryOperator::logical_and},
        {"or", BinaryOperator::logical_or},
        {"coalesce", BinaryOperator::coalesce},
    };
    for (const auto& [operator_name, operation] : operators) {
        if (operator_name == name)
            return operation;
    }
    return std::nullopt;
}

/** What `action_origin` reads from an expression, decoded once. */
[[nodiscard]] std::optional<ProgramActionOrigin> action_origin(const JsonValue expression) {
    const JsonValue span = expression.find("span");
    const JsonValue start = span.find("start");
    const JsonValue end = span.find("end");
    const std::optional<std::string_view> source_id = span.find("sourceId").string();
    if (!source_id.has_value() || !start || !end)
        return std::nullopt;
    const auto position = [](const JsonValue value, const std::string_view field) {
        const std::optional<std::int64_t> number = value.find(field).integer();
        return number.has_value() && *number > 0 && *number <= static_cast<std::int64_t>(UINT32_MAX)
                   ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(*number))
                   : std::nullopt;
    };
    ProgramActionOrigin result{
        ActionOrigin{
            std::string(*source_id),
            position(start, "line"),
            position(start, "column"),
            position(end, "line"),
            position(end, "column"),
            std::nullopt,
        },
        std::nullopt,
    };
    if (const std::optional<std::string_view> path = expression.find("path").string();
        path.has_value()) {
        const std::size_t arguments = path->find("/arguments/");
        result.fallback_component_path = arguments == std::string_view::npos
                                             ? std::string(*path)
                                             : std::string(path->substr(0U, arguments));
    }
    return result;
}

} // namespace

bool collection_helper(const HelperKind helper) noexcept {
    switch (helper) {
    case HelperKind::filter:
    case HelperKind::map:
    case HelperKind::sort_by:
    case HelperKind::distinct_by:
    case HelperKind::group_by:
    case HelperKind::flatten:
    case HelperKind::take_while:
    case HelperKind::window:
    case HelperKind::page:
        return true;
    default:
        return false;
    }
}

/** Builds one Program from portable IR, in the order its tables need. */
class ProgramLowering final {
  public:
    ProgramLowering(Program& program, const Program::StateDeclarationIndex* state_declaration)
        : program_(program), state_declaration_(state_declaration) {}

    [[nodiscard]] ExpressionId expression(const JsonValue source) {
        const std::string_view kind = string_field(source, "kind");
        if (kind == "group")
            return expression(required(source, "expression"));
        ProgramExpression node;
        node.source = source;
        if (kind == "literal") {
            node.kind = ExpressionKind::literal;
            if (!literal(required(source, "value"), node))
                node.kind = ExpressionKind::unknown;
        } else if (kind == "variable") {
            node.kind = ExpressionKind::variable;
            node.name = Symbol::intern(string_field(source, "name"));
            node.state_binding = source.find("stateBinding").boolean().value_or(false);
            const std::string_view binding = optional_string(source, "binding").value_or("");
            if (binding == "host") {
                node.fallback = VariableFallback::host;
                node.host = add_host_path(ProgramHostPath{node.name, {}, std::nullopt});
            } else if (binding == "style" || binding == "animation" || binding == "component") {
                node.fallback = VariableFallback::name;
                node.value = Value(std::string(node.name.name()));
            }
        } else if (kind == "list") {
            node.kind = ExpressionKind::list;
            std::vector<ProgramArgument> elements;
            for (const JsonValue element : array_field(source, "elements")) {
                elements.push_back(ProgramArgument{{}, false, expression(element), element});
            }
            store_arguments(node, std::move(elements));
        } else if (kind == "map") {
            node.kind = ExpressionKind::map;
            std::vector<ProgramArgument> entries;
            for (const auto& [name, value] : object_field(source, "entries")) {
                entries.push_back(
                    ProgramArgument{Symbol::intern(name), true, expression(value), value});
            }
            store_arguments(node, std::move(entries));
        } else if (kind == "unary") {
            node.kind = ExpressionKind::unary;
            const std::string_view operation = string_field(source, "operator");
            if (operation == "not") {
                node.unary = UnaryOperator::logical_not;
            } else if (operation == "negate") {
                node.unary = UnaryOperator::negate;
            } else {
                node.kind = ExpressionKind::unknown;
                node.text = "unary " + std::string(operation);
            }
            if (node.kind == ExpressionKind::unary)
                node.first = expression(required(source, "operand"));
        } else if (kind == "binary") {
            node.kind = ExpressionKind::binary;
            const std::string_view operation = string_field(source, "operator");
            if (const std::optional<BinaryOperator> known = binary_operator(operation)) {
                node.binary = *known;
            } else {
                // Its operands are still evaluated; the operator is reported unknown.
                node.text = std::string(operation);
            }
            node.first = expression(required(source, "left"));
            node.second = expression(required(source, "right"));
        } else if (kind == "conditional") {
            node.kind = ExpressionKind::conditional;
            node.first = expression(required(source, "condition"));
            node.second = expression(required(source, "then"));
            node.third = expression(required(source, "else"));
        } else if (kind == "property") {
            node.kind = ExpressionKind::property;
            node.text = std::string(string_field(source, "name"));
            node.name = Symbol::intern(node.text);
            node.first = expression(required(source, "receiver"));
            if (const std::uint32_t receiver = program_.expressions_[node.first].host;
                receiver != no_program_id) {
                ProgramHostPath path = program_.host_paths_[receiver];
                path.steps.push_back(
                    ProgramHostStep{false, HostPathSegment::named(node.text), no_program_id});
                node.host = add_host_path(std::move(path));
            }
        } else if (kind == "index") {
            node.kind = ExpressionKind::index;
            node.first = expression(required(source, "receiver"));
            node.second = expression(required(source, "index"));
            if (const std::uint32_t receiver = program_.expressions_[node.first].host;
                receiver != no_program_id) {
                ProgramHostPath path = program_.host_paths_[receiver];
                const ProgramExpression& index = program_.expressions_[node.second];
                if (index.kind == ExpressionKind::literal) {
                    path.steps.push_back(
                        ProgramHostStep{false, lookup_segment(index.value), no_program_id});
                } else {
                    path.steps.push_back(ProgramHostStep{true, {}, node.second});
                }
                node.host = add_host_path(std::move(path));
            }
        } else if (kind == "helper") {
            node.kind = ExpressionKind::helper;
            node.text = std::string(string_field(source, "name"));
            node.helper = helper_kind(node.text);
            store_arguments(node, arguments(source));
            if (node.helper == HelperKind::sequence || node.helper == HelperKind::parallel)
                node.origin = add_origin(source);
        } else if (kind == "action") {
            node.kind = ExpressionKind::action;
            node.text = std::string(string_field(source, "id"));
            store_arguments(node, arguments(source));
            node.origin = add_origin(source);
        } else if (kind == "componentTemplate") {
            node.kind = ExpressionKind::component_template;
            node.text = std::string(string_field(source, "component"));
            store_arguments(node, arguments(source));
        } else if (kind == "lambda") {
            node.kind = ExpressionKind::lambda;
            node.name = Symbol::intern(string_field(source, "parameter"));
            node.first = expression(required(source, "body"));
            std::vector<Symbol> free;
            collect_free_variables(node.first, free);
            std::ranges::sort(free);
            const auto duplicates = std::ranges::unique(free);
            free.erase(duplicates.begin(), duplicates.end());
            std::erase(free, node.name);
            node.free_variables_begin = static_cast<std::uint32_t>(program_.free_variables_.size());
            node.free_variables_count = static_cast<std::uint32_t>(free.size());
            program_.free_variables_.insert(program_.free_variables_.end(), free.begin(),
                                            free.end());
        } else if (kind == "materialReference" || kind == "materialCall") {
            node.kind = ExpressionKind::material;
            node.text = std::string(string_field(source, "id"));
            node.material_call = kind == "materialCall";
            if (node.material_call) {
                std::vector<ProgramArgument> parameters;
                for (const JsonValue parameter : array_field(source, "parameters")) {
                    parameters.push_back(ProgramArgument{
                        Symbol::intern(string_field(parameter, "name")),
                        true,
                        expression(required(parameter, "value")),
                        parameter,
                    });
                }
                store_arguments(node, std::move(parameters));
            }
        } else {
            node.kind = ExpressionKind::unknown;
            node.text = std::string(kind);
        }
        program_.expressions_.push_back(std::move(node));
        return static_cast<ExpressionId>(program_.expressions_.size() - 1U);
    }

    [[nodiscard]] ExpressionId optional_expression(const JsonValue source) {
        return !source || source.is_null() ? no_program_id : expression(source);
    }

    /** A Repeater's body is the one block whose loop carries a row-key extractor. */
    [[nodiscard]] BlockId block(const JsonValue source, const std::string_view declaration_scope,
                                const bool repeater_body = false) {
        std::vector<ProgramStatement> statements;
        for (const JsonValue encoded : array_field(source, "statements")) {
            statements.push_back(statement(encoded, declaration_scope, repeater_body));
        }
        ProgramBlock result{static_cast<std::uint32_t>(program_.statements_.size()),
                            static_cast<std::uint32_t>(statements.size())};
        program_.statements_.insert(program_.statements_.end(),
                                    std::make_move_iterator(statements.begin()),
                                    std::make_move_iterator(statements.end()));
        program_.blocks_.push_back(result);
        return static_cast<BlockId>(program_.blocks_.size() - 1U);
    }

    void register_component(const JsonValue source) {
        ProgramComponent component;
        component.name = std::string(string_field(source, "name"));
        component.path = std::string(optional_string(source, "path").value_or(""));
        component.declaration_scope = "component " + component.name;
        component.parameters_begin = static_cast<std::uint32_t>(program_.parameters_.size());
        for (const JsonValue parameter : array_field(source, "parameters")) {
            const JsonValue schema = required(parameter, "schema");
            const std::optional<bool> state_binding = required(schema, "stateBinding").boolean();
            if (!state_binding.has_value()) {
                throw std::logic_error("validated component parameter binding flag changed type");
            }
            program_.parameters_.push_back(ProgramParameter{
                Symbol::intern(string_field(schema, "name")), no_program_id, *state_binding});
        }
        component.parameters_count =
            static_cast<std::uint32_t>(program_.parameters_.size()) - component.parameters_begin;
        const std::uint32_t index = static_cast<std::uint32_t>(program_.components_.size());
        if (!program_.component_indexes_.emplace(component.name, index).second) {
            throw std::logic_error("portable IR contains duplicate component '" + component.name +
                                   "'");
        }
        program_.components_.push_back(std::move(component));
    }

    void lower_component(const std::uint32_t index, const JsonValue source) {
        ProgramComponent& registered = program_.components_[index];
        const std::string declaration_scope = registered.declaration_scope;
        const std::uint32_t parameters_begin = registered.parameters_begin;
        const JsonArray parameters = array_field(source, "parameters");
        for (std::size_t position = 0U; position < parameters.size(); ++position) {
            const ExpressionId default_value =
                optional_expression(required(parameters[position], "default"));
            program_.parameters_[parameters_begin + position].default_value = default_value;
        }
        std::vector<ProgramWidgetDefault> defaults;
        for (const JsonValue entry : array_field(source, "widgetDefaults")) {
            defaults.push_back(ProgramWidgetDefault{
                std::string(string_field(entry, "name")),
                optional_expression(required(entry, "style")),
                optional_expression(required(entry, "variant")),
            });
        }
        const BlockId body = block(required(source, "body"), declaration_scope);
        ProgramComponent& component = program_.components_[index];
        component.widget_defaults_begin =
            static_cast<std::uint32_t>(program_.widget_defaults_.size());
        component.widget_defaults_count = static_cast<std::uint32_t>(defaults.size());
        program_.widget_defaults_.insert(program_.widget_defaults_.end(),
                                         std::make_move_iterator(defaults.begin()),
                                         std::make_move_iterator(defaults.end()));
        component.body = body;
    }

    void layer(const JsonValue source, const std::string_view role,
               std::vector<ProgramLayer>& layers,
               std::map<std::string, std::uint32_t, std::less<>>& indexes) {
        ProgramLayer layer;
        layer.name = std::string(string_field(source, "name"));
        layer.path = std::string(optional_string(source, "path").value_or(""));
        layer.declaration_scope = std::string(role) + " " + layer.name;
        layer.body = block(required(source, "body"), layer.declaration_scope);
        const std::uint32_t index = static_cast<std::uint32_t>(layers.size());
        if (!indexes.emplace(layer.name, index).second) {
            throw std::logic_error("portable IR contains duplicate " + std::string(role) + " '" +
                                   layer.name + "'");
        }
        layers.push_back(std::move(layer));
    }

    void style(const JsonValue source) {
        ProgramStyle style;
        style.name = std::string(string_field(source, "name"));
        for (const JsonValue base : array_field(source, "bases")) {
            if (const std::optional<std::string_view> name = base.string(); name.has_value())
                style.bases.emplace_back(*name);
        }
        for (const auto& [property, value] : object_field(source, "properties")) {
            style.properties.emplace_back(std::string(property), expression(value));
        }
        const std::uint32_t index = static_cast<std::uint32_t>(program_.styles_.size());
        if (!program_.style_indexes_.emplace(style.name, index).second) {
            throw std::logic_error("portable IR contains duplicate style '" + style.name + "'");
        }
        program_.styles_.push_back(std::move(style));
    }

  private:
    [[nodiscard]] bool literal(const JsonValue literal, ProgramExpression& node) {
        const std::string_view kind = string_field(literal, "kind");
        if (kind == "null") {
            node.value = Value{};
        } else if (kind == "boolean") {
            node.value = Value(*required(literal, "value").boolean());
        } else if (kind == "number") {
            node.value = Value(json_number(required(literal, "value")));
        } else if (kind == "duration") {
            node.value = Value(DurationValue{*required(literal, "nanos").integer()});
        } else if (kind == "string") {
            node.value = Value(std::string(string_field(literal, "value")));
        } else if (kind == "image") {
            node.value = Value(ImageValue{std::string(string_field(literal, "value"))});
        } else if (kind == "key") {
            node.value = Value(KeyValue{std::string(string_field(literal, "value"))});
        } else if (kind == "color") {
            node.value = Value(parse_color(string_field(literal, "rgba")));
        } else if (kind == "themeToken") {
            node.value = Value(ThemeTokenValue{std::string(string_field(literal, "name"))});
        } else if (kind == "styleReference" || kind == "animation") {
            node.value = Value(std::string(string_field(literal, "name")));
        } else {
            node.text = "literal " + std::string(kind);
            return false;
        }
        return true;
    }

    [[nodiscard]] static HostPathSegment lookup_segment(const Value& index) {
        return HostPathSegment::lookup(index.string() != nullptr ? *index.string()
                                                                 : display_string(index),
                                       bounded_index(index));
    }

    [[nodiscard]] std::vector<ProgramArgument> arguments(const JsonValue source) {
        std::vector<ProgramArgument> result;
        for (const JsonValue argument : array_field(source, "arguments")) {
            const JsonValue name = argument.find("name");
            const std::optional<std::string_view> text = name.string();
            result.push_back(ProgramArgument{
                text.has_value() ? Symbol::intern(*text) : Symbol{},
                text.has_value(),
                expression(required(argument, "value")),
                argument,
            });
        }
        return result;
    }

    void store_arguments(ProgramExpression& node, std::vector<ProgramArgument> arguments) {
        node.arguments_begin = static_cast<std::uint32_t>(program_.arguments_.size());
        node.arguments_count = static_cast<std::uint32_t>(arguments.size());
        program_.arguments_.insert(program_.arguments_.end(),
                                   std::make_move_iterator(arguments.begin()),
                                   std::make_move_iterator(arguments.end()));
    }

    [[nodiscard]] std::uint32_t add_host_path(ProgramHostPath path) {
        if (std::ranges::none_of(path.steps, &ProgramHostStep::dynamic)) {
            std::vector<HostPathSegment> fixed;
            fixed.reserve(path.steps.size() + 1U);
            fixed.push_back(HostPathSegment::named(std::string(path.root.name())));
            for (const ProgramHostStep& step : path.steps)
                fixed.push_back(step.segment);
            path.fixed = std::move(fixed);
        }
        program_.host_paths_.push_back(std::move(path));
        return static_cast<std::uint32_t>(program_.host_paths_.size() - 1U);
    }

    [[nodiscard]] std::uint32_t add_origin(const JsonValue source) {
        std::optional<ProgramActionOrigin> origin = action_origin(source);
        if (!origin.has_value())
            return no_program_id;
        program_.origins_.push_back(std::move(*origin));
        return static_cast<std::uint32_t>(program_.origins_.size() - 1U);
    }

    /** Every name a lambda body reads, including those its own lambdas read from it. */
    void collect_free_variables(const ExpressionId id, std::vector<Symbol>& names) const {
        const ProgramExpression& node = program_.expressions_[id];
        switch (node.kind) {
        case ExpressionKind::variable:
            names.push_back(node.name);
            return;
        case ExpressionKind::lambda: {
            for (const Symbol name : program_.free_variables(node))
                names.push_back(name);
            return;
        }
        default:
            break;
        }
        for (const ExpressionId child : {node.first, node.second, node.third}) {
            if (child != no_program_id)
                collect_free_variables(child, names);
        }
        for (std::uint32_t index = 0U; index < node.arguments_count; ++index) {
            collect_free_variables(program_.arguments_[node.arguments_begin + index].value, names);
        }
    }

    [[nodiscard]] ProgramStatement statement(const JsonValue source,
                                             const std::string_view declaration_scope,
                                             const bool repeater_body) {
        const std::string_view kind = string_field(source, "kind");
        ProgramStatement result;
        result.source = source;
        if (kind == "state") {
            result.kind = StatementKind::state;
            const std::string_view name = string_field(source, "name");
            result.name = Symbol::intern(name);
            result.expression = optional_expression(required(source, "initializer"));
            if (state_declaration_ != nullptr) {
                result.state_declaration =
                    (*state_declaration_)(declaration_scope, name, result.expression);
            }
        } else if (kind == "derived") {
            result.kind = StatementKind::derived;
            result.name = Symbol::intern(string_field(source, "name"));
            result.expression = expression(required(source, "expression"));
        } else if (kind == "node") {
            result.kind = StatementKind::node;
            result.call = call(required(source, "call"), declaration_scope);
        } else if (kind == "if") {
            result.kind = StatementKind::conditional;
            result.expression = expression(required(source, "condition"));
            result.block = block(required(source, "then"), declaration_scope);
            const JsonValue otherwise = required(source, "else");
            result.otherwise =
                otherwise.is_null() ? no_program_id : block(otherwise, declaration_scope);
        } else if (kind == "when") {
            result.kind = StatementKind::when;
            result.expression = expression(required(source, "subject"));
            std::vector<ProgramWhenBranch> branches;
            for (const JsonValue branch : array_field(source, "branches")) {
                const ExpressionId match = optional_expression(required(branch, "match"));
                branches.push_back(
                    ProgramWhenBranch{match, block(required(branch, "block"), declaration_scope)});
            }
            result.branches_begin = static_cast<std::uint32_t>(program_.branches_.size());
            result.branches_count = static_cast<std::uint32_t>(branches.size());
            program_.branches_.insert(program_.branches_.end(), branches.begin(), branches.end());
        } else if (kind == "for") {
            result.kind = StatementKind::loop;
            result.name = Symbol::intern(string_field(source, "itemName"));
            if (const std::optional<std::string_view> index =
                    required(source, "indexName").string();
                index.has_value()) {
                result.index_name = Symbol::intern(*index);
                result.has_index = true;
            }
            result.expression = expression(required(source, "collection"));
            result.filter = optional_expression(required(source, "filter"));
            if (repeater_body)
                result.identity = this->identity(required(source, "identity"));
            result.block = block(required(source, "block"), declaration_scope);
        } else {
            throw std::logic_error("validated portable IR contains an unknown statement kind");
        }
        return result;
    }

    [[nodiscard]] CallId call(const JsonValue source, const std::string_view declaration_scope) {
        ProgramCall result;
        result.source = source;
        result.component = string_field(source, "kind") == "component";
        result.type = std::string(string_field(source, "name"));
        result.path = std::string(optional_string(source, "path").value_or(""));
        std::vector<ProgramCallArgument> arguments;
        std::vector<ProgramBehavior> behaviors;
        for (const auto& [name, value] : object_field(source, "arguments")) {
            if (name == "behaviors") {
                result.has_behaviors = true;
                if (string_field(value, "kind") == "list") {
                    for (const JsonValue element : array_field(value, "elements")) {
                        if (string_field(element, "kind") != "map")
                            continue;
                        const JsonValue entries = required(element, "entries");
                        const JsonValue id = entries.find("id");
                        if (!id)
                            continue;
                        ProgramBehavior behavior;
                        behavior.id = expression(id);
                        if (const JsonValue enabled = entries.find("enabled"); enabled)
                            behavior.enabled = expression(enabled);
                        if (const JsonValue options = entries.find("options"); options)
                            behavior.options = expression(options);
                        if (const JsonValue action = entries.find("action"); action)
                            behavior.action = expression(action);
                        behaviors.push_back(behavior);
                    }
                }
                continue;
            }
            if (name == "key")
                result.key_argument = static_cast<std::uint32_t>(arguments.size());
            arguments.push_back(ProgramCallArgument{Symbol::intern(name), expression(value)});
        }
        const JsonValue children = required(source, "children");
        result.children = children.is_null()
                              ? no_program_id
                              : block(children, declaration_scope, result.type == "Repeater");

        result.arguments_begin = static_cast<std::uint32_t>(program_.call_arguments_.size());
        result.arguments_count = static_cast<std::uint32_t>(arguments.size());
        program_.call_arguments_.insert(program_.call_arguments_.end(), arguments.begin(),
                                        arguments.end());
        result.behaviors_begin = static_cast<std::uint32_t>(program_.behaviors_.size());
        result.behaviors_count = static_cast<std::uint32_t>(behaviors.size());
        program_.behaviors_.insert(program_.behaviors_.end(), behaviors.begin(), behaviors.end());

        if (result.component) {
            const auto component = program_.component_indexes_.find(result.type);
            if (component == program_.component_indexes_.end()) {
                throw std::logic_error("component call '" + result.type +
                                       "' names no declared component");
            }
            result.component_index = component->second;
            const ProgramComponent& declaration = program_.components_[component->second];
            result.parameter_arguments_begin =
                static_cast<std::uint32_t>(program_.parameter_arguments_.size());
            for (std::uint32_t parameter = 0U; parameter < declaration.parameters_count;
                 ++parameter) {
                const Symbol name =
                    program_.parameters_[declaration.parameters_begin + parameter].name;
                const auto supplied =
                    std::ranges::find(arguments, name, &ProgramCallArgument::name);
                program_.parameter_arguments_.push_back(
                    supplied != arguments.end()
                        ? static_cast<std::uint32_t>(supplied - arguments.begin())
                        : no_program_id);
            }
            if (result.children != no_program_id) {
                const ProgramBlock& block = program_.blocks_[result.children];
                result.slot_fills_begin = static_cast<std::uint32_t>(program_.slot_fills_.size());
                result.projected_statements_begin =
                    static_cast<std::uint32_t>(program_.projected_statements_.size());
                for (std::uint32_t index = 0U; index < block.statements_count; ++index) {
                    const ProgramStatement& statement =
                        program_.statements_[block.statements_begin + index];
                    if (statement.kind != StatementKind::node)
                        continue;
                    const ProgramCall& fill = program_.calls_[statement.call];
                    if (fill.component || fill.type != "Slot")
                        continue;
                    program_.projected_statements_.push_back(index);
                    ProgramSlotFill slot;
                    slot.children = fill.children;
                    for (std::uint32_t argument = 0U; argument < fill.arguments_count; ++argument) {
                        const ProgramCallArgument& entry =
                            program_.call_arguments_[fill.arguments_begin + argument];
                        if (entry.name.name() == "name")
                            slot.name = entry.value;
                    }
                    program_.slot_fills_.push_back(slot);
                }
                result.slot_fills_count = static_cast<std::uint32_t>(program_.slot_fills_.size()) -
                                          result.slot_fills_begin;
                result.projected_statements_count =
                    static_cast<std::uint32_t>(program_.projected_statements_.size()) -
                    result.projected_statements_begin;
            }
        }
        program_.calls_.push_back(std::move(result));
        return static_cast<CallId>(program_.calls_.size() - 1U);
    }

    [[nodiscard]] IdentityId identity(const JsonValue source) {
        const std::string_view kind = string_field(source, "kind");
        ProgramIdentity result;
        if (kind == "key") {
            result.kind = IdentityKind::key;
            result.expression = expression(required(source, "expression"));
        } else if (kind == "block") {
            result.kind = IdentityKind::block;
            std::vector<IdentityId> children;
            for (const JsonValue statement : array_field(source, "statements"))
                children.push_back(identity(statement));
            result.children_begin = static_cast<std::uint32_t>(program_.identity_children_.size());
            result.children_count = static_cast<std::uint32_t>(children.size());
            program_.identity_children_.insert(program_.identity_children_.end(), children.begin(),
                                               children.end());
        } else if (kind == "if") {
            result.kind = IdentityKind::conditional;
            result.expression = expression(required(source, "condition"));
            result.then_identity = identity(required(source, "then"));
            result.else_identity = identity(required(source, "else"));
        } else if (kind == "when") {
            result.kind = IdentityKind::when;
            result.expression = expression(required(source, "subject"));
            std::vector<ProgramIdentityBranch> branches;
            for (const JsonValue branch : array_field(source, "branches")) {
                const ExpressionId match = optional_expression(required(branch, "match"));
                branches.push_back(
                    ProgramIdentityBranch{match, identity(required(branch, "identity"))});
            }
            result.branches_begin = static_cast<std::uint32_t>(program_.identity_branches_.size());
            result.branches_count = static_cast<std::uint32_t>(branches.size());
            program_.identity_branches_.insert(program_.identity_branches_.end(), branches.begin(),
                                               branches.end());
        } else {
            throw std::logic_error(
                "validated Repeater identity contains an unknown extractor kind");
        }
        program_.identities_.push_back(result);
        return static_cast<IdentityId>(program_.identities_.size() - 1U);
    }

    Program& program_;
    const Program::StateDeclarationIndex* state_declaration_;
};

Program::Program() {
    static std::atomic<std::uint64_t> next{1U};
    serial_ = next.fetch_add(1U, std::memory_order_relaxed);
}

std::shared_ptr<const Program> Program::lower_unit(const data::JsonView unit,
                                                   const StateDeclarationIndex& state_declaration) {
    auto program = std::shared_ptr<Program>(new Program());
    ProgramLowering lowering(*program, &state_declaration);
    const JsonArray components = array_field(unit, "components");
    for (const JsonValue component : components)
        lowering.register_component(component);
    for (std::size_t index = 0U; index < components.size(); ++index) {
        lowering.lower_component(static_cast<std::uint32_t>(index), components[index]);
    }
    for (const JsonValue screen : array_field(unit, "screens")) {
        lowering.layer(screen, "screen", program->screens_, program->screen_indexes_);
    }
    for (const JsonValue overlay : array_field(unit, "overlays")) {
        lowering.layer(overlay, "overlay", program->overlays_, program->overlay_indexes_);
    }
    for (const JsonValue style : array_field(unit, "styles"))
        lowering.style(style);
    return program;
}

std::shared_ptr<const Program> Program::lower_expression(const data::JsonView expression) {
    auto program = std::shared_ptr<Program>(new Program());
    program->owned_source_ =
        std::make_shared<const data::JsonValue>(data::materialize_json(expression));
    ProgramLowering lowering(*program, nullptr);
    program->root_ = lowering.expression(data::JsonView(*program->owned_source_));
    return program;
}

std::span<const std::uint32_t> Program::parameter_arguments(const ProgramCall& call) const {
    if (!call.component)
        return {};
    return {parameter_arguments_.data() + call.parameter_arguments_begin,
            components_[call.component_index].parameters_count};
}

std::optional<std::uint32_t> Program::component_index(const std::string_view name) const {
    const auto found = component_indexes_.find(name);
    return found != component_indexes_.end() ? std::optional<std::uint32_t>(found->second)
                                             : std::nullopt;
}

const ProgramLayer* Program::screen(const std::string_view name) const {
    const auto found = screen_indexes_.find(name);
    return found != screen_indexes_.end() ? &screens_[found->second] : nullptr;
}

const ProgramLayer* Program::overlay(const std::string_view name) const {
    const auto found = overlay_indexes_.find(name);
    return found != overlay_indexes_.end() ? &overlays_[found->second] : nullptr;
}

const ProgramStyle* Program::style(const std::string_view name) const {
    const auto found = style_indexes_.find(name);
    return found != style_indexes_.end() ? &styles_[found->second] : nullptr;
}

} // namespace strata::runtime
