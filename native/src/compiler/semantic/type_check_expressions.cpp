#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "compiler/semantic/type_check.hpp"

namespace strata::compiler::semantic {
namespace {

[[nodiscard]] std::string operator_symbol(const BinaryOperator operation) {
    switch (operation) {
    case BinaryOperator::add: return "+";
    case BinaryOperator::subtract: return "-";
    case BinaryOperator::multiply: return "*";
    case BinaryOperator::divide: return "/";
    case BinaryOperator::modulo: return "%";
    case BinaryOperator::less: return "<";
    case BinaryOperator::less_equal: return "<=";
    case BinaryOperator::greater: return ">";
    case BinaryOperator::greater_equal: return ">=";
    default: return "?";
    }
}

} // namespace

void TypeChecker::unknown_identifier(const IdentifierExpression& identifier,
                                     const Expression& expression,
                                     const std::string& component_path) {
    if (std::ranges::contains(contextual_host_roots, identifier.name)) return;
    const auto visible = analysis_.visible_names.find(expression.id);
    const std::vector<std::string> names =
        visible != analysis_.visible_names.end() ? visible->second : std::vector<std::string>{};
    const std::optional<std::string> suggestion = nearest_name(identifier.name, names);
    if (!registry_.host_roots_declared() && !suggestion.has_value()) return;
    // Event values reach actions through their own parameters, never through a name.
    const std::string hint =
        identifier.name == "event"
            ? " An action receives its event through fromEvent: true or state.setFromEvent."
        : suggestion.has_value() ? " Did you mean '" + *suggestion + "'?"
                                 : std::string{};
    // Only a local declaration makes an identifier a value: the state, derived value or
    // parameter it names would be in this module, not in one that failed to load.
    unresolved("STRATA.DSL.SEMANTIC_UNKNOWN_IDENTIFIER", identifier.name, false,
               "'" + identifier.name +
                   "' is not a state, parameter, local value or declared host root." + hint,
               expression.span, component_path, expected_names(names));
}

bool TypeChecker::theme_token_operand(const Expression& expression,
                                      std::initializer_list<const Expression*> operands,
                                      const std::string& symbol,
                                      const std::string& component_path) {
    for (const Expression* operand : operands) {
        const auto* property = std::get_if<PropertyAccessExpression>(&ungrouped(*operand).node);
        const auto* receiver =
            property != nullptr ? std::get_if<IdentifierExpression>(&property->receiver->node)
                                : nullptr;
        if (receiver == nullptr || receiver->name != "theme") continue;
        report("STRATA.DSL.SEMANTIC_TYPE_MISMATCH",
               "Operator '" + symbol + "' cannot read theme." + property->property_name +
                   ": a theme token is resolved by the surface's theme after evaluation, so "
                   "use it directly as a style value.",
               expression.span, component_path, "a value computed without theme tokens");
        return true;
    }
    return false;
}

void TypeChecker::arithmetic(const Expression& expression, const std::string& symbol,
                             const SemanticType& left, const SemanticType& right,
                             const std::string& component_path) {
    for (const SemanticType* operand : {&left, &right}) {
        const OperandClass kind = operand_class(*operand);
        if (kind != OperandClass::text && kind != OperandClass::other) continue;
        report("STRATA.DSL.SEMANTIC_TYPE_MISMATCH",
               "Operator '" + symbol + "' needs numbers or durations" +
                   (symbol == "+" ? " or text" : "") + ", but found " +
                   operand->diagnostic_name() + ".",
               expression.span, component_path, "number or duration");
        return;
    }
}

SemanticTypePtr TypeChecker::infer(const Expression& expression,
                                   const std::string& component_path) {
    check(expression, component_path);
    return oracle_.type_of(expression);
}

void TypeChecker::check(const Expression& expression, const std::string& component_path) {
    if (checked_[expression.id]) return;
    checked_[expression.id] = true;
    if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.node)) {
        const Symbol* symbol = model_.symbol(expression);
        if (symbol == nullptr || symbol->kind == SymbolKind::unresolved)
            unknown_identifier(*identifier, expression, component_path);
    } else if (const auto* grouping = std::get_if<GroupingExpression>(&expression.node)) {
        check(*grouping->expression, component_path);
    } else if (const auto* list = std::get_if<ListExpression>(&expression.node)) {
        for (const ExpressionPtr& element : list->elements) check(*element, component_path);
    } else if (const auto* map = std::get_if<MapExpression>(&expression.node)) {
        for (const MapEntry& entry : map->entries) check(*entry.value, component_path);
    } else if (const auto* unary = std::get_if<UnaryExpression>(&expression.node)) {
        const SemanticTypePtr operand = infer(*unary->operand, component_path);
        if (unary->operation == UnaryOperator::negate &&
            !theme_token_operand(expression, {unary->operand.get()}, "-", component_path)) {
            arithmetic(expression, "-", *operand, *simple(SemanticTypeKind::number),
                       component_path);
        }
    } else if (const auto* binary = std::get_if<BinaryExpression>(&expression.node)) {
        const SemanticTypePtr left = infer(*binary->left, component_path);
        const SemanticTypePtr right = infer(*binary->right, component_path);
        switch (binary->operation) {
        case BinaryOperator::equal:
        case BinaryOperator::not_equal:
        case BinaryOperator::logical_and:
        case BinaryOperator::logical_or:
        case BinaryOperator::coalesce:
            break;
        default:
            if (theme_token_operand(expression, {binary->left.get(), binary->right.get()},
                                    operator_symbol(binary->operation), component_path)) {
                return;
            }
            break;
        }
        switch (binary->operation) {
        case BinaryOperator::less:
        case BinaryOperator::less_equal:
        case BinaryOperator::greater:
        case BinaryOperator::greater_equal:
        case BinaryOperator::subtract:
        case BinaryOperator::multiply:
        case BinaryOperator::divide:
        case BinaryOperator::modulo:
            arithmetic(expression, operator_symbol(binary->operation), *left, *right,
                       component_path);
            break;
        case BinaryOperator::add:
            if (operand_class(*left) == OperandClass::text ||
                operand_class(*right) == OperandClass::text ||
                operand_class(*left) == OperandClass::undetermined ||
                operand_class(*right) == OperandClass::undetermined) {
                break;
            }
            arithmetic(expression, "+", *left, *right, component_path);
            break;
        default:
            break;
        }
    } else if (const auto* conditional = std::get_if<ConditionalExpression>(&expression.node)) {
        check(*conditional->condition, component_path);
        check(*conditional->then_expression, component_path);
        check(*conditional->else_expression, component_path);
    } else if (const auto* property = std::get_if<PropertyAccessExpression>(&expression.node)) {
        property_access(expression, *property, component_path);
    } else if (const auto* indexed = std::get_if<IndexExpression>(&expression.node)) {
        const SemanticTypePtr receiver = infer(*indexed->receiver, component_path);
        check(*indexed->index, component_path);
        if (receiver->kind == SemanticTypeKind::async_value) {
            if (const std::string* field = string_literal_value(*indexed->index);
                field != nullptr && *field == "value") {
                report("STRATA.DSL.SEMANTIC_ASYNC_VALUE_ACCESS",
                       "An async value is available only inside its READY when branch.",
                       expression.span, component_path, "READY branch");
            }
        }
    } else if (const auto* call = std::get_if<CallExpression>(&expression.node)) {
        call_expression(expression, *call, component_path);
    } else if (const auto* lambda = std::get_if<LambdaExpression>(&expression.node)) {
        lambda_body(expression, *lambda, component_path);
    } else if (const auto* spread = std::get_if<SpreadExpression>(&expression.node)) {
        check(*spread->operand, component_path);
    } else if (const auto* awaited = std::get_if<AwaitExpression>(&expression.node)) {
        if (!permitted_awaits_.contains(expression.id)) {
            report("STRATA.DSL.SCRIPT_AWAIT_POSITION",
                   "'await' must be a whole statement's value: let x = await ..., "
                   "x = await ..., await ...; or return await ....",
                   expression.span, component_path, "await as a statement's value");
        }
        check(*awaited->operand, component_path);
    }
}

