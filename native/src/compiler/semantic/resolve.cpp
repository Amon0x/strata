#include <algorithm>
#include <cctype>
#include <set>
#include <utility>

#include "compiler/semantic/analysis.hpp"

// Pass 2: resolve every name. Identifiers get the symbol they denote under lexical scoping, widget
// and expression calls get their callee and the parameter each argument fills, and `bind:`
// shorthands get the expansion lowering emits. The pass reports nothing; the checking passes read
// what it records.
namespace strata::compiler::semantic {
namespace {

class Resolver final {
public:
    explicit Resolver(Analysis& analysis) : analysis_(analysis), model_(analysis.model) {}

    void run() {
        for (const Declaration& declaration : analysis_.file.declarations) {
            std::visit([&](const auto& node) { declaration_node(declaration, node); },
                       declaration.node);
        }
    }

private:
    struct Binding final {
        std::string_view name;
        SymbolId symbol;
    };

    void declaration_node(const Declaration&, const ScreenDeclaration& screen) {
        if (screen.body != nullptr) block(*screen.body);
    }

    void declaration_node(const Declaration&, const OverlayDeclaration& overlay) {
        if (overlay.body != nullptr) block(*overlay.body);
    }

    void declaration_node(const Declaration&, const ComponentDeclaration& component) {
        const auto found = model_.components.find(component.name);
        const ComponentInfo* info = found != model_.components.end() &&
                                            found->second.declaration == &component
                                        ? &found->second
                                        : nullptr;
        const std::size_t mark = scope_.size();
        for (std::size_t index = 0U; index < component.parameters.size(); ++index) {
            const Parameter& parameter = component.parameters[index];
            if (parameter.default_value != nullptr) expression(*parameter.default_value);
            Symbol symbol;
            symbol.kind = SymbolKind::parameter;
            symbol.name = parameter.name;
            symbol.component = info;
            symbol.parameter_index = index;
            scope_.push_back(Binding{parameter.name, analysis_.add_symbol(std::move(symbol))});
        }
        if (component.body != nullptr) block(*component.body);
        scope_.resize(mark);
    }

    void declaration_node(const Declaration&, const StyleDeclaration& style) {
        for (const Property& property : style.properties) expression(*property.value);
        if (style.body != nullptr) block(*style.body);
    }

    void declaration_node(const Declaration&, const AnimationDeclaration& animation) {
        for (const AnimationEntry& entry : animation.entries) {
            if (const auto* timing = std::get_if<AnimationProperty>(&entry)) {
                expression(*timing->property.value);
            } else {
                for (const Property& property : std::get<AnimationFrame>(entry).properties)
                    expression(*property.value);
            }
        }
    }

    [[nodiscard]] SymbolId local(const std::string_view name) const {
        for (auto binding = scope_.rbegin(); binding != scope_.rend(); ++binding) {
            if (binding->name == name) return binding->symbol;
        }
        return no_symbol;
    }

    [[nodiscard]] SymbolId global(const std::string& name, const SymbolKind kind,
                                  const ComponentInfo* component, std::string source_id) {
        const auto key = std::make_pair(kind, name);
        if (const auto found = globals_.find(key); found != globals_.end()) return found->second;
        Symbol symbol;
        symbol.kind = kind;
        symbol.name = name;
        symbol.component = component;
        symbol.source_id = std::move(source_id);
        const SymbolId id = analysis_.add_symbol(std::move(symbol));
        globals_.emplace(key, id);
        return id;
    }

    /** What `name` denotes here: locals, then host roots, then components, styles, animations. */
    [[nodiscard]] SymbolId lookup(const std::string& name) {
        if (const SymbolId id = local(name); id != no_symbol) return id;
        if (analysis_.registry.host_types().contains(name))
            return global(name, SymbolKind::host, nullptr, {});
        if (const auto component = model_.components.find(name);
            component != model_.components.end()) {
            return global(name, SymbolKind::component, &component->second,
                          component->second.source_id);
        }
        if (const auto style = model_.styles.find(name); style != model_.styles.end())
            return global(name, SymbolKind::style, nullptr, style->second.source_id);
        if (const auto animation = model_.animations.find(name);
            animation != model_.animations.end()) {
            return global(name, SymbolKind::animation, nullptr, animation->second.source_id);
        }
        return global(name, SymbolKind::unresolved, nullptr, {});
    }

