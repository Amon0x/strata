#include "compiler/parser.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "compiler/security.hpp"

namespace strata::compiler {
namespace {

template <typename Node>
[[nodiscard]] ExpressionPtr make_expression(Node node, SourceSpan span) {
    return std::make_shared<const Expression>(Expression{std::move(node), std::move(span)});
}

template <typename Node>
[[nodiscard]] StatementPtr make_statement(Node node, SourceSpan span) {
    return std::make_shared<const Statement>(Statement{std::move(node), std::move(span)});
}

} // namespace

Parser::Parser(
    std::string source_id,
    std::vector<Token> tokens,
    std::vector<Diagnostic> diagnostics,
    ParserLimits limits
)
    : source_id_(std::move(source_id)),
      diagnostics_(std::move(diagnostics)),
      limits_(limits) {
    if (limits_.max_source_bytes == 0U || limits_.max_tokens == 0U ||
        limits_.max_nesting_depth == 0U || limits_.max_declarations == 0U ||
        limits_.max_statements == 0U || limits_.max_expressions == 0U ||
        limits_.max_collection_items == 0U) {
        throw std::invalid_argument("parser limits must be positive");
    }
    tokens_.reserve(tokens.size());
    for (Token& token : tokens) {
        if (token.type == TokenType::trivia) {
            trivia_.push_back(std::move(token.span));
        } else {
            tokens_.push_back(std::move(token));
        }
    }
    if (tokens_.empty() || tokens_.back().type != TokenType::end_of_file) {
        throw std::invalid_argument("parser token stream must end with EOF");
    }
}

Parser::NestingGuard::NestingGuard(Parser& parser) : parser_(parser) {
    if (parser_.nesting_depth_ >= parser_.limits_.max_nesting_depth) {
        parser_.limit(
            "STRATA.DSL.LIMIT_NESTING_DEPTH",
            "Source exceeds the compiler nesting-depth limit."
        );
    }
    ++parser_.nesting_depth_;
}

Parser::NestingGuard::~NestingGuard() {
    --parser_.nesting_depth_;
}

ParseResult Parser::parse() {
    std::vector<Import> imports;
    std::vector<Declaration> declarations;
    const SourceSpan first_span = peek().span;
    try {
        while (!at_end()) {
            if (match(TokenType::import_keyword)) {
                count_declaration();
                imports.push_back(parse_import(previous_token()));
            } else if (declaration_start()) {
                count_declaration();
                if (auto declaration = parse_declaration()) {
                    declarations.push_back(std::move(*declaration));
                }
            } else if (match(TokenType::error)) {
                continue;
            } else {
                static_cast<void>(error(
                    peek(),
                    "STRATA.DSL.PARSE_EXPECTED_DECLARATION",
                    "Expected import, screen, overlay, component, style, or animation declaration."
                ));
                static_cast<void>(advance());
                synchronize_top_level();
            }
        }
    } catch (const LimitPanic&) {
        // A limit diagnostic is terminal for this source so recovery cannot consume more budget.
    }
    const SourceSpan file_span = covering(first_span, previous_or_peek().span);
    File file{
        source_id_,
        std::move(imports),
        std::move(declarations),
        std::move(trivia_),
        file_span,
    };
    return ParseResult{std::move(file), std::move(diagnostics_)};
}

Import Parser::parse_import(const Token& keyword) {
    std::string path;
    if (check(TokenType::string)) {
        path = decode_string(advance());
    } else {
        static_cast<void>(error(
            peek(),
            "STRATA.DSL.PARSE_IMPORT_PATH",
            "Expected string path after import."
        ));
    }
    static_cast<void>(match(TokenType::semicolon));
    return Import{std::move(path), covering(keyword.span, previous_or_peek().span)};
}

std::optional<Declaration> Parser::parse_declaration() {
    try {
        if (match(TokenType::screen)) {
            return parse_named_block_declaration(previous_token(), DeclarationKind::screen);
        }
        if (match(TokenType::overlay)) {
            return parse_named_block_declaration(previous_token(), DeclarationKind::overlay);
        }
        if (match(TokenType::component)) {
            return parse_component_declaration(previous_token());
        }
        if (match(TokenType::style)) {
            return parse_style_declaration(previous_token());
        }
        if (match(TokenType::animation)) {
            return parse_animation_declaration(previous_token());
        }
    } catch (const ParsePanic&) {
        synchronize_top_level();
    }
    return std::nullopt;
}

Declaration Parser::parse_named_block_declaration(
    const Token& keyword,
    const DeclarationKind kind
) {
    const std::string label = kind == DeclarationKind::screen ? "screen" : "overlay";
    Token name = consume_identifier(
        "STRATA.DSL.PARSE_DECLARATION_NAME",
        "Expected " + label + " name."
    );
    BlockPtr body = parse_block();
    const SourceSpan span = covering(keyword.span, body->span);
    if (kind == DeclarationKind::screen) {
        return Declaration{ScreenDeclaration{std::move(name.lexeme), std::move(body)}, span};
    }
    return Declaration{OverlayDeclaration{std::move(name.lexeme), std::move(body)}, span};
}

Declaration Parser::parse_component_declaration(const Token& keyword) {
    Token name = consume_identifier(
        "STRATA.DSL.PARSE_DECLARATION_NAME",
        "Expected component name."
    );
    std::vector<Parameter> parameters;
    if (match(TokenType::left_parenthesis)) {
        parameters = parse_parameters();
    }
    BlockPtr body = parse_block();
    const SourceSpan span = covering(keyword.span, body->span);
    return Declaration{
        ComponentDeclaration{std::move(name.lexeme), std::move(parameters), std::move(body)},
        span,
    };
}

Declaration Parser::parse_style_declaration(const Token& keyword) {
    Token name = consume_identifier(
        "STRATA.DSL.PARSE_DECLARATION_NAME",
        "Expected style name."
    );
    std::vector<StyleBase> bases;
    if (match(TokenType::extends_keyword)) {
        do {
            Token base = consume_identifier_like(
                "STRATA.DSL.PARSE_STYLE_BASE",
                "Expected a style name after 'extends'."
            );
            bases.push_back(StyleBase{std::move(base.lexeme), std::move(base.span)});
            check_collection_size(bases.size());
        } while (match(TokenType::comma));
    }
    BlockPtr body = parse_block();
    std::vector<Property> properties;
    for (const StatementPtr& statement : body->statements) {
        if (const auto* property = std::get_if<PropertyStatement>(&statement->node)) {
            properties.push_back(property->property);
        }
    }
    const SourceSpan span = covering(keyword.span, body->span);
    return Declaration{
        StyleDeclaration{
            std::move(name.lexeme),
            std::move(bases),
            std::move(properties),
            std::move(body),
        },
        span,
    };
}

Declaration Parser::parse_animation_declaration(const Token& keyword) {
    Token name = consume_identifier(
        "STRATA.DSL.PARSE_DECLARATION_NAME",
        "Expected animation name."
    );
    BlockPtr body = parse_block(BodyKind::animation);
    std::vector<AnimationEntry> entries;
    for (const StatementPtr& statement : body->statements) {
        if (const auto* frame = std::get_if<AnimationFrameStatement>(&statement->node)) {
            entries.emplace_back(frame->frame);
        } else if (const auto* property = std::get_if<PropertyStatement>(&statement->node)) {
            entries.emplace_back(AnimationProperty{property->property, statement->span});
        }
    }
    const SourceSpan span = covering(keyword.span, body->span);
    return Declaration{
        AnimationDeclaration{
            std::move(name.lexeme),
            std::move(entries),
            std::move(body),
        },
        span,
    };
}

std::vector<Parameter> Parser::parse_parameters() {
    std::vector<Parameter> parameters;
    if (!check(TokenType::right_parenthesis)) {
        do {
            Token name = consume_identifier_like(
                "STRATA.DSL.PARSE_PARAMETER_NAME",
                "Expected component parameter name."
            );
            std::optional<TypeReference> type_reference;
            if (match(TokenType::colon)) {
                type_reference = parse_type_reference();
            }
            ExpressionPtr default_value;
            if (match(TokenType::equal)) {
                default_value = expression();
            }
            const SourceSpan span = default_value != nullptr
                                        ? covering(name.span, default_value->span)
                                        : type_reference.has_value()
                                              ? covering(name.span, type_reference->span)
                                              : name.span;
            parameters.push_back(Parameter{
                std::move(name.lexeme),
                std::move(type_reference),
                std::move(default_value),
                span,
            });
            check_collection_size(parameters.size());
        } while (match(TokenType::comma));
    }
    static_cast<void>(consume(
        TokenType::right_parenthesis,
        "STRATA.DSL.PARSE_PARAMETER_LIST",
        "Expected ')' after component parameters."
    ));
    return parameters;
}

TypeReference Parser::parse_type_reference() {
    const NestingGuard nesting(*this);
    Token name = consume_name_token(
        "STRATA.DSL.PARSE_PARAMETER_TYPE",
        "Expected parameter type name after ':'."
    );
    std::vector<TypeReference> arguments;
    if (match(TokenType::less)) {
        if (!check(TokenType::greater)) {
            do {
                arguments.push_back(parse_type_reference());
                check_collection_size(arguments.size());
            } while (match(TokenType::comma));
        }
        static_cast<void>(consume(
            TokenType::greater,
            "STRATA.DSL.PARSE_PARAMETER_TYPE",
            "Expected '>' after type arguments."
        ));
    }
    const bool nullable = match(TokenType::question);
    const SourceSpan end_span = nullable
                                    ? previous_token().span
                                    : !arguments.empty() ? arguments.back().span : name.span;
    return TypeReference{
        std::move(name.lexeme),
        std::move(arguments),
        nullable,
        covering(name.span, end_span),
    };
}

BlockPtr Parser::parse_block(const BodyKind kind) {
    const NestingGuard nesting(*this);
    const Token left = consume(
        TokenType::left_brace,
        "STRATA.DSL.PARSE_BLOCK_OPEN",
        "Expected '{' to start block."
    );
    std::vector<StatementPtr> statements;
    while (!check(TokenType::right_brace) && !at_end()) {
        StatementPtr statement = parse_statement(kind);
        if (statement != nullptr) {
            statements.push_back(std::move(statement));
            check_collection_size(statements.size());
        }
    }
    const Token right = consume(
        TokenType::right_brace,
        "STRATA.DSL.PARSE_BLOCK_CLOSE",
        "Expected '}' after block."
    );
    return std::make_shared<const Block>(Block{
        std::move(statements),
        covering(left.span, right.span),
    });
}

StatementPtr Parser::parse_statement(const BodyKind kind) {
    count_statement();
    try {
        if (name_token() && check_next(TokenType::colon)) return parse_property_statement();
        if (match(TokenType::state)) return parse_state_declaration(previous_token());
        if (match(TokenType::derived)) return parse_derived_declaration(previous_token());
        if (match(TokenType::root)) return parse_root_statement(previous_token());
        if (match(TokenType::if_keyword)) return parse_if_statement(previous_token());
        if (match(TokenType::when_keyword)) return parse_when_statement(previous_token());
        if (match(TokenType::for_keyword)) return parse_for_statement(previous_token());
        if (kind == BodyKind::animation &&
            (match(TokenType::from) || match(TokenType::to))) {
            return parse_animation_frame_statement(previous_token());
        }
        if (widget_name()) return parse_widget_statement();
        if (match(TokenType::semicolon)) return nullptr;
        if (match(TokenType::error)) {
            return make_statement(ErrorStatement{}, previous_token().span);
        }
        static_cast<void>(error(
            peek(),
            "STRATA.DSL.PARSE_EXPECTED_STATEMENT",
            "Expected state, property, widget call, if, when, or for statement."
        ));
        const Token token = advance();
        synchronize_block();
        return make_statement(ErrorStatement{}, token.span);
    } catch (const ParsePanic&) {
        const SourceSpan span = previous_or_peek().span;
        synchronize_block();
        return make_statement(ErrorStatement{}, span);
    }
}

StatementPtr Parser::parse_state_declaration(const Token& keyword) {
    Token name = consume_identifier_like(
        "STRATA.DSL.PARSE_STATE_NAME",
        "Expected state name."
    );
    std::optional<TypeReference> type_reference;
    if (match(TokenType::colon)) {
        type_reference = parse_type_reference();
    }
    ExpressionPtr initializer;
    if (match(TokenType::equal)) {
        initializer = expression();
    } else {
        static_cast<void>(error(
            name,
            "STRATA.DSL.PARSE_STATE_INITIALIZER",
            "Expected '=' and initializer after state name."
        ));
    }
    static_cast<void>(match(TokenType::semicolon));
    const SourceSpan end_span = initializer != nullptr ? initializer->span : name.span;
    const SourceSpan span = covering(keyword.span, end_span);
    return make_statement(
        StateStatement{std::move(name.lexeme), std::move(type_reference), initializer},
        span
    );
}

StatementPtr Parser::parse_derived_declaration(const Token& keyword) {
    Token name = consume_identifier_like(
        "STRATA.DSL.PARSE_DERIVED_NAME",
        "Expected derived value name."
    );
    static_cast<void>(consume(
        TokenType::equal,
        "STRATA.DSL.PARSE_DERIVED_EXPRESSION",
        "Expected '=' after derived value name."
    ));
    ExpressionPtr value = expression();
    static_cast<void>(match(TokenType::semicolon));
    const SourceSpan span = covering(keyword.span, value->span);
    return make_statement(DerivedStatement{std::move(name.lexeme), value}, span);
}

StatementPtr Parser::parse_root_statement(const Token& keyword) {
    WidgetCall call = parse_widget_call();
    const SourceSpan span = covering(keyword.span, call.span);
    return make_statement(RootStatement{std::move(call)}, span);
}

StatementPtr Parser::parse_if_statement(const Token& keyword) {
    ExpressionPtr condition = parse_condition();
    BlockPtr then_block = parse_block();
    BlockPtr else_block;
    if (match(TokenType::else_keyword)) {
        if (match(TokenType::if_keyword)) {
            StatementPtr nested = parse_if_statement(previous_token());
            else_block = std::make_shared<const Block>(Block{
                std::vector<StatementPtr>{nested},
                nested->span,
            });
        } else {
            else_block = parse_block();
        }
    }
    const SourceSpan span = covering(keyword.span, else_block != nullptr ? else_block->span : then_block->span);
    return make_statement(
        IfStatement{condition, std::move(then_block), std::move(else_block)},
        span
    );
}

StatementPtr Parser::parse_when_statement(const Token& keyword) {
    ExpressionPtr subject = parse_condition();
    static_cast<void>(consume(
        TokenType::left_brace,
        "STRATA.DSL.PARSE_WHEN_OPEN",
        "Expected '{' after when subject."
    ));
    std::vector<WhenBranch> branches;
    while (!check(TokenType::right_brace) && !at_end()) {
        const SourceSpan branch_start = peek().span;
        ExpressionPtr match_expression;
        if (!match(TokenType::else_keyword)) {
            match_expression = expression();
        }
        static_cast<void>(consume(
            TokenType::arrow,
            "STRATA.DSL.PARSE_WHEN_ARROW",
            "Expected '->' after when branch value."
        ));
        BlockPtr block = parse_block();
        branches.push_back(WhenBranch{
            std::move(match_expression),
            block,
            covering(branch_start, block->span),
        });
        check_collection_size(branches.size());
        static_cast<void>(match(TokenType::comma));
        static_cast<void>(match(TokenType::semicolon));
    }
    const Token right = consume(
        TokenType::right_brace,
        "STRATA.DSL.PARSE_WHEN_CLOSE",
        "Expected '}' after when branches."
    );
    const SourceSpan span = covering(keyword.span, right.span);
    return make_statement(WhenStatement{std::move(subject), std::move(branches)}, span);
}

StatementPtr Parser::parse_for_statement(const Token& keyword) {
    Token item = consume_identifier_like(
        "STRATA.DSL.PARSE_FOR_ITEM",
        "Expected loop item name after for."
    );
    std::optional<std::string> index_name;
    if (match(TokenType::comma)) {
        Token index = consume_identifier_like(
            "STRATA.DSL.PARSE_FOR_INDEX",
            "Expected loop index name after ','."
        );
        index_name = std::move(index.lexeme);
    }
    static_cast<void>(consume(
        TokenType::in_keyword,
        "STRATA.DSL.PARSE_FOR_IN",
        "Expected 'in' after loop item name."
    ));
    ExpressionPtr collection = expression();
    ExpressionPtr filter;
    if (match(TokenType::where)) {
        filter = expression();
    }
    BlockPtr body = parse_block();
    const SourceSpan span = covering(keyword.span, body->span);
    return make_statement(
        ForStatement{
            std::move(item.lexeme),
            std::move(index_name),
            std::move(collection),
            std::move(filter),
            std::move(body),
        },
        span
    );
}

StatementPtr Parser::parse_animation_frame_statement(const Token& keyword) {
    auto [properties, body_span] = parse_property_block();
    const AnimationFramePhase phase =
        keyword.type == TokenType::from ? AnimationFramePhase::from : AnimationFramePhase::to;
    const SourceSpan span = covering(keyword.span, body_span);
    return make_statement(
        AnimationFrameStatement{AnimationFrame{phase, std::move(properties), span}},
        span
    );
}

std::pair<std::vector<Property>, SourceSpan> Parser::parse_property_block() {
    const Token left = consume(
        TokenType::left_brace,
        "STRATA.DSL.PARSE_BLOCK_OPEN",
        "Expected '{' to start property block."
    );
    std::vector<Property> properties;
    while (!check(TokenType::right_brace) && !at_end()) {
        try {
            if (name_token() && check_next(TokenType::colon)) {
                properties.push_back(parse_property());
                check_collection_size(properties.size());
                static_cast<void>(match(TokenType::comma));
                static_cast<void>(match(TokenType::semicolon));
            } else {
                static_cast<void>(error(
                    peek(),
                    "STRATA.DSL.PARSE_EXPECTED_PROPERTY",
                    "Expected property assignment."
                ));
                static_cast<void>(advance());
                synchronize_block();
            }
        } catch (const ParsePanic&) {
            synchronize_block();
        }
    }
    const Token right = consume(
        TokenType::right_brace,
        "STRATA.DSL.PARSE_BLOCK_CLOSE",
        "Expected '}' after property block."
    );
    return {std::move(properties), covering(left.span, right.span)};
}

StatementPtr Parser::parse_property_statement() {
    Property property = parse_property();
    static_cast<void>(match(TokenType::comma));
    static_cast<void>(match(TokenType::semicolon));
    const SourceSpan span = property.span;
    return make_statement(PropertyStatement{std::move(property)}, span);
}

Property Parser::parse_property() {
    Token name = consume_name_token(
        "STRATA.DSL.PARSE_PROPERTY_NAME",
        "Expected property name."
    );
    static_cast<void>(consume(
        TokenType::colon,
        "STRATA.DSL.PARSE_PROPERTY_COLON",
        "Expected ':' after property name."
    ));
    ExpressionPtr value = expression();
    const SourceSpan span = covering(name.span, value->span);
    return Property{std::move(name.lexeme), std::move(value), span};
}

StatementPtr Parser::parse_widget_statement() {
    WidgetCall call = parse_widget_call();
    static_cast<void>(match(TokenType::semicolon));
    const SourceSpan span = call.span;
    return make_statement(WidgetStatement{std::move(call)}, span);
}

WidgetCall Parser::parse_widget_call() {
    Token name = consume_widget_name(
        "STRATA.DSL.PARSE_WIDGET_NAME",
        "Expected widget name."
    );
    std::vector<Argument> arguments;
    if (match(TokenType::left_parenthesis)) {
        arguments = parse_arguments(TokenType::right_parenthesis);
    }
    BlockPtr body;
    if (check(TokenType::left_brace)) {
        body = parse_block();
    }
    const SourceSpan span = covering(name.span, body != nullptr ? body->span : previous_or_peek().span);
    return WidgetCall{std::move(name.lexeme), std::move(arguments), std::move(body), span};
}

std::vector<Argument> Parser::parse_arguments(const TokenType terminator) {
    std::vector<Argument> arguments;
    if (!check(terminator)) {
        do {
            arguments.push_back(parse_argument());
            check_collection_size(arguments.size());
        } while (match(TokenType::comma) && !check(terminator));
    }
    static_cast<void>(consume(
        terminator,
        "STRATA.DSL.PARSE_ARGUMENT_LIST",
        "Expected '" + std::string(close_lexeme(terminator)) + "' after arguments."
    ));
    return arguments;
}

Argument Parser::parse_argument() {
    if (name_token() && check_next(TokenType::colon)) {
        Token name = advance();
        static_cast<void>(advance());
        ExpressionPtr value = expression();
        const SourceSpan span = covering(name.span, value->span);
        return Argument{std::move(name.lexeme), std::move(value), span};
    }
    ExpressionPtr value = expression();
    const SourceSpan span = value->span;
    return Argument{std::nullopt, std::move(value), span};
}

ExpressionPtr Parser::parse_condition() {
    if (match(TokenType::left_parenthesis)) {
        const Token left = previous_token();
        ExpressionPtr condition = expression();
        const Token right = consume(
            TokenType::right_parenthesis,
            "STRATA.DSL.PARSE_CONDITION_CLOSE",
            "Expected ')' after condition."
        );
        const SourceSpan span = covering(left.span, right.span);
        return make_expression(GroupingExpression{std::move(condition)}, span);
    }
    return expression();
}

ExpressionPtr Parser::expression() {
    return parse_expression(Precedence::lowest);
}

ExpressionPtr Parser::parse_expression(const Precedence minimum) {
    const NestingGuard nesting(*this);
    count_expression();
    ExpressionPtr left = parse_prefix();
    while (!at_end() && minimum < current_precedence()) {
        left = parse_infix(std::move(left));
    }
    return left;
}

ExpressionPtr Parser::parse_prefix() {
    if (match(TokenType::string)) {
        const Token& token = previous_token();
        return make_expression(LiteralExpression{StringLiteral{decode_string(token)}}, token.span);
    }
    if (match(TokenType::number)) {
        const Token& token = previous_token();
        return make_expression(LiteralExpression{number_literal(token)}, token.span);
    }
    if (match(TokenType::color)) {
        const Token& token = previous_token();
        return make_expression(LiteralExpression{ColorLiteral{token.lexeme}}, token.span);
    }
    if (match(TokenType::true_keyword)) {
        const Token& token = previous_token();
        return make_expression(LiteralExpression{BooleanLiteral{true}}, token.span);
    }
    if (match(TokenType::false_keyword)) {
        const Token& token = previous_token();
        return make_expression(LiteralExpression{BooleanLiteral{false}}, token.span);
    }
    if (match(TokenType::null_keyword)) {
        const Token& token = previous_token();
        return make_expression(LiteralExpression{NullLiteral{}}, token.span);
    }
    if (identifier_like()) {
        const Token token = advance();
        if (match(TokenType::arrow)) {
            ExpressionPtr body = expression();
            const SourceSpan span = covering(token.span, body->span);
            return make_expression(LambdaExpression{token.lexeme, std::move(body)}, span);
        }
        return make_expression(IdentifierExpression{token.lexeme}, token.span);
    }
    if (match(TokenType::minus)) return parse_unary(previous_token(), UnaryOperator::negate);
    if (match(TokenType::bang)) return parse_unary(previous_token(), UnaryOperator::logical_not);
    if (match(TokenType::left_parenthesis)) return parse_grouping(previous_token());
    if (match(TokenType::left_bracket)) return parse_list(previous_token());
    if (match(TokenType::left_brace)) return parse_map(previous_token());

    const Token token = peek();
    static_cast<void>(error(
        token,
        "STRATA.DSL.PARSE_EXPECTED_EXPRESSION",
        "Expected expression."
    ));
    if (!at_end() && !expression_recovery_token(token.type)) {
        static_cast<void>(advance());
    }
    return make_expression(ErrorExpression{}, token.span);
}

ExpressionPtr Parser::parse_unary(const Token& token, const UnaryOperator operation) {
    ExpressionPtr operand = parse_expression(Precedence::unary);
    const SourceSpan span = covering(token.span, operand->span);
    return make_expression(UnaryExpression{operation, std::move(operand)}, span);
}

ExpressionPtr Parser::parse_grouping(const Token& left) {
    ExpressionPtr value = expression();
    const Token right = consume(
        TokenType::right_parenthesis,
        "STRATA.DSL.PARSE_GROUP_CLOSE",
        "Expected ')' after grouped expression."
    );
    const SourceSpan span = covering(left.span, right.span);
    return make_expression(GroupingExpression{std::move(value)}, span);
}

ExpressionPtr Parser::parse_list(const Token& left) {
    std::vector<ExpressionPtr> elements;
    if (!check(TokenType::right_bracket)) {
        do {
            elements.push_back(expression());
            check_collection_size(elements.size());
        } while (match(TokenType::comma) && !check(TokenType::right_bracket));
    }
    const Token right = consume(
        TokenType::right_bracket,
        "STRATA.DSL.PARSE_LIST_CLOSE",
        "Expected ']' after list literal."
    );
    const SourceSpan span = covering(left.span, right.span);
    return make_expression(ListExpression{std::move(elements)}, span);
}

ExpressionPtr Parser::parse_map(const Token& left) {
    std::vector<MapEntry> entries;
    if (!check(TokenType::right_brace)) {
        do {
            entries.push_back(parse_map_entry());
            check_collection_size(entries.size());
        } while (match(TokenType::comma, TokenType::semicolon) &&
                 !check(TokenType::right_brace));
    }
    const Token right = consume(
        TokenType::right_brace,
        "STRATA.DSL.PARSE_MAP_CLOSE",
        "Expected '}' after map literal."
    );
    const SourceSpan span = covering(left.span, right.span);
    return make_expression(MapExpression{std::move(entries)}, span);
}

MapEntry Parser::parse_map_entry() {
    MapKey key;
    SourceSpan key_span = peek().span;
    if (name_token() && check_next(TokenType::colon)) {
        Token token = advance();
        key_span = token.span;
        key = IdentifierMapKey{std::move(token.lexeme), token.span};
    } else if (check(TokenType::string) && check_next(TokenType::colon)) {
        Token token = advance();
        key_span = token.span;
        key = StringMapKey{decode_string(token), token.span};
    } else {
        ExpressionPtr key_expression = expression();
        key_span = key_expression->span;
        key = ExpressionMapKey{std::move(key_expression), key_span};
    }
    static_cast<void>(consume(
        TokenType::colon,
        "STRATA.DSL.PARSE_MAP_COLON",
        "Expected ':' after map key."
    ));
    ExpressionPtr value = expression();
    const SourceSpan span = covering(key_span, value->span);
    return MapEntry{std::move(key), std::move(value), span};
}

ExpressionPtr Parser::parse_infix(ExpressionPtr left) {
    if (match(TokenType::left_parenthesis)) return finish_call(std::move(left), previous_token());
    if (match(TokenType::dot)) return finish_property_access(std::move(left), previous_token());
    if (match(TokenType::left_bracket)) return finish_index(std::move(left), previous_token());
    if (match(TokenType::question)) return finish_conditional(std::move(left), previous_token());
    return finish_binary(std::move(left));
}

ExpressionPtr Parser::finish_call(ExpressionPtr callee, const Token& left_parenthesis) {
    std::vector<Argument> arguments = parse_arguments(TokenType::right_parenthesis);
    const SourceSpan end_span = previous_or_peek().span;
    std::optional<CallTarget> target = call_target(callee);
    if (!target.has_value()) {
        diagnostics_.push_back(Diagnostic{
            "STRATA.DSL.PARSE_CALL_TARGET",
            DiagnosticSeverity::error,
            "Function calls must target a registered helper name.",
            covering(callee->span, left_parenthesis.span).range(),
            std::nullopt,
            std::nullopt,
        });
        return make_expression(
            ErrorExpression{},
            covering(covering(callee->span, end_span), left_parenthesis.span)
        );
    }
    const SourceSpan span = covering(covering(callee->span, end_span), left_parenthesis.span);
    return make_expression(CallExpression{std::move(*target), std::move(arguments)}, span);
}

std::optional<CallTarget> Parser::call_target(const ExpressionPtr& expression) const {
    std::deque<std::string> parts;
    ExpressionPtr current = expression;
    SourceSpan span = expression->span;
    while (true) {
        if (const auto* identifier = std::get_if<IdentifierExpression>(&current->node)) {
            parts.push_front(identifier->name);
            span = covering(current->span, span);
            return CallTarget{
                std::vector<std::string>(parts.begin(), parts.end()),
                std::move(span),
            };
        }
        if (const auto* property = std::get_if<PropertyAccessExpression>(&current->node)) {
            parts.push_front(property->property_name);
            span = covering(current->span, span);
            current = property->receiver;
            continue;
        }
        return std::nullopt;
    }
}

ExpressionPtr Parser::finish_property_access(ExpressionPtr receiver, const Token& dot) {
    Token name = consume_identifier_like(
        "STRATA.DSL.PARSE_PROPERTY_ACCESS",
        "Expected property name after '.'."
    );
    const SourceSpan span = covering(covering(receiver->span, dot.span), name.span);
    return make_expression(
        PropertyAccessExpression{std::move(receiver), std::move(name.lexeme)},
        span
    );
}

ExpressionPtr Parser::finish_index(ExpressionPtr receiver, const Token& left_bracket) {
    ExpressionPtr index = expression();
    const Token right = consume(
        TokenType::right_bracket,
        "STRATA.DSL.PARSE_INDEX_CLOSE",
        "Expected ']' after index expression."
    );
    const SourceSpan span = covering(covering(receiver->span, left_bracket.span), right.span);
    return make_expression(IndexExpression{std::move(receiver), std::move(index)}, span);
}

ExpressionPtr Parser::finish_conditional(ExpressionPtr condition, const Token& question) {
    ExpressionPtr then_expression = expression();
    static_cast<void>(consume(
        TokenType::colon,
        "STRATA.DSL.PARSE_CONDITIONAL_COLON",
        "Expected ':' in conditional expression."
    ));
    ExpressionPtr else_expression = parse_expression(previous(Precedence::conditional));
    const SourceSpan span = covering(covering(condition->span, question.span), else_expression->span);
    return make_expression(
        ConditionalExpression{
            std::move(condition),
            std::move(then_expression),
            std::move(else_expression),
        },
        span
    );
}

ExpressionPtr Parser::finish_binary(ExpressionPtr left) {
    const Token operation = advance();
    const Precedence operation_precedence = precedence(operation.type);
    const Precedence right_precedence = operation.type == TokenType::question_question
                                            ? previous(Precedence::coalesce)
                                            : operation_precedence;
    ExpressionPtr right = parse_expression(right_precedence);
    const SourceSpan span = covering(left->span, right->span);
    return make_expression(
        BinaryExpression{std::move(left), binary_operator(operation.type), std::move(right)},
        span
    );
}

Parser::Precedence Parser::current_precedence() const noexcept {
    return precedence(peek().type);
}

Parser::Precedence Parser::precedence(const TokenType type) noexcept {
    switch (type) {
    case TokenType::left_parenthesis:
    case TokenType::dot:
    case TokenType::left_bracket: return Precedence::call;
    case TokenType::star:
    case TokenType::slash:
    case TokenType::percent: return Precedence::factor;
    case TokenType::plus:
    case TokenType::minus: return Precedence::term;
    case TokenType::less:
    case TokenType::less_equal:
    case TokenType::greater:
    case TokenType::greater_equal: return Precedence::comparison;
    case TokenType::equal_equal:
    case TokenType::bang_equal: return Precedence::equality;
    case TokenType::amp_amp: return Precedence::logical_and;
    case TokenType::pipe_pipe: return Precedence::logical_or;
    case TokenType::question_question: return Precedence::coalesce;
    case TokenType::question: return Precedence::conditional;
    default: return Precedence::lowest;
    }
}

Parser::Precedence Parser::previous(const Precedence value) noexcept {
    const auto ordinal = static_cast<unsigned int>(value);
    return static_cast<Precedence>(ordinal == 0U ? 0U : ordinal - 1U);
}

BinaryOperator Parser::binary_operator(const TokenType type) {
    switch (type) {
    case TokenType::plus: return BinaryOperator::add;
    case TokenType::minus: return BinaryOperator::subtract;
    case TokenType::star: return BinaryOperator::multiply;
    case TokenType::slash: return BinaryOperator::divide;
    case TokenType::percent: return BinaryOperator::modulo;
    case TokenType::equal_equal: return BinaryOperator::equal;
    case TokenType::bang_equal: return BinaryOperator::not_equal;
    case TokenType::less: return BinaryOperator::less;
    case TokenType::less_equal: return BinaryOperator::less_equal;
    case TokenType::greater: return BinaryOperator::greater;
    case TokenType::greater_equal: return BinaryOperator::greater_equal;
    case TokenType::amp_amp: return BinaryOperator::logical_and;
    case TokenType::pipe_pipe: return BinaryOperator::logical_or;
    case TokenType::question_question: return BinaryOperator::coalesce;
    default: throw std::logic_error("token is not a binary operator");
    }
}

NumberLiteral Parser::number_literal(const Token& token) {
    std::size_t unit_start = token.lexeme.size();
    while (unit_start > 0U) {
        const char value = token.lexeme[unit_start - 1U];
        if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')) {
            --unit_start;
        } else {
            break;
        }
    }
    std::optional<std::string> unit;
    if (unit_start != token.lexeme.size()) {
        unit = token.lexeme.substr(unit_start);
    }
    return NumberLiteral{token.lexeme.substr(0U, unit_start), std::move(unit)};
}

