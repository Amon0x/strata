#include <algorithm>
#include <cctype>
#include <functional>
#include <initializer_list>
#include <set>
#include <utility>

#include "compiler/semantic/analysis.hpp"

// Pass 3: type check. Every expression is checked once, in the slot it fills: its own problems
// (unknown names and fields, operators, calls) and whether its type fits what the slot expects.
namespace strata::compiler::semantic {
namespace {

enum class BlockContext { component, ui, lazy_ui };

[[nodiscard]] BlockContext nested_context(const BlockContext context) {
    return context == BlockContext::component ? BlockContext::ui : context;
}

/** The enumeration values a type accepts, through any unions it is made of. */
void collect_enumeration_values(const SemanticType& type, std::vector<std::string>& values) {
    if (type.kind == SemanticTypeKind::enumeration)
        values.insert(values.end(), type.values.begin(), type.values.end());
    for (const SemanticTypePtr& option : type.options) {
        if (option != nullptr) collect_enumeration_values(*option, values);
    }
}

/** A likely intended value for a mismatched string literal, or nothing. */
[[nodiscard]] std::string mismatch_hint(const SemanticType& expected, const SemanticType& actual) {
    if (actual.kind != SemanticTypeKind::string_literal) return {};
    if (expected.label == "layout size" && (actual.literal == "fill" || actual.literal == "FILL"))
        return " Use { weight: 1 } to fill the remaining space.";
    std::vector<std::string> values;
    collect_enumeration_values(expected, values);
    return did_you_mean(actual.literal, values);
}

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

/**
 * The declared object type an authored object must satisfy. A union offers several shapes, so the
 * authored keys select it: the one shape that declares every key present. No match means the
 * object fits nothing the type allows; several matches stay unchecked rather than guessed.
 */
void collect_object_options(const SemanticType& expected,
                            std::vector<const SemanticType*>& output) {
    if (expected.kind == SemanticTypeKind::map && !expected.fields.empty()) {
        output.push_back(&expected);
        return;
    }
    if (expected.kind != SemanticTypeKind::union_value) return;
    for (const SemanticTypePtr& option : expected.options) {
        if (option != nullptr) collect_object_options(*option, output);
    }
}

[[nodiscard]] std::vector<std::string> parameter_names(
    const std::vector<SchemaParameter>& parameters
) {
    std::vector<std::string> names;
    names.reserve(parameters.size());
    for (const SchemaParameter& parameter : parameters) names.push_back(parameter.name);
    return names;
}

[[nodiscard]] std::vector<std::string> slot_names(const ComponentInfo& component) {
    std::vector<std::string> names;
    for (const ComponentSlot& slot : component.slots) names.push_back(slot.name);
    return names;
}

class TypeChecker final {
public:
    TypeChecker(Analysis& analysis, TypeOracle& oracle)
        : analysis_(analysis),
          oracle_(oracle),
          registry_(analysis.registry),
          model_(analysis.model),
          checked_(analysis.file.node_id_limit, false) {}

    void run() {
        for (const Declaration& declaration : analysis_.file.declarations) declaration_check(declaration);
    }

private:
    void report(std::string code, std::string message, const SourceSpan& span, std::string path,
                std::string expected) {
        analysis_.diagnostics.report(std::move(code), std::move(message), span, std::move(path),
                                     std::move(expected));
    }

