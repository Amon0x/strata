#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "compiler/semantic/type_check.hpp"

namespace strata::compiler::semantic {

void TypeChecker::helper_arguments(const Expression& expression, const CallExpression& call,
                                   const HelperSchema& helper, const std::string& component_path) {
    const ResolvedCall& resolved = *model_.call(expression);
    std::set<std::string, std::less<>> consumed;
    std::size_t positional_index = 0U;
    for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
        const Argument& argument = call.arguments[index];
        const ArgumentBinding& binding = resolved.arguments[index];
        if (!argument.name.has_value()) {
            if (binding.out_of_order) {
                report("STRATA.DSL.SEMANTIC_POSITIONAL_ARGUMENT_ORDER",
                       "Positional helper arguments must appear before named arguments.",
                       argument.span, component_path, "named argument");
            }
            ++positional_index;
        }
        const SchemaParameter* parameter = binding.parameter;
        if (parameter == nullptr && !binding.vararg) {
            const std::vector<std::string> names = parameter_names(helper.parameters);
            report("STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT",
                   "Helper '" + helper.name + "' does not accept argument '" +
                       argument.name.value_or("#" + std::to_string(positional_index)) + "'." +
                       (argument.name.has_value() ? did_you_mean(*argument.name, names)
                                                  : std::string{}),
                   argument.span, component_path, expected_names(names));
            static_cast<void>(infer(*argument.value, component_path));
            continue;
        }
        if (parameter != nullptr && !consumed.insert(parameter->name).second) {
            report("STRATA.DSL.SEMANTIC_DUPLICATE_ARGUMENT",
                   "Helper argument '" + parameter->name + "' is provided more than once.",
                   argument.span, component_path, "single value for '" + parameter->name + "'");
        }
        const SemanticType& expected =
            parameter != nullptr ? *parameter->type : *helper.vararg_type;
        validate_expected(*argument.value, expected, parameter != nullptr && parameter->nullable,
                          component_path + "." +
                              (parameter != nullptr ? parameter->name
                                                    : argument.name.value_or("arg")));
    }
    for (const SchemaParameter& parameter : helper.parameters) {
        if (parameter.required && !consumed.contains(parameter.name)) {
            report("STRATA.DSL.SEMANTIC_MISSING_ARGUMENT",
                   "Required helper argument '" + parameter.name + "' is missing for '" +
                       helper.name + "'.",
                   call.target.span, component_path,
                   parameter.name + ": " + parameter.type->diagnostic_name());
        }
    }
}

