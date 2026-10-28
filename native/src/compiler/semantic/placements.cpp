#include <algorithm>
#include <utility>

#include "compiler/semantic/analysis.hpp"

// Pass 5: placement. Decides where each node sits in the portable IR and in runtime component
// paths, how lowering reads each expression in the slot it fills (a style reference, an action, a
// key literal, ...), and the schema each state's slot is declared with. Portable lowering and the
// source map read these facts instead of deciding them again.
namespace strata::compiler::semantic {
namespace {

[[nodiscard]] std::string child(const std::string_view path, const std::string_view segment) {
    return std::string(path) + "/" + std::string(segment);
}

[[nodiscard]] bool accepts_kind(const SemanticType* expected, const SemanticTypeKind kind) {
    if (expected == nullptr) return false;
    if (expected->kind == kind) return true;
    if (expected->kind == SemanticTypeKind::union_value) {
        return std::ranges::any_of(expected->options, [kind](const SemanticTypePtr& option) {
            return accepts_kind(option.get(), kind);
        });
    }
    return false;
}

/** The type a field of an object slot expects; the first union option that names it wins. */
[[nodiscard]] const SemanticType* nested_expected(const SemanticType* expected,
                                                  const std::string_view field) {
    if (expected == nullptr) return nullptr;
    if (expected->kind == SemanticTypeKind::map) {
        if (const ObjectField* object_field = expected->find_field(field); object_field != nullptr)
            return object_field->type.get();
        return expected->value.get();
    }
    if (expected->kind == SemanticTypeKind::union_value) {
        for (const SemanticTypePtr& option : expected->options) {
            if (const SemanticType* nested = nested_expected(option.get(), field); nested != nullptr)
                return nested;
        }
    }
    return nullptr;
}

/** The id a material or action reads: a string literal, or a call's literal first argument. */
[[nodiscard]] const std::string* literal_id(const Expression& expression) {
    if (const auto* call = std::get_if<CallExpression>(&expression.node)) {
        return !call->arguments.empty() ? string_literal_value(*call->arguments.front().value)
                                        : nullptr;
    }
    return string_literal_value(expression);
}

[[nodiscard]] std::optional<std::string> map_key_name(const MapKey& key) {
    if (const std::string* name = static_map_key(key)) return *name;
    return std::nullopt;
}

class Placer final {
public:
    Placer(Analysis& analysis, TypeOracle& oracle)
        : analysis_(analysis), oracle_(oracle), registry_(analysis.registry),
          model_(analysis.model) {}

    void run() {
        model_.declaration_placements.reserve(analysis_.file.declarations.size());
        for (const Declaration& declaration : analysis_.file.declarations) {
            std::visit([&](const auto& node) { declaration_node(node); }, declaration.node);
        }
    }

private:
    void declaration_node(const ScreenDeclaration& screen) {
        const std::string path = "/screen/" + screen.name;
        const std::string runtime = "screen " + screen.name;
        model_.declaration_placements.push_back(Placement{path, runtime});
        block(*screen.body, child(path, "body"), runtime);
    }

    void declaration_node(const OverlayDeclaration& overlay) {
        const std::string path = "/overlay/" + overlay.name;
        const std::string runtime = "overlay " + overlay.name;
        model_.declaration_placements.push_back(Placement{path, runtime});
        block(*overlay.body, child(path, "body"), runtime);
    }

