#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "compiler/semantic/type_check.hpp"

namespace strata::compiler::semantic {

void TypeChecker::declaration_check(const Declaration& declaration) {
    if (const auto* screen = std::get_if<ScreenDeclaration>(&declaration.node)) {
        block(*screen->body, "screen " + screen->name, maximum_eager_loop_items,
              BlockContext::ui);
    } else if (const auto* overlay = std::get_if<OverlayDeclaration>(&declaration.node)) {
        block(*overlay->body, "overlay " + overlay->name, maximum_eager_loop_items,
              BlockContext::ui);
    } else if (const auto* component = std::get_if<ComponentDeclaration>(&declaration.node)) {
        const ComponentInfo& info = model_.components.at(component->name);
        // A duplicate is already reported; its parameters are not the schema's.
        if (info.declaration != component) return;
        for (std::size_t index = 0U; index < component->parameters.size(); ++index) {
            const Parameter& authored = component->parameters[index];
            const SchemaParameter& parameter = info.parameters[index];
            if (authored.default_value == nullptr) continue;
            const std::string path =
                "component " + component->name + "(" + parameter.name + ")";
            if (parameter.type->kind == SemanticTypeKind::state_binding) {
                report("STRATA.DSL.SEMANTIC_BINDING_TARGET",
                       "Binding component parameter '" + parameter.name +
                           "' cannot declare a default value.",
                       authored.default_value->span, path, "required Binding<T> parameter");
            } else {
                validate_expected(*authored.default_value, *parameter.type, parameter.nullable,
                                  path);
            }
        }
        block(*component->body, "component " + component->name, maximum_eager_loop_items,
              BlockContext::component);
    } else if (const auto* style = std::get_if<StyleDeclaration>(&declaration.node)) {
        const std::string path = "style " + style->name;
        for (const Property& property : style->properties) style_property(property, path);
    } else if (const auto* animation = std::get_if<AnimationDeclaration>(&declaration.node)) {
        check_animation(
            analysis_, *animation, declaration.span,
            [this](const Expression& expression, const SemanticType& expected,
                   const bool nullable, const std::string& path) {
                validate_expected(expression, expected, nullable, path);
            },
            [this](const Expression& expression, const std::string& path) {
                static_cast<void>(infer(expression, path));
            });
    }
}

void TypeChecker::style_property(const Property& property, const std::string& path) {
    const SchemaParameter* schema = registry_.style_property(property.name);
    if (schema == nullptr) {
        report("STRATA.DSL.SEMANTIC_UNKNOWN_PROPERTY",
               "Style property '" + property.name + "' is not supported." +
                   did_you_mean(property.name, registry_.style_property_names()),
               property.span, path, expected_names(registry_.style_property_names()));
        static_cast<void>(infer(*property.value, path + "." + property.name));
        return;
    }
    validate_expected(*property.value, *schema->type, schema->nullable,
                      path + "." + property.name);
}

void TypeChecker::block(const Block& block, const std::string& path,
                        const std::size_t maximum_loop_items, const BlockContext context) {
    for (const StatementPtr& statement : block.statements)
        this->statement(*statement, path, maximum_loop_items, context);
}

void TypeChecker::statement(const Statement& statement, const std::string& path,
                            const std::size_t maximum_loop_items, const BlockContext context) {
    if (const auto* state = std::get_if<StateStatement>(&statement.node)) {
        const std::string state_path = path + "/" + state->name;
        if (const auto unresolved = analysis_.unresolved_state_types.find(statement.id);
            unresolved != analysis_.unresolved_state_types.end()) {
            const std::vector<std::string> names = registry_.type_names();
            for (const TypeReference* reference : unresolved->second) {
                report("STRATA.DSL.SEMANTIC_UNKNOWN_TYPE",
                       "Type '" + reference->name + "' is not declared." +
                           did_you_mean(reference->name, names),
                       reference->span, state_path, expected_names(names));
            }
        }
        if (state->initializer == nullptr) return;
        static_cast<void>(infer(*state->initializer, state_path));
        if (const auto declared = model_.state_declared_types.find(statement.id);
            declared != model_.state_declared_types.end()) {
            validate_expected(*state->initializer, *declared->second,
                              state->type_reference->nullable, state_path);
        }
    } else if (const auto* derived = std::get_if<DerivedStatement>(&statement.node)) {
        static_cast<void>(infer(*derived->expression, path + "/" + derived->name));
    } else if (const auto* property = std::get_if<PropertyStatement>(&statement.node)) {
        if (property->property.name == "defaults") {
            if (context == BlockContext::component) widget_defaults(property->property, path);
        } else {
            style_property(property->property, path);
        }
    } else if (const auto* widget = std::get_if<WidgetStatement>(&statement.node)) {
        call(widget->call, path, maximum_loop_items, context);
    } else if (const auto* root = std::get_if<RootStatement>(&statement.node)) {
        call(root->call, path + "/root", maximum_loop_items, context);
    } else if (const auto* conditional = std::get_if<IfStatement>(&statement.node)) {
        validate_expected(*conditional->condition, *simple(SemanticTypeKind::boolean), false,
                          path + "/if");
        block(*conditional->then_block, path + "/then", maximum_loop_items,
              nested_context(context));
        if (conditional->else_block != nullptr) {
            block(*conditional->else_block, path + "/else", maximum_loop_items,
                  nested_context(context));
        }
    } else if (const auto* when = std::get_if<WhenStatement>(&statement.node)) {
        const SemanticTypePtr subject = infer(*when->subject, path + "/when");
        for (std::size_t index = 0U; index < when->branches.size(); ++index) {
            const WhenBranch& branch = when->branches[index];
            const std::string branch_path = path + "/when[" + std::to_string(index) + "]";
            if (branch.match != nullptr)
                validate_expected(*branch.match, *subject, false, branch_path);
            block(*branch.block, branch_path, maximum_loop_items, nested_context(context));
        }
    } else if (const auto* loop = std::get_if<ForStatement>(&statement.node)) {
        for_statement(*loop, path, maximum_loop_items, context);
    }
}

void TypeChecker::for_statement(const ForStatement& loop, const std::string& path,
                                const std::size_t maximum_loop_items, const BlockContext context) {
    const SemanticTypePtr collection = infer(*loop.collection, path + "/for");
    const bool sequence = collection->kind == SemanticTypeKind::list ||
                          collection->kind == SemanticTypeKind::collection;
    if (!sequence || collection->element == nullptr) {
        report("STRATA.DSL.SEMANTIC_UNKNOWN_COLLECTION",
               "Loop collection for '" + loop.item_name + "' is not a known bounded collection.",
               loop.collection->span, path + "/for", "bounded collection");
    }
    if (sequence) {
        if (!collection->maximum_items.has_value()) {
            report("STRATA.DSL.SEMANTIC_UNBOUNDED_COLLECTION",
                   "Loop collection for '" + loop.item_name +
                       "' does not declare a maximum item count.",
                   loop.collection->span, path + "/for", "collection with a bounded maximum");
        } else if (*collection->maximum_items > maximum_loop_items) {
            report("STRATA.DSL.SEMANTIC_LOOP_BOUND_EXCEEDED",
                   "Loop collection for '" + loop.item_name + "' can contain " +
                       std::to_string(*collection->maximum_items) +
                       " items, exceeding the limit of " + std::to_string(maximum_loop_items) +
                       ".",
                   loop.collection->span, path + "/for",
                   "collection with maxItems <= " + std::to_string(maximum_loop_items));
        }
    }
    if (loop.filter != nullptr) {
        validate_expected(*loop.filter, *simple(SemanticTypeKind::boolean), false,
                          path + "/for/filter");
    }
    block(*loop.block, path + "/" + loop.item_name, maximum_loop_items,
          nested_context(context));
}

void TypeChecker::widget_defaults(const Property& property, const std::string& path) {
    const auto* defaults = std::get_if<MapExpression>(&property.value->node);
    if (defaults == nullptr) {
        report("STRATA.DSL.SEMANTIC_INVALID_WIDGET_DEFAULTS",
               "Component widget defaults must be an object keyed by widget name.",
               property.value->span, path,
               "{ Button: { style: StyleName, variant: \"primary\" } }");
        return;
    }
    std::set<std::string, std::less<>> seen_widgets;
    for (const MapEntry& entry : defaults->entries) {
        const std::string* widget_name = static_map_key(entry.key);
        if (widget_name == nullptr || registry_.widget(*widget_name) == nullptr) {
            report("STRATA.DSL.SEMANTIC_UNKNOWN_WIDGET_DEFAULT",
                   "Widget default '" +
                       (widget_name != nullptr ? *widget_name : std::string("<dynamic>")) +
                       "' does not name a registered widget." +
                       (widget_name != nullptr
                            ? did_you_mean(*widget_name, registry_.widget_names())
                            : std::string{}),
                   map_key_span(entry.key), path, expected_names(registry_.widget_names()));
            static_cast<void>(infer(*entry.value, path + "/defaults"));
            continue;
        }
        if (!seen_widgets.insert(*widget_name).second) {
            report("STRATA.DSL.SEMANTIC_DUPLICATE_WIDGET_DEFAULT",
                   "Widget default '" + *widget_name + "' is declared more than once.",
                   map_key_span(entry.key), path, "one default object per widget");
        }
        const auto* values = std::get_if<MapExpression>(&entry.value->node);
        if (values == nullptr) {
            report("STRATA.DSL.SEMANTIC_INVALID_WIDGET_DEFAULTS",
                   "Widget default '" + *widget_name + "' must be an object.",
                   entry.value->span, path, "style and/or variant properties");
            continue;
        }
        std::set<std::string, std::less<>> seen_properties;
        for (const MapEntry& value : values->entries) {
            const std::string* name = static_map_key(value.key);
            if (name == nullptr || (*name != "style" && *name != "variant")) {
                report("STRATA.DSL.SEMANTIC_UNKNOWN_WIDGET_DEFAULT_PROPERTY",
                       "Widget defaults support only 'style' and 'variant', not '" +
                           (name != nullptr ? *name : std::string("<dynamic>")) + "'.",
                       map_key_span(value.key), path, "style, variant");
                static_cast<void>(infer(*value.value, path + "/defaults/" + *widget_name));
                continue;
            }
            if (!seen_properties.insert(*name).second) {
                report("STRATA.DSL.SEMANTIC_DUPLICATE_PROPERTY",
                       "Widget default property '" + *name + "' is declared more than once.",
                       map_key_span(value.key), path + "/defaults/" + *widget_name,
                       "unique widget default property");
            }
            validate_expected(
                *value.value,
                *simple(*name == "style" ? SemanticTypeKind::style : SemanticTypeKind::string),
                false, path + "/defaults/" + *widget_name + "." + *name);
        }
    }
}

void TypeChecker::call(const WidgetCall& call, const std::string& path,
                       const std::size_t maximum_loop_items, const BlockContext context) {
    const std::string call_path = path + "/" + call.name;
    const ResolvedWidgetCall& resolved = model_.widget_calls.at(call.id);
    if (resolved.target == WidgetCallTarget::unknown) {
        std::vector<std::string> names = registry_.widget_names();
        names.insert(names.end(), analysis_.component_names.begin(),
                     analysis_.component_names.end());
        unresolved("STRATA.DSL.SEMANTIC_UNKNOWN_WIDGET", call.name, true,
                   "Widget or component '" + call.name + "' is not declared." +
                       did_you_mean(call.name, names),
                   call.span, call_path, expected_names(names));
        if (call.body != nullptr)
            block(*call.body, call_path, maximum_loop_items, nested_context(context));
        return;
    }
    const WidgetSchema* widget = resolved.widget;
    const std::vector<SchemaParameter>& parameters =
        widget != nullptr ? widget->parameters : resolved.component->parameters;
    std::set<std::string, std::less<>> consumed;
    // Parameters a misspelled argument was matched to; their "missing" error would only repeat
    // the suggestion.
    std::set<std::string, std::less<>> suggested;
    std::size_t positional_index = 0U;
    for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
        const Argument& argument = call.arguments[index];
        const ArgumentBinding& binding = resolved.arguments[index];
        if (!argument.name.has_value()) ++positional_index;
        if (binding.out_of_order) {
            report("STRATA.DSL.SEMANTIC_POSITIONAL_ARGUMENT_ORDER",
                   "Positional arguments must appear before named arguments.", argument.span,
                   call_path, "named argument");
        }
        if (binding.parameter == nullptr) {
            const std::vector<std::string> names = parameter_names(parameters);
            const std::string display_name =
                argument.name.has_value() ? *argument.name
                                          : "#" + std::to_string(positional_index);
            const std::optional<std::string> suggestion =
                argument.name.has_value() ? nearest_name(*argument.name, names) : std::nullopt;
            if (suggestion.has_value()) suggested.insert(*suggestion);
            report(argument.name.has_value() ? "STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT"
                                             : "STRATA.DSL.SEMANTIC_TOO_MANY_ARGUMENTS",
                   "Argument '" + display_name + "' is not accepted by '" + call.name + "'." +
                       (suggestion.has_value() ? " Did you mean '" + *suggestion + "'?"
                                               : std::string{}),
                   argument.span, call_path, expected_names(names));
            continue;
        }
        consumed.insert(binding.parameter->name);
        if (binding.duplicate) {
            report("STRATA.DSL.SEMANTIC_DUPLICATE_ARGUMENT",
                   "Argument '" + binding.parameter->name + "' is provided more than once.",
                   argument.span, call_path,
                   "single value for '" + binding.parameter->name + "'");
        }
    }
    const std::set<std::string, std::less<>> supplied = consumed;
    for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
        const Argument& argument = call.arguments[index];
        const SchemaParameter* parameter = resolved.arguments[index].parameter;
        if (parameter == nullptr) {
            static_cast<void>(infer(
                *argument.value,
                call_path + "." + argument.name.value_or("#" + std::to_string(index))));
            continue;
        }
        const WidgetBindingSchema* binding =
            widget != nullptr ? widget->find_binding(parameter->name) : nullptr;
        if (binding != nullptr) {
            if (supplied.contains(binding->value_parameter) ||
                supplied.contains(binding->event_parameter)) {
                report("STRATA.DSL.SEMANTIC_BINDING_CONFLICT",
                       "Binding '" + binding->shorthand_parameter +
                           "' cannot be combined with explicit '" + binding->value_parameter +
                           "' or '" + binding->event_parameter + "'.",
                       argument.span, call_path + "." + binding->shorthand_parameter,
                       "binding shorthand or explicit controlled value/callback, not both");
            }
            if (!names_binding_target(model_, *argument.value)) {
                report("STRATA.DSL.SEMANTIC_BINDING_TARGET",
                       "Binding '" + binding->shorthand_parameter +
                           "' must name retained state or a Binding<T> component parameter.",
                       argument.value->span, call_path + "." + binding->shorthand_parameter,
                       binding->shorthand_parameter + ": retainedState");
            }
            if (const SchemaParameter* controlled =
                    widget->find_parameter(binding->value_parameter);
                controlled != nullptr) {
                validate_expected(*argument.value, *controlled->type, controlled->nullable,
                                  call_path + "." + binding->shorthand_parameter);
            }
            consumed.insert(binding->value_parameter);
            consumed.insert(binding->event_parameter);
            continue;
        }
        if (parameter->type->kind == SemanticTypeKind::state_binding) {
            if (!names_binding_target(model_, *argument.value)) {
                report("STRATA.DSL.SEMANTIC_BINDING_TARGET",
                       "Binding component parameter '" + parameter->name +
                           "' must receive retained state directly.",
                       argument.value->span, call_path + "." + parameter->name,
                       parameter->name + ": retainedState");
            }
            if (parameter->type->value != nullptr) {
                validate_expected(*argument.value, *parameter->type->value, parameter->nullable,
                                  call_path + "." + parameter->name);
            }
            continue;
        }
        validate_expected(*argument.value, *parameter->type, parameter->nullable,
                          call_path + "." + parameter->name);
    }
    for (const SchemaParameter& parameter : parameters) {
        if (parameter.required && !consumed.contains(parameter.name) &&
            !suggested.contains(parameter.name)) {
            report("STRATA.DSL.SEMANTIC_MISSING_ARGUMENT",
                   "Required argument '" + parameter.name + "' is missing for '" + call.name +
                       "'.",
                   call.span, call_path,
                   parameter.name + ": " + parameter.type->diagnostic_name());
        }
    }
    if (call.body != nullptr) {
        block(*call.body, call_path,
              resolved.lazy_rows ? maximum_lazy_loop_items : maximum_loop_items,
              resolved.lazy_rows ? BlockContext::lazy_ui : nested_context(context));
    }
}

void check_types(Analysis& analysis, TypeOracle& oracle) {
    TypeChecker(analysis, oracle).run();
}

} // namespace strata::compiler::semantic
