#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "compiler/semantic/analysis.hpp"

namespace strata::compiler {

const SchemaParameter* ComponentInfo::find_parameter(const std::string_view name) const noexcept {
    const auto found = std::ranges::find_if(parameters, [name](const SchemaParameter& parameter) {
        return parameter.accepts_name(name);
    });
    return found != parameters.end() ? &*found : nullptr;
}

const ExpressionFacts& SemanticModel::facts(const Expression& expression) const {
    return expressions.at(expression.id);
}

const Symbol* SemanticModel::symbol(const Expression& expression) const {
    const SymbolId id = facts(expression).symbol;
    return id == no_symbol ? nullptr : &symbols.at(id);
}

const ResolvedCall* SemanticModel::call(const Expression& expression) const {
    const std::uint32_t index = facts(expression).call;
    return index < calls.size() ? &calls[index] : nullptr;
}

const ResolvedWidgetCall* SemanticModel::widget_call(const WidgetCall& call) const {
    const auto found = widget_calls.find(call.id);
    return found != widget_calls.end() ? &found->second : nullptr;
}

const Placement& SemanticModel::placement(const NodeId id) const {
    return placements.at(id);
}

const Symbol& SemanticModel::origin(SymbolId id) const {
    while (symbols.at(id).narrows != no_symbol) id = symbols[id].narrows;
    return symbols[id];
}

} // namespace strata::compiler