    void declaration_node(const ComponentDeclaration& component) {
        const std::string path = "/component/" + component.name;
        const std::string runtime = "component " + component.name;
        model_.declaration_placements.push_back(Placement{path, runtime});
        const auto found = model_.components.find(component.name);
        const ComponentInfo* info = found != model_.components.end() &&
                                            found->second.declaration == &component
                                        ? &found->second
                                        : nullptr;
        for (std::size_t index = 0U; index < component.parameters.size(); ++index) {
            const Parameter& parameter = component.parameters[index];
            if (parameter.default_value == nullptr) continue;
            expression(*parameter.default_value,
                       child(child(child(path, "parameter"), parameter.name), "default"),
                       runtime + "(" + parameter.name + ")",
                       info != nullptr ? info->parameters[index].type.get() : nullptr);
        }
        if (info != nullptr) {
            const SemanticTypePtr style = simple(SemanticTypeKind::style);
            const SemanticTypePtr variant = simple(SemanticTypeKind::string);
            for (const ComponentWidgetDefault& entry : info->widget_defaults) {
                const std::string defaults = child(child(path, "defaults"), entry.widget);
                const std::string defaults_runtime = runtime + "/defaults/" + entry.widget;
                if (entry.style != nullptr) {
                    expression(*entry.style, child(defaults, "style"), defaults_runtime + ".style",
                               style.get());
                }
                if (entry.variant != nullptr) {
                    expression(*entry.variant, child(defaults, "variant"),
                               defaults_runtime + ".variant", variant.get());
                }
            }
        }
        block(*component.body, child(path, "body"), runtime);
    }

    void declaration_node(const StyleDeclaration& style) {
        const std::string path = "/style/" + style.name;
        const std::string runtime = "style " + style.name;
        model_.declaration_placements.push_back(Placement{path, runtime});
        for (const Property& property : style.properties)
            expression(*property.value, child(path, property.name), runtime + "." + property.name,
                       nullptr);
    }

    void declaration_node(const AnimationDeclaration& animation) {
        // Animations lower from their validated data, so their expressions have no placement.
        model_.declaration_placements.push_back(
            Placement{"/animation/" + animation.name, "animation " + animation.name});
    }

    void place(const NodeId id, std::string path, std::string runtime) {
        model_.placements.at(id) = Placement{std::move(path), std::move(runtime)};
    }

    void block(const Block& block, const std::string& path, const std::string& runtime) {
        place(block.id, path, runtime);
        for (std::size_t index = 0U; index < block.statements.size(); ++index) {
            const Statement& statement = *block.statements[index];
            const std::string statement_path = child(path, std::to_string(index));
            // A state or derived value's runtime path names it; other statements sit in the block.
            std::string statement_runtime = runtime;
            if (const auto* state = std::get_if<StateStatement>(&statement.node)) {
                statement_runtime += "/" + state->name;
            } else if (const auto* derived = std::get_if<DerivedStatement>(&statement.node)) {
                statement_runtime += "/" + derived->name;
            }
            place(statement.id, statement_path, std::move(statement_runtime));
            if (const auto* state = std::get_if<StateStatement>(&statement.node)) {
                if (state->initializer != nullptr) {
                    expression(*state->initializer, child(statement_path, "initializer"),
                               runtime + "/" + state->name, nullptr);
                }
                state_schema(statement, *state);
            } else if (const auto* derived = std::get_if<DerivedStatement>(&statement.node)) {
                expression(*derived->expression, child(statement_path, "expression"),
                           runtime + "/" + derived->name, nullptr);
            } else if (const auto* widget = std::get_if<WidgetStatement>(&statement.node)) {
                call(widget->call, statement_path, runtime + "/" + widget->call.name);
            } else if (const auto* root = std::get_if<RootStatement>(&statement.node)) {
                call(root->call, child(statement_path, "root"),
                     runtime + "/root/" + root->call.name);
            } else if (const auto* conditional = std::get_if<IfStatement>(&statement.node)) {
                expression(*conditional->condition, child(statement_path, "condition"),
                           runtime + "/if", nullptr);
                this->block(*conditional->then_block, child(statement_path, "then"),
                            runtime + "/then");
                if (conditional->else_block != nullptr) {
                    this->block(*conditional->else_block, child(statement_path, "else"),
                                runtime + "/else");
                }
            } else if (const auto* when = std::get_if<WhenStatement>(&statement.node)) {
                expression(*when->subject, child(statement_path, "subject"), runtime + "/when",
                           nullptr);
                for (std::size_t branch_index = 0U; branch_index < when->branches.size();
                     ++branch_index) {
                    const WhenBranch& branch = when->branches[branch_index];
                    const std::string branch_path =
                        child(child(statement_path, "branch"), std::to_string(branch_index));
                    const std::string branch_runtime =
                        runtime + "/when[" + std::to_string(branch_index) + "]";
                    if (branch.match != nullptr) {
                        expression(*branch.match, child(branch_path, "match"), branch_runtime,
                                   nullptr);
                    }
                    this->block(*branch.block, branch_path, branch_runtime);
                }
            } else if (const auto* loop = std::get_if<ForStatement>(&statement.node)) {
                expression(*loop->collection, child(statement_path, "collection"),
                           runtime + "/for", nullptr);
                if (loop->filter != nullptr) {
                    expression(*loop->filter, child(statement_path, "filter"),
                               runtime + "/for/filter", nullptr);
                }
                this->block(*loop->block, child(statement_path, "body"),
                            runtime + "/" + loop->item_name);
            }
        }
    }