    /** Every name an unresolved identifier could have meant, for its suggestion. */
    [[nodiscard]] std::vector<std::string> visible_names() const {
        std::vector<std::string> names;
        for (const Binding& binding : scope_) names.emplace_back(binding.name);
        for (const auto& [name, type] : analysis_.registry.host_types()) {
            static_cast<void>(type);
            names.push_back(name);
        }
        names.insert(names.end(), analysis_.component_names.begin(),
                     analysis_.component_names.end());
        names.insert(names.end(), analysis_.style_names.begin(), analysis_.style_names.end());
        names.insert(names.end(), analysis_.animation_names.begin(),
                     analysis_.animation_names.end());
        return names;
    }

    void block(const Block& body) {
        const std::size_t mark = scope_.size();
        // A derived value is visible to its whole block, before its declaration included.
        for (const StatementPtr& statement : body.statements) {
            const auto* derived = std::get_if<DerivedStatement>(&statement->node);
            if (derived == nullptr) continue;
            const SymbolId id = analysis_.add_symbol(Symbol{
                .kind = SymbolKind::derived,
                .name = derived->name,
                .statement = statement.get(),
            });
            derived_symbols_.emplace(statement.get(), id);
            if (local_in_block(derived->name, mark) == no_symbol)
                scope_.push_back(Binding{derived->name, id});
        }
        for (const StatementPtr& statement : body.statements) this->statement(*statement);
        scope_.resize(mark);
    }

    [[nodiscard]] SymbolId local_in_block(const std::string_view name,
                                          const std::size_t mark) const {
        for (std::size_t index = scope_.size(); index > mark; --index) {
            if (scope_[index - 1U].name == name) return scope_[index - 1U].symbol;
        }
        return no_symbol;
    }

    void statement(const Statement& statement) {
        if (const auto* state = std::get_if<StateStatement>(&statement.node)) {
            if (state->type_reference.has_value()) {
                std::vector<const TypeReference*> unresolved;
                model_.state_declared_types.emplace(
                    statement.id,
                    resolve_type_reference(*state->type_reference, analysis_.registry, &unresolved)
                );
                if (!unresolved.empty())
                    analysis_.unresolved_state_types.emplace(statement.id, std::move(unresolved));
            }
            if (state->initializer != nullptr) expression(*state->initializer);
            const SymbolId id = analysis_.add_symbol(Symbol{
                .kind = SymbolKind::state,
                .name = state->name,
                .statement = &statement,
            });
            scope_.push_back(Binding{state->name, id});
        } else if (const auto* derived = std::get_if<DerivedStatement>(&statement.node)) {
            expression(*derived->expression);
            scope_.push_back(Binding{derived->name, derived_symbols_.at(&statement)});
        } else if (const auto* property = std::get_if<PropertyStatement>(&statement.node)) {
            expression(*property->property.value);
        } else if (const WidgetCall* call = statement_call(statement)) {
            widget_call(*call);
        } else if (const auto* conditional = std::get_if<IfStatement>(&statement.node)) {
            expression(*conditional->condition);
            block(*conditional->then_block);
            if (conditional->else_block != nullptr) block(*conditional->else_block);
        } else if (const auto* when = std::get_if<WhenStatement>(&statement.node)) {
            when_statement(*when);
        } else if (const auto* loop = std::get_if<ForStatement>(&statement.node)) {
            expression(*loop->collection);
            const std::size_t mark = scope_.size();
            scope_.push_back(Binding{
                loop->item_name,
                analysis_.add_symbol(Symbol{
                    .kind = SymbolKind::loop_item,
                    .name = loop->item_name,
                    .statement = &statement,
                }),
            });
            if (loop->index_name.has_value()) {
                scope_.push_back(Binding{
                    *loop->index_name,
                    analysis_.add_symbol(Symbol{
                        .kind = SymbolKind::loop_index,
                        .name = *loop->index_name,
                        .statement = &statement,
                    }),
                });
            }
            if (loop->filter != nullptr) expression(*loop->filter);
            block(*loop->block);
            scope_.resize(mark);
        } else if (const auto* frame = std::get_if<AnimationFrameStatement>(&statement.node)) {
            for (const Property& entry : frame->frame.properties) expression(*entry.value);
        }
    }