std::string Parser::decode_string(const Token& token) {
    const std::string& raw = token.lexeme;
    if (raw.size() < 2U || raw.front() != '"' || raw.back() != '"') {
        return raw;
    }
    std::string result;
    std::size_t index = 1U;
    while (index + 1U < raw.size()) {
        const char value = raw[index];
        if (value != '\\' || index + 2U >= raw.size()) {
            result.push_back(value);
            ++index;
            continue;
        }
        const char escaped = raw[index + 1U];
        switch (escaped) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        default: result.push_back(escaped); break;
        }
        index += 2U;
    }
    return result;
}

Token Parser::consume_identifier(std::string code, std::string message) {
    if (check(TokenType::identifier)) return advance();
    throw error(peek(), std::move(code), std::move(message));
}

Token Parser::consume_identifier_like(std::string code, std::string message) {
    if (identifier_like()) return advance();
    throw error(peek(), std::move(code), std::move(message));
}

Token Parser::consume_widget_name(std::string code, std::string message) {
    if (widget_name()) return advance();
    throw error(peek(), std::move(code), std::move(message));
}

Token Parser::consume_name_token(std::string code, std::string message) {
    if (name_token()) return advance();
    throw error(peek(), std::move(code), std::move(message));
}

Token Parser::consume(const TokenType type, std::string code, std::string message) {
    if (check(type)) return advance();
    throw error(peek(), std::move(code), std::move(message));
}