    /**
     * A typed state declares its type; an untyped one declares what its literal initializer has
     * the shape of, with strings, lists and records widened to what the state can come to hold.
     */
    void state_schema(const Statement& statement, const StateStatement& state) {
        if (const auto declared = model_.state_declared_types.find(statement.id);
            declared != model_.state_declared_types.end()) {
            model_.state_schemas.emplace(statement.id, declared->second);
            return;
        }
        if (SemanticTypePtr structured = structured_type(state.initializer.get()))
            model_.state_schemas.emplace(statement.id, std::move(structured));
    }

    [[nodiscard]] SemanticTypePtr structured_type(const Expression* expression) const {
        if (expression == nullptr) return nullptr;
        if (const auto* literal = std::get_if<LiteralExpression>(&expression->node)) {
            if (std::holds_alternative<StringLiteral>(literal->value))
                return simple(SemanticTypeKind::string);
            if (std::holds_alternative<NumberLiteral>(literal->value))
                return simple(SemanticTypeKind::number);
            if (std::holds_alternative<ColorLiteral>(literal->value))
                return simple(SemanticTypeKind::color);
            if (std::holds_alternative<BooleanLiteral>(literal->value))
                return simple(SemanticTypeKind::boolean);
            return simple(SemanticTypeKind::null_value);
        }
        if (const auto* list = std::get_if<ListExpression>(&expression->node)) {
            auto type = std::make_shared<SemanticType>();
            type->kind = SemanticTypeKind::list;
            type->element = list->elements.empty() ? simple(SemanticTypeKind::any)
                                                   : structured_type(list->elements.front().get());
            type->maximum_items = maximum_eager_loop_items;
            return type;
        }
        if (const auto* map = std::get_if<MapExpression>(&expression->node)) {
            auto type = std::make_shared<SemanticType>();
            type->kind = SemanticTypeKind::map;
            type->label = "record";
            for (const MapEntry& entry : map->entries) {
                const std::string* key = static_map_key(entry.key);
                if (key == nullptr) {
                    type->allow_unknown_fields = true;
                    type->value = simple(SemanticTypeKind::any);
                    continue;
                }
                SemanticTypePtr field = structured_type(entry.value.get());
                type->fields.push_back(ObjectField{
                    *key,
                    field != nullptr ? std::move(field) : simple(SemanticTypeKind::any),
                    true,
                    false,
                });
            }
            return type;
        }
        if (const auto* call = std::get_if<CallExpression>(&expression->node)) {
            const ResolvedCall* resolved = model_.call(*expression);
            if (resolved != nullptr && resolved->helper != nullptr &&
                resolved->helper->implementation == "persisted") {
                for (std::size_t index = 0U; index < call->arguments.size(); ++index) {
                    const SchemaParameter* parameter = resolved->arguments[index].parameter;
                    if (parameter != nullptr && parameter->name == "initial")
                        return structured_type(call->arguments[index].value.get());
                }
            }
        }
        return nullptr;
    }

