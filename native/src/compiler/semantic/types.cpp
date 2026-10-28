#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

#include "compiler/semantic/analysis.hpp"

namespace strata::compiler::semantic {

OperandClass operand_class(const SemanticType& type) {
    switch (type.kind) {
    case SemanticTypeKind::number:
    case SemanticTypeKind::duration:
        return OperandClass::number;
    case SemanticTypeKind::string:
    case SemanticTypeKind::string_literal:
    case SemanticTypeKind::enumeration:
        return OperandClass::text;
    case SemanticTypeKind::state_binding:
        return type.value != nullptr ? operand_class(*type.value) : OperandClass::undetermined;
    case SemanticTypeKind::null_value:
    case SemanticTypeKind::boolean:
    case SemanticTypeKind::color:
    case SemanticTypeKind::list:
    case SemanticTypeKind::map:
    case SemanticTypeKind::host_object:
    case SemanticTypeKind::collection:
    case SemanticTypeKind::async_value:
    case SemanticTypeKind::action:
    case SemanticTypeKind::lambda:
    case SemanticTypeKind::component_template:
        return OperandClass::other;
    default:
        return OperandClass::undetermined;
    }
}

SemanticTypePtr theme_token_type(const SchemaRegistry& registry, const std::string_view name) {
    return simple(registry.theme_token_kind(name));
}

namespace {

/**
 * A list a state starts from can grow to the retained-list bound at runtime, which is the bound
 * lowering declares for it, rather than staying at the length it was written with.
 */
[[nodiscard]] SemanticTypePtr growable(const SemanticTypePtr& type) {
    if (type == nullptr ||
        (type->kind != SemanticTypeKind::list && type->kind != SemanticTypeKind::map)) {
        return type;
    }
    auto copy = std::make_shared<SemanticType>(*type);
    if (copy->kind == SemanticTypeKind::list) {
        copy->minimum_items.reset();
        copy->maximum_items = maximum_eager_loop_items;
        copy->element = growable(copy->element);
    }
    for (ObjectField& field : copy->fields) field.type = growable(field.type);
    return copy;
}

/** Whether arithmetic over these operands is defined; the runtime fails on anything else. */
[[nodiscard]] const SemanticType* non_numeric_operand(const SemanticType& left,
                                                      const SemanticType& right) {
    for (const SemanticType* operand : {&left, &right}) {
        const OperandClass kind = operand_class(*operand);
        if (kind == OperandClass::text || kind == OperandClass::other) return operand;
    }
    return nullptr;
}

/** The result of numeric arithmetic: a duration when either operand is one. */
[[nodiscard]] SemanticTypePtr arithmetic_type(const SemanticType& left, const SemanticType& right) {
    if (non_numeric_operand(left, right) != nullptr) return simple(SemanticTypeKind::unknown);
    return simple(left.kind == SemanticTypeKind::duration ||
                          right.kind == SemanticTypeKind::duration
                      ? SemanticTypeKind::duration
                      : SemanticTypeKind::number);
}

} // namespace

SemanticTypePtr retained_state_type(
    const StateStatement& state,
    SemanticTypePtr declared,
    const SemanticTypePtr& initializer
) {
    if (!state.type_reference.has_value()) declared = growable(declared);
    // A Record's fields are fixed by its initializer, so reads are checked against them; their
    // values change at runtime, so literal field types widen.
    if (declared->kind == SemanticTypeKind::map && declared->label == "record" &&
        initializer->kind == SemanticTypeKind::map && !initializer->fields.empty()) {
        auto record = std::make_shared<SemanticType>(*initializer);
        record->label = "record '" + state.name + "'";
        for (ObjectField& field : record->fields) {
            if (field.type != nullptr && field.type->kind == SemanticTypeKind::string_literal) {
                field.type = simple(SemanticTypeKind::string);
            }
        }
        declared = std::move(record);
    }
    return declared;
}

TypeOracle::TypeOracle(Analysis& analysis) : analysis_(analysis) {}

SemanticTypePtr TypeOracle::type_of(const Expression& expression) {
    ExpressionFacts& facts = analysis_.facts(expression);
    if (facts.type == nullptr) facts.type = compute(expression);
    return facts.type;
}

SemanticTypePtr TypeOracle::symbol_type(const SymbolId symbol) {
    if (symbol >= symbol_types_.size()) {
        symbol_types_.resize(analysis_.model.symbols.size());
        resolving_.resize(analysis_.model.symbols.size(), false);
    }
    if (symbol_types_[symbol] != nullptr) return symbol_types_[symbol];
    // A derived cycle is reported on its own; its values type as unknown meanwhile.
    if (resolving_[symbol]) return simple(SemanticTypeKind::unknown);
    resolving_[symbol] = true;
    SemanticTypePtr type = compute_symbol(symbol);
    resolving_[symbol] = false;
    symbol_types_[symbol] = type;
    return type;
}

SemanticTypePtr TypeOracle::compute_symbol(const SymbolId id) {
    const Symbol& symbol = analysis_.model.symbols[id];
    if (symbol.narrows != no_symbol) {
        SemanticTypePtr narrowed = symbol_type(symbol.narrows);
        if (narrowed->kind != SemanticTypeKind::async_value) return narrowed;
        auto ready = std::make_shared<SemanticType>(*narrowed);
        ready->kind = SemanticTypeKind::host_object;
        return ready;
    }
    switch (symbol.kind) {
    case SymbolKind::state: {
        const auto& state = std::get<StateStatement>(symbol.statement->node);
        const SemanticTypePtr initializer = state.initializer != nullptr
                                                ? type_of(*state.initializer)
                                                : simple(SemanticTypeKind::unknown);
        SemanticTypePtr declared = initializer;
        if (const auto found = analysis_.model.state_declared_types.find(symbol.statement->id);
            found != analysis_.model.state_declared_types.end()) {
            declared = found->second;
        }
        return retained_state_type(state, std::move(declared), initializer);
    }
    case SymbolKind::derived:
        return type_of(*std::get<DerivedStatement>(symbol.statement->node).expression);
    case SymbolKind::loop_item: {
        const auto& loop = std::get<ForStatement>(symbol.statement->node);
        const SemanticTypePtr collection = type_of(*loop.collection);
        if ((collection->kind == SemanticTypeKind::list ||
             collection->kind == SemanticTypeKind::collection) &&
            collection->element != nullptr) {
            return collection->element;
        }
        return simple(SemanticTypeKind::any);
    }
    case SymbolKind::loop_index:
        return simple(SemanticTypeKind::number);
    case SymbolKind::lambda_parameter: {
        // The helper signature names the argument whose items the lambda is applied to.
        const SchemaParameter* parameter = lambda_parameter_of(*symbol.lambda);
        if (parameter == nullptr || parameter->items_from.empty())
            return simple(SemanticTypeKind::unknown);
        const Argument* source =
            argument_for(*analysis_.lambda_arguments.at(symbol.lambda->id).first,
                         parameter->items_from);
        if (source == nullptr) return simple(SemanticTypeKind::unknown);
        const SemanticTypePtr source_type = type_of(*source->value);
        return source_type->element != nullptr ? source_type->element
                                                : simple(SemanticTypeKind::any);
    }
    case SymbolKind::parameter:
        return symbol.component != nullptr
                   ? symbol.component->parameters[symbol.parameter_index].type
                   : simple(SemanticTypeKind::unknown);
    case SymbolKind::host: {
        const auto& hosts = analysis_.registry.host_types();
        const auto found = hosts.find(symbol.name);
        return found != hosts.end() ? found->second : simple(SemanticTypeKind::unknown);
    }
    case SymbolKind::component:
        return component_template_type(*symbol.component);
    case SymbolKind::style:
        return simple(SemanticTypeKind::style);
    case SymbolKind::animation:
        return simple(SemanticTypeKind::animation);
    case SymbolKind::unresolved:
        break;
    }
    return simple(SemanticTypeKind::unknown);
}

SemanticTypePtr TypeOracle::component_template_type(
    const ComponentInfo& component,
    const std::set<std::string, std::less<>>& bound
) const {
    auto type = std::make_shared<SemanticType>();
    type->kind = SemanticTypeKind::component_template;
    type->fields.reserve(component.parameters.size());
    for (const SchemaParameter& parameter : component.parameters) {
        if (bound.contains(parameter.name)) continue;
        type->fields.push_back(ObjectField{
            parameter.name,
            parameter.type,
            parameter.required,
            parameter.nullable,
        });
    }
    return type;
}

const SchemaParameter* TypeOracle::lambda_parameter_of(const Expression& lambda) const {
    const auto found = analysis_.lambda_arguments.find(lambda.id);
    if (found == analysis_.lambda_arguments.end()) return nullptr;
    const ResolvedCall* resolved = analysis_.model.call(*found->second.first);
    if (resolved == nullptr || resolved->kind != CallKind::helper) return nullptr;
    return resolved->arguments[found->second.second].parameter;
}

const Argument* TypeOracle::argument_for(const Expression& expression,
                                         const std::string_view name) const {
    const auto* call = std::get_if<CallExpression>(&expression.node);
    const ResolvedCall* resolved = analysis_.model.call(expression);
    if (call == nullptr || resolved == nullptr) return nullptr;
    for (std::size_t index = 0U; index < call->arguments.size(); ++index) {
        const SchemaParameter* parameter = resolved->arguments[index].parameter;
        if (parameter != nullptr && parameter->name == name) return &call->arguments[index];
    }
    return nullptr;
}

SemanticTypePtr TypeOracle::compute(const Expression& expression) {
    if (const auto* literal = std::get_if<LiteralExpression>(&expression.node)) {
        if (const auto* string = std::get_if<StringLiteral>(&literal->value))
            return semantic::literal_string(string->value);
        if (const auto* number = std::get_if<NumberLiteral>(&literal->value)) {
            return simple(number->unit.has_value() ? SemanticTypeKind::duration
                                                   : SemanticTypeKind::number);
        }
        if (std::holds_alternative<ColorLiteral>(literal->value))
            return simple(SemanticTypeKind::color);
        if (std::holds_alternative<BooleanLiteral>(literal->value))
            return simple(SemanticTypeKind::boolean);
        return simple(SemanticTypeKind::null_value);
    }
    if (std::holds_alternative<IdentifierExpression>(expression.node)) {
        const SymbolId symbol = analysis_.facts(expression).symbol;
        return symbol != no_symbol ? symbol_type(symbol) : simple(SemanticTypeKind::unknown);
    }
    if (const auto* grouping = std::get_if<GroupingExpression>(&expression.node))
        return type_of(*grouping->expression);
    if (const auto* list = std::get_if<ListExpression>(&expression.node)) {
        auto type = std::make_shared<SemanticType>();
        type->kind = SemanticTypeKind::list;
        type->maximum_items = list->elements.size();
        type->minimum_items = list->elements.size();
        type->element = list->elements.empty() ? simple(SemanticTypeKind::any)
                                               : type_of(*list->elements.front());
        return type;
    }
    if (const auto* map = std::get_if<MapExpression>(&expression.node)) {
        auto type = std::make_shared<SemanticType>();
        type->kind = SemanticTypeKind::map;
        type->label = "object";
        for (const MapEntry& entry : map->entries) {
            const std::string* name = static_map_key(entry.key);
            if (name == nullptr || name->empty()) continue;
            type->fields.push_back(ObjectField{*name, type_of(*entry.value), true, false});
        }
        return type;
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.node)) {
        const SemanticTypePtr operand = type_of(*unary->operand);
        if (unary->operation == UnaryOperator::logical_not) return simple(SemanticTypeKind::boolean);
        return arithmetic_type(*operand, *simple(SemanticTypeKind::number));
    }
    if (const auto* binary = std::get_if<BinaryExpression>(&expression.node)) {
        const SemanticTypePtr left = type_of(*binary->left);
        const SemanticTypePtr right = type_of(*binary->right);
        switch (binary->operation) {
        case BinaryOperator::equal:
        case BinaryOperator::not_equal:
        case BinaryOperator::logical_and:
        case BinaryOperator::logical_or:
        case BinaryOperator::less:
        case BinaryOperator::less_equal:
        case BinaryOperator::greater:
        case BinaryOperator::greater_equal:
            return simple(SemanticTypeKind::boolean);
        case BinaryOperator::coalesce:
            return left;
        case BinaryOperator::add:
            // The runtime joins the operands as text when either one is text.
            if (operand_class(*left) == OperandClass::text ||
                operand_class(*right) == OperandClass::text) {
                return simple(SemanticTypeKind::string);
            }
            if (operand_class(*left) == OperandClass::undetermined ||
                operand_class(*right) == OperandClass::undetermined) {
                return simple(SemanticTypeKind::unknown);
            }
            return arithmetic_type(*left, *right);
        case BinaryOperator::subtract:
        case BinaryOperator::multiply:
        case BinaryOperator::divide:
        case BinaryOperator::modulo:
            return arithmetic_type(*left, *right);
        }
        return simple(SemanticTypeKind::unknown);
    }
    if (const auto* conditional = std::get_if<ConditionalExpression>(&expression.node)) {
        const SemanticTypePtr then_type = type_of(*conditional->then_expression);
        const SemanticTypePtr else_type = type_of(*conditional->else_expression);
        if (then_type->diagnostic_name() == else_type->diagnostic_name()) return then_type;
        // Either branch can be the value, so a use must accept both.
        auto either = std::make_shared<SemanticType>();
        either->kind = SemanticTypeKind::union_value;
        either->label = then_type->diagnostic_name() + " or " + else_type->diagnostic_name();
        either->options = {then_type, else_type};
        return either;
    }
    if (const auto* property = std::get_if<PropertyAccessExpression>(&expression.node)) {
        // `theme.<token>` lowers to a theme token read whatever else is in scope.
        if (const auto* receiver = std::get_if<IdentifierExpression>(&property->receiver->node);
            receiver != nullptr && receiver->name == "theme") {
            return theme_token_type(analysis_.registry, property->property_name);
        }
        const SemanticTypePtr receiver = type_of(*property->receiver);
        if (const ObjectField* field = receiver->find_field(property->property_name);
            field != nullptr) {
            return field->type;
        }
        return simple(SemanticTypeKind::unknown);
    }
    if (const auto* indexed = std::get_if<IndexExpression>(&expression.node)) {
        const SemanticTypePtr receiver = type_of(*indexed->receiver);
        return receiver->element != nullptr ? receiver->element : simple(SemanticTypeKind::unknown);
    }
    if (const auto* call = std::get_if<CallExpression>(&expression.node))
        return compute_call(expression, *call);
    if (std::holds_alternative<LambdaExpression>(expression.node))
        return simple(SemanticTypeKind::lambda);
    return simple(SemanticTypeKind::unknown);
}