Parser::ParsePanic Parser::error(const Token& token, std::string code, std::string message) {
    diagnostics_.push_back(Diagnostic{
        std::move(code),
        DiagnosticSeverity::error,
        std::move(message),
        token.span.range(),
        std::nullopt,
        std::nullopt,
    });
    return ParsePanic{};
}

void Parser::limit(std::string code, std::string message) {
    diagnostics_.push_back(Diagnostic{
        std::move(code),
        DiagnosticSeverity::error,
        std::move(message),
        peek().span.range(),
        std::nullopt,
        std::nullopt,
    });
    throw LimitPanic{};
}

void Parser::count_declaration() {
    if (declaration_count_ >= limits_.max_declarations) {
        limit(
            "STRATA.DSL.LIMIT_DECLARATIONS",
            "Source exceeds the compiler declaration limit."
        );
    }
    ++declaration_count_;
}

void Parser::count_statement() {
    if (statement_count_ >= limits_.max_statements) {
        limit(
            "STRATA.DSL.LIMIT_STATEMENTS",
            "Source exceeds the compiler statement limit."
        );
    }
    ++statement_count_;
}

void Parser::count_expression() {
    if (expression_count_ >= limits_.max_expressions) {
        limit(
            "STRATA.DSL.LIMIT_EXPRESSIONS",
            "Source exceeds the compiler expression limit."
        );
    }
    ++expression_count_;
}

