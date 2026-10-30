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
    } else if (const auto* function = std::get_if<FunctionDeclaration>(&declaration.node)) {
        function_check(*function, "function " + function->name);
    } else if (const auto* declared = std::get_if<TypeDeclaration>(&declaration.node)) {
        type_declaration(*declared);
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
    } else if (const auto* function = std::get_if<FunctionStatement>(&statement.node)) {
        function_check(function->function, path + "/" + function->function.name);
    } else if (const auto* declared = std::get_if<TypeStatement>(&statement.node)) {
        type_declaration(declared->declaration);
    }
}

void TypeChecker::type_declaration(const TypeDeclaration& declared) {
    std::vector<const TypeReference*> unresolved;
    static_cast<void>(resolve_type_reference(declared.type, registry_, &unresolved));
    const std::vector<std::string> names = registry_.type_names();
    for (const TypeReference* reference : unresolved) {
        report("STRATA.DSL.SEMANTIC_UNKNOWN_TYPE",
               "Type '" + reference->name + "' is not declared." +
                   did_you_mean(reference->name, names),
               reference->span, "type " + declared.name, expected_names(names));
    }
}

void TypeChecker::function_check(const FunctionDeclaration& function, const std::string& path) {
    const auto info = model_.functions.find(&function);
    for (std::size_t index = 0U; index < function.parameters.size(); ++index) {
        const Parameter& parameter = function.parameters[index];
        if (parameter.type_reference.has_value()) {
            std::vector<const TypeReference*> unresolved;
            static_cast<void>(
                resolve_type_reference(*parameter.type_reference, registry_, &unresolved));
            const std::vector<std::string> names = registry_.type_names();
            for (const TypeReference* reference : unresolved) {
                report("STRATA.DSL.SEMANTIC_UNKNOWN_TYPE",
                       "Type '" + reference->name + "' is not declared." +
                           did_you_mean(reference->name, names),
                       reference->span, path + "(" + parameter.name + ")", expected_names(names));
            }
        }
        if (parameter.default_value == nullptr) continue;
        if (info != model_.functions.end()) {
            validate_expected(*parameter.default_value, *info->second.parameters[index].type,
                              parameter.type_reference.has_value() &&
                                  parameter.type_reference->nullable,
                              path + "(" + parameter.name + ")");
        } else {
            check(*parameter.default_value, path + "(" + parameter.name + ")");
        }
    }
    ++effects_allowed_;
    struct Restore {
        std::size_t& depth;
        ~Restore() { --depth; }
    } restore{effects_allowed_};
    if (function.expression_body != nullptr) {
        if (function.return_type.has_value()) {
            validate_expected(*function.expression_body,
                              *resolve_type_reference(*function.return_type, registry_),
                              function.return_type->nullable, path);
        } else {
            check(*function.expression_body, path);
        }
    }
    if (function.block_body != nullptr) {
        script_block(*function.block_body, path,
                     ScriptContext{false, true, function.asynchronous});
    }
}

void TypeChecker::script_block(const Block& block, const std::string& path,
                               const ScriptContext context) {
    for (const StatementPtr& statement : block.statements)
        script_statement(*statement, path, context);
}