    void call(const WidgetCall& call, const std::string& path, const std::string& runtime) {
        place(call.id, path, runtime);
        const ResolvedWidgetCall* resolved = model_.widget_call(call);
        const std::string arguments = child(path, "arguments");
        for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
            const Argument& argument = call.arguments[index];
            const SchemaParameter* parameter =
                resolved != nullptr && index < resolved->arguments.size()
                    ? resolved->arguments[index].parameter
                    : nullptr;
            const BindingExpansion* expansion = nullptr;
            if (resolved != nullptr) {
                const auto found = std::ranges::find(resolved->bindings, index,
                                                     &BindingExpansion::shorthand);
                if (found != resolved->bindings.end()) expansion = &*found;
            }
            if (expansion != nullptr) {
                // The shorthand is read as the controlled value it binds.
                const std::string& shorthand = expansion->binding->shorthand_parameter;
                expression(*argument.value, child(arguments, shorthand), runtime + "." + shorthand,
                           expansion->controlled->type.get());
                continue;
            }
            const std::string name = parameter != nullptr
                                         ? parameter->name
                                         : argument.name.value_or("#" + std::to_string(index));
            expression(*argument.value, child(arguments, name), runtime + "." + name,
                       parameter != nullptr ? parameter->type.get() : nullptr);
        }
        if (call.body != nullptr) block(*call.body, child(path, "children"), runtime);
    }