void Parser::check_collection_size(const std::size_t size) {
    if (size > limits_.max_collection_items) {
        limit(
            "STRATA.DSL.LIMIT_COLLECTION_ITEMS",
            "Source exceeds the compiler per-collection item limit."
        );
    }
}

void Parser::synchronize_top_level() {
    while (!at_end()) {
        if (previous_or_peek().type == TokenType::right_brace) return;
        if (declaration_start() || check(TokenType::import_keyword)) return;
        static_cast<void>(advance());
    }
}

void Parser::synchronize_block() {
    while (!at_end()) {
        if (previous_or_peek().type == TokenType::semicolon ||
            previous_or_peek().type == TokenType::right_brace) {
            return;
        }
        switch (peek().type) {
        case TokenType::state:
        case TokenType::root:
        case TokenType::if_keyword:
        case TokenType::when_keyword:
        case TokenType::for_keyword:
        case TokenType::from:
        case TokenType::to:
        case TokenType::right_brace: return;
        default: static_cast<void>(advance()); break;
        }
    }
}

bool Parser::declaration_start() const noexcept {
    switch (peek().type) {
    case TokenType::screen:
    case TokenType::overlay:
    case TokenType::component:
    case TokenType::style:
    case TokenType::animation: return true;
    default: return false;
    }
}