void TypeChecker::script_statement(const Statement& statement, const std::string& path,
                                   const ScriptContext context) {
    if (const auto* let = std::get_if<LetStatement>(&statement.node)) {
        const std::string local = path + "/" + let->name;
        if (let->type_reference.has_value()) {
            if (const auto* awaited = std::get_if<AwaitExpression>(&let->initializer->node)) {
                script_value(*let->initializer, local, context);
                static_cast<void>(awaited);
            } else {
                validate_expected(*let->initializer,
                                  *resolve_type_reference(*let->type_reference, registry_),
                                  let->type_reference->nullable, local);
            }
        } else {
            script_value(*let->initializer, local, context);
        }
    } else if (const auto* assignment = std::get_if<AssignmentStatement>(&statement.node)) {
        assignment_target(*assignment, path);
        script_value(*assignment->value, path, context);
    } else if (const auto* effect = std::get_if<ExpressionStatement>(&statement.node)) {
        script_value(*effect->expression, path, context);
    } else if (const auto* repeat = std::get_if<WhileStatement>(&statement.node)) {
        validate_expected(*repeat->condition, *simple(SemanticTypeKind::boolean), false,
                          path + "/while");
        script_block(*repeat->block, path + "/while", ScriptContext{true, context.in_function,
                                                                    context.may_await});
    } else if (std::holds_alternative<BreakStatement>(statement.node) ||
               std::holds_alternative<ContinueStatement>(statement.node)) {
        if (!context.in_loop) {
            const bool is_break = std::holds_alternative<BreakStatement>(statement.node);
            report("STRATA.DSL.SCRIPT_JUMP_OUTSIDE_LOOP",
                   std::string(is_break ? "'break'" : "'continue'") +
                       " is allowed only inside a while or for loop.",
                   statement.span, path, "a loop");
        }
    } else if (const auto* returned = std::get_if<ReturnStatement>(&statement.node)) {
        if (!context.in_function) {
            report("STRATA.DSL.SCRIPT_RETURN_OUTSIDE_FUNCTION",
                   "'return' is allowed only inside a function or lambda.", statement.span, path,
                   "a function body");
        }
        if (returned->value != nullptr) script_value(*returned->value, path, context);
    } else if (const auto* thrown = std::get_if<ThrowStatement>(&statement.node)) {
        script_value(*thrown->value, path, context);
    } else if (const auto* attempt = std::get_if<TryStatement>(&statement.node)) {
        script_block(*attempt->block, path + "/try", context);
        script_block(*attempt->catch_block, path + "/catch", context);
    } else if (const auto* conditional = std::get_if<IfStatement>(&statement.node)) {
        validate_expected(*conditional->condition, *simple(SemanticTypeKind::boolean), false,
                          path + "/if");
        script_block(*conditional->then_block, path + "/then", context);
        if (conditional->else_block != nullptr)
            script_block(*conditional->else_block, path + "/else", context);
    } else if (const auto* when = std::get_if<WhenStatement>(&statement.node)) {
        const SemanticTypePtr subject = infer(*when->subject, path + "/when");
        for (std::size_t index = 0U; index < when->branches.size(); ++index) {
            const WhenBranch& branch = when->branches[index];
            const std::string branch_path = path + "/when[" + std::to_string(index) + "]";
            if (branch.match != nullptr)
                validate_expected(*branch.match, *subject, false, branch_path);
            script_block(*branch.block, branch_path, context);
        }
    } else if (const auto* loop = std::get_if<ForStatement>(&statement.node)) {
        // A script loop runs once per item however many there are; its step budget bounds it.
        const SemanticTypePtr collection = infer(*loop->collection, path + "/for");
        if (collection->kind != SemanticTypeKind::list &&
            collection->kind != SemanticTypeKind::collection &&
            collection->kind != SemanticTypeKind::any &&
            collection->kind != SemanticTypeKind::unknown &&
            collection->kind != SemanticTypeKind::state_binding) {
            report("STRATA.DSL.SEMANTIC_UNKNOWN_COLLECTION",
                   "Loop collection for '" + loop->item_name + "' is " +
                       collection->diagnostic_name() + ", not a list.",
                   loop->collection->span, path + "/for", "list");
        }
        if (loop->filter != nullptr) {
            validate_expected(*loop->filter, *simple(SemanticTypeKind::boolean), false,
                              path + "/for/filter");
        }
        script_block(*loop->block, path + "/" + loop->item_name,
                     ScriptContext{true, context.in_function, context.may_await});
    } else if (const auto* function = std::get_if<FunctionStatement>(&statement.node)) {
        function_check(function->function, path + "/" + function->function.name);
    }
}