namespace strata::compiler::semantic {

SemanticTypePtr simple(const SemanticTypeKind kind) {
    auto type = std::make_shared<SemanticType>();
    type->kind = kind;
    return type;
}

SemanticTypePtr literal_string(std::string value) {
    auto type = std::make_shared<SemanticType>();
    type->kind = SemanticTypeKind::string_literal;
    type->literal = std::move(value);
    return type;
}

std::string lower(const std::string_view value) {
    std::string normalized(value);
    std::ranges::transform(normalized, normalized.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return normalized;
}

std::string expected_names(std::vector<std::string> names) {
    std::ranges::sort(names);
    names.erase(std::ranges::unique(names).begin(), names.end());
    if (names.empty()) return "none";
    std::string output;
    for (std::size_t index = 0U; index < names.size(); ++index) {
        if (index != 0U) output += ", ";
        output += names[index];
    }
    return output;
}

namespace {

/**
 * Edit distance between two names, case-insensitively, counting a swapped pair of adjacent letters
 * as one edit (the commonest typo); stops counting past `limit`.
 */
[[nodiscard]] std::size_t name_distance(
    const std::string_view left,
    const std::string_view right,
    const std::size_t limit
) {
    const std::size_t length_gap =
        left.size() > right.size() ? left.size() - right.size() : right.size() - left.size();
    if (length_gap > limit) return limit + 1U;
    const auto folded = [](const char character) {
        return std::tolower(static_cast<unsigned char>(character));
    };
    std::vector<std::size_t> before_previous(right.size() + 1U);
    std::vector<std::size_t> previous(right.size() + 1U);
    std::vector<std::size_t> current(right.size() + 1U);
    for (std::size_t column = 0U; column <= right.size(); ++column) previous[column] = column;
    for (std::size_t row = 1U; row <= left.size(); ++row) {
        current[0] = row;
        std::size_t row_minimum = current[0];
        for (std::size_t column = 1U; column <= right.size(); ++column) {
            const bool same = folded(left[row - 1U]) == folded(right[column - 1U]);
            current[column] = std::min({previous[column] + 1U, current[column - 1U] + 1U,
                                        previous[column - 1U] + (same ? 0U : 1U)});
            if (row > 1U && column > 1U && folded(left[row - 1U]) == folded(right[column - 2U]) &&
                folded(left[row - 2U]) == folded(right[column - 1U])) {
                current[column] = std::min(current[column], before_previous[column - 2U] + 1U);
            }
            row_minimum = std::min(row_minimum, current[column]);
        }
        if (row_minimum > limit) return limit + 1U;
        std::swap(before_previous, previous);
        std::swap(previous, current);
    }
    return previous[right.size()];
}

} // namespace

/**
 * The closest candidate within one edit for short names and two otherwise. Ties go to the
 * alphabetically first so suggestions are stable.
 */
std::optional<std::string> nearest_name(
    const std::string_view name,
    const std::vector<std::string>& candidates
) {
    const std::size_t limit = name.size() <= 4U ? 1U : 2U;
    std::optional<std::string> best;
    std::size_t best_distance = limit + 1U;
    for (const std::string& candidate : candidates) {
        if (candidate == name) continue;
        const std::size_t distance = name_distance(name, candidate, limit);
        if (distance < best_distance ||
            (distance == best_distance && best.has_value() && candidate < *best)) {
            best = candidate;
            best_distance = distance;
        }
    }
    if (best_distance > limit) return std::nullopt;
    return best;
}

std::string did_you_mean(const std::string_view name, const std::vector<std::string>& candidates) {
    const std::optional<std::string> suggestion = nearest_name(name, candidates);
    return suggestion.has_value() ? " Did you mean '" + *suggestion + "'?" : std::string{};
}

const std::string* string_literal_value(const Expression& expression) {
    const auto* literal = std::get_if<LiteralExpression>(&expression.node);
    if (literal == nullptr) return nullptr;
    const auto* string = std::get_if<StringLiteral>(&literal->value);
    return string == nullptr ? nullptr : &string->value;
}

std::optional<bool> boolean_literal_value(const Expression& expression) {
    const auto* literal = std::get_if<LiteralExpression>(&expression.node);
    if (literal == nullptr) return std::nullopt;
    const auto* boolean = std::get_if<BooleanLiteral>(&literal->value);
    return boolean == nullptr ? std::nullopt : std::optional<bool>(boolean->value);
}

bool type_matches(const SemanticType& expected, const SemanticType& actual, const bool nullable) {
    if (actual.kind == SemanticTypeKind::unknown) return true;
    if (actual.kind == SemanticTypeKind::null_value) return nullable;
    if (actual.kind == SemanticTypeKind::union_value) {
        return std::ranges::all_of(
            actual.options, [&expected, nullable](const SemanticTypePtr& option) {
                return option != nullptr && type_matches(expected, *option, nullable);
            });
    }
    if (expected.kind == SemanticTypeKind::unsafe_component_parameter) return false;
    if (expected.kind == SemanticTypeKind::state_binding) {
        const SemanticType* actual_value =
            actual.kind == SemanticTypeKind::state_binding ? actual.value.get() : &actual;
        return expected.value != nullptr && actual_value != nullptr &&
               type_matches(*expected.value, *actual_value, nullable);
    }
    if (actual.kind == SemanticTypeKind::state_binding) {
        return actual.value != nullptr && type_matches(expected, *actual.value, nullable);
    }
    return expected.accepts(actual);
}

const std::string* static_map_key(const MapKey& key) {
    if (const auto* identifier = std::get_if<IdentifierMapKey>(&key)) return &identifier->name;
    if (const auto* string = std::get_if<StringMapKey>(&key)) return &string->value;
    return nullptr;
}

const SourceSpan& map_key_span(const MapKey& key) {
    return std::visit([](const auto& value) -> const SourceSpan& { return value.span; }, key);
}

const Expression& ungrouped(const Expression& expression) {
    const Expression* current = &expression;
    while (const auto* grouping = std::get_if<GroupingExpression>(&current->node)) {
        current = grouping->expression.get();
    }
    return *current;
}

void for_each_child(const Expression& expression,
                    const std::function<void(const Expression&)>& visit) {
    std::visit(
        [&visit](const auto& node) {
            using Node = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<Node, GroupingExpression>) {
                visit(*node.expression);
            } else if constexpr (std::is_same_v<Node, ListExpression>) {
                for (const ExpressionPtr& element : node.elements) visit(*element);
            } else if constexpr (std::is_same_v<Node, MapExpression>) {
                for (const MapEntry& entry : node.entries) visit(*entry.value);
            } else if constexpr (std::is_same_v<Node, UnaryExpression>) {
                visit(*node.operand);
            } else if constexpr (std::is_same_v<Node, BinaryExpression>) {
                visit(*node.left);
                visit(*node.right);
            } else if constexpr (std::is_same_v<Node, ConditionalExpression>) {
                visit(*node.condition);
                visit(*node.then_expression);
                visit(*node.else_expression);
            } else if constexpr (std::is_same_v<Node, PropertyAccessExpression>) {
                visit(*node.receiver);
            } else if constexpr (std::is_same_v<Node, IndexExpression>) {
                visit(*node.receiver);
                visit(*node.index);
            } else if constexpr (std::is_same_v<Node, CallExpression>) {
                for (const Argument& argument : node.arguments) visit(*argument.value);
            } else if constexpr (std::is_same_v<Node, LambdaExpression>) {
                if (node.body != nullptr) visit(*node.body);
                if (node.block != nullptr) for_each_block_expression(*node.block, visit);
            } else if constexpr (std::is_same_v<Node, SpreadExpression>) {
                visit(*node.operand);
            } else if constexpr (std::is_same_v<Node, AwaitExpression>) {
                visit(*node.operand);
            }
        },
        expression.node);
}

FunctionInfo function_info(const FunctionDeclaration& function, const SchemaRegistry& registry) {
    FunctionInfo info;
    info.declaration = &function;
    for (const Parameter& parameter : function.parameters) {
        SchemaParameter schema;
        schema.name = parameter.name;
        schema.type = parameter.type_reference.has_value()
                          ? resolve_type_reference(*parameter.type_reference, registry)
                          : simple(SemanticTypeKind::any);
        schema.required = parameter.default_value == nullptr;
        schema.nullable = parameter.type_reference.has_value() && parameter.type_reference->nullable;
        info.parameters.push_back(std::move(schema));
    }
    return info;
}

void for_each_block_expression(const Block& block,
                               const std::function<void(const Expression&)>& visit) {
    for (const StatementPtr& statement : block.statements) {
        std::visit(
            [&visit](const auto& node) {
                using Node = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<Node, LetStatement>) {
                    visit(*node.initializer);
                } else if constexpr (std::is_same_v<Node, AssignmentStatement>) {
                    visit(*node.target);
                    visit(*node.value);
                } else if constexpr (std::is_same_v<Node, ExpressionStatement>) {
                    visit(*node.expression);
                } else if constexpr (std::is_same_v<Node, WhileStatement>) {
                    visit(*node.condition);
                    for_each_block_expression(*node.block, visit);
                } else if constexpr (std::is_same_v<Node, ReturnStatement>) {
                    if (node.value != nullptr) visit(*node.value);
                } else if constexpr (std::is_same_v<Node, ThrowStatement>) {
                    visit(*node.value);
                } else if constexpr (std::is_same_v<Node, TryStatement>) {
                    for_each_block_expression(*node.block, visit);
                    for_each_block_expression(*node.catch_block, visit);
                } else if constexpr (std::is_same_v<Node, IfStatement>) {
                    visit(*node.condition);
                    for_each_block_expression(*node.then_block, visit);
                    if (node.else_block != nullptr) for_each_block_expression(*node.else_block, visit);
                } else if constexpr (std::is_same_v<Node, ForStatement>) {
                    visit(*node.collection);
                    if (node.filter != nullptr) visit(*node.filter);
                    for_each_block_expression(*node.block, visit);
                } else if constexpr (std::is_same_v<Node, WhenStatement>) {
                    visit(*node.subject);
                    for (const WhenBranch& branch : node.branches) {
                        if (branch.match != nullptr) visit(*branch.match);
                        for_each_block_expression(*branch.block, visit);
                    }
                } else if constexpr (std::is_same_v<Node, FunctionStatement>) {
                    if (node.function.expression_body != nullptr) visit(*node.function.expression_body);
                    if (node.function.block_body != nullptr)
                        for_each_block_expression(*node.function.block_body, visit);
                }
            },
            statement->node);
    }
}