void TypeChecker::lambda_body(const Expression& expression, const LambdaExpression& lambda,
                              const std::string& component_path) {
    // A script function's body may have effects: it runs when it is called, by a handler.
    const bool script = lambda.script;
    if (script) ++effects_allowed_;
    struct Restore {
        std::size_t& depth;
        bool active;
        ~Restore() {
            if (active) --depth;
        }
    } restore{effects_allowed_, script};
    if (lambda.block != nullptr) {
        script_block(*lambda.block, component_path, ScriptContext{false, true, true});
        return;
    }
    const SchemaParameter* parameter = oracle_.lambda_parameter_of(expression);
    const SemanticType* returns =
        parameter != nullptr && parameter->type->kind == SemanticTypeKind::lambda
            ? parameter->type->returns.get()
            : nullptr;
    if (returns != nullptr)
        validate_expected(*lambda.body, *returns, false, component_path);
    else
        check(*lambda.body, component_path);
}

void TypeChecker::property_access(const Expression& expression,
                                  const PropertyAccessExpression& property,
                                  const std::string& component_path) {
    if (const auto* receiver = std::get_if<IdentifierExpression>(&property.receiver->node);
        receiver != nullptr && receiver->name == "theme") {
        if (registry_.theme_token_kind(property.property_name) != SemanticTypeKind::unknown)
            return;
        const std::vector<std::string> names = registry_.theme_token_names();
        report("STRATA.DSL.SEMANTIC_UNKNOWN_THEME_TOKEN",
               "Theme token '" + property.property_name + "' is not defined." +
                   did_you_mean(property.property_name, names),
               expression.span, component_path, expected_names(names));
        return;
    }
    const SemanticTypePtr receiver = infer(*property.receiver, component_path);
    if (receiver->kind == SemanticTypeKind::async_value && property.property_name == "value") {
        report("STRATA.DSL.SEMANTIC_ASYNC_VALUE_ACCESS",
               "An async value is available only inside its READY when branch.",
               expression.span, component_path, "READY branch");
    }
    if (receiver->find_field(property.property_name) != nullptr) return;
    // A closed object's fields are all known, so any other name reads nothing at runtime.
    if ((receiver->kind == SemanticTypeKind::map ||
         receiver->kind == SemanticTypeKind::host_object) &&
        !receiver->fields.empty() && !receiver->allow_unknown_fields) {
        std::vector<std::string> names;
        names.reserve(receiver->fields.size());
        for (const ObjectField& declared : receiver->fields) names.push_back(declared.name);
        report("STRATA.DSL.SEMANTIC_UNKNOWN_FIELD",
               "Field '" + property.property_name + "' is not part of " +
                   receiver->diagnostic_name() + "." +
                   did_you_mean(property.property_name, names),
               expression.span, component_path, expected_names(names));
    }
}