void TypeChecker::script_value(const Expression& expression, const std::string& path,
                               const ScriptContext& context) {
    if (const auto* awaited = std::get_if<AwaitExpression>(&expression.node)) {
        if (context.may_await) {
            permitted_awaits_.insert(expression.id);
            awaited_calls_.insert(ungrouped(*awaited->operand).id);
        } else {
            report("STRATA.DSL.SCRIPT_AWAIT_CONTEXT",
                   "'await' is allowed only in an async function or a handler's block.",
                   expression.span, path, "async fn or handler");
            permitted_awaits_.insert(expression.id);
        }
    }
    static_cast<void>(infer(expression, path));
}

void TypeChecker::assignment_target(const AssignmentStatement& assignment,
                                    const std::string& path) {
    const Expression* root = assignment.target.get();
    bool whole = true;
    while (true) {
        root = &ungrouped(*root);
        if (const auto* property = std::get_if<PropertyAccessExpression>(&root->node)) {
            root = property->receiver.get();
        } else if (const auto* indexed = std::get_if<IndexExpression>(&root->node)) {
            check(*indexed->index, path);
            root = indexed->receiver.get();
        } else {
            break;
        }
        whole = false;
    }
    const auto* identifier = std::get_if<IdentifierExpression>(&root->node);
    if (identifier == nullptr) {
        report("STRATA.DSL.SCRIPT_ASSIGNMENT_TARGET",
               "Only a state, a var, or a field or item of one can be assigned.",
               assignment.target->span, path, "state or var");
        return;
    }
    check(*root, path);
    const Symbol* symbol = model_.symbol(*root);
    if (symbol == nullptr || symbol->kind == SymbolKind::unresolved) return;
    const bool assignable = symbol->kind == SymbolKind::state ||
                            (symbol->kind == SymbolKind::local && symbol->mutable_binding);
    if (!assignable) {
        const std::string why =
            symbol->kind == SymbolKind::local
                ? "it is declared with let; declare it with var to change it"
            : symbol->kind == SymbolKind::derived ? "a derived value follows what it is derived from"
            : symbol->kind == SymbolKind::function ? "it is a function"
                                                   : "parameters and loop items are read-only";
        report("STRATA.DSL.SCRIPT_ASSIGNMENT_TARGET",
               "'" + identifier->name + "' cannot be assigned: " + why + ".",
               assignment.target->span, path, "state or var");
        return;
    }
    // A whole state takes only what its declared type admits.
    if (whole && assignment.operation == AssignmentOperator::assign &&
        symbol->kind == SymbolKind::state &&
        std::get<StateStatement>(symbol->statement->node).type_reference.has_value() &&
        !std::holds_alternative<AwaitExpression>(assignment.value->node)) {
        const auto declared = model_.state_declared_types.find(symbol->statement->id);
        if (declared != model_.state_declared_types.end()) {
            validate_expected(*assignment.value, *declared->second,
                              std::get<StateStatement>(symbol->statement->node)
                                  .type_reference->nullable,
                              path + "/" + identifier->name);
        }
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

void TypeChecker::behavior_attachments(const Expression& behaviors,
                                       const std::string& component_path) {
    const auto* list = std::get_if<ListExpression>(&behaviors.node);
    if (list == nullptr)
        return;
    std::set<std::string, std::less<>> attached;
    for (const ExpressionPtr& element : list->elements) {
        const auto* map = element != nullptr ? std::get_if<MapExpression>(&element->node) : nullptr;
        if (map == nullptr)
            continue;
        for (const MapEntry& entry : map->entries) {
            const auto* name = std::get_if<IdentifierMapKey>(&entry.key);
            const auto* quoted = std::get_if<StringMapKey>(&entry.key);
            if ((name == nullptr || name->name != "id") &&
                (quoted == nullptr || quoted->value != "id"))
                continue;
            const auto* literal = std::get_if<LiteralExpression>(&entry.value->node);
            const auto* id =
                literal != nullptr ? std::get_if<StringLiteral>(&literal->value) : nullptr;
            if (id != nullptr && !attached.insert(id->value).second) {
                report("STRATA.DSL.SEMANTIC_DUPLICATE_BEHAVIOR",
                       "Behavior '" + id->value +
                           "' is attached more than once; a node attaches each behavior once, "
                           "with all its options in one entry.",
                       entry.span, component_path, "one '" + id->value + "' entry");
            }
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
        if (parameter->name == "behaviors")
            behavior_attachments(*argument.value, call_path + "." + parameter->name);
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

bool TypeChecker::effectful(const FunctionDeclaration& function) {
    if (const auto known = effectful_.find(&function); known != effectful_.end())
        return known->second;
    // A function calling itself has whatever effects the rest of it has.
    if (!deciding_effects_.insert(&function).second)
        return false;
    bool result = function.asynchronous;
    if (!result && function.expression_body != nullptr)
        result = expression_effectful(*function.expression_body);
    if (!result && function.block_body != nullptr)
        result = block_effectful(*function.block_body);
    deciding_effects_.erase(&function);
    effectful_.emplace(&function, result);
    return result;
}

bool TypeChecker::block_effectful(const Block& block) {
    for (const StatementPtr& statement : block.statements) {
        const bool effects = std::visit(
            [this](const auto& node) -> bool {
                using Node = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<Node, AssignmentStatement>) {
                    const Expression* root = node.target.get();
                    while (true) {
                        root = &ungrouped(*root);
                        if (const auto* property = std::get_if<PropertyAccessExpression>(&root->node))
                            root = property->receiver.get();
                        else if (const auto* indexed = std::get_if<IndexExpression>(&root->node))
                            root = indexed->receiver.get();
                        else
                            break;
                    }
                    const Symbol* symbol = model_.symbol(*root);
                    return (symbol != nullptr && symbol->kind == SymbolKind::state) ||
                           expression_effectful(*node.value);
                } else if constexpr (std::is_same_v<Node, ExpressionStatement>) {
                    return oracle_.type_of(*node.expression)->kind == SemanticTypeKind::action ||
                           expression_effectful(*node.expression);
                } else if constexpr (std::is_same_v<Node, LetStatement>) {
                    return expression_effectful(*node.initializer);
                } else if constexpr (std::is_same_v<Node, ReturnStatement>) {
                    return node.value != nullptr && expression_effectful(*node.value);
                } else if constexpr (std::is_same_v<Node, ThrowStatement>) {
                    return expression_effectful(*node.value);
                } else if constexpr (std::is_same_v<Node, WhileStatement>) {
                    return expression_effectful(*node.condition) || block_effectful(*node.block);
                } else if constexpr (std::is_same_v<Node, TryStatement>) {
                    return block_effectful(*node.block) || block_effectful(*node.catch_block);
                } else if constexpr (std::is_same_v<Node, IfStatement>) {
                    return expression_effectful(*node.condition) ||
                           block_effectful(*node.then_block) ||
                           (node.else_block != nullptr && block_effectful(*node.else_block));
                } else if constexpr (std::is_same_v<Node, WhenStatement>) {
                    if (expression_effectful(*node.subject)) return true;
                    for (const WhenBranch& branch : node.branches) {
                        if (block_effectful(*branch.block)) return true;
                    }
                    return false;
                } else if constexpr (std::is_same_v<Node, ForStatement>) {
                    return expression_effectful(*node.collection) || block_effectful(*node.block);
                } else {
                    return false;
                }
            },
            statement->node);
        if (effects) return true;
    }
    return false;
}

bool TypeChecker::expression_effectful(const Expression& expression) {
    if (std::holds_alternative<AwaitExpression>(expression.node)) return true;
    // A lambda is a value; what it does happens when it is called.
    if (std::holds_alternative<LambdaExpression>(expression.node)) return false;
    if (std::holds_alternative<CallExpression>(expression.node)) {
        const ResolvedCall* resolved = model_.call(expression);
        if (resolved != nullptr && resolved->kind == CallKind::function) {
            if (resolved->function != nullptr && effectful(*resolved->function->declaration))
                return true;
            if (resolved->function == nullptr &&
                oracle_.symbol_type(resolved->callee)->kind == SemanticTypeKind::action) {
                return true;
            }
        }
    }
    bool found = false;
    for_each_child(expression, [this, &found](const Expression& child) {
        if (!found) found = expression_effectful(child);
    });
    return found;
}

} // namespace strata::compiler::semantic
