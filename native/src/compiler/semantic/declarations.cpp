#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "compiler/semantic/analysis.hpp"

// Pass 1: collect the module's declarations (components with their parameters and slots, styles,
// animations) and check what is wrong with them on their own.
namespace strata::compiler::semantic {
namespace {

class DeclarationCollector final {
public:
    explicit DeclarationCollector(Analysis& analysis) : analysis_(analysis) {}

    void run() {
        std::set<std::pair<std::string, std::string>> declaration_names;
        for (const Declaration& declaration : analysis_.file.declarations) {
            const auto note = [&](std::string kind, const std::string& name) {
                if (!declaration_names.emplace(kind, name).second) {
                    report("STRATA.DSL.SEMANTIC_DUPLICATE_DECLARATION",
                           "The " + kind + " declaration '" + name +
                               "' is declared more than once.",
                           declaration.span, kind + " " + name, "unique " + kind + " name");
                }
            };
            if (const auto* component = std::get_if<ComponentDeclaration>(&declaration.node)) {
                note("component", component->name);
                collect_component(declaration, *component);
            } else if (const auto* style = std::get_if<StyleDeclaration>(&declaration.node)) {
                note("style", style->name);
                analysis_.style_names.push_back(style->name);
                analysis_.style_declarations.try_emplace(style->name, style);
                analysis_.model.styles.try_emplace(
                    style->name, NamedDeclaration{&declaration, declaration.span.source_id}
                );
            } else if (const auto* animation =
                           std::get_if<AnimationDeclaration>(&declaration.node)) {
                note("animation", animation->name);
                analysis_.animation_names.push_back(animation->name);
                analysis_.model.animations.try_emplace(
                    animation->name, NamedDeclaration{&declaration, declaration.span.source_id}
                );
            } else if (const auto* screen = std::get_if<ScreenDeclaration>(&declaration.node)) {
                note("screen", screen->name);
            } else if (const auto* overlay = std::get_if<OverlayDeclaration>(&declaration.node)) {
                note("overlay", overlay->name);
            }
        }
        std::ranges::sort(analysis_.style_names);
        std::ranges::sort(analysis_.animation_names);
        for (const auto& [name, component] : analysis_.model.components) {
            static_cast<void>(component);
            analysis_.component_names.push_back(name);
        }
        collect_state_names();
        check_style_composition();
    }

private:
    void report(std::string code, std::string message, const SourceSpan& span,
                std::string path, std::string expected) {
        analysis_.diagnostics.report(std::move(code), std::move(message), span, std::move(path),
                                     std::move(expected));
    }

    void collect_component(const Declaration& declaration, const ComponentDeclaration& component) {
        const SchemaRegistry& registry = analysis_.registry;
        // A call resolves to the built-in first, so a same-named component is unreachable.
        if (registry.widget(component.name) != nullptr) {
            report("STRATA.DSL.SEMANTIC_DUPLICATE_DECLARATION",
                   "Component '" + component.name +
                       "' has the name of a built-in widget, which calls would use "
                       "instead; give the component its own name.",
                   declaration.span, "component " + component.name,
                   "component name distinct from built-in widgets");
        }
        ComponentInfo info;
        info.declaration = &component;
        info.source_id = declaration.span.source_id;
        std::set<std::string, std::less<>> parameter_names;
        for (const Parameter& parameter : component.parameters) {
            if (!parameter_names.insert(parameter.name).second) {
                report("STRATA.DSL.SEMANTIC_DUPLICATE_PARAMETER",
                       "Component parameter '" + parameter.name + "' is declared more than once.",
                       parameter.span, "component " + component.name,
                       "unique component parameter");
            }
            std::vector<const TypeReference*> unresolved;
            SemanticTypePtr type = resolve_parameter_type(parameter, registry, &unresolved);
            const std::vector<std::string> names = registry.type_names();
            for (const TypeReference* reference : unresolved) {
                report("STRATA.DSL.SEMANTIC_UNKNOWN_TYPE",
                       "Type '" + reference->name + "' is not declared." +
                           did_you_mean(reference->name, names),
                       reference->span, "component " + component.name + "(" + parameter.name + ")",
                       expected_names(names));
            }
            info.parameters.push_back(SchemaParameter{
                .name = parameter.name,
                .type = std::move(type),
                .required = parameter.default_value == nullptr,
                .nullable =
                    parameter.type_reference.has_value() && parameter.type_reference->nullable,
                .aliases = {},
                .material_type = std::nullopt,
            });
        }
        if (component.body != nullptr) {
            scan_slots(component, info, *component.body, false);
            collect_widget_defaults(*component.body, info);
        }
        analysis_.model.components.try_emplace(component.name, std::move(info));
    }