bool Parser::name_token() const noexcept {
    switch (peek().type) {
    case TokenType::when_keyword:
    case TokenType::from:
    case TokenType::to:
    case TokenType::state:
    case TokenType::root:
    case TokenType::if_keyword:
    case TokenType::for_keyword:
    case TokenType::in_keyword:
    case TokenType::else_keyword: return true;
    default: return identifier_like();
    }
}

bool Parser::identifier_like() const noexcept {
    switch (peek().type) {
    case TokenType::identifier:
    case TokenType::style:
    case TokenType::animation:
    case TokenType::screen:
    case TokenType::overlay:
    case TokenType::component:
    case TokenType::import_keyword: return true;
    default: return false;
    }
}

bool Parser::widget_name() const noexcept {
    return identifier_like();
}

bool Parser::expression_recovery_token(const TokenType type) noexcept {
    switch (type) {
    case TokenType::right_brace:
    case TokenType::right_parenthesis:
    case TokenType::right_bracket:
    case TokenType::comma:
    case TokenType::colon:
    case TokenType::semicolon:
    case TokenType::end_of_file: return true;
    default: return false;
    }
}

bool Parser::match(const TokenType type) {
    if (!check(type)) return false;
    static_cast<void>(advance());
    return true;
}

bool Parser::match(const TokenType first, const TokenType second) {
    return match(first) || match(second);
}