void TypeChecker::call_expression(const Expression& expression, const CallExpression& call,
                                  const std::string& component_path) {
    const ResolvedCall& resolved = *model_.call(expression);
    const std::string name = call.target.qualified_name();
    // These validate against their own kind as if a slot expected it.
    const auto as_kind = [&](const SemanticTypeKind kind) {
        checked_[expression.id] = false;
        validate_expected(expression, *simple(kind), false, component_path);
        checked_[expression.id] = true;
    };
    switch (resolved.kind) {
    case CallKind::action:
        action_call(expression, call, component_path);
        return;
    case CallKind::material:
        as_kind(SemanticTypeKind::material);
        return;
    case CallKind::style:
        as_kind(SemanticTypeKind::style);
        return;
    case CallKind::effect:
        as_kind(SemanticTypeKind::effect);
        return;
    case CallKind::component_template:
        static_cast<void>(component_template_arguments(
            expression, call, name, *resolved.component, nullptr, component_path));
        return;
    case CallKind::unknown_helper:
        report("STRATA.DSL.SEMANTIC_UNKNOWN_HELPER",
               "Helper function '" + name + "' is not registered as a pure DSL helper.",
               call.target.span, component_path, "registered helper");
        for (const Argument& argument : call.arguments) check(*argument.value, component_path);
        return;
    case CallKind::function:
        function_call(expression, call, resolved, component_path);
        return;
    case CallKind::helper:
        break;
    }
    if (name == "animation") {
        as_kind(SemanticTypeKind::animation);
        return;
    }
    helper_arguments(expression, call, *resolved.helper, component_path);
}