    void expression(const Expression& expression, const std::string& path,
                    const std::string& runtime, const SemanticType* expected) {
        place(expression.id, path, runtime);
        // Every placed expression gets its type, including ones no check needed to ask about.
        static_cast<void>(oracle_.type_of(expression));
        ExpressionFacts& facts = analysis_.facts(expression);
        if (const auto* property = std::get_if<PropertyAccessExpression>(&expression.node)) {
            if (const auto* receiver = std::get_if<IdentifierExpression>(&property->receiver->node);
                receiver != nullptr && receiver->name == "theme") {
                facts.form = ExpressionForm::theme_token;
                return;
            }
        }
        if (const auto* grouping = std::get_if<GroupingExpression>(&expression.node)) {
            this->expression(*grouping->expression, child(path, "grouped"), runtime, expected);
            return;
        }
        const auto* call = std::get_if<CallExpression>(&expression.node);
        // A material or action slot reads a literal id, or a call whose first argument is one, as
        // a material or action whatever the call is named.
        if (accepts_kind(expected, SemanticTypeKind::material) &&
            literal_id(expression) != nullptr) {
            identify(expression, call, ExpressionForm::material, path, runtime);
            return;
        }
        if (accepts_kind(expected, SemanticTypeKind::action) && literal_id(expression) != nullptr) {
            identify(expression, call, ExpressionForm::action, path, runtime);
            return;
        }
        if (accepts_kind(expected, SemanticTypeKind::animation)) {
            const auto* identifier = std::get_if<IdentifierExpression>(&expression.node);
            const Symbol* symbol = identifier != nullptr ? model_.symbol(expression) : nullptr;
            if (string_literal_value(expression) != nullptr ||
                (identifier != nullptr && (symbol == nullptr || !reads_value(symbol->kind)))) {
                facts.form = ExpressionForm::animation_name;
                return;
            }
        }
        if (accepts_kind(expected, SemanticTypeKind::style) &&
            std::holds_alternative<IdentifierExpression>(expression.node)) {
            const Symbol* symbol = model_.symbol(expression);
            if (symbol == nullptr || !reads_value(symbol->kind)) {
                facts.form = ExpressionForm::style_reference;
                return;
            }
        }
        if (const auto* literal = std::get_if<LiteralExpression>(&expression.node)) {
            if (std::holds_alternative<StringLiteral>(literal->value)) {
                if (accepts_kind(expected, SemanticTypeKind::key))
                    facts.form = ExpressionForm::key_literal;
                else if (accepts_kind(expected, SemanticTypeKind::image))
                    facts.form = ExpressionForm::image_literal;
            }
            return;
        }
        if (std::holds_alternative<IdentifierExpression>(expression.node)) {
            facts.state_binding =
                expected != nullptr && expected->kind == SemanticTypeKind::state_binding;
            return;
        }
        if (const auto* list = std::get_if<ListExpression>(&expression.node)) {
            const SemanticType* element = expected != nullptr ? expected->element.get() : nullptr;
            for (std::size_t index = 0U; index < list->elements.size(); ++index) {
                this->expression(*list->elements[index], child(path, std::to_string(index)),
                                 runtime + "[" + std::to_string(index) + "]", element);
            }
            return;
        }
        if (const auto* map = std::get_if<MapExpression>(&expression.node)) {
            for (std::size_t index = 0U; index < map->entries.size(); ++index) {
                const MapEntry& entry = map->entries[index];
                const std::string key =
                    map_key_name(entry.key).value_or("dynamic_" + std::to_string(index));
                this->expression(*entry.value, child(path, key), runtime + "." + key,
                                 nested_expected(expected, key));
            }
            return;
        }
        if (const auto* unary = std::get_if<UnaryExpression>(&expression.node)) {
            this->expression(*unary->operand, child(path, "operand"), runtime, nullptr);
            return;
        }
        if (const auto* binary = std::get_if<BinaryExpression>(&expression.node)) {
            this->expression(*binary->left, child(path, "left"), runtime + ".left", nullptr);
            this->expression(*binary->right, child(path, "right"), runtime + ".right", nullptr);
            return;
        }
        if (const auto* conditional = std::get_if<ConditionalExpression>(&expression.node)) {
            this->expression(*conditional->condition, child(path, "condition"),
                             runtime + ".condition", nullptr);
            this->expression(*conditional->else_expression, child(path, "else"), runtime + ".else",
                             expected);
            this->expression(*conditional->then_expression, child(path, "then"), runtime + ".then",
                             expected);
            return;
        }
        if (const auto* property = std::get_if<PropertyAccessExpression>(&expression.node)) {
            this->expression(*property->receiver, child(path, "receiver"), runtime + ".receiver",
                             nullptr);
            return;
        }
        if (const auto* indexed = std::get_if<IndexExpression>(&expression.node)) {
            this->expression(*indexed->index, child(path, "index"), runtime + ".index", nullptr);
            this->expression(*indexed->receiver, child(path, "receiver"), runtime + ".receiver",
                             nullptr);
            return;
        }
        if (call != nullptr) {
            const std::string name = call->target.qualified_name();
            if (name == "action" || name == "material") {
                if (literal_id(expression) == nullptr) {
                    // A dynamic id never lowers; its arguments are placed for the source map.
                    static_cast<void>(identified_arguments(
                        *call, path, runtime,
                        [](const std::string&) -> const SchemaParameter* { return nullptr; }));
                } else {
                    identify(expression, call,
                             name == "action" ? ExpressionForm::action : ExpressionForm::material,
                             path, runtime);
                }
                return;
            }
            call_arguments(expression, *call, path, runtime);
            return;
        }
        if (const auto* lambda = std::get_if<LambdaExpression>(&expression.node)) {
            this->expression(*lambda->body, child(path, "body"),
                             runtime + "/" + lambda->parameter_name, nullptr);
        }
    }

