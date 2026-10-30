#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <type_traits>
#include <utility>

#include "compiler/semantic/analysis.hpp"

// Pass 4: policy lints. Rules that are not about types: roots, keys, persistence, layout kinds,
// Repeater bodies, slots, widget defaults and where statements may appear.
namespace strata::compiler::semantic {

void check_layout_kind(
    Analysis& analysis,
    TypeOracle* const oracle,
    const std::vector<LayoutProperty>& properties,
    const std::string& path,
    const std::string_view implicit_kind
) {
    const auto find = [&properties](const std::string_view name) -> const LayoutProperty* {
        const auto found = std::ranges::find(properties, name, &LayoutProperty::name);
        return found != properties.end() ? &*found : nullptr;
    };
    const LayoutProperty* kind_property = find("kind");
    const std::string* kind = kind_property != nullptr && kind_property->value != nullptr
                                  ? string_literal_value(*kind_property->value)
                                  : nullptr;
    const std::string implicit_kind_storage(implicit_kind);
    if (kind == nullptr && !implicit_kind_storage.empty()) kind = &implicit_kind_storage;
    if (kind == nullptr) return;
    const bool linear = *kind == "ROW" || *kind == "COLUMN";
    const bool layered =
        *kind == "PANEL" || *kind == "OVERLAY" || *kind == "PORTAL" || *kind == "STACK";
    const auto incompatible = [&](const std::string_view name, const std::string_view expected) {
        const LayoutProperty* property = find(name);
        if (property == nullptr) return;
        if (property->value != nullptr &&
            !analysis.diagnosed_layout_properties.insert(property->value).second) {
            return;
        }
        analysis.diagnostics.report(
            "STRATA.DSL.SEMANTIC_LAYOUT_PROPERTY_KIND",
            "Layout property '" + std::string(name) + "' has no effect for layout kind '" + *kind +
                "'.",
            property->span, path + "." + std::string(name), std::string(expected));
    };
    if (!linear) {
        incompatible("wrap", "ROW or COLUMN");
        incompatible("alignContent", "ROW or COLUMN");
    }
    if (*kind == "GRID" || *kind == "SCROLL" || *kind == "SPACER" || *kind == "MASONRY" ||
        *kind == "CANVAS") {
        incompatible("alignItems", "ROW, COLUMN, PANEL, OVERLAY, PORTAL, or STACK");
        incompatible("justifyContent", "ROW, COLUMN, PANEL, OVERLAY, PORTAL, or STACK");
    }
    if (*kind != "MASONRY") {
        incompatible("columns", "MASONRY");
        incompatible("minColumnWidth", "MASONRY");
    }
    if (*kind == "CANVAS")
        incompatible("gap", "ROW, COLUMN, GRID, MASONRY, or SCROLL");
    if (layered) {
        const LayoutProperty* justify = find("justifyContent");
        const std::string* value = justify != nullptr && justify->value != nullptr
                                       ? string_literal_value(*justify->value)
                                       : nullptr;
        bool unsupported = value != nullptr && value->starts_with("SPACE_");
        if (!unsupported && value == nullptr && justify != nullptr && justify->value != nullptr &&
            oracle != nullptr) {
            const SemanticTypePtr actual = oracle->type_of(*justify->value);
            unsupported = actual->kind == SemanticTypeKind::enumeration &&
                          std::ranges::any_of(actual->values, [](const std::string& candidate) {
                              return candidate != "START" && candidate != "CENTER" &&
                                     candidate != "END";
                          });
        }
        if (unsupported && (justify->value == nullptr ||
                            analysis.diagnosed_layout_properties.insert(justify->value).second)) {
            analysis.diagnostics.report(
                "STRATA.DSL.SEMANTIC_LAYOUT_PROPERTY_KIND",
                value != nullptr
                    ? "Layered layout kind '" + *kind +
                          "' supports START, CENTER, or END justification, not '" + *value + "'."
                    : "Layered layout kind '" + *kind +
                          "' requires a justification constrained to START, CENTER, or END.",
                justify->span, path + ".justifyContent",
                value != nullptr ? "START, CENTER, or END" : "a layerJustify value");
        } else if (unsupported) {
            return;
        }
    }
    if (layered || *kind == "SPACER") {
        incompatible("gap", "ROW, COLUMN, GRID, MASONRY, or SCROLL");
    }
    if (*kind != "PORTAL") {
        incompatible("anchorPoint", "PORTAL");
        incompatible("detachFromParentClip", "PORTAL");
        incompatible("portalTarget", "PORTAL");
    }
    // The remaining anchor presentation fields are also consumed from authored Select/Menu popup
    // roots and hoisted onto their synthesized portal, so they are not kind-local.
}

void claim_persistence_key(
    Analysis& analysis,
    const std::string& key,
    const Expression& key_expression,
    const std::string& owner_path,
    const std::string& component_path
) {
    const auto [found, inserted] =
        analysis.persistence_keys.try_emplace(key, PersistenceOwner{&key_expression, owner_path});
    if (inserted || found->second.key == &key_expression) return;
    analysis.diagnostics.report(
        "STRATA.DSL.SEMANTIC_DUPLICATE_PERSISTENCE_KEY",
        "Persistence key '" + key + "' is already used by '" + found->second.path + "' at line " +
            std::to_string(found->second.key->span.start.line) + ".",
        key_expression.span, component_path, "unique persistence key");
}

namespace {

enum class BlockContext { component, ui, lazy_ui };

[[nodiscard]] BlockContext nested_context(const BlockContext context) {
    return context == BlockContext::component ? BlockContext::ui : context;
}

using LayoutPropertyMap = std::map<std::string, LayoutProperty, std::less<>>;
using DefaultStyles = std::map<std::string, const Expression*, std::less<>>;

/** The string-literal `key:` a widget call is given, when it has one. */
[[nodiscard]] const Argument* literal_key_argument(const WidgetCall& call) {
    for (const Argument& argument : call.arguments) {
        if (argument.name == "key" && argument.value != nullptr &&
            string_literal_value(*argument.value) != nullptr) {
            return &argument;
        }
    }
    return nullptr;
}

[[nodiscard]] bool call_has_explicit_key(const WidgetCall& call) {
    return std::ranges::any_of(call.arguments, [](const Argument& argument) {
        return argument.name == "key" || argument.name == "rowKey";
    });
}

[[nodiscard]] bool every_root_is_keyed(const Block& block) {
    for (const StatementPtr& statement : block.statements) {
        if (const WidgetCall* call = statement_call(*statement)) {
            if (!call_has_explicit_key(*call)) return false;
        } else if (const auto* conditional = std::get_if<IfStatement>(&statement->node)) {
            if (!every_root_is_keyed(*conditional->then_block) ||
                conditional->else_block == nullptr ||
                !every_root_is_keyed(*conditional->else_block)) {
                return false;
            }
        } else if (const auto* when = std::get_if<WhenStatement>(&statement->node)) {
            if (when->branches.empty() ||
                !std::ranges::all_of(when->branches, [](const WhenBranch& branch) {
                    return every_root_is_keyed(*branch.block);
                })) {
                return false;
            }
        } else if (std::holds_alternative<ForStatement>(statement->node)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool repeater_identity_is_extractable(const Block& block) {
    for (const StatementPtr& statement : block.statements) {
        if (statement_call(*statement) != nullptr) continue;
        if (const auto* conditional = std::get_if<IfStatement>(&statement->node)) {
            if (!repeater_identity_is_extractable(*conditional->then_block) ||
                conditional->else_block == nullptr ||
                !repeater_identity_is_extractable(*conditional->else_block)) {
                return false;
            }
            continue;
        }
        if (const auto* when = std::get_if<WhenStatement>(&statement->node)) {
            if (when->branches.empty() ||
                !std::ranges::all_of(when->branches, [](const WhenBranch& branch) {
                    return repeater_identity_is_extractable(*branch.block);
                })) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

void collect_referenced_identifiers(const Expression& expression,
                                    std::set<std::string, std::less<>>& output,
                                    std::set<std::string, std::less<>> bound = {}) {
    if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.node)) {
        if (!bound.contains(identifier->name)) output.insert(identifier->name);
        return;
    }
    if (const auto* lambda = std::get_if<LambdaExpression>(&expression.node)) {
        for (const LambdaParameter& parameter : lambda->parameters) bound.insert(parameter.name);
    }
    for_each_child(expression, [&](const Expression& child) {
        collect_referenced_identifiers(child, output, bound);
    });
}

class PolicyChecker final {
public:
    PolicyChecker(Analysis& analysis, TypeOracle& oracle)
        : analysis_(analysis), oracle_(oracle), registry_(analysis.registry),
          model_(analysis.model) {}

    void run() {
        composed_style_layouts();
        for (const Declaration& declaration : analysis_.file.declarations) declaration_check(declaration);
    }

private:
    void report(std::string code, std::string message, const SourceSpan& span, std::string path,
                std::string expected) {
        analysis_.diagnostics.report(std::move(code), std::move(message), span, std::move(path),
                                     std::move(expected));
    }

    void merge_named_style(const std::string& name, LayoutPropertyMap& properties,
                           std::set<std::string, std::less<>>& resolving) const {
        const auto declaration = analysis_.style_declarations.find(name);
        if (declaration == analysis_.style_declarations.end() || !resolving.insert(name).second)
            return;
        for (const StyleBase& base : declaration->second->bases)
            merge_named_style(base.name, properties, resolving);
        for (const Property& property : declaration->second->properties) {
            properties.insert_or_assign(
                property.name, LayoutProperty{property.name, property.value.get(), property.span});
        }
        resolving.erase(name);
    }

    void merge_style(const Expression& expression, LayoutPropertyMap& properties,
                     std::set<std::string, std::less<>>& resolving) const {
        const Expression& value = ungrouped(expression);
        if (const auto* identifier = std::get_if<IdentifierExpression>(&value.node)) {
            merge_named_style(identifier->name, properties, resolving);
            return;
        }
        if (const auto* map = std::get_if<MapExpression>(&value.node)) {
            merge_map(*map, properties);
            return;
        }
        const auto* call = std::get_if<CallExpression>(&value.node);
        if (call == nullptr || call->target.qualified_name() != "style") return;
        for (const Argument& argument : call->arguments) {
            if (!argument.name.has_value()) {
                merge_style(*argument.value, properties, resolving);
                continue;
            }
            properties.insert_or_assign(
                *argument.name, LayoutProperty{*argument.name, argument.value.get(), argument.span});
        }
    }

    static void merge_map(const MapExpression& map, LayoutPropertyMap& properties) {
        for (const MapEntry& entry : map.entries) {
            const std::string* name = static_map_key(entry.key);
            if (name == nullptr || name->empty()) continue;
            properties.insert_or_assign(*name,
                                        LayoutProperty{*name, entry.value.get(), entry.span});
        }
    }

    [[nodiscard]] static std::vector<LayoutProperty> values(const LayoutPropertyMap& properties) {
        std::vector<LayoutProperty> result;
        result.reserve(properties.size());
        for (const auto& [name, property] : properties) {
            static_cast<void>(name);
            result.push_back(property);
        }
        return result;
    }

    void composed_style_layouts() {
        for (const auto& [name, style] : analysis_.style_declarations) {
            static_cast<void>(style);
            LayoutPropertyMap properties;
            std::set<std::string, std::less<>> resolving;
            merge_named_style(name, properties, resolving);
            check_layout_kind(analysis_, nullptr, values(properties), "style " + name);
        }
    }

    /** A screen or overlay draws exactly one root, whichever branch its conditions take. */
    void layer_root(const Block& body, const std::string& path) {
        const RootCardinality roots = root_cardinality(body);
        if ((roots.minimum == 1U && roots.maximum == 1U) || roots.uncertain) return;
        report("STRATA.DSL.SEMANTIC_LAYER_ROOT",
               "'" + path + "' must declare exactly one root widget on every control-flow path" +
                   (roots.maximum == 0U ? ", but declares none." : "."),
               body.span, path, "single root widget");
    }

    void declaration_check(const Declaration& declaration) {
        if (const auto* screen = std::get_if<ScreenDeclaration>(&declaration.node)) {
            const std::string path = "screen " + screen->name;
            layer_root(*screen->body, path);
            block(*screen->body, path, BlockContext::ui, {});
        } else if (const auto* overlay = std::get_if<OverlayDeclaration>(&declaration.node)) {
            const std::string path = "overlay " + overlay->name;
            layer_root(*overlay->body, path);
            block(*overlay->body, path, BlockContext::ui, {});
        } else if (const auto* component = std::get_if<ComponentDeclaration>(&declaration.node)) {
            const ComponentInfo& info = model_.components.at(component->name);
            if (info.declaration != component) return;
            const std::string path = "component " + component->name;
            for (const Parameter& parameter : component->parameters) {
                if (parameter.default_value != nullptr)
                    persisted_calls(*parameter.default_value, path + "(" + parameter.name + ")",
                                    nullptr);
            }
            const RootCardinality roots = root_cardinality(*component->body);
            if ((roots.minimum != 1U || roots.maximum != 1U) && !roots.uncertain) {
                report("STRATA.DSL.SEMANTIC_COMPONENT_ROOT",
                       "Component '" + component->name +
                           "' must produce exactly one root node on every control-flow path.",
                       component->body->span, path, "single root node");
            }
            default_styles_.push_back(&info.widget_default_styles);
            block(*component->body, path, BlockContext::component, {});
            default_styles_.pop_back();
        } else if (const auto* style = std::get_if<StyleDeclaration>(&declaration.node)) {
            const std::string path = "style " + style->name;
            std::set<std::string, std::less<>> seen;
            std::vector<LayoutProperty> layout_properties;
            for (const Property& property : style->properties) {
                layout_properties.push_back(
                    LayoutProperty{property.name, property.value.get(), property.span});
                if (!seen.insert(property.name).second) {
                    report("STRATA.DSL.SEMANTIC_DUPLICATE_PROPERTY",
                           "Style property '" + property.name + "' is declared more than once.",
                           property.span, path, "unique style property");
                }
                persisted_calls(*property.value, path + "." + property.name, nullptr);
            }
            check_layout_kind(analysis_, &oracle_, layout_properties, path);
            for (const StatementPtr& statement : style->body->statements) {
                if (!std::holds_alternative<PropertyStatement>(statement->node) &&
                    !std::holds_alternative<ErrorStatement>(statement->node)) {
                    report("STRATA.DSL.SEMANTIC_STATEMENT_NOT_ALLOWED",
                           "Style declarations may only contain property assignments.",
                           statement->span, path, "style property");
                }
            }
        }
    }

    void derived_cycles(const Block& block, const std::string& path) {
        std::map<std::string, const Statement*, std::less<>> declarations;
        for (const StatementPtr& statement : block.statements) {
            if (const auto* derived = std::get_if<DerivedStatement>(&statement->node))
                declarations.try_emplace(derived->name, statement.get());
        }
        if (declarations.empty()) return;
        std::map<std::string, std::set<std::string, std::less<>>, std::less<>> dependencies;
        for (const auto& [name, statement] : declarations) {
            const auto& derived = std::get<DerivedStatement>(statement->node);
            std::set<std::string, std::less<>> referenced;
            collect_referenced_identifiers(*derived.expression, referenced);
            for (const std::string& candidate : referenced) {
                if (declarations.contains(candidate)) dependencies[name].insert(candidate);
            }
        }
        std::set<std::string, std::less<>> visiting;
        std::set<std::string, std::less<>> visited;
        std::set<std::string, std::less<>> reported;
        const auto visit = [&](const auto& self, const std::string& name) -> void {
            if (visited.contains(name)) return;
            if (!visiting.insert(name).second) {
                if (reported.insert(name).second) {
                    report("STRATA.DSL.SEMANTIC_DERIVED_CYCLE",
                           "Derived value cycle detected at '" + name + "'.",
                           declarations.at(name)->span, path + "/" + name,
                           "acyclic derived values");
                }
                return;
            }
            for (const std::string& dependency : dependencies[name]) self(self, dependency);
            visiting.erase(name);
            visited.insert(name);
        };
        for (const auto& [name, statement] : declarations) {
            static_cast<void>(statement);
            visit(visit, name);
        }
    }

    /**
     * Widgets written side by side in one block are siblings, so their literal keys must differ;
     * the surface rejects a duplicate on its first frame. Branches of an if/when are not compared
     * with each other, since only one of them is drawn.
     */
    void sibling_keys(const Block& block, const std::string& path) {
        std::set<std::string, std::less<>> seen;
        for (const StatementPtr& statement : block.statements) {
            const WidgetCall* call = statement_call(*statement);
            const Argument* key = call != nullptr ? literal_key_argument(*call) : nullptr;
            if (key == nullptr) continue;
            const std::string& value = *string_literal_value(*key->value);
            if (!seen.insert(value).second) {
                report("STRATA.DSL.SEMANTIC_DUPLICATE_KEY",
                       "Key '" + value + "' is already used by a sibling widget.", key->value->span,
                       path + "/" + call->name, "unique sibling key");
            }
        }
    }

    void block(const Block& block, const std::string& path, const BlockContext context,
               std::set<std::string, std::less<>> local_names) {
        derived_cycles(block, path);
        sibling_keys(block, path);
        for (const StatementPtr& statement : block.statements)
            this->statement(*statement, path, context, local_names);
    }

    void statement(const Statement& statement, const std::string& path,
                   const BlockContext context, std::set<std::string, std::less<>>& local_names) {
        const auto note_local = [&](const std::string& name) {
            if (!local_names.insert(name).second) {
                report("STRATA.DSL.SEMANTIC_DUPLICATE_STATE",
                       "State or local value '" + name + "' is already defined in this scope.",
                       statement.span, path + "/" + name, "unique state or derived name");
            }
        };
        if (const auto* state = std::get_if<StateStatement>(&statement.node)) {
            note_local(state->name);
            if (state->initializer != nullptr)
                persisted_calls(*state->initializer, path + "/" + state->name,
                                state->initializer.get());
        } else if (const auto* derived = std::get_if<DerivedStatement>(&statement.node)) {
            note_local(derived->name);
            persisted_calls(*derived->expression, path + "/" + derived->name, nullptr);
        } else if (const auto* property = std::get_if<PropertyStatement>(&statement.node)) {
            if (property->property.name == "defaults" && context != BlockContext::component) {
                report("STRATA.DSL.SEMANTIC_STATEMENT_NOT_ALLOWED",
                       "Widget defaults are only allowed at the top level of a component "
                       "declaration.",
                       statement.span, path, "component-level defaults");
            }
            persisted_calls(*property->property.value, path, nullptr);
        } else if (const auto* widget = std::get_if<WidgetStatement>(&statement.node)) {
            call(widget->call, path, context);
        } else if (const auto* root = std::get_if<RootStatement>(&statement.node)) {
            call(root->call, path + "/root", context);
        } else if (const auto* conditional = std::get_if<IfStatement>(&statement.node)) {
            persisted_calls(*conditional->condition, path + "/if", nullptr);
            block(*conditional->then_block, path + "/then", nested_context(context), local_names);
            if (conditional->else_block != nullptr)
                block(*conditional->else_block, path + "/else", nested_context(context),
                      local_names);
        } else if (const auto* when = std::get_if<WhenStatement>(&statement.node)) {
            when_statement(statement, *when, path, context, local_names);
        } else if (const auto* loop = std::get_if<ForStatement>(&statement.node)) {
            persisted_calls(*loop->collection, path + "/for", nullptr);
            if (loop->filter != nullptr)
                persisted_calls(*loop->filter, path + "/for/filter", nullptr);
            // Every item draws the loop body, so a literal key there repeats per item.
            for (const StatementPtr& body_statement : loop->block->statements) {
                const auto* row = std::get_if<WidgetStatement>(&body_statement->node);
                const Argument* key = row != nullptr ? literal_key_argument(row->call) : nullptr;
                if (key == nullptr) continue;
                report("STRATA.DSL.SEMANTIC_LOOP_CONSTANT_KEY",
                       "Key '" + *string_literal_value(*key->value) + "' is the same for every '" +
                           loop->item_name + "'; derive it from the item, for example key: " +
                           loop->item_name + ".id.",
                       key->value->span, path + "/" + loop->item_name,
                       "key derived from the loop item");
            }
            block(*loop->block, path + "/" + loop->item_name, nested_context(context),
                  local_names);
        }
    }

    void when_statement(const Statement& statement, const WhenStatement& when,
                        const std::string& path, const BlockContext context,
                        std::set<std::string, std::less<>>& local_names) {
        persisted_calls(*when.subject, path + "/when", nullptr);
        const auto* status = std::get_if<PropertyAccessExpression>(&when.subject->node);
        const bool async_when =
            status != nullptr && status->property_name == "status" &&
            std::holds_alternative<IdentifierExpression>(status->receiver->node) &&
            oracle_.type_of(*status->receiver)->kind == SemanticTypeKind::async_value;
        std::set<std::string, std::less<>> async_matches;
        std::size_t async_match_count = 0U;
        std::size_t else_count = 0U;
        for (std::size_t index = 0U; index < when.branches.size(); ++index) {
            const WhenBranch& branch = when.branches[index];
            const std::string branch_path = path + "/when[" + std::to_string(index) + "]";
            if (branch.match != nullptr) {
                persisted_calls(*branch.match, branch_path, nullptr);
                if (const std::string* match = string_literal_value(*branch.match);
                    async_when && match != nullptr) {
                    std::string normalized = *match;
                    std::ranges::transform(normalized, normalized.begin(),
                                           [](const unsigned char value) {
                                               return static_cast<char>(std::toupper(value));
                                           });
                    async_matches.insert(normalized);
                    ++async_match_count;
                }
            } else {
                ++else_count;
                if (index + 1U != when.branches.size()) {
                    report("STRATA.DSL.SEMANTIC_WHEN_ELSE_ORDER",
                           "The else branch must be the final when branch.", branch.span,
                           path + "/when", "else as final branch");
                }
            }
            block(*branch.block, branch_path, nested_context(context), local_names);
        }
        const std::set<std::string, std::less<>> required_async{"FAILED", "IDLE", "LOADING",
                                                                 "READY"};
        if (async_when) {
            if (async_matches != required_async || async_match_count != required_async.size() ||
                else_count != 0U) {
                report("STRATA.DSL.SEMANTIC_ASYNC_WHEN_NOT_EXHAUSTIVE",
                       "An async-state when must declare IDLE, LOADING, READY, and FAILED "
                       "branches exactly.",
                       statement.span, path + "/when", "IDLE, LOADING, READY, FAILED");
            }
        } else if (else_count != 1U) {
            report("STRATA.DSL.SEMANTIC_WHEN_NOT_EXHAUSTIVE",
                   "A when expression must declare exactly one else branch.", statement.span,
                   path + "/when", "one final else branch");
        }
    }

    /**
     * persisted() stores its value under a literal key, as the direct initializer of a state; the
     * key belongs to that state alone. Every call nested anywhere, a lambda body included, is
     * checked.
     */
    void persisted_calls(const Expression& expression, const std::string& path,
                         const Expression* owner) {
        for_each_child(expression, [&](const Expression& child) {
            persisted_calls(child, path, owner);
        });
        if (std::holds_alternative<CallExpression>(expression.node))
            persisted_call(expression, path, owner);
    }

    void persisted_call(const Expression& expression, const std::string& path,
                        const Expression* owner) {
        const ResolvedCall* resolved = model_.call(expression);
        if (resolved == nullptr || resolved->helper == nullptr ||
            resolved->helper->implementation != "persisted") {
            return;
        }
        if (owner != &expression) {
            report("STRATA.DSL.SEMANTIC_PERSISTED_CONTEXT",
                   "persisted() is valid only as the direct initializer of a state declaration.",
                   expression.span, path, "state value: type = persisted(\"stable.key\", initial)");
        }
        const Argument* key_argument = oracle_.argument_for(expression, "key");
        const std::string* key =
            key_argument != nullptr ? string_literal_value(*key_argument->value) : nullptr;
        if (key == nullptr || key->empty()) {
            report("STRATA.DSL.SEMANTIC_PERSISTENCE_KEY_STATIC",
                   "persisted() requires a non-empty string-literal key.",
                   key_argument != nullptr ? key_argument->value->span : expression.span, path,
                   "persisted(\"stable.key\", initial)");
            return;
        }
        claim_persistence_key(analysis_, *key, *key_argument->value, path, path);
    }

    void repeater_body(const WidgetCall& call, const std::string& call_path) {
        const ForStatement* loop = nullptr;
        if (call.body != nullptr && call.body->statements.size() == 1U)
            loop = std::get_if<ForStatement>(&call.body->statements.front()->node);
        if (loop == nullptr) {
            report("STRATA.DSL.SEMANTIC_REPEATER_BODY",
                   "Repeater requires exactly one keyed for statement so it can virtualize without "
                   "eager expansion.",
                   call.body != nullptr ? call.body->span : call.span, call_path,
                   "Repeater { for item in items { Panel(key: item.id) { ... } } }");
            return;
        }
        const RootCardinality roots = root_cardinality(*loop->block);
        if (roots.uncertain) return;
        if (roots.minimum != 1U || roots.maximum != 1U || !every_root_is_keyed(*loop->block)) {
            report("STRATA.DSL.SEMANTIC_REPEATER_ITEM_ROOT",
                   "Each Repeater iteration must produce exactly one explicitly keyed root node.",
                   loop->block->span, call_path, "one root widget with key: ...");
        } else if (!repeater_identity_is_extractable(*loop->block)) {
            report("STRATA.DSL.SEMANTIC_REPEATER_IDENTITY_EXTRACTOR",
                   "A Repeater root key must be selectable from the item/index and outer values "
                   "without executing row-local state, derivations, or nested loops.",
                   loop->block->span, call_path, "direct keyed root or exhaustive keyed if/when roots");
        }
    }

    void slot_fills(const WidgetCall& call, const ComponentInfo& component,
                    const std::string& call_path) {
        std::vector<std::string> names;
        for (const ComponentSlot& slot : component.slots) names.push_back(slot.name);
        std::set<std::string, std::less<>> occupied;
        bool has_raw_content = false;
        if (call.body != nullptr) {
            for (const StatementPtr& statement : call.body->statements) {
                const auto* widget = std::get_if<WidgetStatement>(&statement->node);
                if (widget == nullptr || !is_slot_widget(registry_, widget->call.name)) {
                    if (!std::holds_alternative<StateStatement>(statement->node) &&
                        !std::holds_alternative<DerivedStatement>(statement->node)) {
                        has_raw_content = true;
                    }
                    continue;
                }
                const Expression* name_expression = nullptr;
                for (const Argument& argument : widget->call.arguments) {
                    if (argument.name == "name" ||
                        (!argument.name.has_value() && name_expression == nullptr)) {
                        name_expression = argument.value.get();
                    }
                }
                const std::string* name =
                    name_expression != nullptr ? string_literal_value(*name_expression) : nullptr;
                if (name == nullptr) {
                    report("STRATA.DSL.SEMANTIC_DYNAMIC_SLOT_NAME",
                           "Component slot fill names must be string literals.",
                           name_expression != nullptr ? name_expression->span : widget->call.span,
                           call_path, expected_names(names));
                    continue;
                }
                if (std::ranges::find(names, *name) == names.end()) {
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_SLOT",
                           "Component does not declare slot '" + *name + "'." +
                               did_you_mean(*name, names),
                           widget->call.span, call_path, expected_names(names));
                } else if (!occupied.insert(*name).second) {
                    report("STRATA.DSL.SEMANTIC_DUPLICATE_SLOT_FILL",
                           "Slot '" + *name + "' is filled more than once.", widget->call.span,
                           call_path, "at most one fill for '" + *name + "'");
                }
            }
        }
        if (has_raw_content) {
            const ComponentSlot* shorthand = nullptr;
            if (component.slots.size() == 1U) {
                shorthand = &component.slots.front();
            } else {
                const auto found =
                    std::ranges::find(component.slots, std::string("content"), &ComponentSlot::name);
                if (found != component.slots.end()) shorthand = &*found;
            }
            const SourceSpan& span = call.body != nullptr ? call.body->span : call.span;
            if (shorthand == nullptr) {
                report("STRATA.DSL.SEMANTIC_AMBIGUOUS_SLOT_SHORTHAND",
                       "Raw child content is ambiguous for the component; fill a named Slot "
                       "explicitly.",
                       span, call_path, expected_names(names));
            } else if (!occupied.insert(shorthand->name).second) {
                report("STRATA.DSL.SEMANTIC_DUPLICATE_SLOT_FILL",
                       "Slot '" + shorthand->name + "' is filled by both shorthand and a named Slot.",
                       span, call_path, "one slot fill");
            }
        }
        for (const ComponentSlot& slot : component.slots) {
            if (slot.required && !occupied.contains(slot.name)) {
                report("STRATA.DSL.SEMANTIC_MISSING_REQUIRED_SLOT",
                       "Required slot '" + slot.name + "' is not filled.", call.span, call_path,
                       "Slot(name: \"" + slot.name + "\") { ... }");
            }
        }
    }

    /** The layout a widget ends up with: its component default style, style and layout. */
    void widget_layout(const WidgetCall& call, const ResolvedWidgetCall& resolved,
                       const std::string& path) {
        LayoutPropertyMap properties;
        std::set<std::string, std::less<>> resolving;
        const bool has_authored_style = std::ranges::any_of(
            resolved.arguments, [](const ArgumentBinding& argument) {
                return argument.parameter != nullptr && argument.parameter->name == "style";
            });
        if (!has_authored_style) {
            for (auto defaults = default_styles_.rbegin(); defaults != default_styles_.rend();
                 ++defaults) {
                const auto style = (*defaults)->find(call.name);
                if (style == (*defaults)->end()) continue;
                merge_style(*style->second, properties, resolving);
                break;
            }
        }
        for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
            const SchemaParameter* parameter = resolved.arguments[index].parameter;
            if (parameter != nullptr && parameter->name == "style")
                merge_style(*call.arguments[index].value, properties, resolving);
        }
        for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
            const SchemaParameter* parameter = resolved.arguments[index].parameter;
            if (parameter == nullptr || parameter->name != "layout") continue;
            if (const auto* map = std::get_if<MapExpression>(&ungrouped(*call.arguments[index].value).node))
                merge_map(*map, properties);
        }
        check_layout_kind(analysis_, &oracle_, values(properties), path,
                          implicit_layout_kind(*resolved.widget));
    }

    void call(const WidgetCall& call, const std::string& path, const BlockContext context) {
        const std::string call_path = path + "/" + call.name;
        const ResolvedWidgetCall& resolved = model_.widget_calls.at(call.id);
        for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
            const Argument& argument = call.arguments[index];
            const SchemaParameter* parameter = resolved.arguments.empty()
                                                   ? nullptr
                                                   : resolved.arguments[index].parameter;
            persisted_calls(*argument.value,
                            call_path + "." +
                                (parameter != nullptr
                                     ? parameter->name
                                     : argument.name.value_or("#" + std::to_string(index))),
                            nullptr);
            if (parameter != nullptr && parameter->name == "undoLabel") {
                const std::string* label = string_literal_value(*argument.value);
                if (label == nullptr || label->empty()) {
                    report("STRATA.DSL.SEMANTIC_UNDO_LABEL_STATIC",
                           "Undoable widget bindings require a non-empty string-literal undoLabel.",
                           argument.value->span, call_path + ".undoLabel",
                           "undoLabel: \"user-facing change\"");
                }
            }
            if (parameter != nullptr && parameter->name == "persistenceKey") {
                const std::string* key = string_literal_value(*argument.value);
                if (key == nullptr || key->empty()) {
                    report("STRATA.DSL.SEMANTIC_PERSISTENCE_KEY_STATIC",
                           "Widget persistenceKey must be a non-empty string literal.",
                           argument.value->span, call_path + ".persistenceKey",
                           "stable non-empty string literal");
                } else {
                    claim_persistence_key(analysis_, *key, *argument.value, call_path,
                                          call_path + ".persistenceKey");
                }
            }
        }
        if (resolved.target == WidgetCallTarget::unknown) {
            if (call.body != nullptr) block(*call.body, call_path, nested_context(context), {});
            return;
        }
        if (resolved.lazy_rows) repeater_body(call, call_path);
        if (resolved.component != nullptr) slot_fills(call, *resolved.component, call_path);
        const WidgetSchema* widget = resolved.widget;
        if (call.body != nullptr && widget != nullptr && !widget->allows_children) {
            report("STRATA.DSL.SEMANTIC_CHILDREN_NOT_ALLOWED",
                   "Widget '" + call.name + "' does not accept child content.", call.body->span,
                   call_path, "no child block");
        }
        if (widget != nullptr) {
            const bool templated = std::ranges::any_of(
                resolved.arguments, [](const ArgumentBinding& argument) {
                    return argument.parameter != nullptr &&
                           argument.parameter->name == "presentationTemplate";
                });
            const bool keyed = std::ranges::any_of(
                resolved.arguments, [](const ArgumentBinding& argument) {
                    return argument.parameter != nullptr && argument.parameter->name == "key";
                });
            if (templated && !keyed) {
                report("STRATA.DSL.SEMANTIC_PRESENTATION_KEY_REQUIRED",
                       "Widget '" + call.name +
                           "' requires an explicit key when presentationTemplate is supplied.",
                       call.span, call_path + ".key", "key: stableKey");
            }
            widget_layout(call, resolved, call_path + ".layout");
        }
        if (call.body == nullptr) return;
        if (resolved.component != nullptr)
            default_styles_.push_back(&resolved.component->widget_default_styles);
        block(*call.body, call_path, resolved.lazy_rows ? BlockContext::lazy_ui
                                                       : nested_context(context),
              {});
        if (resolved.component != nullptr) default_styles_.pop_back();
    }

    Analysis& analysis_;
    TypeOracle& oracle_;
    const SchemaRegistry& registry_;
    SemanticModel& model_;
    std::vector<const DefaultStyles*> default_styles_;
};

} // namespace

void check_policies(Analysis& analysis, TypeOracle& oracle) {
    PolicyChecker(analysis, oracle).run();
}

} // namespace strata::compiler::semantic