    void declaration_check(const Declaration& declaration) {
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

    void style_property(const Property& property, const std::string& path) {
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

    void block(const Block& block, const std::string& path, const std::size_t maximum_loop_items,
               const BlockContext context) {
        for (const StatementPtr& statement : block.statements)
            this->statement(*statement, path, maximum_loop_items, context);
    }

    void statement(const Statement& statement, const std::string& path,
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

    void for_statement(const ForStatement& loop, const std::string& path,
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

    void widget_defaults(const Property& property, const std::string& path) {
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

    void call(const WidgetCall& call, const std::string& path, const std::size_t maximum_loop_items,
              const BlockContext context) {
        const std::string call_path = path + "/" + call.name;
        const ResolvedWidgetCall& resolved = model_.widget_calls.at(call.id);
        if (resolved.target == WidgetCallTarget::unknown) {
            std::vector<std::string> names = registry_.widget_names();
            names.insert(names.end(), analysis_.component_names.begin(),
                         analysis_.component_names.end());
            report("STRATA.DSL.SEMANTIC_UNKNOWN_WIDGET",
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

    /** Positional, named and variadic helper arguments against the helper's parameters. */
    void helper_arguments(const Expression& expression, const CallExpression& call,
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

    void action_call(const Expression& expression, const CallExpression& call,
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
        if (id->starts_with("state.")) state_action(*id, expression, call, component_path);
    }

    /** An action's argument `name`; the first argument is always the action id. */
    [[nodiscard]] static const Expression* named_argument(const CallExpression& call,
                                                          const std::string_view name) {
        for (std::size_t index = 1U; index < call.arguments.size(); ++index) {
            const Argument& argument = call.arguments[index];
            if (argument.name == name && argument.value != nullptr) return argument.value.get();
        }
        return nullptr;
    }

    /** The type a state can hold, rather than the literal it starts from. */
    [[nodiscard]] static SemanticTypePtr widened(const SemanticTypePtr& type) {
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

    /** The innermost state a state action's target names where the action is written. */
    [[nodiscard]] SemanticTypePtr state_in_scope(const Expression& action,
                                                 const std::string& name) {
        const auto found = analysis_.action_state_targets.find(action.id);
        if (found == analysis_.action_state_targets.end() || found->second == no_symbol)
            return nullptr;
        static_cast<void>(name);
        return oracle_.symbol_type(found->second);
    }

    /**
     * A state action names its target with a string, which the compiler can check: the name must
     * be a declared state, and where that state is in scope its value must fit. Actions in a
     * component may target a state of whatever screen hosts it, so only the name is checked there
     * when the state is not local.
     */
    void state_action(const std::string& id, const Expression& expression,
                      const CallExpression& call, const std::string& component_path) {
        const Expression* name_expression = named_argument(call, "name");
        const std::string* name =
            name_expression != nullptr ? string_literal_value(*name_expression) : nullptr;
        if (name == nullptr) return;
        const std::vector<std::string>& states = analysis_.state_names;
        if (!std::ranges::binary_search(states, *name)) {
            report("STRATA.DSL.SEMANTIC_UNKNOWN_STATE",
                   "Action '" + id + "' targets state '" + *name + "', which no state declares." +
                       did_you_mean(*name, states),
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

    [[nodiscard]] std::set<std::string, std::less<>> component_template_arguments(
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

    void component_template(const Expression& expression, const SemanticType& expected,
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
            report("STRATA.DSL.SEMANTIC_UNKNOWN_COMPONENT_TEMPLATE",
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

    /** Style entries, checked property by property, then their layout kind. */
    void style_entries(const std::vector<LayoutProperty>& entries,
                       const std::vector<const Expression*>& values,
                       const std::string& component_path) {
        for (std::size_t index = 0U; index < entries.size(); ++index) {
            const LayoutProperty& entry = entries[index];
            const SchemaParameter* property = registry_.style_property(entry.name);
            if (property == nullptr) {
                report("STRATA.DSL.SEMANTIC_UNKNOWN_PROPERTY",
                       "Style property '" + entry.name + "' is not supported." +
                           did_you_mean(entry.name, registry_.style_property_names()),
                       entry.span, component_path + "." + entry.name,
                       expected_names(registry_.style_property_names()));
                static_cast<void>(infer(*values[index], component_path));
            } else {
                validate_expected(*values[index], *property->type, property->nullable,
                                  component_path + "." + entry.name);
            }
        }
        check_layout_kind(analysis_, &oracle_, entries, component_path);
    }

    [[nodiscard]] bool validate_material(const Expression& expression,
                                         const std::string& component_path) {
        if (const std::string* id = string_literal_value(expression); id != nullptr) {
            if (!registry_.has_material(*id)) {
                report("STRATA.DSL.SEMANTIC_UNKNOWN_MATERIAL",
                       "Material '" + *id + "' is not a built-in material." +
                           did_you_mean(*id, registry_.material_ids()),
                       expression.span, component_path, expected_names(registry_.material_ids()));
            }
            return true;
        }
        const auto* call = std::get_if<CallExpression>(&expression.node);
        if (call == nullptr || call->target.qualified_name() != "material") return false;
        checked_[expression.id] = true;
        const ResolvedCall& resolved = *model_.call(expression);
        const std::string* id =
            !call->arguments.empty() ? string_literal_value(*call->arguments.front().value) : nullptr;
        if (id == nullptr) {
            report("STRATA.DSL.SEMANTIC_DYNAMIC_MATERIAL",
                   "material(...) requires a static material id string.", expression.span,
                   component_path, expected_names(registry_.material_ids()));
            for (const Argument& argument : call->arguments)
                static_cast<void>(infer(*argument.value, component_path));
            return true;
        }
        const MaterialSchema* material = resolved.material;
        if (material == nullptr) {
            report("STRATA.DSL.SEMANTIC_UNKNOWN_MATERIAL",
                   "Material '" + *id + "' is not a built-in material." +
                       did_you_mean(*id, registry_.material_ids()),
                   call->arguments.front().span, component_path,
                   expected_names(registry_.material_ids()));
        }
        for (std::size_t index = 1U; index < call->arguments.size(); ++index) {
            const Argument& argument = call->arguments[index];
            if (!argument.name.has_value()) {
                report("STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT", "Material parameters must be named.",
                       argument.span, component_path, "named material parameter");
                static_cast<void>(infer(*argument.value, component_path));
                continue;
            }
            if (resolved.arguments[index].duplicate) {
                report("STRATA.DSL.SEMANTIC_DUPLICATE_ARGUMENT",
                       "Material parameter '" + *argument.name + "' is supplied more than once.",
                       argument.span, component_path + "." + *argument.name,
                       "unique material parameter");
            }
            const SchemaParameter* parameter = resolved.arguments[index].parameter;
            if (parameter == nullptr) {
                const std::vector<std::string> names =
                    material != nullptr ? parameter_names(material->parameters)
                                        : std::vector<std::string>{};
                report("STRATA.DSL.SEMANTIC_UNKNOWN_MATERIAL_PARAMETER",
                       "Material '" + *id + "' does not define parameter '" + *argument.name +
                           "'." + did_you_mean(*argument.name, names),
                       argument.span, component_path + "." + *argument.name,
                       expected_names(names));
                static_cast<void>(infer(*argument.value, component_path));
                continue;
            }
            validate_expected(*argument.value, *parameter->type, false,
                              component_path + "." + parameter->name);
        }
        return true;
    }

    [[nodiscard]] bool validate_effect(const Expression& expression,
                                       const std::string& component_path) {
        if (const std::string* name = string_literal_value(expression); name != nullptr) {
            if (registry_.effect(*name) == nullptr) {
                report("STRATA.DSL.SEMANTIC_UNKNOWN_EFFECT",
                       "Effect '" + *name + "' is not declared." +
                           did_you_mean(*name, registry_.effect_names()),
                       expression.span, component_path, expected_names(registry_.effect_names()));
            }
            return true;
        }
        const auto* call = std::get_if<CallExpression>(&expression.node);
        if (call == nullptr || call->target.qualified_name() != "effect") return false;
        checked_[expression.id] = true;
        const ResolvedCall& resolved = *model_.call(expression);
        const std::string* name =
            !call->arguments.empty() ? string_literal_value(*call->arguments.front().value) : nullptr;
        if (name == nullptr) {
            report("STRATA.DSL.SEMANTIC_DYNAMIC_EFFECT",
                   "effect(...) requires a static effect name string.", expression.span,
                   component_path, expected_names(registry_.effect_names()));
        }
        const EffectSchema* effect = resolved.effect;
        if (name != nullptr && effect == nullptr) {
            report("STRATA.DSL.SEMANTIC_UNKNOWN_EFFECT",
                   "Effect '" + *name + "' is not declared." +
                       did_you_mean(*name, registry_.effect_names()),
                   call->arguments.front().span, component_path,
                   expected_names(registry_.effect_names()));
        }
        std::set<std::string, std::less<>> seen;
        for (std::size_t index = 1U; index < call->arguments.size(); ++index) {
            const Argument& argument = call->arguments[index];
            if (!argument.name.has_value()) {
                report("STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT", "Effect parameters must be named.",
                       argument.span, component_path, "named effect parameter");
                static_cast<void>(infer(*argument.value, component_path));
                continue;
            }
            seen.insert(*argument.name);
            if (resolved.arguments[index].duplicate) {
                report("STRATA.DSL.SEMANTIC_DUPLICATE_ARGUMENT",
                       "Effect parameter '" + *argument.name + "' is supplied more than once.",
                       argument.span, component_path, "unique effect parameter");
            }
            if (*argument.name == "backdropSource") {
                const std::string* literal = string_literal_value(*argument.value);
                if (literal == nullptr || (*literal != "CURRENT" && *literal != "SURFACE")) {
                    report("STRATA.DSL.SEMANTIC_TYPE_MISMATCH",
                           "Effect backdropSource must be \"CURRENT\" or \"SURFACE\".",
                           argument.span, component_path + ".backdropSource", "CURRENT, SURFACE");
                } else if (effect != nullptr && effect->input != "BACKDROP") {
                    report("STRATA.DSL.SEMANTIC_TYPE_MISMATCH",
                           "Effect backdropSource is available only to BACKDROP effects.",
                           argument.span, component_path + ".backdropSource", "BACKDROP effect");
                }
                continue;
            }
            if (*argument.name == "refreshRate") {
                const SemanticTypePtr actual =
                    infer(*argument.value, component_path + ".refreshRate");
                const std::string* literal = string_literal_value(*argument.value);
                const std::optional<StaticNumber> number = static_number(*argument.value);
                const bool accepted_type = actual->kind == SemanticTypeKind::number ||
                                           (literal != nullptr && *literal == "UNBOUNDED");
                const bool accepted_number =
                    !number.has_value() || (!number->unit.has_value() && number->value > 0.0);
                if (!accepted_type || !accepted_number) {
                    report("STRATA.DSL.SEMANTIC_TYPE_MISMATCH",
                           "Effect refreshRate must be a positive number or \"UNBOUNDED\".",
                           argument.span, component_path + ".refreshRate", "number, UNBOUNDED");
                }
                continue;
            }
            const SchemaParameter* parameter = resolved.arguments[index].parameter;
            if (parameter == nullptr) {
                if (effect != nullptr) {
                    std::vector<std::string> names;
                    if (effect->input == "BACKDROP") names.push_back("backdropSource");
                    names.push_back("refreshRate");
                    for (const SchemaParameter& candidate : effect->parameters)
                        names.push_back(candidate.name);
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT",
                           "Effect '" + *name + "' does not define parameter '" + *argument.name +
                               "'." + did_you_mean(*argument.name, names),
                           argument.span, component_path, expected_names(names));
                }
                static_cast<void>(infer(*argument.value, component_path));
            } else {
                validate_expected(*argument.value, *parameter->type, parameter->nullable,
                                  component_path + "." + parameter->name);
            }
        }
        if (effect != nullptr) {
            for (const SchemaParameter& parameter : effect->parameters) {
                if (parameter.required && !seen.contains(parameter.name)) {
                    report("STRATA.DSL.SEMANTIC_MISSING_ARGUMENT",
                           "Effect '" + *name + "' requires parameter '" + parameter.name + "'.",
                           expression.span, component_path, parameter.name);
                }
            }
        }
        return true;
    }

    [[nodiscard]] bool validate_style(const Expression& expression, const SemanticType& expected,
                                      const std::string& component_path) {
        if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.node)) {
            const Symbol* symbol = model_.symbol(expression);
            if (symbol != nullptr && reads_value(symbol->kind)) return false;
            if (model_.styles.contains(identifier->name)) return true;
            report("STRATA.DSL.SEMANTIC_UNKNOWN_STYLE",
                   "Style '" + identifier->name + "' is not declared." +
                       did_you_mean(identifier->name, analysis_.style_names),
                   expression.span, component_path, expected_names(analysis_.style_names));
            return true;
        }
        if (const auto* style = std::get_if<MapExpression>(&expression.node)) {
            checked_[expression.id] = true;
            // The parser already reports a key written twice in a map literal.
            std::vector<LayoutProperty> entries;
            std::vector<const Expression*> values;
            for (const MapEntry& entry : style->entries) {
                const std::string* name = static_map_key(entry.key);
                entries.push_back(LayoutProperty{name != nullptr ? *name : std::string{},
                                                 entry.value.get(), entry.span});
                values.push_back(entry.value.get());
            }
            style_entries(entries, values, component_path);
            return true;
        }
        const auto* call = std::get_if<CallExpression>(&expression.node);
        if (call == nullptr || call->target.qualified_name() != "style") return false;
        checked_[expression.id] = true;
        const ResolvedCall& resolved = *model_.call(expression);
        std::vector<LayoutProperty> layout_properties;
        for (std::size_t index = 0U; index < call->arguments.size(); ++index) {
            const Argument& argument = call->arguments[index];
            const ArgumentBinding& binding = resolved.arguments[index];
            if (!argument.name.has_value()) {
                if (binding.out_of_order) {
                    report("STRATA.DSL.SEMANTIC_POSITIONAL_ARGUMENT_ORDER",
                           "Positional style bases must appear before named property overrides.",
                           argument.span, component_path, "named style property");
                }
                validate_expected(*argument.value, expected, false, component_path + ".base");
                continue;
            }
            const std::string& name = *argument.name;
            layout_properties.push_back(LayoutProperty{name, argument.value.get(), argument.span});
            if (binding.duplicate) {
                report("STRATA.DSL.SEMANTIC_DUPLICATE_PROPERTY",
                       "Style property '" + name + "' is provided more than once.", argument.span,
                       component_path + "." + name, "single value for '" + name + "'");
            }
            const SchemaParameter* property = binding.parameter;
            if (property == nullptr) {
                report("STRATA.DSL.SEMANTIC_UNKNOWN_PROPERTY",
                       "Style property '" + name + "' is not supported." +
                           did_you_mean(name, registry_.style_property_names()),
                       argument.span, component_path + "." + name,
                       expected_names(registry_.style_property_names()));
                static_cast<void>(infer(*argument.value, component_path));
            } else {
                validate_expected(*argument.value, *property->type, property->nullable,
                                  component_path + "." + name);
            }
        }
        check_layout_kind(analysis_, &oracle_, layout_properties, component_path);
        return true;
    }

    [[nodiscard]] bool validate_animation(const Expression& expression,
                                          const std::string& component_path) {
        std::optional<std::string> name;
        if (const std::string* literal = string_literal_value(expression); literal != nullptr) {
            name = *literal;
        } else if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.node)) {
            const Symbol* symbol = model_.symbol(expression);
            if (symbol == nullptr || !reads_value(symbol->kind)) name = identifier->name;
        }
        const std::vector<std::string>& animations = analysis_.animation_names;
        if (name.has_value()) {
            if (!std::ranges::binary_search(animations, *name)) {
                report("STRATA.DSL.SEMANTIC_UNKNOWN_ANIMATION",
                       "Animation '" + *name + "' is not declared." + did_you_mean(*name, animations),
                       expression.span, component_path, expected_names(animations));
            }
            return true;
        }
        const auto* call = std::get_if<CallExpression>(&expression.node);
        if (call == nullptr || call->target.qualified_name() != "animation") return false;
        checked_[expression.id] = true;
        if (const HelperSchema* helper = registry_.helper("animation"); helper != nullptr)
            helper_arguments(expression, *call, *helper, component_path);
        const std::string* animation_name =
            !call->arguments.empty() ? string_literal_value(*call->arguments.front().value) : nullptr;
        if (animation_name != nullptr && !std::ranges::binary_search(animations, *animation_name)) {
            report("STRATA.DSL.SEMANTIC_UNKNOWN_ANIMATION",
                   "Animation '" + *animation_name + "' is not declared." +
                       did_you_mean(*animation_name, animations),
                   call->arguments.front().span, component_path, expected_names(animations));
        }
        return true;
    }

    void validate_layout(const MapExpression& layout, const std::string& component_path) {
        std::vector<LayoutProperty> layout_properties;
        for (const MapEntry& entry : layout.entries) {
            const std::string* key = static_map_key(entry.key);
            const std::string name = key != nullptr ? *key : std::string{};
            layout_properties.push_back(LayoutProperty{name, entry.value.get(), entry.span});
            const SchemaParameter* property = registry_.layout_property(name);
            if (property == nullptr) {
                report("STRATA.DSL.SEMANTIC_UNKNOWN_LAYOUT_PROPERTY",
                       "Layout property '" + name + "' is not declared." +
                           did_you_mean(name, registry_.layout_property_names()),
                       entry.span, component_path + "." + name,
                       expected_names(registry_.layout_property_names()));
                static_cast<void>(infer(*entry.value, component_path + "." + name));
            } else {
                validate_expected(*entry.value, *property->type, property->nullable,
                                  component_path + "." + property->name);
            }
        }
        check_layout_kind(analysis_, &oracle_, layout_properties, component_path);
    }

    void validate_expected(const Expression& expression, const SemanticType& expected,
                           const bool nullable, const std::string& component_path) {
        if (const auto* grouping = std::get_if<GroupingExpression>(&expression.node)) {
            checked_[expression.id] = true;
            validate_expected(*grouping->expression, expected, nullable, component_path);
            return;
        }
        if (const auto* conditional = std::get_if<ConditionalExpression>(&expression.node)) {
            checked_[expression.id] = true;
            validate_expected(*conditional->condition, *simple(SemanticTypeKind::boolean), false,
                              component_path + ".condition");
            validate_expected(*conditional->then_expression, expected, nullable,
                              component_path + ".then");
            validate_expected(*conditional->else_expression, expected, nullable,
                              component_path + ".else");
            return;
        }
        if (const auto* binary = std::get_if<BinaryExpression>(&expression.node);
            binary != nullptr && binary->operation == BinaryOperator::coalesce) {
            checked_[expression.id] = true;
            validate_expected(*binary->left, expected, true, component_path + ".left");
            validate_expected(*binary->right, expected, nullable, component_path + ".right");
            return;
        }
        if (const auto* literal = std::get_if<LiteralExpression>(&expression.node);
            literal != nullptr && std::holds_alternative<NullLiteral>(literal->value)) {
            if (!nullable)
                type_mismatch(expected, *simple(SemanticTypeKind::null_value), expression,
                              component_path);
            return;
        }
        if (expected.kind == SemanticTypeKind::action) {
            if (const std::string* id = string_literal_value(expression); id != nullptr) {
                const ActionSchema* action = registry_.action(*id);
                if (action == nullptr) {
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_ACTION",
                           "Action '" + *id + "' is not registered." +
                               did_you_mean(*id, registry_.action_names()),
                           expression.span, component_path,
                           expected_names(registry_.action_names()));
                } else {
                    for (const SchemaParameter& parameter : action->parameters) {
                        if (!parameter.required) continue;
                        report("STRATA.DSL.SEMANTIC_MISSING_ACTION_ARGUMENT",
                               "Action '" + *id + "' requires argument '" + parameter.name +
                                   "'; use action(...) with named arguments.",
                               expression.span, component_path,
                               parameter.name + ": " + parameter.type->diagnostic_name());
                    }
                }
                return;
            }
            if (const auto* call = std::get_if<CallExpression>(&expression.node);
                call != nullptr && call->target.qualified_name() == "action") {
                checked_[expression.id] = true;
                action_call(expression, *call, component_path);
                return;
            }
        }
        if (expected.kind == SemanticTypeKind::component_template) {
            component_template(expression, expected, component_path);
            return;
        }
        if (expected.kind == SemanticTypeKind::material && validate_material(expression, component_path))
            return;
        if (expected.kind == SemanticTypeKind::animation &&
            validate_animation(expression, component_path)) {
            return;
        }
        if (expected.kind == SemanticTypeKind::style &&
            validate_style(expression, expected, component_path)) {
            return;
        }
        if (expected.kind == SemanticTypeKind::effect && validate_effect(expression, component_path))
            return;
        if (expected.kind == SemanticTypeKind::layout) {
            if (const auto* layout = std::get_if<MapExpression>(&expression.node)) {
                checked_[expression.id] = true;
                validate_layout(*layout, component_path);
                return;
            }
        }
        if (const auto* list = std::get_if<ListExpression>(&expression.node);
            list != nullptr && expected.kind == SemanticTypeKind::list &&
            expected.element != nullptr) {
            checked_[expression.id] = true;
            for (std::size_t index = 0U; index < list->elements.size(); ++index) {
                validate_expected(*list->elements[index], *expected.element,
                                  expected.element_nullable,
                                  component_path + "[" + std::to_string(index) + "]");
            }
            if ((expected.minimum_items.has_value() &&
                 list->elements.size() < *expected.minimum_items) ||
                (expected.maximum_items.has_value() &&
                 list->elements.size() > *expected.maximum_items)) {
                report("STRATA.DSL.SEMANTIC_TYPE_MISMATCH",
                       "Expected " + expected.diagnostic_name() + ", but found " +
                           std::to_string(list->elements.size()) + " items.",
                       expression.span, component_path, expected.diagnostic_name());
            }
            return;
        }
        if (const auto* map = std::get_if<MapExpression>(&expression.node); map != nullptr) {
            std::vector<const SemanticType*> options;
            collect_object_options(expected, options);
            if (!options.empty()) {
                std::vector<std::string> keys;
                for (const MapEntry& entry : map->entries) {
                    if (const std::string* key = static_map_key(entry.key)) keys.push_back(*key);
                }
                std::vector<const SemanticType*> matches;
                for (const SemanticType* option : options) {
                    const bool declares_every_key =
                        std::ranges::all_of(keys, [option](const std::string& key) {
                            return option->allow_unknown_fields ||
                                   option->find_field(key) != nullptr;
                        });
                    if (declares_every_key) matches.push_back(option);
                }
                if (matches.size() == 1U) {
                    checked_[expression.id] = true;
                    object_fields(*matches.front(), *map, expression.span, component_path);
                    return;
                }
                if (matches.empty()) {
                    type_mismatch(expected, *infer(expression, component_path), expression,
                                  component_path);
                    return;
                }
            }
        }
        if (expected.kind == SemanticTypeKind::path) {
            if (const std::string* commands = string_literal_value(expression);
                commands != nullptr) {
                if (const std::optional<std::string> error = path_outline_error(*commands)) {
                    report("STRATA.DSL.SEMANTIC_INVALID_PATH", "Path outline is invalid: " + *error,
                           expression.span, component_path,
                           "a path outline of M/L/H/V/Q/C/Z commands");
                }
                return;
            }
        }
        const SemanticTypePtr actual = infer(expression, component_path);
        if (!type_matches(expected, *actual, nullable))
            type_mismatch(expected, *actual, expression, component_path);
    }

    /**
     * Structural validation of a declared object type: every authored entry is checked against its
     * declared field type, unknown entries are rejected unless the type opts into them, and missing
     * required fields are reported at the object itself.
     */
    void object_fields(const SemanticType& expected, const MapExpression& map,
                       const SourceSpan& span, const std::string& component_path) {
        std::set<std::string, std::less<>> seen;
        for (const MapEntry& entry : map.entries) {
            const std::string* key = static_map_key(entry.key);
            const std::string name = key != nullptr ? *key : std::string{};
            const std::string path = component_path + "." + name;
            const ObjectField* field = expected.find_field(name);
            if (field == nullptr) {
                if (expected.allow_unknown_fields && expected.value != nullptr) {
                    validate_expected(*entry.value, *expected.value, expected.value_nullable, path);
                    continue;
                }
                if (expected.allow_unknown_fields) {
                    static_cast<void>(infer(*entry.value, path));
                    continue;
                }
                std::vector<std::string> names;
                names.reserve(expected.fields.size());
                for (const ObjectField& declared : expected.fields) names.push_back(declared.name);
                report("STRATA.DSL.SEMANTIC_UNKNOWN_FIELD",
                       "Field '" + name + "' is not part of " + expected.diagnostic_name() + "." +
                           did_you_mean(name, names),
                       entry.span, path, expected_names(names));
                static_cast<void>(infer(*entry.value, path));
                continue;
            }
            seen.insert(name);
            validate_expected(*entry.value, *field->type, field->nullable, path);
        }
        for (const ObjectField& field : expected.fields) {
            if (!field.required || seen.contains(field.name)) continue;
            report("STRATA.DSL.SEMANTIC_MISSING_FIELD",
                   "Field '" + field.name + "' is required by " + expected.diagnostic_name() + ".",
                   span, component_path + "." + field.name, field.type->diagnostic_name());
        }
    }

    void type_mismatch(const SemanticType& expected, const SemanticType& actual,
                       const Expression& expression, const std::string& component_path) {
        report("STRATA.DSL.SEMANTIC_TYPE_MISMATCH",
               "Expected " + expected.diagnostic_name() + ", but found " + actual.diagnostic_name() +
                   "." + mismatch_hint(expected, actual),
               expression.span, component_path, expected.diagnostic_name());
    }

    /**
     * A name that resolves to nothing would read as a missing host binding at runtime and quietly
     * draw nothing. With the host roots declared it is certainly a mistake; without them it may be
     * an undeclared host root, so it is reported only when it is a near miss of a name in scope.
     */
    void unknown_identifier(const IdentifierExpression& identifier, const Expression& expression,
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
        report("STRATA.DSL.SEMANTIC_UNKNOWN_IDENTIFIER",
               "'" + identifier.name + "' is not a state, parameter, local value or declared host "
               "root." + hint,
               expression.span, component_path, expected_names(names));
    }

    /**
     * Reports a `theme.<name>` operand. A token stays symbolic until the surface resolves its
     * effective theme, after evaluation, so an operator never sees its colour or number.
     */
    [[nodiscard]] bool theme_token_operand(const Expression& expression,
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

    /** Reports an operand the runtime cannot do arithmetic with. */
    void arithmetic(const Expression& expression, const std::string& symbol,
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

    /** Checks an expression on its own and returns its type. */
    [[nodiscard]] SemanticTypePtr infer(const Expression& expression,
                                        const std::string& component_path) {
        check(expression, component_path);
        return oracle_.type_of(expression);
    }

    void check(const Expression& expression, const std::string& component_path) {
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
        }
    }

    void property_access(const Expression& expression, const PropertyAccessExpression& property,
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

    void call_expression(const Expression& expression, const CallExpression& call,
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
        if (resolved.helper->implementation != "map") return;
        const Argument* transform = oracle_.argument_for(expression, "transform");
        const auto* lambda =
            transform != nullptr ? std::get_if<LambdaExpression>(&ungrouped(*transform->value).node)
                                 : nullptr;
        if (lambda != nullptr) check(*lambda->body, component_path + ".transform");
    }

    Analysis& analysis_;
    TypeOracle& oracle_;
    const SchemaRegistry& registry_;
    SemanticModel& model_;
    std::vector<bool> checked_;
};

} // namespace

void check_types(Analysis& analysis, TypeOracle& oracle) {
    TypeChecker(analysis, oracle).run();
}

} // namespace strata::compiler::semantic
