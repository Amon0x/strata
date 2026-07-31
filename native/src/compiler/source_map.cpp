#include "compiler/source_map.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace strata::compiler {
namespace {

[[nodiscard]] std::string child(const std::string_view path, const std::string_view segment) {
    if (path == "/") {
        return "/" + std::string(segment);
    }
    return std::string(path) + "/" + std::string(segment);
}

[[nodiscard]] std::optional<std::string> static_map_key(const MapKey& key) {
    if (const auto* identifier = std::get_if<IdentifierMapKey>(&key)) return identifier->name;
    if (const auto* string = std::get_if<StringMapKey>(&key)) return string->value;
    return std::nullopt;
}

class Builder final {
public:
    Builder(std::string source_id, const std::vector<std::string>& widget_names)
        : source_id_(std::move(source_id)), widget_names_(widget_names) {}

    [[nodiscard]] CompiledSourceMap build(const File& file) {
        for (const Declaration& declaration : file.declarations) {
            compile_declaration(declaration);
        }
        return CompiledSourceMap{std::move(source_id_), std::move(entries_)};
    }

private:
    void add(
        std::string path,
        std::string kind,
        std::optional<std::string> name,
        const SourceSpan& span,
        std::string runtime_path
    ) {
        entries_.push_back(CompiledSourceMapEntry{
            std::move(path),
            std::move(kind),
            std::move(name),
            span,
            std::move(runtime_path),
        });
    }

    void compile_declaration(const Declaration& declaration) {
        if (const auto* screen = std::get_if<ScreenDeclaration>(&declaration.node)) {
            const std::string path = "/screen/" + screen->name;
            const std::string runtime = "screen " + screen->name;
            add(path, "screen", screen->name, declaration.span, runtime);
            compile_block(*screen->body, child(path, "body"), runtime);
        } else if (const auto* overlay = std::get_if<OverlayDeclaration>(&declaration.node)) {
            const std::string path = "/overlay/" + overlay->name;
            const std::string runtime = "overlay " + overlay->name;
            add(path, "overlay", overlay->name, declaration.span, runtime);
            compile_block(*overlay->body, child(path, "body"), runtime);
        } else if (const auto* component = std::get_if<ComponentDeclaration>(&declaration.node)) {
            const std::string path = "/component/" + component->name;
            const std::string runtime = "component " + component->name;
            add(path, "component", component->name, declaration.span, runtime);
            for (const Parameter& parameter : component->parameters) {
                if (parameter.default_value != nullptr) {
                    compile_expression(
                        *parameter.default_value,
                        child(child(child(path, "parameter"), parameter.name), "default"),
                        runtime + "(" + parameter.name + ")"
                    );
                }
            }
            compile_block(*component->body, child(path, "body"), runtime);
        } else if (const auto* style = std::get_if<StyleDeclaration>(&declaration.node)) {
            const std::string path = "/style/" + style->name;
            const std::string runtime = "style " + style->name;
            add(path, "style", style->name, declaration.span, runtime);
            for (const Property& property : style->properties) {
                compile_expression(
                    *property.value,
                    child(path, property.name),
                    runtime + "." + property.name
                );
            }
        }
        /* Animations compile to a separate table and intentionally have no source-map entries. */
    }

    void compile_block(const Block& block, const std::string& path, const std::string& runtime) {
        add(path, "block", std::nullopt, block.span, runtime);
        for (std::size_t index = 0U; index < block.statements.size(); ++index) {
            const Statement& statement = *block.statements[index];
            const std::string statement_path = child(path, std::to_string(index));
            if (const auto* state = std::get_if<StateStatement>(&statement.node)) {
                if (state->initializer != nullptr) {
                    compile_expression(
                        *state->initializer,
                        child(statement_path, "initializer"),
                        runtime + "/" + state->name
                    );
                }
                add(statement_path, "state", state->name, statement.span, runtime + "/" + state->name);
            } else if (const auto* derived = std::get_if<DerivedStatement>(&statement.node)) {
                compile_expression(
                    *derived->expression,
                    child(statement_path, "expression"),
                    runtime + "/" + derived->name
                );
                add(
                    statement_path,
                    "derived",
                    derived->name,
                    statement.span,
                    runtime + "/" + derived->name
                );
            } else if (const auto* widget = std::get_if<WidgetStatement>(&statement.node)) {
                compile_call(widget->call, statement_path, runtime + "/" + widget->call.name);
            } else if (const auto* root = std::get_if<RootStatement>(&statement.node)) {
                compile_call(
                    root->call,
                    child(statement_path, "root"),
                    runtime + "/root/" + root->call.name
                );
            } else if (const auto* conditional = std::get_if<IfStatement>(&statement.node)) {
                compile_expression(
                    *conditional->condition,
                    child(statement_path, "condition"),
                    runtime + "/if"
                );
                compile_block(*conditional->then_block, child(statement_path, "then"), runtime + "/then");
                if (conditional->else_block != nullptr) {
                    compile_block(*conditional->else_block, child(statement_path, "else"), runtime + "/else");
                }
            } else if (const auto* when = std::get_if<WhenStatement>(&statement.node)) {
                compile_expression(
                    *when->subject,
                    child(statement_path, "subject"),
                    runtime + "/when"
                );
                for (std::size_t branch_index = 0U;
                     branch_index < when->branches.size();
                     ++branch_index) {
                    const WhenBranch& branch = when->branches[branch_index];
                    const std::string branch_path = child(
                        child(statement_path, "branch"),
                        std::to_string(branch_index)
                    );
                    const std::string branch_runtime =
                        runtime + "/when[" + std::to_string(branch_index) + "]";
                    if (branch.match != nullptr) {
                        compile_expression(
                            *branch.match,
                            child(branch_path, "match"),
                            branch_runtime
                        );
                    }
                    compile_block(*branch.block, branch_path, branch_runtime);
                }
            } else if (const auto* loop = std::get_if<ForStatement>(&statement.node)) {
                compile_expression(
                    *loop->collection,
                    child(statement_path, "collection"),
                    runtime + "/for"
                );
                if (loop->filter != nullptr) {
                    compile_expression(
                        *loop->filter,
                        child(statement_path, "filter"),
                        runtime + "/for/filter"
                    );
                }
                compile_block(
                    *loop->block,
                    child(statement_path, "body"),
                    runtime + "/" + loop->item_name
                );
            }
        }
    }