    /**
     * `when value.status` branches on an async value; its READY branch reads the loaded value under
     * the same name. The branch gets a narrowed symbol, which typing gives the loaded type when the
     * subject is async and the original type otherwise.
     */
    void when_statement(const WhenStatement& when) {
        expression(*when.subject);
        const auto* status = std::get_if<PropertyAccessExpression>(&when.subject->node);
        const auto* receiver = status != nullptr && status->property_name == "status"
                                   ? std::get_if<IdentifierExpression>(&status->receiver->node)
                                   : nullptr;
        const SymbolId subject = receiver != nullptr ? local_or_host(receiver->name) : no_symbol;
        for (const WhenBranch& branch : when.branches) {
            if (branch.match != nullptr) expression(*branch.match);
            const std::size_t mark = scope_.size();
            const std::string* match =
                branch.match != nullptr ? string_literal_value(*branch.match) : nullptr;
            if (subject != no_symbol && match != nullptr && upper(*match) == "READY") {
                Symbol narrowed = model_.symbols[subject];
                narrowed.narrows = subject;
                scope_.push_back(Binding{receiver->name, analysis_.add_symbol(std::move(narrowed))});
            }
            block(*branch.block);
            scope_.resize(mark);
        }
    }

    [[nodiscard]] SymbolId local_or_host(const std::string& name) {
        if (const SymbolId id = local(name); id != no_symbol) return id;
        if (analysis_.registry.host_types().contains(name))
            return global(name, SymbolKind::host, nullptr, {});
        return no_symbol;
    }