    /**
     * `fills` marks the body of a call to another component, whose direct Slot children fill that
     * component's slots; what a fill holds can declare this component's slots in turn, which
     * forwards them.
     */
    void scan_slots(const ComponentDeclaration& component, ComponentInfo& info, const Block& block,
                    const bool fills) {
        for (const StatementPtr& statement : block.statements) {
            const WidgetCall* call = statement_call(*statement);
            const bool slot = call != nullptr && is_slot_widget(analysis_.registry, call->name);
            if (slot && fills) {
                if (call->body != nullptr) scan_slots(component, info, *call->body, false);
            } else if (slot) {
                declare_slot(component, info, *call);
            } else if (call != nullptr && call->body != nullptr) {
                scan_slots(component, info, *call->body,
                           analysis_.registry.widget(call->name) == nullptr);
            }
            if (const auto* conditional = std::get_if<IfStatement>(&statement->node)) {
                scan_slots(component, info, *conditional->then_block, false);
                if (conditional->else_block != nullptr)
                    scan_slots(component, info, *conditional->else_block, false);
            } else if (const auto* when = std::get_if<WhenStatement>(&statement->node)) {
                for (const WhenBranch& branch : when->branches)
                    scan_slots(component, info, *branch.block, false);
            } else if (const auto* loop = std::get_if<ForStatement>(&statement->node)) {
                scan_slots(component, info, *loop->block, false);
            }
        }
    }

    void declare_slot(const ComponentDeclaration& component, ComponentInfo& info,
                      const WidgetCall& call) {
        const Expression* name_expression = nullptr;
        const Expression* required_expression = nullptr;
        for (const Argument& argument : call.arguments) {
            if (argument.name == "name" ||
                (!argument.name.has_value() && name_expression == nullptr)) {
                name_expression = argument.value.get();
            } else if (argument.name == "required") {
                required_expression = argument.value.get();
            }
        }
        const std::string* name =
            name_expression != nullptr ? string_literal_value(*name_expression) : nullptr;
        if (name == nullptr) {
            report("STRATA.DSL.SEMANTIC_DYNAMIC_SLOT_DECLARATION",
                   "Component slot declaration names must be string literals.",
                   name_expression != nullptr ? name_expression->span : call.span,
                   "component " + component.name, "Slot(name: \"static-name\")");
            return;
        }
        const bool required = required_expression != nullptr
                                  ? boolean_literal_value(*required_expression).value_or(false)
                                  : false;
        const auto duplicate = std::ranges::find(info.slots, *name, &ComponentSlot::name);
        if (duplicate != info.slots.end()) {
            report("STRATA.DSL.SEMANTIC_DUPLICATE_SLOT_DECLARATION",
                   "Component '" + component.name + "' declares slot '" + *name +
                       "' more than once.",
                   call.span, "component " + component.name, "unique slot name");
            return;
        }
        info.slots.push_back(ComponentSlot{*name, required, call.span});
    }

    void collect_widget_defaults(const Block& body, ComponentInfo& info) const {
        bool lowered = false;
        for (const StatementPtr& statement : body.statements) {
            const auto* property = std::get_if<PropertyStatement>(&statement->node);
            if (property == nullptr || property->property.name != "defaults") continue;
            const auto* defaults = std::get_if<MapExpression>(&property->property.value->node);
            if (defaults == nullptr) continue;
            for (const MapEntry& widget_entry : defaults->entries) {
                const std::string* widget_name = static_map_key(widget_entry.key);
                const auto* values = std::get_if<MapExpression>(&widget_entry.value->node);
                if (widget_name == nullptr || values == nullptr) continue;
                ComponentWidgetDefault lowered_default{*widget_name, nullptr, nullptr};
                for (const MapEntry& value : values->entries) {
                    const std::string* name = static_map_key(value.key);
                    if (name == nullptr) continue;
                    if (*name == "style") {
                        info.widget_default_styles.insert_or_assign(*widget_name,
                                                                    value.value.get());
                        lowered_default.style = value.value.get();
                    } else if (*name == "variant") {
                        lowered_default.variant = value.value.get();
                    }
                }
                // Lowering emits the first defaults object, for registered widgets only.
                if (!lowered && analysis_.registry.widget(*widget_name) != nullptr) {
                    info.widget_defaults.push_back(std::move(lowered_default));
                }
            }
            lowered = true;
        }
        std::ranges::sort(info.widget_defaults, {}, &ComponentWidgetDefault::widget);
    }

