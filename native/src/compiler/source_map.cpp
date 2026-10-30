#include "compiler/source_map.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace strata::compiler {
namespace {

/**
 * Lists the authored nodes with the IR path and runtime component path analysis placed them at,
 * so a runtime path or a compiled IR path leads back to source.
 */
class Builder final {
public:
    Builder(const File& file, const SemanticModel& model) : file_(file), model_(model) {}

    [[nodiscard]] CompiledSourceMap build() {
        for (std::size_t index = 0U; index < file_.declarations.size(); ++index)
            declaration(file_.declarations[index], model_.declaration_placements.at(index));
        return CompiledSourceMap{file_.source_id, std::move(entries_)};
    }

private:
    void add(const Placement& placement, std::string kind, std::optional<std::string> name,
             const SourceSpan& span) {
        if (placement.path.empty()) return;
        entries_.push_back(CompiledSourceMapEntry{
            placement.path,
            std::move(kind),
            std::move(name),
            span,
            placement.component_path,
        });
    }

    void declaration(const Declaration& declaration, const Placement& placement) {
        if (const auto* screen = std::get_if<ScreenDeclaration>(&declaration.node)) {
            add(placement, "screen", screen->name, declaration.span);
            block(*screen->body);
        } else if (const auto* overlay = std::get_if<OverlayDeclaration>(&declaration.node)) {
            add(placement, "overlay", overlay->name, declaration.span);
            block(*overlay->body);
        } else if (const auto* component = std::get_if<ComponentDeclaration>(&declaration.node)) {
            add(placement, "component", component->name, declaration.span);
            for (const Parameter& parameter : component->parameters) {
                if (parameter.default_value != nullptr) expression(*parameter.default_value);
            }
            block(*component->body);
        } else if (const auto* style = std::get_if<StyleDeclaration>(&declaration.node)) {
            add(placement, "style", style->name, declaration.span);
            for (const Property& property : style->properties) expression(*property.value);
        } else if (const auto* function = std::get_if<FunctionDeclaration>(&declaration.node)) {
            add(placement, "function", function->name, declaration.span);
            function_body(*function);
        }
        // Animations compile to a separate table and intentionally have no source-map entries.
    }

    void block(const Block& body) {
        add(model_.placement(body.id), "block", std::nullopt, body.span);
        for (const StatementPtr& pointer : body.statements) {
            const Statement& statement = *pointer;
            if (const auto* state = std::get_if<StateStatement>(&statement.node)) {
                if (state->initializer != nullptr) expression(*state->initializer);
                add(model_.placement(statement.id), "state", state->name, statement.span);
            } else if (const auto* derived = std::get_if<DerivedStatement>(&statement.node)) {
                expression(*derived->expression);
                add(model_.placement(statement.id), "derived", derived->name, statement.span);
            } else if (const auto* widget = std::get_if<WidgetStatement>(&statement.node)) {
                call(widget->call);
            } else if (const auto* root = std::get_if<RootStatement>(&statement.node)) {
                call(root->call);
            } else if (const auto* conditional = std::get_if<IfStatement>(&statement.node)) {
                expression(*conditional->condition);
                block(*conditional->then_block);
                if (conditional->else_block != nullptr) block(*conditional->else_block);
            } else if (const auto* when = std::get_if<WhenStatement>(&statement.node)) {
                expression(*when->subject);
                for (const WhenBranch& branch : when->branches) {
                    if (branch.match != nullptr) expression(*branch.match);
                    block(*branch.block);
                }
            } else if (const auto* loop = std::get_if<ForStatement>(&statement.node)) {
                expression(*loop->collection);
                if (loop->filter != nullptr) expression(*loop->filter);
                block(*loop->block);
            } else if (const auto* function = std::get_if<FunctionStatement>(&statement.node)) {
                add(model_.placement(statement.id), "function", function->function.name,
                    statement.span);
                function_body(function->function);
            } else if (const auto* let = std::get_if<LetStatement>(&statement.node)) {
                expression(*let->initializer);
                add(model_.placement(statement.id), "local", let->name, statement.span);
            } else if (const auto* assignment = std::get_if<AssignmentStatement>(&statement.node)) {
                expression(*assignment->target);
                expression(*assignment->value);
                add(model_.placement(statement.id), "statement", std::nullopt, statement.span);
            } else if (const auto* effect = std::get_if<ExpressionStatement>(&statement.node)) {
                expression(*effect->expression);
                add(model_.placement(statement.id), "statement", std::nullopt, statement.span);
            } else if (const auto* repeat = std::get_if<WhileStatement>(&statement.node)) {
                expression(*repeat->condition);
                block(*repeat->block);
            } else if (const auto* returned = std::get_if<ReturnStatement>(&statement.node)) {
                if (returned->value != nullptr) expression(*returned->value);
                add(model_.placement(statement.id), "statement", std::nullopt, statement.span);
            } else if (const auto* thrown = std::get_if<ThrowStatement>(&statement.node)) {
                expression(*thrown->value);
                add(model_.placement(statement.id), "statement", std::nullopt, statement.span);
            } else if (const auto* attempt = std::get_if<TryStatement>(&statement.node)) {
                block(*attempt->block);
                block(*attempt->catch_block);
            }
        }
    }