void TypeChecker::function_call(const Expression& expression, const CallExpression& call,
                                const ResolvedCall& resolved, const std::string& component_path) {
    const std::string name = call.target.qualified_name();
    if (resolved.function == nullptr) {
        // A local holding a function value: whatever it is called with is checked at runtime.
        const SemanticTypePtr callee = oracle_.symbol_type(resolved.callee);
        // An action called like a function runs the handler it holds, or is performed.
        if (callee->kind != SemanticTypeKind::function && callee->kind != SemanticTypeKind::any &&
            callee->kind != SemanticTypeKind::unknown && callee->kind != SemanticTypeKind::action) {
            report("STRATA.DSL.SCRIPT_NOT_CALLABLE",
                   "'" + name + "' is " + callee->diagnostic_name() + ", not a function.",
                   call.target.span, component_path, "function");
        }
        for (const Argument& argument : call.arguments) check(*argument.value, component_path);
        return;
    }
    const FunctionInfo& function = *resolved.function;
    if (effects_allowed_ == 0U && effectful(*function.declaration)) {
        report("STRATA.DSL.SCRIPT_EFFECT_IN_PURE_CONTEXT",
               "Function '" + name + "' writes state or performs actions, so it can be called "
               "only by a handler or another function, not where a value is computed.",
               call.target.span, component_path, "a function without effects");
    }
    if (function.declaration->asynchronous && !awaited_calls_.contains(expression.id)) {
        report("STRATA.DSL.SCRIPT_ASYNC_NOT_AWAITED",
               "Async function '" + name + "' must be awaited: await " + name + "(...).",
               call.target.span, component_path, "await " + name + "(...)");
    }
    std::set<std::string, std::less<>> bound;
    for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
        const Argument& argument = call.arguments[index];
        const ArgumentBinding& binding = resolved.arguments[index];
        const std::string argument_path =
            component_path + "." + argument.name.value_or(std::to_string(index));
        if (binding.parameter == nullptr) {
            report("STRATA.DSL.SCRIPT_UNKNOWN_ARGUMENT",
                   argument.name.has_value()
                       ? "Function '" + name + "' has no parameter '" + *argument.name + "'." +
                             did_you_mean(*argument.name, parameter_names(function.parameters))
                       : "Function '" + name + "' takes " +
                             std::to_string(function.parameters.size()) + " arguments.",
                   argument.span, argument_path, expected_names(parameter_names(function.parameters)));
            check(*argument.value, argument_path);
            continue;
        }
        if (binding.duplicate) {
            report("STRATA.DSL.SCRIPT_DUPLICATE_ARGUMENT",
                   "Parameter '" + binding.parameter->name + "' of '" + name +
                       "' is given more than once.",
                   argument.span, argument_path, binding.parameter->name);
        }
        bound.insert(binding.parameter->name);
        validate_expected(*argument.value, *binding.parameter->type,
                          binding.parameter->nullable, argument_path);
    }
    for (const SchemaParameter& parameter : function.parameters) {
        if (!parameter.required || bound.contains(parameter.name)) continue;
        report("STRATA.DSL.SCRIPT_MISSING_ARGUMENT",
               "Function '" + name + "' requires argument '" + parameter.name + "'.",
               expression.span, component_path,
               parameter.name + ": " + parameter.type->diagnostic_name());
    }
}

} // namespace strata::compiler::semantic
