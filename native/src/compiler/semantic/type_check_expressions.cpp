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
    }
}

void TypeChecker::lambda_body(const Expression& expression, const LambdaExpression& lambda,
                              const std::string& component_path) {
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
    case CallKind::helper:
        break;
    }
    if (name == "animation") {
        as_kind(SemanticTypeKind::animation);
        return;
    }
    helper_arguments(expression, call, *resolved.helper, component_path);
}

} // namespace strata::compiler::semantic