    void function_body(const FunctionDeclaration& function) {
        for (const Parameter& parameter : function.parameters) {
            if (parameter.default_value != nullptr) expression(*parameter.default_value);
        }
        if (function.expression_body != nullptr) expression(*function.expression_body);
        if (function.block_body != nullptr) block(*function.block_body);
    }

    void call(const WidgetCall& call) {
        for (const Argument& argument : call.arguments) expression(*argument.value);
        const ResolvedWidgetCall* resolved = model_.widget_call(call);
        const bool widget = resolved != nullptr && resolved->target == WidgetCallTarget::widget;
        add(model_.placement(call.id), widget ? "widget" : "component", call.name, call.span);
        if (call.body != nullptr) block(*call.body);
    }

    void expression(const Expression& expression) {
        const Placement& placement = model_.placement(expression.id);
        if (placement.path.empty()) return;
        add(placement, "expression", std::nullopt, expression.span);
        if (const auto* property = std::get_if<PropertyAccessExpression>(&expression.node)) {
            if (model_.facts(expression).form != ExpressionForm::theme_token)
                this->expression(*property->receiver);
        } else if (const auto* grouping = std::get_if<GroupingExpression>(&expression.node)) {
            this->expression(*grouping->expression);
        } else if (const auto* list = std::get_if<ListExpression>(&expression.node)) {
            for (const ExpressionPtr& element : list->elements) this->expression(*element);
        } else if (const auto* map = std::get_if<MapExpression>(&expression.node)) {
            for (const MapEntry& entry : map->entries) this->expression(*entry.value);
        } else if (const auto* unary = std::get_if<UnaryExpression>(&expression.node)) {
            this->expression(*unary->operand);
        } else if (const auto* binary = std::get_if<BinaryExpression>(&expression.node)) {
            this->expression(*binary->left);
            this->expression(*binary->right);
        } else if (const auto* conditional = std::get_if<ConditionalExpression>(&expression.node)) {
            this->expression(*conditional->condition);
            this->expression(*conditional->then_expression);
            this->expression(*conditional->else_expression);
        } else if (const auto* indexed = std::get_if<IndexExpression>(&expression.node)) {
            this->expression(*indexed->receiver);
            this->expression(*indexed->index);
        } else if (const auto* call = std::get_if<CallExpression>(&expression.node)) {
            // An action's or material's id is part of its own node; its other arguments are named.
            const std::string target = call->target.qualified_name();
            const bool identified = target == "action" || target == "material";
            for (std::size_t index = identified ? 1U : 0U; index < call->arguments.size(); ++index) {
                const Argument& argument = call->arguments[index];
                if (identified && !argument.name.has_value()) continue;
                this->expression(*argument.value);
            }
        } else if (const auto* lambda = std::get_if<LambdaExpression>(&expression.node)) {
            if (lambda->body != nullptr) this->expression(*lambda->body);
            if (lambda->block != nullptr) block(*lambda->block);
        } else if (const auto* spread = std::get_if<SpreadExpression>(&expression.node)) {
            this->expression(*spread->operand);
        } else if (const auto* awaited = std::get_if<AwaitExpression>(&expression.node)) {
            this->expression(*awaited->operand);
        }
    }

    const File& file_;
    const SemanticModel& model_;
    std::vector<CompiledSourceMapEntry> entries_;
};

} // namespace

CompiledSourceMap build_compiled_source_map(const File& file, const SemanticModel& model) {
    return Builder(file, model).build();
}

} // namespace strata::compiler