    [[nodiscard]] static std::string upper(std::string value) {
        std::ranges::transform(value, value.begin(), [](const unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
        return value;
    }

    void widget_call(const WidgetCall& call) {
        ResolvedWidgetCall resolved;
        const WidgetSchema* widget = analysis_.registry.widget(call.name);
        const auto component = model_.components.find(call.name);
        const std::vector<SchemaParameter>* parameters = nullptr;
        if (widget != nullptr) {
            resolved.target = WidgetCallTarget::widget;
            resolved.widget = widget;
            resolved.lazy_rows = generates_lazy_rows(*widget);
            parameters = &widget->parameters;
        } else if (component != model_.components.end()) {
            resolved.target = WidgetCallTarget::component;
            resolved.component = &component->second;
            parameters = &component->second.parameters;
        }
        for (const Argument& argument : call.arguments) expression(*argument.value);
        if (parameters != nullptr) {
            resolved.arguments = bind_arguments(call.arguments, *parameters);
            if (widget != nullptr) resolved.bindings = expansions(call, *widget, resolved);
        }
        model_.widget_calls.emplace(call.id, std::move(resolved));
        if (call.body != nullptr) block(*call.body);
    }

    /** Named arguments by name or alias, positional ones in declaration order. */
    [[nodiscard]] static std::vector<ArgumentBinding> bind_arguments(
        const std::vector<Argument>& arguments,
        const std::vector<SchemaParameter>& parameters
    ) {
        std::vector<ArgumentBinding> bindings;
        bindings.reserve(arguments.size());
        std::set<std::string, std::less<>> consumed;
        std::size_t positional_index = 0U;
        bool seen_named = false;
        for (const Argument& argument : arguments) {
            ArgumentBinding binding;
            if (argument.name.has_value()) {
                seen_named = true;
                const auto found =
                    std::ranges::find_if(parameters, [&argument](const SchemaParameter& candidate) {
                        return candidate.accepts_name(*argument.name);
                    });
                if (found != parameters.end()) binding.parameter = &*found;
            } else {
                binding.out_of_order = seen_named;
                if (positional_index < parameters.size())
                    binding.parameter = &parameters[positional_index];
                ++positional_index;
            }
            if (binding.parameter != nullptr)
                binding.duplicate = !consumed.insert(binding.parameter->name).second;
            bindings.push_back(binding);
        }
        return bindings;
    }

    /** The shorthands lowering expands: unconflicted ones naming a state or Binding parameter. */
    [[nodiscard]] std::vector<BindingExpansion> expansions(
        const WidgetCall& call,
        const WidgetSchema& widget,
        const ResolvedWidgetCall& resolved
    ) const {
        std::set<std::string, std::less<>> supplied;
        std::optional<std::size_t> undo_label;
        std::optional<std::size_t> undo_coalesce;
        for (std::size_t index = 0U; index < resolved.arguments.size(); ++index) {
            const SchemaParameter* parameter = resolved.arguments[index].parameter;
            if (parameter == nullptr) continue;
            supplied.insert(parameter->name);
            if (parameter->name == "undoLabel" && !undo_label.has_value()) undo_label = index;
            if (parameter->name == "undoCoalesce" && !undo_coalesce.has_value())
                undo_coalesce = index;
        }
        std::vector<BindingExpansion> result;
        for (const WidgetBindingSchema& binding : widget.bindings) {
            const auto shorthand = std::ranges::find_if(
                resolved.arguments, [&binding](const ArgumentBinding& argument) {
                    return argument.parameter != nullptr &&
                           argument.parameter->name == binding.shorthand_parameter;
                });
            if (shorthand == resolved.arguments.end() ||
                supplied.contains(binding.value_parameter) ||
                supplied.contains(binding.event_parameter)) {
                continue;
            }
            const auto index =
                static_cast<std::size_t>(std::distance(resolved.arguments.begin(), shorthand));
            const SymbolId target = binding_target(model_, *call.arguments[index].value);
            const SchemaParameter* controlled = widget.find_parameter(binding.value_parameter);
            if (target == no_symbol || controlled == nullptr) continue;
            result.push_back(BindingExpansion{
                &binding, controlled, index, target, undo_label, undo_coalesce,
            });
        }
        return result;
    }

    void expression(const Expression& expression) {
        std::visit([&](const auto& node) { expression_node(expression, node); }, expression.node);
    }

    void expression_node(const Expression&, const LiteralExpression&) {}
    void expression_node(const Expression&, const ErrorExpression&) {}

    void expression_node(const Expression& expression, const IdentifierExpression& identifier) {
        const SymbolId id = lookup(identifier.name);
        analysis_.facts(expression).symbol = id;
        if (model_.symbols[id].kind == SymbolKind::unresolved)
            analysis_.visible_names.emplace(expression.id, visible_names());
    }

    void expression_node(const Expression&, const GroupingExpression& grouping) {
        expression(*grouping.expression);
    }

    void expression_node(const Expression&, const ListExpression& list) {
        for (const ExpressionPtr& element : list.elements) expression(*element);
    }

    void expression_node(const Expression&, const MapExpression& map) {
        for (const MapEntry& entry : map.entries) expression(*entry.value);
    }

    void expression_node(const Expression&, const UnaryExpression& unary) {
        expression(*unary.operand);
    }

    void expression_node(const Expression&, const BinaryExpression& binary) {
        expression(*binary.left);
        expression(*binary.right);
    }

    void expression_node(const Expression&, const ConditionalExpression& conditional) {
        expression(*conditional.condition);
        expression(*conditional.then_expression);
        expression(*conditional.else_expression);
    }

    void expression_node(const Expression&, const PropertyAccessExpression& property) {
        expression(*property.receiver);
    }

    void expression_node(const Expression&, const IndexExpression& indexed) {
        expression(*indexed.receiver);
        expression(*indexed.index);
    }

    void expression_node(const Expression& expression, const LambdaExpression& lambda) {
        const std::size_t mark = scope_.size();
        scope_.push_back(Binding{
            lambda.parameter_name,
            analysis_.add_symbol(Symbol{
                .kind = SymbolKind::lambda_parameter,
                .name = lambda.parameter_name,
                .lambda = &expression,
            }),
        });
        this->expression(*lambda.body);
        scope_.resize(mark);
    }

    void expression_node(const Expression& expression, const CallExpression& call) {
        for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
            const Expression& value = *call.arguments[index].value;
            this->expression(value);
            if (const Expression& bare = ungrouped(value);
                std::holds_alternative<LambdaExpression>(bare.node)) {
                analysis_.lambda_arguments.insert_or_assign(bare.id,
                                                            std::make_pair(&expression, index));
            }
        }
        analysis_.facts(expression).call = static_cast<std::uint32_t>(model_.calls.size());
        model_.calls.push_back(resolve_call(call));
        // A state action names its target by string; record what that name means here.
        const ResolvedCall& resolved = model_.calls.back();
        const std::string* id = !call.arguments.empty()
                                    ? string_literal_value(*call.arguments.front().value)
                                    : nullptr;
        if (resolved.kind != CallKind::action || id == nullptr || !id->starts_with("state."))
            return;
        for (std::size_t index = 1U; index < call.arguments.size(); ++index) {
            const Argument& argument = call.arguments[index];
            if (argument.name != "name") continue;
            if (const std::string* target = string_literal_value(*argument.value))
                analysis_.action_state_targets.emplace(expression.id, local_or_host(*target));
            break;
        }
    }

    [[nodiscard]] ResolvedCall resolve_call(const CallExpression& call) const {
        const SchemaRegistry& registry = analysis_.registry;
        const std::string name = call.target.qualified_name();
        ResolvedCall resolved;
        const std::string* id = !call.arguments.empty()
                                    ? string_literal_value(*call.arguments.front().value)
                                    : nullptr;
        if (name == "action") {
            resolved.kind = CallKind::action;
            resolved.action = id != nullptr ? registry.action(*id) : nullptr;
            resolved.arguments = identified_arguments(call, [&](const std::string& argument) {
                return resolved.action != nullptr ? find(resolved.action->parameters, argument)
                                                  : nullptr;
            }, true);
        } else if (name == "material") {
            resolved.kind = CallKind::material;
            resolved.material = id != nullptr ? registry.material(*id) : nullptr;
            resolved.arguments = identified_arguments(call, [&](const std::string& argument) {
                return resolved.material != nullptr ? resolved.material->find_parameter(argument)
                                                    : nullptr;
            }, false);
        } else if (name == "effect") {
            resolved.kind = CallKind::effect;
            resolved.effect = id != nullptr ? registry.effect(*id) : nullptr;
            resolved.arguments = identified_arguments(call, [&](const std::string& argument) {
                return resolved.effect != nullptr ? resolved.effect->find_parameter(argument)
                                                  : nullptr;
            }, false);
        } else if (name == "style") {
            resolved.kind = CallKind::style;
            std::set<std::string, std::less<>> seen;
            bool seen_named = false;
            for (const Argument& argument : call.arguments) {
                ArgumentBinding binding;
                if (!argument.name.has_value()) {
                    binding.out_of_order = seen_named;
                } else {
                    seen_named = true;
                    binding.parameter = registry.style_property(*argument.name);
                    binding.duplicate = !seen.insert(*argument.name).second;
                }
                resolved.arguments.push_back(binding);
            }
        } else if (const auto component = model_.components.find(name);
                   name != "animation" && component != model_.components.end()) {
            resolved.kind = CallKind::component_template;
            resolved.component = &component->second;
            resolved.arguments = bind_arguments(call.arguments, component->second.parameters);
        } else if (const HelperSchema* helper = registry.helper(name);
                   helper != nullptr || name == "animation") {
            resolved.kind = CallKind::helper;
            resolved.helper = helper;
            if (helper != nullptr) {
                resolved.arguments = bind_arguments(call.arguments, helper->parameters);
                for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
                    ArgumentBinding& binding = resolved.arguments[index];
                    binding.vararg = binding.parameter == nullptr &&
                                     helper->vararg_type != nullptr &&
                                     (!call.arguments[index].name.has_value() ||
                                      helper->allow_named_varargs);
                }
            }
        } else {
            resolved.kind = CallKind::unknown_helper;
        }
        if (resolved.arguments.size() != call.arguments.size())
            resolved.arguments.resize(call.arguments.size());
        return resolved;
    }