    /** Every state declared anywhere in the module, since a state action can only target one. */
    void collect_state_names() {
        std::vector<std::string>& names = analysis_.state_names;
        const auto visit = [&names](const auto& self, const Block& block) -> void {
            for (const StatementPtr& statement : block.statements) {
                std::visit(
                    [&](const auto& node) {
                        using Node = std::decay_t<decltype(node)>;
                        if constexpr (std::is_same_v<Node, StateStatement>) {
                            names.push_back(node.name);
                        } else if constexpr (std::is_same_v<Node, WidgetStatement> ||
                                             std::is_same_v<Node, RootStatement>) {
                            if (node.call.body != nullptr) self(self, *node.call.body);
                        } else if constexpr (std::is_same_v<Node, IfStatement>) {
                            self(self, *node.then_block);
                            if (node.else_block != nullptr) self(self, *node.else_block);
                        } else if constexpr (std::is_same_v<Node, WhenStatement>) {
                            for (const WhenBranch& branch : node.branches) self(self, *branch.block);
                        } else if constexpr (std::is_same_v<Node, ForStatement>) {
                            self(self, *node.block);
                        }
                    },
                    statement->node);
            }
        };
        for (const Declaration& declaration : analysis_.file.declarations) {
            std::visit(
                [&](const auto& node) {
                    using Node = std::decay_t<decltype(node)>;
                    if constexpr (std::is_same_v<Node, ScreenDeclaration> ||
                                  std::is_same_v<Node, OverlayDeclaration> ||
                                  std::is_same_v<Node, ComponentDeclaration>) {
                        if (node.body != nullptr) visit(visit, *node.body);
                    }
                },
                declaration.node);
        }
        std::ranges::sort(names);
        names.erase(std::ranges::unique(names).begin(), names.end());
    }

    void check_style_composition() {
        const auto& styles = analysis_.style_declarations;
        for (const auto& [name, style] : styles) {
            static_cast<void>(name);
            std::set<std::string, std::less<>> seen_bases;
            for (const StyleBase& base : style->bases) {
                if (!seen_bases.insert(base.name).second) {
                    report("STRATA.DSL.SEMANTIC_DUPLICATE_STYLE_BASE",
                           "Style '" + style->name + "' extends '" + base.name +
                               "' more than once.",
                           base.span, "style " + style->name, "unique style base");
                }
                if (!styles.contains(base.name)) {
                    analysis_.diagnostics.report_unresolved(
                        "STRATA.DSL.SEMANTIC_UNKNOWN_STYLE_BASE", base.name, true,
                        "Style '" + style->name + "' extends unknown style '" + base.name +
                            "'." + did_you_mean(base.name, analysis_.style_names),
                        base.span, "style " + style->name,
                        expected_names(analysis_.style_names));
                }
            }
        }
        std::set<std::string, std::less<>> visiting;
        std::set<std::string, std::less<>> visited;
        const auto visit = [&](const auto& self, const std::string& name) -> void {
            if (visited.contains(name)) return;
            const StyleDeclaration* style = styles.at(name);
            if (!visiting.insert(name).second) {
                report("STRATA.DSL.SEMANTIC_STYLE_CYCLE",
                       "Style composition cycle detected at '" + name + "'.", style->body->span,
                       "style " + name, "acyclic style composition");
                return;
            }
            for (const StyleBase& base : style->bases) {
                if (styles.contains(base.name)) self(self, base.name);
            }
            visiting.erase(name);
            visited.insert(name);
        };
        for (const auto& [name, style] : styles) {
            static_cast<void>(style);
            visit(visit, name);
        }
    }

    Analysis& analysis_;
};

/**
 * A component template handed to a widget's row-template parameter describes each of the widget's
 * generated rows inside the component that handed it over. The slots it declares, and those of the
 * row templates it hands on in turn, are that component's own: its callers fill them.
 */
class RowTemplateSlotCollector final {
public:
    explicit RowTemplateSlotCollector(Analysis& analysis) : analysis_(analysis) {}