bool Parser::check(const TokenType type) const noexcept {
    return !at_end() && peek().type == type;
}

bool Parser::check_next(const TokenType type) const noexcept {
    return current_ + 1U < tokens_.size() && tokens_[current_ + 1U].type == type;
}

Token Parser::advance() {
    if (!at_end()) {
        ++current_;
    }
    return previous_token();
}

bool Parser::at_end() const noexcept {
    return peek().type == TokenType::end_of_file;
}

const Token& Parser::peek() const noexcept {
    return tokens_[current_];
}

const Token& Parser::previous_token() const noexcept {
    return tokens_[current_ - 1U];
}

const Token& Parser::previous_or_peek() const noexcept {
    return current_ > 0U ? previous_token() : peek();
}

std::string_view Parser::close_lexeme(const TokenType type) noexcept {
    switch (type) {
    case TokenType::right_parenthesis: return ")";
    case TokenType::right_bracket: return "]";
    case TokenType::right_brace: return "}";
    default: return token_type_name(type);
    }
}

ParseResult parse_source(
    std::string source_id,
    std::string source,
    const ParserLimits limits,
    std::pmr::memory_resource* const scratch
) {
    if (limits.max_source_bytes == 0U || limits.max_tokens == 0U ||
        limits.max_nesting_depth == 0U || limits.max_declarations == 0U ||
        limits.max_statements == 0U || limits.max_expressions == 0U ||
        limits.max_collection_items == 0U) {
        throw std::invalid_argument("parser limits must be positive");
    }
    if (source.size() > limits.max_source_bytes) {
        const SourcePosition start{1U, 1U, 0U};
        const SourceRange range{source_id, start, start};
        File file{
            source_id,
            {},
            {},
            {},
            SourceSpan{source_id, start, start, 0U, {}},
        };
        std::vector<Diagnostic> diagnostics;
        diagnostics.push_back(Diagnostic{
            "STRATA.DSL.LIMIT_SOURCE_BYTES",
            DiagnosticSeverity::error,
            "Source exceeds the compiler byte-size limit.",
            range,
            std::nullopt,
            std::nullopt,
        });
        return ParseResult{std::move(file), std::move(diagnostics)};
    }
    LexResult lexed = Lexer(source_id, source, limits.max_tokens, scratch).lex();
    std::vector<Diagnostic> security = scan_security_boundary(source_id, source);
    lexed.diagnostics.insert(
        lexed.diagnostics.end(),
        std::make_move_iterator(security.begin()),
        std::make_move_iterator(security.end())
    );
    return Parser(
        std::move(source_id),
        std::move(lexed.tokens),
        std::move(lexed.diagnostics),
        limits
    ).parse();
}

} // namespace strata::compiler