    void call_arguments(const Expression& expression, const CallExpression& call,
                        const std::string& path, const std::string& runtime) {
        const ResolvedCall& resolved = *model_.call(expression);
        if (resolved.kind == CallKind::component_template) {
            analysis_.facts(expression).form = ExpressionForm::component_template;
            for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
                const SchemaParameter* parameter = resolved.arguments[index].parameter;
                const std::string name =
                    parameter != nullptr
                        ? parameter->name
                        : call.arguments[index].name.value_or("#" + std::to_string(index));
                this->expression(*call.arguments[index].value, child(path, name),
                                 runtime + "." + name,
                                 parameter != nullptr ? parameter->type.get() : nullptr);
            }
            return;
        }
        // Anything else lowers as a helper call, whose arguments keep the names they were written
        // with and take the helper's parameter types.
        const HelperSchema* helper = registry_.helper(call.target.qualified_name());
        for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
            const Argument& argument = call.arguments[index];
            const SemanticType* expected = nullptr;
            if (helper != nullptr) {
                const SchemaParameter* parameter = nullptr;
                if (resolved.kind == CallKind::helper) {
                    parameter = resolved.arguments[index].parameter;
                } else if (argument.name.has_value()) {
                    const auto found = std::ranges::find_if(
                        helper->parameters, [&argument](const SchemaParameter& candidate) {
                            return candidate.accepts_name(*argument.name);
                        });
                    if (found != helper->parameters.end()) parameter = &*found;
                } else if (index < helper->parameters.size()) {
                    parameter = &helper->parameters[index];
                }
                expected = parameter != nullptr ? parameter->type.get() : helper->vararg_type.get();
            }
            const std::string segment = argument.name.value_or(std::to_string(index));
            this->expression(*argument.value, child(path, segment), runtime + "." + segment,
                             expected);
        }
    }

    /**
     * The named arguments after an action's or material's id, under the parameter each fills.
     * One the id's schema does not define never lowers and keeps the name it was written with.
     */
    template <typename Find>
    [[nodiscard]] std::vector<std::pair<std::size_t, const SchemaParameter*>> identified_arguments(
        const CallExpression& call, const std::string& path, const std::string& runtime,
        const Find& find_parameter) {
        std::vector<std::pair<std::size_t, const SchemaParameter*>> lowered;
        for (std::size_t index = 1U; index < call.arguments.size(); ++index) {
            const Argument& argument = call.arguments[index];
            if (!argument.name.has_value()) continue;
            const SchemaParameter* parameter = find_parameter(*argument.name);
            const std::string& name = parameter != nullptr ? parameter->name : *argument.name;
            expression(*argument.value, child(path, name), runtime + "." + name,
                       parameter != nullptr ? parameter->type.get() : nullptr);
            if (parameter != nullptr) lowered.emplace_back(index, parameter);
        }
        return lowered;
    }

    /** Records an expression lowering reads as an action or material with a literal id. */
    void identify(const Expression& expression, const CallExpression* call,
                  const ExpressionForm form, const std::string& path, const std::string& runtime) {
        analysis_.facts(expression).form = form;
        IdentifiedCall identified;
        identified.id = *literal_id(expression);
        if (form == ExpressionForm::action) {
            identified.action = registry_.action(identified.id);
        } else {
            identified.material = registry_.material(identified.id);
        }
        if (call != nullptr) {
            const ActionSchema* action = identified.action;
            const MaterialSchema* material = identified.material;
            identified.arguments = identified_arguments(
                *call, path, runtime,
                [action, material](const std::string& name) -> const SchemaParameter* {
                    if (action != nullptr) {
                        const auto found = std::ranges::find_if(
                            action->parameters, [&name](const SchemaParameter& candidate) {
                                return candidate.accepts_name(name);
                            });
                        return found != action->parameters.end() ? &*found : nullptr;
                    }
                    return material != nullptr ? material->find_parameter(name) : nullptr;
                });
        }
        model_.identified_calls.insert_or_assign(expression.id, std::move(identified));
    }

    Analysis& analysis_;
    TypeOracle& oracle_;
    const SchemaRegistry& registry_;
    SemanticModel& model_;
};

} // namespace

void assign_placements(Analysis& analysis, TypeOracle& oracle) {
    Placer(analysis, oracle).run();
}

} // namespace strata::compiler::semantic