SemanticTypePtr TypeOracle::compute_call(const Expression& expression, const CallExpression& call) {
    const ResolvedCall& resolved = *analysis_.model.call(expression);
    switch (resolved.kind) {
    case CallKind::action:
        return simple(SemanticTypeKind::action);
    case CallKind::material:
        return simple(SemanticTypeKind::material);
    case CallKind::effect:
        return simple(SemanticTypeKind::effect);
    case CallKind::style:
        return simple(SemanticTypeKind::style);
    case CallKind::component_template: {
        std::set<std::string, std::less<>> bound;
        for (const ArgumentBinding& binding : resolved.arguments) {
            if (binding.parameter != nullptr) bound.insert(binding.parameter->name);
        }
        return component_template_type(*resolved.component, bound);
    }
    case CallKind::unknown_helper:
        return simple(SemanticTypeKind::unknown);
    case CallKind::helper:
        break;
    }
    if (call.target.qualified_name() == "animation") return simple(SemanticTypeKind::animation);
    const HelperSchema& helper = *resolved.helper;
    const std::string& implementation = helper.implementation;
    if (implementation == "persisted") {
        const Argument* initial = argument_for(expression, "initial");
        return initial != nullptr ? type_of(*initial->value) : helper.return_type;
    }
    const bool preserves_source = implementation == "filter" || implementation == "sort_by" ||
                                  implementation == "distinct_by" ||
                                  implementation == "take_while" || implementation == "window" ||
                                  implementation == "page";
    if (implementation != "map" && !preserves_source) return helper.return_type;
    const Argument* source_argument = argument_for(expression, "source");
    if (source_argument == nullptr) return helper.return_type;
    const SemanticTypePtr source = type_of(*source_argument->value);
    auto projected = std::make_shared<SemanticType>();
    projected->kind = SemanticTypeKind::collection;
    projected->element = source->element != nullptr ? source->element : simple(SemanticTypeKind::any);
    projected->maximum_items = source->maximum_items;
    if (implementation == "map") {
        const Argument* transform = argument_for(expression, "transform");
        const auto* lambda = transform != nullptr
                                 ? std::get_if<LambdaExpression>(&ungrouped(*transform->value).node)
                                 : nullptr;
        if (lambda != nullptr) projected->element = type_of(*lambda->body);
    }
    const std::string_view bound_name = implementation == "window" ? std::string_view("limit")
                                        : implementation == "page" ? std::string_view("size")
                                                                   : std::string_view{};
    if (!bound_name.empty()) {
        const Argument* bound = argument_for(expression, bound_name);
        const std::optional<StaticNumber> number =
            bound != nullptr ? static_number(*bound->value) : std::nullopt;
        if (number.has_value() && !number->unit.has_value() && number->value >= 0.0) {
            const double floored = std::floor(number->value);
            const std::size_t maximum =
                floored >= static_cast<double>(std::numeric_limits<std::size_t>::max())
                    ? std::numeric_limits<std::size_t>::max()
                    : static_cast<std::size_t>(floored);
            projected->maximum_items =
                projected->maximum_items.has_value()
                    ? std::optional<std::size_t>(std::min(*projected->maximum_items, maximum))
                    : std::optional<std::size_t>(maximum);
        }
    }
    return projected;
}

} // namespace strata::compiler::semantic