void TypeChecker::action_call(const Expression& expression, const CallExpression& call,
                              const std::string& component_path) {
    if (call.arguments.empty()) {
        report("STRATA.DSL.SEMANTIC_MISSING_ARGUMENT",
               "Required action argument 'name' is missing for 'action'.", call.target.span,
               component_path, "name: string");
        return;
    }
    const ResolvedCall& resolved = *model_.call(expression);
    const Argument& first = call.arguments.front();
    if (first.name.has_value() && *first.name != "name") {
        report("STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT",
               "action(...) first argument must be positional or named 'name'.", first.span,
               component_path, "name");
    }
    const std::string* id = string_literal_value(*first.value);
    if (id == nullptr) {
        report("STRATA.DSL.SEMANTIC_DYNAMIC_ACTION",
               "action(...) requires a static action id string.", first.span, component_path,
               expected_names(registry_.action_names()));
    }
    const ActionSchema* action = resolved.action;
    if (id != nullptr && action == nullptr) {
        report("STRATA.DSL.SEMANTIC_UNKNOWN_ACTION",
               "Action '" + *id + "' is not registered." +
                   did_you_mean(*id, registry_.action_names()),
               first.span, component_path, expected_names(registry_.action_names()));
    }
    std::set<std::string, std::less<>> consumed;
    std::set<std::string, std::less<>> suggested;
    for (std::size_t index = 1U; index < call.arguments.size(); ++index) {
        const Argument& argument = call.arguments[index];
        const ArgumentBinding& binding = resolved.arguments[index];
        if (!argument.name.has_value()) {
            report("STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT", "Action arguments must be named.",
                   argument.span, component_path, "named action argument");
            static_cast<void>(infer(*argument.value, component_path));
            continue;
        }
        const SchemaParameter* schema = binding.parameter;
        const std::string canonical_name = schema != nullptr ? schema->name : *argument.name;
        consumed.insert(canonical_name);
        if (binding.duplicate) {
            report("STRATA.DSL.SEMANTIC_DUPLICATE_ARGUMENT",
                   "Action argument '" + canonical_name + "' is provided more than once.",
                   argument.span, component_path, "single value for '" + canonical_name + "'");
        }
        if (schema == nullptr) {
            if (action != nullptr) {
                const std::vector<std::string> names = parameter_names(action->parameters);
                report("STRATA.DSL.SEMANTIC_UNKNOWN_ACTION_ARGUMENT",
                       "Action '" + *id + "' does not define argument '" + *argument.name +
                           "'." + did_you_mean(*argument.name, names),
                       argument.span, component_path, expected_names(names));
                if (const std::optional<std::string> suggestion =
                        nearest_name(*argument.name, names);
                    suggestion.has_value()) {
                    suggested.insert(*suggestion);
                }
            }
            static_cast<void>(infer(*argument.value, component_path));
            continue;
        }
        validate_expected(*argument.value, *schema->type, schema->nullable,
                          component_path + "." + schema->name);
        if (schema->name == "undoLabel") {
            const std::string* label = string_literal_value(*argument.value);
            if (label == nullptr || label->empty()) {
                report("STRATA.DSL.SEMANTIC_UNDO_LABEL_STATIC",
                       "Undoable state actions require a non-empty string-literal undoLabel.",
                       argument.value->span, component_path + ".undoLabel",
                       "undoLabel: \"user-facing change\"");
            }
        }
    }
    if (action == nullptr) return;
    for (const SchemaParameter& parameter : action->parameters) {
        if (parameter.required && !consumed.contains(parameter.name) &&
            !suggested.contains(parameter.name)) {
            report("STRATA.DSL.SEMANTIC_MISSING_ACTION_ARGUMENT",
                   "Action '" + *id + "' requires argument '" + parameter.name + "'.",
                   call.target.span, component_path,
                   parameter.name + ": " + parameter.type->diagnostic_name());
        }
    }
    // An append takes its value as written, or from the event that runs it.
    if (id != nullptr && *id == "state.listAppend" && !consumed.contains("value") &&
        !consumed.contains("fromEvent") && !suggested.contains("value")) {
        report("STRATA.DSL.SEMANTIC_MISSING_ACTION_ARGUMENT",
               "Action 'state.listAppend' requires argument 'value', or fromEvent: true.",
               call.target.span, component_path, "value: any");
    }
    if (id->starts_with("state.")) state_action(*id, expression, call, component_path);
}

const Expression* TypeChecker::named_argument(const CallExpression& call,
                                              const std::string_view name) {
    for (std::size_t index = 1U; index < call.arguments.size(); ++index) {
        const Argument& argument = call.arguments[index];
        if (argument.name == name && argument.value != nullptr) return argument.value.get();
    }
    return nullptr;
}

SemanticTypePtr TypeChecker::widened(const SemanticTypePtr& type) {
    if (type == nullptr) return type;
    if (type->kind == SemanticTypeKind::string_literal) return simple(SemanticTypeKind::string);
    if ((type->kind == SemanticTypeKind::list || type->kind == SemanticTypeKind::map) &&
        (type->element != nullptr || !type->fields.empty())) {
        auto copy = std::make_shared<SemanticType>(*type);
        copy->element = widened(copy->element);
        for (ObjectField& field : copy->fields) field.type = widened(field.type);
        return copy;
    }
    return type;
}

SemanticTypePtr TypeChecker::state_in_scope(const Expression& action,
                                            const std::string& name) {
    const auto found = analysis_.action_state_targets.find(action.id);
    if (found == analysis_.action_state_targets.end() || found->second == no_symbol)
        return nullptr;
    static_cast<void>(name);
    return oracle_.symbol_type(found->second);
}