std::optional<StaticNumber> static_number(const Expression& expression) {
    const Expression& value = ungrouped(expression);
    bool negate = false;
    const Expression* literal_expression = &value;
    if (const auto* unary = std::get_if<UnaryExpression>(&value.node);
        unary != nullptr && unary->operation == UnaryOperator::negate) {
        negate = true;
        literal_expression = &ungrouped(*unary->operand);
    }
    const auto* literal = std::get_if<LiteralExpression>(&literal_expression->node);
    const auto* number = literal != nullptr ? std::get_if<NumberLiteral>(&literal->value) : nullptr;
    if (number == nullptr) return std::nullopt;
    std::string raw = number->raw;
    std::erase(raw, '_');
    double parsed = 0.0;
    const auto converted = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
    if (converted.ec != std::errc{} || converted.ptr != raw.data() + raw.size() ||
        !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return StaticNumber{negate ? -parsed : parsed, number->unit};
}

const LiteralExpression* static_literal(const Expression& expression) {
    return std::get_if<LiteralExpression>(&ungrouped(expression).node);
}

std::string canonical_color(std::string raw) {
    if (!raw.empty() && raw.front() == '#') raw.erase(raw.begin());
    std::ranges::transform(raw, raw.begin(), [](const unsigned char character) {
        return character >= 'A' && character <= 'F' ? static_cast<char>(character - 'A' + 'a')
                                                    : static_cast<char>(character);
    });
    if (raw.size() == 6U) raw += "ff";
    return raw;
}

const WidgetCall* statement_call(const Statement& statement) {
    if (const auto* widget = std::get_if<WidgetStatement>(&statement.node)) return &widget->call;
    if (const auto* root = std::get_if<RootStatement>(&statement.node)) return &root->call;
    return nullptr;
}

namespace {

[[nodiscard]] std::size_t capped_root_sum(const std::size_t left, const std::size_t right) {
    return std::min<std::size_t>(2U, left + right);
}

} // namespace

RootCardinality root_cardinality(const Block& block) {
    RootCardinality total;
    for (const StatementPtr& statement : block.statements) {
        RootCardinality contribution;
        if (std::holds_alternative<WidgetStatement>(statement->node) ||
            std::holds_alternative<RootStatement>(statement->node)) {
            contribution = {1U, 1U, false};
        } else if (std::holds_alternative<ErrorStatement>(statement->node)) {
            contribution.uncertain = true;
        } else if (const auto* conditional = std::get_if<IfStatement>(&statement->node)) {
            const RootCardinality then_value = root_cardinality(*conditional->then_block);
            const RootCardinality else_value = conditional->else_block != nullptr
                                                   ? root_cardinality(*conditional->else_block)
                                                   : RootCardinality{};
            contribution = {
                std::min(then_value.minimum, else_value.minimum),
                std::max(then_value.maximum, else_value.maximum),
                then_value.uncertain || else_value.uncertain,
            };
        } else if (const auto* when = std::get_if<WhenStatement>(&statement->node)) {
            if (!when->branches.empty()) {
                contribution = root_cardinality(*when->branches.front().block);
                for (std::size_t index = 1U; index < when->branches.size(); ++index) {
                    const RootCardinality branch = root_cardinality(*when->branches[index].block);
                    contribution.minimum = std::min(contribution.minimum, branch.minimum);
                    contribution.maximum = std::max(contribution.maximum, branch.maximum);
                    contribution.uncertain = contribution.uncertain || branch.uncertain;
                }
            }
        } else if (std::holds_alternative<ForStatement>(statement->node)) {
            contribution = {0U, 2U, false};
        }
        total.minimum = capped_root_sum(total.minimum, contribution.minimum);
        total.maximum = capped_root_sum(total.maximum, contribution.maximum);
        total.uncertain = total.uncertain || contribution.uncertain;
    }
    return total;
}

SymbolId binding_target(const SemanticModel& model, const Expression& value) {
    if (!std::holds_alternative<IdentifierExpression>(value.node)) return no_symbol;
    const SymbolId id = model.expressions.at(value.id).symbol;
    if (id == no_symbol) return no_symbol;
    const Symbol& symbol = model.origin(id);
    if (symbol.kind == SymbolKind::state) return id;
    if (symbol.kind == SymbolKind::parameter && symbol.component != nullptr &&
        symbol.component->parameters[symbol.parameter_index].type->kind ==
            SemanticTypeKind::state_binding) {
        return id;
    }
    return no_symbol;
}

bool is_slot_widget(const SchemaRegistry& registry, const std::string_view name) {
    const WidgetSchema* widget = registry.widget(name);
    return widget != nullptr && widget->slot;
}

bool generates_lazy_rows(const WidgetSchema& widget) {
    return widget.lazy_rows;
}

std::string_view implicit_layout_kind(const WidgetSchema& widget) {
    return widget.layout_kind;
}

bool MissingDeclarations::may_declare(const std::string_view name, const bool module_level) const {
    if (truncated || (imports && module_level)) return true;
    return unparsed_names != nullptr && std::ranges::binary_search(*unparsed_names, name);
}

void Diagnostics::report_unresolved(
    std::string code,
    const std::string_view name,
    const bool module_level,
    std::string message,
    const SourceSpan& span,
    std::string component_path,
    std::string expected
) {
    if (missing_.may_declare(name, module_level)) return;
    report(std::move(code), std::move(message), span, std::move(component_path),
           std::move(expected));
}

void Diagnostics::report(
    std::string code,
    std::string message,
    const SourceSpan& span,
    std::string component_path,
    std::string expected
) {
    if (!reported_.emplace(code, message, span.source_id, span.start.offset, span.end.offset)
             .second) {
        return;
    }
    diagnostics_.push_back(Diagnostic{
        std::move(code),
        DiagnosticSeverity::error,
        std::move(message),
        span.range(),
        std::move(component_path),
        std::move(expected),
    });
}

Analysis::Analysis(const File& analysed, const SchemaRegistry& schemas, SemanticModel& output)
    : file(analysed), registry(schemas), model(output) {
    model.expressions.assign(file.node_id_limit, ExpressionFacts{});
    model.placements.assign(file.node_id_limit, Placement{});
}

ExpressionFacts& Analysis::facts(const Expression& expression) {
    if (expression.id >= model.expressions.size()) {
        throw std::logic_error("semantic analysis received an expression outside its file");
    }
    return model.expressions[expression.id];
}

SymbolId Analysis::add_symbol(Symbol symbol) {
    model.symbols.push_back(std::move(symbol));
    return static_cast<SymbolId>(model.symbols.size() - 1U);
}

} // namespace strata::compiler::semantic