    void run() {
        std::map<const ComponentInfo*, std::vector<const ComponentInfo*>> references;
        for (const auto& [name, component] : analysis_.model.components) {
            static_cast<void>(name);
            std::vector<const ComponentInfo*>& referenced = references[&component];
            if (component.declaration != nullptr && component.declaration->body != nullptr)
                block(*component.declaration->body, referenced);
        }
        // Each component's own slots, before any template's are added to them.
        std::map<const ComponentInfo*, std::vector<ComponentSlot>> own;
        for (const auto& [name, component] : analysis_.model.components) {
            static_cast<void>(name);
            own.emplace(&component, component.slots);
        }
        for (auto& [name, component] : analysis_.model.components) {
            static_cast<void>(name);
            std::set<const ComponentInfo*> reached{&component};
            std::vector<const ComponentInfo*> pending = references[&component];
            while (!pending.empty()) {
                const ComponentInfo* const current = pending.back();
                pending.pop_back();
                if (!reached.insert(current).second) continue;
                for (const ComponentSlot& slot : own[current]) add(component, slot);
                const std::vector<const ComponentInfo*>& next = references[current];
                pending.insert(pending.end(), next.begin(), next.end());
            }
        }
    }

private:
    /** A slot declared twice, by the component and a template or by two templates, is one. */
    static void add(ComponentInfo& component, const ComponentSlot& slot) {
        const auto existing = std::ranges::find(component.slots, slot.name, &ComponentSlot::name);
        if (existing == component.slots.end())
            component.slots.push_back(slot);
        else
            existing->required = existing->required || slot.required;
    }

    void block(const Block& body, std::vector<const ComponentInfo*>& referenced) const {
        for (const StatementPtr& statement : body.statements) {
            if (const WidgetCall* call = statement_call(*statement)) {
                widget_call(*call, referenced);
            } else if (const auto* conditional = std::get_if<IfStatement>(&statement->node)) {
                if (conditional->then_block != nullptr) block(*conditional->then_block, referenced);
                if (conditional->else_block != nullptr) block(*conditional->else_block, referenced);
            } else if (const auto* when = std::get_if<WhenStatement>(&statement->node)) {
                for (const WhenBranch& branch : when->branches) {
                    if (branch.block != nullptr) block(*branch.block, referenced);
                }
            } else if (const auto* loop = std::get_if<ForStatement>(&statement->node)) {
                if (loop->block != nullptr) block(*loop->block, referenced);
            }
        }
    }

    void widget_call(const WidgetCall& call, std::vector<const ComponentInfo*>& referenced) const {
        const ResolvedWidgetCall* resolved = analysis_.model.widget_call(call);
        if (resolved != nullptr && resolved->widget != nullptr &&
            !resolved->widget->row_templates.empty()) {
            for (std::size_t index = 0U;
                 index < call.arguments.size() && index < resolved->arguments.size(); ++index) {
                const SchemaParameter* parameter = resolved->arguments[index].parameter;
                if (parameter != nullptr &&
                    std::ranges::contains(resolved->widget->row_templates, parameter->name)) {
                    templates(*call.arguments[index].value, referenced);
                }
            }
        }
        if (call.body != nullptr) block(*call.body, referenced);
    }

    /** The component templates an argument names, wherever in it they are written. */
    void templates(const Expression& expression,
                   std::vector<const ComponentInfo*>& referenced) const {
        const auto visit = [&](const ExpressionPtr& child) {
            if (child != nullptr) templates(*child, referenced);
        };
        std::visit(
            [&](const auto& node) {
                using Node = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<Node, IdentifierExpression>) {
                    const Symbol* symbol = analysis_.model.symbol(expression);
                    if (symbol != nullptr && symbol->kind == SymbolKind::component &&
                        symbol->component != nullptr) {
                        referenced.push_back(symbol->component);
                    }
                } else if constexpr (std::is_same_v<Node, CallExpression>) {
                    const ResolvedCall* call = analysis_.model.call(expression);
                    if (call != nullptr && call->kind == CallKind::component_template &&
                        call->component != nullptr) {
                        referenced.push_back(call->component);
                        for (const Argument& argument : node.arguments) visit(argument.value);
                    }
                } else if constexpr (std::is_same_v<Node, GroupingExpression>) {
                    visit(node.expression);
                } else if constexpr (std::is_same_v<Node, ListExpression>) {
                    for (const ExpressionPtr& element : node.elements) visit(element);
                } else if constexpr (std::is_same_v<Node, MapExpression>) {
                    for (const MapEntry& entry : node.entries) visit(entry.value);
                } else if constexpr (std::is_same_v<Node, ConditionalExpression>) {
                    visit(node.then_expression);
                    visit(node.else_expression);
                }
            },
            expression.node);
    }

    Analysis& analysis_;
};

} // namespace

void collect_declarations(Analysis& analysis) {
    DeclarationCollector(analysis).run();
}

void collect_row_template_slots(Analysis& analysis) {
    RowTemplateSlotCollector(analysis).run();
}

} // namespace strata::compiler::semantic