void TypeChecker::state_action(const std::string& id, const Expression& expression,
                               const CallExpression& call, const std::string& component_path) {
    const Expression* name_expression = named_argument(call, "name");
    const std::string* name =
        name_expression != nullptr ? string_literal_value(*name_expression) : nullptr;
    if (name == nullptr) return;
    const std::vector<std::string>& states = analysis_.state_names;
    if (!std::ranges::binary_search(states, *name)) {
        unresolved("STRATA.DSL.SEMANTIC_UNKNOWN_STATE", *name, true,
                   "Action '" + id + "' targets state '" + *name +
                       "', which no state declares." + did_you_mean(*name, states),
                   name_expression->span, component_path + ".name", expected_names(states));
        return;
    }
    SemanticTypePtr state = state_in_scope(expression, *name);
    if (state == nullptr) return;
    if (state->kind == SemanticTypeKind::state_binding) state = state->value;
    if (state == nullptr || state->kind == SemanticTypeKind::unknown ||
        state->kind == SemanticTypeKind::any ||
        state->kind == SemanticTypeKind::unsafe_component_parameter) {
        return;
    }
    state = widened(state);
    const auto require_kind = [&](const SemanticTypeKind kind, const std::string_view what) {
        if (state->kind == kind) return true;
        report("STRATA.DSL.SEMANTIC_STATE_ACTION_TYPE",
               "Action '" + id + "' needs " + std::string(what) + " state, but '" + *name +
                   "' holds " + state->diagnostic_name() + ".",
               name_expression->span, component_path + ".name", std::string(what));
        return false;
    };
    const auto check_value = [&](const std::string_view parameter, const SemanticType& type) {
        if (const Expression* value = named_argument(call, parameter); value != nullptr)
            validate_expected(*value, type, false, component_path + "." + std::string(parameter));
    };
    if (id == "state.toggle") {
        static_cast<void>(require_kind(SemanticTypeKind::boolean, "boolean"));
    } else if (id == "state.adjust") {
        static_cast<void>(require_kind(SemanticTypeKind::number, "number"));
    } else if (id == "state.set") {
        check_value("value", *state);
    } else if (id == "state.listAppend" || id == "state.listInsert" ||
               id == "state.listRemoveValue" || id == "state.listToggle") {
        if (require_kind(SemanticTypeKind::list, "list") && state->element != nullptr)
            check_value("value", *state->element);
    } else if (id == "state.listClear" || id == "state.listRemoveAt") {
        static_cast<void>(require_kind(SemanticTypeKind::list, "list"));
    } else if (id == "state.recordSet") {
        if (!require_kind(SemanticTypeKind::map, "record") || state->fields.empty()) return;
        const Expression* field_expression = named_argument(call, "field");
        const std::string* field =
            field_expression != nullptr ? string_literal_value(*field_expression) : nullptr;
        if (field == nullptr) return;
        const ObjectField* declared = state->find_field(*field);
        if (declared == nullptr) {
            std::vector<std::string> names;
            for (const ObjectField& candidate : state->fields) names.push_back(candidate.name);
            report("STRATA.DSL.SEMANTIC_UNKNOWN_FIELD",
                   "Field '" + *field + "' is not part of " + state->diagnostic_name() + "." +
                       did_you_mean(*field, names),
                   field_expression->span, component_path + ".field", expected_names(names));
        } else if (declared->type != nullptr) {
            check_value("value", *declared->type);
        }
    }
}