    [[nodiscard]] static const SchemaParameter* find(const std::vector<SchemaParameter>& parameters,
                                                     const std::string& name) {
        const auto found = std::ranges::find_if(
            parameters, [&name](const SchemaParameter& candidate) {
                return candidate.accepts_name(name);
            });
        return found != parameters.end() ? &*found : nullptr;
    }

    /**
     * `action(id, ...)`, `material(id, ...)` and `effect(id, ...)` take their id first and named
     * arguments after it. Actions count a repeated argument by its canonical name, materials and
     * effects by the name written.
     */
    template <typename Find>
    [[nodiscard]] static std::vector<ArgumentBinding> identified_arguments(
        const CallExpression& call, const Find& find_parameter, const bool canonical_duplicates
    ) {
        std::vector<ArgumentBinding> bindings(call.arguments.size());
        std::set<std::string, std::less<>> seen;
        for (std::size_t index = 1U; index < call.arguments.size(); ++index) {
            const Argument& argument = call.arguments[index];
            if (!argument.name.has_value()) continue;
            ArgumentBinding& binding = bindings[index];
            binding.parameter = find_parameter(*argument.name);
            const std::string counted = canonical_duplicates && binding.parameter != nullptr
                                            ? binding.parameter->name
                                            : *argument.name;
            binding.duplicate = !seen.insert(counted).second;
        }
        return bindings;
    }

    Analysis& analysis_;
    SemanticModel& model_;
    std::vector<Binding> scope_;
    std::map<std::pair<SymbolKind, std::string>, SymbolId> globals_;
    std::map<const Statement*, SymbolId> derived_symbols_;
};

} // namespace

void resolve_names(Analysis& analysis) {
    Resolver(analysis).run();
}

} // namespace strata::compiler::semantic