    void compile_call(const WidgetCall& call, const std::string& path, const std::string& runtime) {
        for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
            const Argument& argument = call.arguments[index];
            const std::string argument_name =
                argument.name.value_or("#" + std::to_string(index));
            compile_expression(
                *argument.value,
                child(child(path, "arguments"), argument_name),
                runtime + "." + argument_name
            );
        }
        const bool widget =
            std::ranges::find(widget_names_, call.name) != widget_names_.end();
        add(path, widget ? "widget" : "component", call.name, call.span, runtime);
        if (call.body != nullptr) {
            compile_block(*call.body, child(path, "children"), runtime);
        }
    }

    void compile_expression(
        const Expression& expression,
        const std::string& path,
        const std::string& runtime
    ) {
        add(path, "expression", std::nullopt, expression.span, runtime);
        if (const auto* property = std::get_if<PropertyAccessExpression>(&expression.node)) {
            const auto* receiver = std::get_if<IdentifierExpression>(&property->receiver->node);
            if (receiver != nullptr && receiver->name == "theme") return;
            compile_expression(*property->receiver, child(path, "receiver"), runtime + ".receiver");
        } else if (const auto* grouping = std::get_if<GroupingExpression>(&expression.node)) {
            compile_expression(*grouping->expression, child(path, "grouped"), runtime);
        } else if (const auto* list = std::get_if<ListExpression>(&expression.node)) {
            for (std::size_t index = 0U; index < list->elements.size(); ++index) {
                compile_expression(
                    *list->elements[index],
                    child(path, std::to_string(index)),
                    runtime + "[" + std::to_string(index) + "]"
                );
            }
        } else if (const auto* map = std::get_if<MapExpression>(&expression.node)) {
            for (std::size_t index = 0U; index < map->entries.size(); ++index) {
                const std::string key = static_map_key(map->entries[index].key)
                                            .value_or("dynamic_" + std::to_string(index));
                compile_expression(
                    *map->entries[index].value,
                    child(path, key),
                    runtime + "." + key
                );
            }
        } else if (const auto* unary = std::get_if<UnaryExpression>(&expression.node)) {
            compile_expression(*unary->operand, child(path, "operand"), runtime);
        } else if (const auto* binary = std::get_if<BinaryExpression>(&expression.node)) {
            compile_expression(*binary->left, child(path, "left"), runtime + ".left");
            compile_expression(*binary->right, child(path, "right"), runtime + ".right");
        } else if (const auto* conditional = std::get_if<ConditionalExpression>(&expression.node)) {
            compile_expression(
                *conditional->condition,
                child(path, "condition"),
                runtime + ".condition"
            );
            compile_expression(
                *conditional->then_expression,
                child(path, "then"),
                runtime + ".then"
            );
            compile_expression(
                *conditional->else_expression,
                child(path, "else"),
                runtime + ".else"
            );
        } else if (const auto* indexed = std::get_if<IndexExpression>(&expression.node)) {
            compile_expression(*indexed->receiver, child(path, "receiver"), runtime + ".receiver");
            compile_expression(*indexed->index, child(path, "index"), runtime + ".index");
        } else if (const auto* call = std::get_if<CallExpression>(&expression.node)) {
            const std::string target = call->target.qualified_name();
            const std::size_t start = target == "action" || target == "material" ? 1U : 0U;
            for (std::size_t index = start; index < call->arguments.size(); ++index) {
                const Argument& argument = call->arguments[index];
                if ((target == "action" || target == "material") && !argument.name.has_value()) {
                    continue;
                }
                const std::string name = argument.name.value_or(std::to_string(index));
                compile_expression(
                    *argument.value,
                    child(path, name),
                    runtime + "." + name
                );
            }
        } else if (const auto* lambda = std::get_if<LambdaExpression>(&expression.node)) {
            compile_expression(
                *lambda->body,
                child(path, "body"),
                runtime + "/" + lambda->parameter_name
            );
        }
    }

    std::string source_id_;
    const std::vector<std::string>& widget_names_;
    std::vector<CompiledSourceMapEntry> entries_;
};

} // namespace

CompiledSourceMap build_compiled_source_map(
    const File& file,
    const std::vector<std::string>& widget_names
) {
    return Builder(file.source_id, widget_names).build(file);
}

} // namespace strata::compiler