std::set<std::string, std::less<>> TypeChecker::component_template_arguments(
   const Expression& expression, const CallExpression& call,
   const std::string& template_name, const ComponentInfo& component,
   const SemanticType* owner_contract, const std::string& component_path) {
    const ResolvedCall& resolved = *model_.call(expression);
    std::set<std::string, std::less<>> bound;
    std::size_t positional_index = 0U;
    for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
        const Argument& argument = call.arguments[index];
        const ArgumentBinding& binding = resolved.arguments[index];
        if (!argument.name.has_value()) {
            if (binding.out_of_order) {
                report("STRATA.DSL.SEMANTIC_POSITIONAL_ARGUMENT_ORDER",
                       "Positional arguments must appear before named arguments.",
                       argument.span, component_path, "named argument");
            }
            ++positional_index;
        }
        const SchemaParameter* parameter = binding.parameter;
        if (parameter == nullptr) {
            const std::vector<std::string> names = parameter_names(component.parameters);
            report(argument.name.has_value() ? "STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT"
                                             : "STRATA.DSL.SEMANTIC_TOO_MANY_ARGUMENTS",
                   "Argument '" +
                       argument.name.value_or("#" + std::to_string(positional_index)) +
                       "' is not accepted by component template '" + template_name + "'." +
                       (argument.name.has_value() ? did_you_mean(*argument.name, names)
                                                  : std::string{}),
                   argument.span, component_path, expected_names(names));
            static_cast<void>(infer(*argument.value, component_path));
            continue;
        }
        if (!bound.insert(parameter->name).second) {
            report("STRATA.DSL.SEMANTIC_DUPLICATE_ARGUMENT",
                   "Argument '" + parameter->name + "' is provided more than once.",
                   argument.span, component_path + "." + parameter->name,
                   "single value for '" + parameter->name + "'");
        }
        if (owner_contract != nullptr &&
            owner_contract->find_field(parameter->name) != nullptr) {
            report("STRATA.DSL.SEMANTIC_COMPONENT_TEMPLATE_BOUND_OWNER_PARAMETER",
                   "Component template parameter '" + parameter->name +
                       "' is supplied by the template owner and cannot be pre-bound.",
                   argument.span, component_path + "." + parameter->name,
                   "bind an additional component parameter");
        }
        validate_expected(*argument.value, *parameter->type, parameter->nullable,
                          component_path + "." + parameter->name);
    }
    return bound;
}

void TypeChecker::component_template(const Expression& expression, const SemanticType& expected,
                                     const std::string& component_path) {
    const auto* identifier = std::get_if<IdentifierExpression>(&expression.node);
    const auto* partial = std::get_if<CallExpression>(&expression.node);
    const std::string template_name = identifier != nullptr ? identifier->name
                                      : partial != nullptr ? partial->target.qualified_name()
                                                           : std::string{};
    const Symbol* symbol = identifier != nullptr ? model_.symbol(expression) : nullptr;
    if (template_name.empty() || (symbol != nullptr && reads_value(symbol->kind))) {
        const SemanticTypePtr actual = infer(expression, component_path);
        if (!type_matches(expected, *actual, false))
            type_mismatch(expected, *actual, expression, component_path);
        return;
    }
    const auto component = model_.components.find(template_name);
    if (component == model_.components.end()) {
        unresolved("STRATA.DSL.SEMANTIC_UNKNOWN_COMPONENT_TEMPLATE", template_name, true,
                   "Component template '" + template_name + "' is not declared." +
                       did_you_mean(template_name, analysis_.component_names),
                   expression.span, component_path, expected_names(analysis_.component_names));
        return;
    }
    checked_[expression.id] = true;
    const std::set<std::string, std::less<>> bound =
        partial != nullptr
            ? component_template_arguments(expression, *partial, template_name,
                                           component->second, &expected, component_path)
            : std::set<std::string, std::less<>>{};
    for (const SchemaParameter& parameter : component->second.parameters) {
        if (bound.contains(parameter.name)) continue;
        const ObjectField* provided = expected.find_field(parameter.name);
        if (provided == nullptr) {
            if (parameter.required) {
                report("STRATA.DSL.SEMANTIC_COMPONENT_TEMPLATE_PARAMETER",
                       "Component template '" + template_name + "' requires parameter '" +
                           parameter.name + "', but the template owner does not provide it.",
                       expression.span, component_path + "." + parameter.name,
                       "remove the parameter or give it a default value");
            }
            continue;
        }
        if (provided->nullable && !parameter.nullable) {
            report("STRATA.DSL.SEMANTIC_COMPONENT_TEMPLATE_PARAMETER",
                   "Component template parameter '" + parameter.name +
                       "' must accept null values.",
                   expression.span, component_path + "." + parameter.name,
                   parameter.name + ": " + parameter.type->diagnostic_name() + "?");
            continue;
        }
        if (provided->type->kind != SemanticTypeKind::any &&
            !type_matches(*parameter.type, *provided->type, parameter.nullable)) {
            report("STRATA.DSL.SEMANTIC_COMPONENT_TEMPLATE_PARAMETER",
                   "Component template parameter '" + parameter.name + "' expects " +
                       parameter.type->diagnostic_name() + ", but the owner provides " +
                       provided->type->diagnostic_name() + ".",
                   expression.span, component_path + "." + parameter.name,
                   parameter.name + ": " + provided->type->diagnostic_name());
        }
    }
}

} // namespace strata::compiler::semantic
