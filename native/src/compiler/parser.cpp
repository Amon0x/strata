#include "compiler/parser.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace strata::compiler {
namespace {

template <typename Node>
[[nodiscard]] ExpressionPtr make_expression(NodeId& next_id, Node node, SourceSpan span) {
    return std::make_shared<const Expression>(
        Expression{std::move(node), std::move(span), next_id++}
    );
}

template <typename Node>
[[nodiscard]] StatementPtr make_statement(NodeId& next_id, Node node, SourceSpan span) {
    return std::make_shared<const Statement>(
        Statement{std::move(node), std::move(span), next_id++}
    );
}

} // namespace

Parser::Parser(
    std::string source_id,
    std::vector<Token> tokens,
    std::vector<Diagnostic> diagnostics,
    ParserLimits limits,
    const NodeId first_node_id
)
    : source_id_(std::move(source_id)),
      diagnostics_(std::move(diagnostics)),
      limits_(limits),
      next_node_id_(first_node_id) {
    if (limits_.max_source_bytes == 0U || limits_.max_tokens == 0U ||
        limits_.max_nesting_depth == 0U || limits_.max_declarations == 0U ||
        limits_.max_statements == 0U || limits_.max_expressions == 0U ||
        limits_.max_collection_items == 0U) {
        throw std::invalid_argument("parser limits must be positive");
    }
    // The lexer stops at its token limit; what follows was never read.
    truncated_ = std::ranges::any_of(diagnostics_, [](const Diagnostic& diagnostic) {
        return diagnostic.code.starts_with("STRATA.DSL.LIMIT_");
    });
    tokens_ = std::move(tokens);
    std::erase_if(tokens_, [](const Token& token) { return token.type == TokenType::trivia; });
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
            } else if (type_declaration_start()) {
                count_declaration();
                const std::size_t first = current_;
                try {
                    const Token& keyword = advance();
                    TypeDeclaration declared = parse_type_declaration(keyword);
                    const SourceSpan span = covering(keyword.span, previous_or_peek().span);
                    declarations.push_back(Declaration{std::move(declared), span});
                } catch (const ParsePanic&) {
                    synchronize_top_level();
                    record_unparsed(first);
                }
            } else if (function_start()) {
                count_declaration();
                const std::size_t first = current_;
                try {
                    const bool asynchronous = contextual("async");
                    if (asynchronous)
                        static_cast<void>(advance());
                    const Token& keyword = advance();
                    FunctionDeclaration function = parse_function(keyword, asynchronous);
                    const SourceSpan span = covering(keyword.span, previous_or_peek().span);
                    declarations.push_back(Declaration{std::move(function), span});
                } catch (const ParsePanic&) {
                    synchronize_top_level();
                    record_unparsed(first);
                }
            } else if (match(TokenType::error)) {
                record_unparsed(current_ - 1U);
                continue;
            } else {
                static_cast<void>(error(
                    peek(),
                    "STRATA.DSL.PARSE_EXPECTED_DECLARATION",
                    "Expected import, screen, overlay, component, style, animation, or fn declaration."
                ));
                const std::size_t first = current_;
                static_cast<void>(advance());
                synchronize_top_level();
                record_unparsed(first);
            }
        }
    } catch (const LimitPanic&) {
        // A limit diagnostic is terminal for this source so recovery cannot consume more budget.
        truncated_ = true;
    }
    std::ranges::sort(unparsed_names_);
    unparsed_names_.erase(std::ranges::unique(unparsed_names_).begin(), unparsed_names_.end());
    const SourceSpan file_span = covering(first_span, previous_or_peek().span);
    File file{
        source_id_,
        std::move(imports),
        std::move(declarations),
        file_span,
        next_node_id_,
        std::move(unparsed_names_),
        truncated_,
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
    const std::size_t first = current_;
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
        record_unparsed(first);
    }
    return std::nullopt;
}

Declaration Parser::parse_named_block_declaration(
    const Token& keyword,
    const DeclarationKind kind
) {
    const std::string label = kind == DeclarationKind::screen ? "screen" : "overlay";
    const Token& name = consume_identifier(
        "STRATA.DSL.PARSE_DECLARATION_NAME",
        "Expected " + label + " name."
    );
    BlockPtr body = parse_block();
    const SourceSpan span = covering(keyword.span, body->span);
    if (kind == DeclarationKind::screen) {
        return Declaration{ScreenDeclaration{name.lexeme, std::move(body)}, span};
    }
    return Declaration{OverlayDeclaration{name.lexeme, std::move(body)}, span};
}

Declaration Parser::parse_component_declaration(const Token& keyword) {
    const Token& name = consume_identifier(
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
        ComponentDeclaration{name.lexeme, std::move(parameters), std::move(body)},
        span,
    };
}

Declaration Parser::parse_style_declaration(const Token& keyword) {
    const Token& name = consume_identifier(
        "STRATA.DSL.PARSE_DECLARATION_NAME",
        "Expected style name."
    );
    std::vector<StyleBase> bases;
    if (match(TokenType::extends_keyword)) {
        do {
            const Token& base = consume_identifier_like(
                "STRATA.DSL.PARSE_STYLE_BASE",
                "Expected a style name after 'extends'."
            );
            bases.push_back(StyleBase{base.lexeme, base.span});
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
            name.lexeme,
            std::move(bases),
            std::move(properties),
            std::move(body),
        },
        span,
    };
}

Declaration Parser::parse_animation_declaration(const Token& keyword) {
    const Token& name = consume_identifier(
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
            name.lexeme,
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
            const Token& name = consume_identifier_like(
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
                name.lexeme,
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
    TypeReference first = parse_type_option();
    if (!check(TokenType::pipe))
        return first;
    TypeReference result{"union", {}, false, first.span, TypeReferenceKind::union_type};
    result.arguments.push_back(std::move(first));
    while (match(TokenType::pipe)) {
        result.arguments.push_back(parse_type_option());
        check_collection_size(result.arguments.size());
    }
    result.span = covering(result.arguments.front().span, result.arguments.back().span);
    // `A | B | null` is a nullable union.
    std::erase_if(result.arguments, [&result](const TypeReference& option) {
        const bool null = option.kind == TypeReferenceKind::named && option.name == "null";
        if (null)
            result.nullable = true;
        return null;
    });
    if (result.arguments.size() == 1U) {
        TypeReference only = std::move(result.arguments.front());
        only.nullable = only.nullable || result.nullable;
        only.span = result.span;
        return only;
    }
    return result;
}

TypeReference Parser::parse_type_option() {
    const NestingGuard nesting(*this);
    if (match(TokenType::string)) {
        const Token& token = previous_token();
        TypeReference literal{decode_string(token), {}, false, token.span,
                              TypeReferenceKind::string_literal};
        literal.nullable = match(TokenType::question);
        return literal;
    }
    if (match(TokenType::left_brace)) {
        const Token& open = previous_token();
        TypeReference record{"record", {}, false, open.span, TypeReferenceKind::record};
        while (!check(TokenType::right_brace) && !at_end()) {
            const Token& field = consume_name_token("STRATA.DSL.PARSE_PARAMETER_TYPE",
                                                   "Expected a field name in a record type.");
            const bool optional = match(TokenType::question);
            static_cast<void>(consume(TokenType::colon, "STRATA.DSL.PARSE_PARAMETER_TYPE",
                                      "Expected ':' after a record type's field name."));
            record.fields.push_back(TypeField{field.lexeme, parse_type_reference(), optional});
            check_collection_size(record.fields.size());
            if (!match(TokenType::comma) && !match(TokenType::semicolon))
                break;
        }
        const Token& close = consume(TokenType::right_brace, "STRATA.DSL.PARSE_PARAMETER_TYPE",
                                     "Expected '}' after a record type's fields.");
        record.nullable = match(TokenType::question);
        record.span = covering(open.span, record.nullable ? previous_token().span : close.span);
        return record;
    }
    if (match(TokenType::left_parenthesis)) {
        const Token& open = previous_token();
        std::vector<TypeReference> parameters;
        if (!check(TokenType::right_parenthesis)) {
            do {
                // A parameter may be named for its reader: `(node: Node) => boolean`.
                if (name_token() && check_next(TokenType::colon)) {
                    static_cast<void>(advance());
                    static_cast<void>(advance());
                }
                parameters.push_back(parse_type_reference());
                check_collection_size(parameters.size());
            } while (match(TokenType::comma));
        }
        static_cast<void>(consume(TokenType::right_parenthesis, "STRATA.DSL.PARSE_PARAMETER_TYPE",
                                  "Expected ')' in a type."));
        if (match(TokenType::fat_arrow)) {
            TypeReference function{"function", std::move(parameters), false, open.span,
                                   TypeReferenceKind::function};
            function.returns.push_back(parse_type_option());
            function.span = covering(open.span, function.returns.front().span);
            return function;
        }
        if (parameters.size() != 1U) {
            throw error(previous_token(), "STRATA.DSL.PARSE_PARAMETER_TYPE",
                        "Expected '=>' after a function type's parameters.");
        }
        TypeReference grouped = std::move(parameters.front());
        grouped.nullable = grouped.nullable || match(TokenType::question);
        return grouped;
    }
    const Token& name = consume_name_token(
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
        name.lexeme,
        std::move(arguments),
        nullable,
        covering(name.span, end_span),
    };
}

BlockPtr Parser::parse_block(const BodyKind kind) {
    const NestingGuard nesting(*this);
    const Token& left = consume(
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
    const Token& right = consume(
        TokenType::right_brace,
        "STRATA.DSL.PARSE_BLOCK_CLOSE",
        unclosed_message("block", left)
    );
    return std::make_shared<const Block>(Block{
        std::move(statements),
        covering(left.span, right.span),
        next_node_id_++,
    });
}

StatementPtr Parser::parse_statement(const BodyKind kind) {
    if (in_script_)
        return parse_script_statement();
    count_statement();
    const std::size_t first = current_;
    try {
        if (type_declaration_start()) {
            const Token& keyword = advance();
            TypeDeclaration declared = parse_type_declaration(keyword);
            const SourceSpan span = covering(keyword.span, previous_or_peek().span);
            return make_statement(next_node_id_, TypeStatement{std::move(declared)}, span);
        }
        if (function_start()) {
            const bool asynchronous = contextual("async");
            if (asynchronous)
                static_cast<void>(advance());
            const Token& keyword = advance();
            FunctionDeclaration function = parse_function(keyword, asynchronous);
            const SourceSpan span = covering(keyword.span, previous_or_peek().span);
            return make_statement(next_node_id_, FunctionStatement{std::move(function)}, span);
        }
        if (name_token() && check_next(TokenType::colon)) return parse_property_statement();
        if (match(TokenType::state)) return parse_state_declaration(previous_token());
        if (match(TokenType::derived)) return parse_derived_declaration(previous_token());
        if (match(TokenType::root)) return parse_root_statement(previous_token());
        if (match(TokenType::if_keyword)) return parse_if_statement(previous_token());
        if (match(TokenType::when_keyword)) return parse_when_statement(previous_token());
        if (match(TokenType::for_keyword)) return parse_for_statement(previous_token());
        if (kind == BodyKind::animation && (contextual("from") || contextual("to")) &&
            peek_at(1U).type == TokenType::left_brace) {
            return parse_animation_frame_statement(advance());
        }
        if (kind == BodyKind::animation && check(TokenType::number) &&
            check_next(TokenType::left_brace)) {
            return parse_animation_frame_statement(advance());
        }
        if (widget_name()) return parse_widget_statement();
        if (match(TokenType::semicolon)) return nullptr;
        if (match(TokenType::error)) {
            record_unparsed(first);
            return make_statement(next_node_id_, ErrorStatement{}, previous_token().span);
        }
        static_cast<void>(error(
            peek(),
            "STRATA.DSL.PARSE_EXPECTED_STATEMENT",
            "Expected state, property, widget call, if, when, or for statement."
        ));
        const Token& token = advance();
        synchronize_block();
        record_unparsed(first);
        return make_statement(next_node_id_, ErrorStatement{}, token.span);
    } catch (const ParsePanic&) {
        const SourceSpan span = previous_or_peek().span;
        synchronize_block();
        record_unparsed(first);
        return make_statement(next_node_id_, ErrorStatement{}, span);
    }
}

StatementPtr Parser::parse_state_declaration(const Token& keyword) {
    const Token& name = consume_identifier_like(
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
        next_node_id_,
        StateStatement{name.lexeme, std::move(type_reference), initializer},
        span
    );
}

StatementPtr Parser::parse_derived_declaration(const Token& keyword) {
    const Token& name = consume_identifier_like(
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
    return make_statement(next_node_id_, DerivedStatement{name.lexeme, value}, span);
}

StatementPtr Parser::parse_root_statement(const Token& keyword) {
    WidgetCall call = parse_widget_call();
    const SourceSpan span = covering(keyword.span, call.span);
    return make_statement(next_node_id_, RootStatement{std::move(call)}, span);
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
                next_node_id_++,
            });
        } else {
            else_block = parse_block();
        }
    }
    const SourceSpan span = covering(keyword.span, else_block != nullptr ? else_block->span : then_block->span);
    return make_statement(
        next_node_id_,
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
        if (identifier_like() && check_next(TokenType::arrow)) {
            // `name -> { ... }` matches the value of `name`; a branch is never a lambda.
            count_expression();
            const Token& name = advance();
            match_expression = make_expression(
                next_node_id_,
                IdentifierExpression{name.lexeme},
                name.span
            );
        } else if (!match(TokenType::else_keyword)) {
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
    const Token& right = consume(
        TokenType::right_brace,
        "STRATA.DSL.PARSE_WHEN_CLOSE",
        "Expected '}' after when branches."
    );
    const SourceSpan span = covering(keyword.span, right.span);
    return make_statement(
        next_node_id_,
        WhenStatement{std::move(subject), std::move(branches)},
        span
    );
}

StatementPtr Parser::parse_for_statement(const Token& keyword) {
    const Token& item = consume_identifier_like(
        "STRATA.DSL.PARSE_FOR_ITEM",
        "Expected loop item name after for."
    );
    std::optional<std::string> index_name;
    if (match(TokenType::comma)) {
        const Token& index = consume_identifier_like(
            "STRATA.DSL.PARSE_FOR_INDEX",
            "Expected loop index name after ','."
        );
        index_name = index.lexeme;
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
        next_node_id_,
        ForStatement{
            item.lexeme,
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
    const AnimationFramePhase phase = keyword.lexeme == "from" ? AnimationFramePhase::from
                                      : keyword.lexeme == "to" ? AnimationFramePhase::to
                                                                      : AnimationFramePhase::at;
    // A keyframe between `from` and `to` is introduced by its offset, a plain number.
    double offset = -1.0;
    if (phase == AnimationFramePhase::at) {
        const NumberLiteral literal = number_literal(keyword);
        std::string digits = literal.raw;
        std::erase(digits, '_');
        double value = 0.0;
        const auto [end, failure] =
            std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (!literal.unit.has_value() && failure == std::errc{} &&
            end == digits.data() + digits.size()) {
            offset = value;
        }
    }
    const SourceSpan span = covering(keyword.span, body_span);
    return make_statement(
        next_node_id_,
        AnimationFrameStatement{AnimationFrame{phase, std::move(properties), span, offset}}, span);
}

std::pair<std::vector<Property>, SourceSpan> Parser::parse_property_block() {
    const Token& left = consume(
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
    const Token& right = consume(
        TokenType::right_brace,
        "STRATA.DSL.PARSE_BLOCK_CLOSE",
        unclosed_message("property block", left)
    );
    return {std::move(properties), covering(left.span, right.span)};
}

StatementPtr Parser::parse_property_statement() {
    Property property = parse_property();
    static_cast<void>(match(TokenType::comma));
    static_cast<void>(match(TokenType::semicolon));
    const SourceSpan span = property.span;
    return make_statement(next_node_id_, PropertyStatement{std::move(property)}, span);
}

Property Parser::parse_property() {
    const Token& name = consume_name_token(
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
    return Property{name.lexeme, std::move(value), span};
}

StatementPtr Parser::parse_widget_statement() {
    WidgetCall call = parse_widget_call();
    static_cast<void>(match(TokenType::semicolon));
    const SourceSpan span = call.span;
    return make_statement(next_node_id_, WidgetStatement{std::move(call)}, span);
}

WidgetCall Parser::parse_widget_call() {
    const Token& name = consume_widget_name(
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
    return WidgetCall{name.lexeme, std::move(arguments), std::move(body), span, next_node_id_++};
}

std::vector<Argument> Parser::parse_arguments(const TokenType terminator) {
    std::vector<Argument> arguments;
    if (!check(terminator)) {
        do {
            arguments.push_back(parse_argument());
            check_collection_size(arguments.size());
        } while (match(TokenType::comma) && !check(terminator));
    }
    const std::string close(close_lexeme(terminator));
    // A named argument right after another one is almost always a missing comma.
    if (!check(terminator) && name_token() && check_next(TokenType::colon)) {
        throw error(peek(), "STRATA.DSL.PARSE_ARGUMENT_LIST",
                    "Expected ',' before argument '" + peek().lexeme + "'.");
    }
    static_cast<void>(consume(
        terminator,
        "STRATA.DSL.PARSE_ARGUMENT_LIST",
        arguments.empty() ? "Expected '" + close + "' after arguments."
                          : "Expected ',' or '" + close + "' after argument."
    ));
    return arguments;
}

std::string Parser::unclosed_message(const std::string_view what, const Token& open) const {
    // At the end of the file the brace that was never closed is the useful location.
    if (at_end()) {
        return "Expected '}' to close the " + std::string(what) + " opened at line " +
               std::to_string(open.span.range().start.line) + ".";
    }
    return "Expected '}' after " + std::string(what) + ".";
}

Argument Parser::parse_argument() {
    if (name_token() && check_next(TokenType::colon)) {
        const Token& name = advance();
        static_cast<void>(advance());
        ExpressionPtr value = expression();
        const SourceSpan span = covering(name.span, value->span);
        return Argument{name.lexeme, std::move(value), span};
    }
    ExpressionPtr value = expression();
    const SourceSpan span = value->span;
    return Argument{std::nullopt, std::move(value), span};
}

ExpressionPtr Parser::parse_condition() {
    if (match(TokenType::left_parenthesis)) {
        const Token& left = previous_token();
        ExpressionPtr condition = expression();
        const Token& right = consume(
            TokenType::right_parenthesis,
            "STRATA.DSL.PARSE_CONDITION_CLOSE",
            "Expected ')' after condition."
        );
        const SourceSpan span = covering(left.span, right.span);
        return make_expression(next_node_id_, GroupingExpression{std::move(condition)}, span);
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
    if (match(TokenType::template_full) || match(TokenType::template_head))
        return parse_template(previous_token());
    if (match(TokenType::string)) {
        const Token& token = previous_token();
        return make_expression(
            next_node_id_,
            LiteralExpression{StringLiteral{decode_string(token)}},
            token.span
        );
    }
    if (match(TokenType::number)) {
        const Token& token = previous_token();
        return make_expression(next_node_id_, LiteralExpression{number_literal(token)}, token.span);
    }
    if (match(TokenType::color)) {
        const Token& token = previous_token();
        return make_expression(
            next_node_id_,
            LiteralExpression{ColorLiteral{token.lexeme}},
            token.span
        );
    }
    if (match(TokenType::true_keyword)) {
        const Token& token = previous_token();
        return make_expression(next_node_id_, LiteralExpression{BooleanLiteral{true}}, token.span);
    }
    if (match(TokenType::false_keyword)) {
        const Token& token = previous_token();
        return make_expression(next_node_id_, LiteralExpression{BooleanLiteral{false}}, token.span);
    }
    if (match(TokenType::null_keyword)) {
        const Token& token = previous_token();
        return make_expression(next_node_id_, LiteralExpression{NullLiteral{}}, token.span);
    }
    if (contextual("await") && identifier_like() && peek_at(1U).type != TokenType::end_of_file &&
        (peek_at(1U).type == TokenType::identifier || peek_at(1U).type == TokenType::string ||
         peek_at(1U).type == TokenType::number || peek_at(1U).type == TokenType::left_parenthesis ||
         peek_at(1U).type == TokenType::left_bracket || peek_at(1U).type == TokenType::left_brace ||
         peek_at(1U).type == TokenType::bang || peek_at(1U).type == TokenType::minus)) {
        const Token& keyword = advance();
        ExpressionPtr operand = parse_expression(Precedence::unary);
        const SourceSpan span = covering(keyword.span, operand->span);
        return make_expression(next_node_id_, AwaitExpression{std::move(operand)}, span);
    }
    if (identifier_like()) {
        const Token& token = advance();
        if (match(TokenType::arrow)) {
            ExpressionPtr body = expression();
            const SourceSpan span = covering(token.span, body->span);
            return make_expression(
                next_node_id_,
                LambdaExpression{{LambdaParameter{token.lexeme, std::nullopt, token.span}},
                                 std::move(body), nullptr, false},
                span
            );
        }
        if (match(TokenType::fat_arrow)) {
            return finish_lambda({LambdaParameter{token.lexeme, std::nullopt, token.span}},
                                 token.span);
        }
        return make_expression(next_node_id_, IdentifierExpression{token.lexeme}, token.span);
    }
    if (match(TokenType::minus)) return parse_unary(previous_token(), UnaryOperator::negate);
    if (match(TokenType::bang)) return parse_unary(previous_token(), UnaryOperator::logical_not);
    if (check(TokenType::left_parenthesis) && lambda_parameters_ahead()) {
        const Token& left = advance();
        std::vector<LambdaParameter> parameters = parse_lambda_parameters();
        static_cast<void>(consume(TokenType::fat_arrow, "STRATA.DSL.PARSE_LAMBDA_ARROW",
                                  "Expected '=>' after lambda parameters."));
        return finish_lambda(std::move(parameters), left.span);
    }
    if (match(TokenType::left_parenthesis)) return parse_grouping(previous_token());
    if (match(TokenType::left_bracket)) return parse_list(previous_token());
    if (match(TokenType::left_brace)) return parse_map(previous_token());

    const Token& token = peek();
    static_cast<void>(error(
        token,
        "STRATA.DSL.PARSE_EXPECTED_EXPRESSION",
        "Expected expression."
    ));
    if (!at_end() && !expression_recovery_token(token.type)) {
        static_cast<void>(advance());
    }
    return make_expression(next_node_id_, ErrorExpression{}, token.span);
}

ExpressionPtr Parser::parse_unary(const Token& token, const UnaryOperator operation) {
    ExpressionPtr operand = parse_expression(Precedence::unary);
    const SourceSpan span = covering(token.span, operand->span);
    return make_expression(next_node_id_, UnaryExpression{operation, std::move(operand)}, span);
}

ExpressionPtr Parser::parse_grouping(const Token& left) {
    ExpressionPtr value = expression();
    const Token& right = consume(
        TokenType::right_parenthesis,
        "STRATA.DSL.PARSE_GROUP_CLOSE",
        "Expected ')' after grouped expression."
    );
    const SourceSpan span = covering(left.span, right.span);
    return make_expression(next_node_id_, GroupingExpression{std::move(value)}, span);
}

ExpressionPtr Parser::parse_list(const Token& left) {
    std::vector<ExpressionPtr> elements;
    if (!check(TokenType::right_bracket)) {
        do {
            if (match(TokenType::ellipsis)) {
                const Token& dots = previous_token();
                ExpressionPtr operand = expression();
                const SourceSpan span = covering(dots.span, operand->span);
                elements.push_back(
                    make_expression(next_node_id_, SpreadExpression{std::move(operand)}, span));
            } else {
                elements.push_back(expression());
            }
            check_collection_size(elements.size());
        } while (match(TokenType::comma) && !check(TokenType::right_bracket));
    }
    const Token& right = consume(
        TokenType::right_bracket,
        "STRATA.DSL.PARSE_LIST_CLOSE",
        "Expected ']' after list literal."
    );
    const SourceSpan span = covering(left.span, right.span);
    return make_expression(next_node_id_, ListExpression{std::move(elements)}, span);
}

ExpressionPtr Parser::parse_map(const Token& left) {
    std::vector<MapEntry> entries;
    // A map literal becomes one object, so a key written twice would silently drop a value.
    std::set<std::string, std::less<>> keys;
    if (!check(TokenType::right_brace)) {
        do {
            std::optional<MapEntry> entry = parse_map_entry();
            if (!entry.has_value()) continue;
            entries.push_back(std::move(*entry));
            check_collection_size(entries.size());
            const MapKey& key = entries.back().key;
            const std::string* name = nullptr;
            if (const auto* identifier = std::get_if<IdentifierMapKey>(&key))
                name = &identifier->name;
            else if (const auto* string = std::get_if<StringMapKey>(&key))
                name = &string->value;
            if (name != nullptr && !keys.insert(*name).second) {
                diagnostics_.push_back(Diagnostic{
                    "STRATA.DSL.PARSE_DUPLICATE_MAP_KEY",
                    DiagnosticSeverity::error,
                    "Key '" + *name + "' is already set in this map.",
                    entries.back().span.range(),
                    std::nullopt,
                    std::nullopt,
                });
            }
        } while (match(TokenType::comma, TokenType::semicolon) &&
                 !check(TokenType::right_brace));
    }
    const Token& right = consume(
        TokenType::right_brace,
        "STRATA.DSL.PARSE_MAP_CLOSE",
        "Expected '}' after map literal."
    );
    const SourceSpan span = covering(left.span, right.span);
    return make_expression(next_node_id_, MapExpression{std::move(entries)}, span);
}

std::optional<MapEntry> Parser::parse_map_entry() {
    std::optional<MapKey> key;
    SourceSpan key_span = peek().span;
    if (match(TokenType::ellipsis)) {
        // `...record`: its fields, in place, before or under the entries after it.
        const Token& dots = previous_token();
        ExpressionPtr value = expression();
        const SourceSpan span = covering(dots.span, value->span);
        return MapEntry{SpreadMapKey{dots.span}, std::move(value), span};
    }
    if (name_token() && check_next(TokenType::colon)) {
        const Token& token = advance();
        key_span = token.span;
        key = IdentifierMapKey{token.lexeme, token.span};
    } else if (check(TokenType::string) && check_next(TokenType::colon)) {
        const Token& token = advance();
        key_span = token.span;
        key = StringMapKey{decode_string(token), token.span};
    } else {
        // A map literal lowers to an object whose field names are fixed at compile time.
        const ExpressionPtr key_expression = expression();
        key_span = key_expression->span;
        diagnostics_.push_back(Diagnostic{
            "STRATA.DSL.PARSE_MAP_KEY",
            DiagnosticSeverity::error,
            "Map keys must be a name or a string literal; computed keys are not supported.",
            key_span.range(),
            std::nullopt,
            std::nullopt,
        });
    }
    static_cast<void>(consume(
        TokenType::colon,
        "STRATA.DSL.PARSE_MAP_COLON",
        "Expected ':' after map key."
    ));
    ExpressionPtr value = expression();
    if (!key.has_value()) return std::nullopt;
    const SourceSpan span = covering(key_span, value->span);
    return MapEntry{std::move(*key), std::move(value), span};
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
            next_node_id_,
            ErrorExpression{},
            covering(covering(callee->span, end_span), left_parenthesis.span)
        );
    }
    const SourceSpan span = covering(covering(callee->span, end_span), left_parenthesis.span);
    return make_expression(
        next_node_id_,
        CallExpression{std::move(*target), std::move(arguments)},
        span
    );
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
    const Token& name = consume_name_token(
        "STRATA.DSL.PARSE_PROPERTY_ACCESS",
        "Expected property name after '.'."
    );
    const SourceSpan span = covering(covering(receiver->span, dot.span), name.span);
    return make_expression(
        next_node_id_,
        PropertyAccessExpression{std::move(receiver), name.lexeme},
        span
    );
}

ExpressionPtr Parser::finish_index(ExpressionPtr receiver, const Token& left_bracket) {
    ExpressionPtr index = expression();
    const Token& right = consume(
        TokenType::right_bracket,
        "STRATA.DSL.PARSE_INDEX_CLOSE",
        "Expected ']' after index expression."
    );
    const SourceSpan span = covering(covering(receiver->span, left_bracket.span), right.span);
    return make_expression(
        next_node_id_,
        IndexExpression{std::move(receiver), std::move(index)},
        span
    );
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
        next_node_id_,
        ConditionalExpression{
            std::move(condition),
            std::move(then_expression),
            std::move(else_expression),
        },
        span
    );
}

ExpressionPtr Parser::finish_binary(ExpressionPtr left) {
    const Token& operation = advance();
    const Precedence operation_precedence = precedence(operation.type);
    const Precedence right_precedence = operation.type == TokenType::question_question
                                            ? previous(Precedence::coalesce)
                                            : operation_precedence;
    ExpressionPtr right = parse_expression(right_precedence);
    const SourceSpan span = covering(left->span, right->span);
    return make_expression(
        next_node_id_,
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

bool Parser::type_declaration_start() const noexcept {
    return contextual("type") && peek_at(1U).type == TokenType::identifier &&
           peek_at(2U).type == TokenType::equal;
}

TypeDeclaration Parser::parse_type_declaration(const Token& keyword) {
    static_cast<void>(keyword);
    const Token& name = consume_identifier("STRATA.DSL.PARSE_EXPECTED_DECLARATION",
                                           "Expected the type's name after 'type'.");
    static_cast<void>(consume(TokenType::equal, "STRATA.DSL.PARSE_EXPECTED_DECLARATION",
                              "Expected '=' after the type's name."));
    TypeReference type = parse_type_reference();
    static_cast<void>(match(TokenType::semicolon));
    return TypeDeclaration{name.lexeme, std::move(type), name.span};
}

std::string Parser::decode_template(const Token& token) const {
    // The text between the opening backtick or `}` and the closing backtick or `${`.
    const std::string& raw = token.lexeme;
    const std::size_t begin = 1U;
    const std::size_t end = token.type == TokenType::template_head ||
                                    token.type == TokenType::template_middle
                                ? raw.size() - 2U
                                : raw.size() - 1U;
    std::string result;
    for (std::size_t index = begin; index < end; ++index) {
        const char value = raw[index];
        if (value != '\\' || index + 1U >= end) {
            result.push_back(value);
            continue;
        }
        const char escaped = raw[++index];
        switch (escaped) {
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        default: result.push_back(escaped); break;
        }
    }
    return result;
}

ExpressionPtr Parser::parse_template(const Token& head) {
    const NestingGuard nesting(*this);
    // Text first, so `+` joins everything after it as text.
    ExpressionPtr result = make_expression(
        next_node_id_, LiteralExpression{StringLiteral{decode_template(head)}}, head.span);
    if (head.type == TokenType::template_full)
        return result;
    while (true) {
        ExpressionPtr part = parse_expression(Precedence::lowest);
        const SourceSpan part_span = part->span;
        result = make_expression(
            next_node_id_,
            BinaryExpression{std::move(result), BinaryOperator::add, std::move(part)},
            covering(head.span, part_span));
        if (!check(TokenType::template_middle) && !check(TokenType::template_tail)) {
            throw error(peek(), "STRATA.DSL.PARSE_EXPECTED_EXPRESSION",
                        "Expected '}' after a template string's expression.");
        }
        const Token& text = advance();
        const std::string decoded = decode_template(text);
        if (!decoded.empty()) {
            ExpressionPtr literal = make_expression(
                next_node_id_, LiteralExpression{StringLiteral{decoded}}, text.span);
            result = make_expression(
                next_node_id_,
                BinaryExpression{std::move(result), BinaryOperator::add, std::move(literal)},
                covering(head.span, text.span));
        }
        if (text.type == TokenType::template_tail)
            return result;
    }
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

const Token& Parser::consume_identifier(std::string code, std::string message) {
    if (check(TokenType::identifier)) return advance();
    throw error(peek(), std::move(code), std::move(message));
}

const Token& Parser::consume_identifier_like(std::string code, std::string message) {
    if (identifier_like()) return advance();
    throw error(peek(), std::move(code), std::move(message));
}

const Token& Parser::consume_widget_name(std::string code, std::string message) {
    if (widget_name()) return advance();
    throw error(peek(), std::move(code), std::move(message));
}

const Token& Parser::consume_name_token(std::string code, std::string message) {
    if (name_token()) return advance();
    throw error(peek(), std::move(code), std::move(message));
}

const Token& Parser::consume(const TokenType type, std::string code, std::string message) {
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

void Parser::record_unparsed(const std::size_t first) {
    const auto word = [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x80U || std::isalnum(byte) != 0 || character == '_';
    };
    for (std::size_t index = first; index < current_ && index < tokens_.size(); ++index) {
        const Token& token = tokens_[index];
        if (token.type == TokenType::string || token.type == TokenType::number ||
            token.type == TokenType::color) {
            continue;
        }
        // An unreadable token may hold several words, the rest of an unterminated string among
        // them; any of them could have been a name.
        const std::string_view lexeme = token.lexeme;
        std::size_t start = 0U;
        while (start < lexeme.size()) {
            if (!word(lexeme[start])) {
                ++start;
                continue;
            }
            std::size_t end = start;
            while (end < lexeme.size() && word(lexeme[end])) ++end;
            if (std::isdigit(static_cast<unsigned char>(lexeme[start])) == 0)
                unparsed_names_.emplace_back(lexeme.substr(start, end - start));
            start = end;
        }
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
    // Statement keywords only start statements, so where a name is expected they are names.
    switch (peek().type) {
    case TokenType::when_keyword:
    case TokenType::state:
    case TokenType::derived:
    case TokenType::root:
    case TokenType::if_keyword:
    case TokenType::for_keyword:
    case TokenType::in_keyword:
    case TokenType::where:
    case TokenType::extends_keyword:
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

const Token& Parser::advance() {
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
    std::pmr::memory_resource* const scratch,
    const NodeId first_node_id
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
            SourceSpan{source_id, start, start, 0U},
            first_node_id,
            {},
            true,
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
    LexResult lexed = Lexer(source_id, source, limits.max_tokens, scratch, false).lex();
    return Parser(
        std::move(source_id),
        std::move(lexed.tokens),
        std::move(lexed.diagnostics),
        limits,
        first_node_id
    ).parse();
}


const Token& Parser::peek_at(const std::size_t offset) const noexcept {
    const std::size_t index = std::min(current_ + offset, tokens_.size() - 1U);
    return tokens_[index];
}

bool Parser::contextual(const std::string_view word) const noexcept {
    return peek().type == TokenType::identifier && peek().lexeme == word;
}

bool Parser::function_start() const noexcept {
    if (contextual("fn"))
        return peek_at(1U).type == TokenType::identifier && peek_at(2U).type == TokenType::left_parenthesis;
    if (contextual("async"))
        return peek_at(1U).type == TokenType::identifier && peek_at(1U).lexeme == "fn" &&
               peek_at(2U).type == TokenType::identifier &&
               peek_at(3U).type == TokenType::left_parenthesis;
    return false;
}

bool Parser::lambda_parameters_ahead() const noexcept {
    // ( ) =>  |  ( name [: Type] {, name [: Type]} ) =>
    std::size_t offset = 1U;
    if (peek_at(offset).type == TokenType::right_parenthesis)
        return peek_at(offset + 1U).type == TokenType::fat_arrow;
    while (true) {
        if (peek_at(offset).type != TokenType::identifier)
            return false;
        ++offset;
        if (peek_at(offset).type == TokenType::colon) {
            ++offset;
            int angles = 0;
            while (true) {
                const TokenType type = peek_at(offset).type;
                if (type == TokenType::end_of_file)
                    return false;
                if (type == TokenType::less)
                    ++angles;
                else if (type == TokenType::greater)
                    --angles;
                else if (angles == 0 &&
                         (type == TokenType::comma || type == TokenType::right_parenthesis))
                    break;
                else if (type != TokenType::identifier && type != TokenType::question &&
                         type != TokenType::comma)
                    return false;
                ++offset;
            }
        }
        if (peek_at(offset).type == TokenType::comma) {
            ++offset;
            continue;
        }
        if (peek_at(offset).type == TokenType::right_parenthesis)
            return peek_at(offset + 1U).type == TokenType::fat_arrow;
        return false;
    }
}

std::vector<LambdaParameter> Parser::parse_lambda_parameters() {
    std::vector<LambdaParameter> parameters;
    if (!check(TokenType::right_parenthesis)) {
        do {
            const Token& name = consume_identifier_like("STRATA.DSL.PARSE_PARAMETER_NAME",
                                                        "Expected lambda parameter name.");
            std::optional<TypeReference> type_reference;
            if (match(TokenType::colon))
                type_reference = parse_type_reference();
            parameters.push_back(LambdaParameter{name.lexeme, std::move(type_reference), name.span});
            check_collection_size(parameters.size());
        } while (match(TokenType::comma));
    }
    static_cast<void>(consume(TokenType::right_parenthesis, "STRATA.DSL.PARSE_PARAMETER_LIST",
                              "Expected ')' after lambda parameters."));
    return parameters;
}

bool Parser::record_literal_ahead() const noexcept {
    // `{ name: ...`, `{ "name": ...` and `{ ...rest` are records; anything else is a block.
    const Token& first = peek_at(1U);
    if (first.type == TokenType::ellipsis)
        return true;
    if ((first.type == TokenType::identifier || first.type == TokenType::string) &&
        peek_at(2U).type == TokenType::colon)
        return true;
    return false;
}

ExpressionPtr Parser::finish_lambda(std::vector<LambdaParameter> parameters, const SourceSpan start) {
    if (check(TokenType::left_brace) && !record_literal_ahead()) {
        BlockPtr block = parse_script_block();
        const SourceSpan span = covering(start, block->span);
        return make_expression(
            next_node_id_, LambdaExpression{std::move(parameters), nullptr, std::move(block), true},
            span);
    }
    ExpressionPtr body = expression();
    const SourceSpan span = covering(start, body->span);
    return make_expression(next_node_id_,
                           LambdaExpression{std::move(parameters), std::move(body), nullptr, true},
                           span);
}

BlockPtr Parser::parse_script_block() {
    const bool outer = in_script_;
    in_script_ = true;
    try {
        BlockPtr block = parse_block();
        in_script_ = outer;
        return block;
    } catch (...) {
        in_script_ = outer;
        throw;
    }
}

FunctionDeclaration Parser::parse_function(const Token& keyword, const bool asynchronous) {
    static_cast<void>(keyword);
    const Token& name = consume_identifier_like("STRATA.DSL.PARSE_DECLARATION_NAME",
                                                "Expected function name after fn.");
    static_cast<void>(consume(TokenType::left_parenthesis, "STRATA.DSL.PARSE_PARAMETER_LIST",
                              "Expected '(' after function name."));
    std::vector<Parameter> parameters = parse_parameters();
    std::optional<TypeReference> return_type;
    if (match(TokenType::colon))
        return_type = parse_type_reference();
    FunctionDeclaration function{name.lexeme, std::move(parameters), std::move(return_type),
                                 nullptr, nullptr, asynchronous, name.span};
    if (match(TokenType::equal)) {
        function.expression_body = expression();
        static_cast<void>(match(TokenType::semicolon));
    } else {
        function.block_body = parse_script_block();
    }
    return function;
}

StatementPtr Parser::parse_let_statement(const Token& keyword) {
    const bool mutable_binding = keyword.lexeme == "var";
    const Token& name = consume_identifier_like("STRATA.DSL.PARSE_LOCAL_NAME",
                                                "Expected a name after " + keyword.lexeme + ".");
    std::optional<TypeReference> type_reference;
    if (match(TokenType::colon))
        type_reference = parse_type_reference();
    static_cast<void>(consume(TokenType::equal, "STRATA.DSL.PARSE_LOCAL_INITIALIZER",
                              "Expected '=' and a value after the name."));
    ExpressionPtr initializer = expression();
    static_cast<void>(match(TokenType::semicolon));
    const SourceSpan span = covering(keyword.span, initializer->span);
    return make_statement(next_node_id_,
                          LetStatement{name.lexeme, mutable_binding, std::move(type_reference),
                                       std::move(initializer), name.span},
                          span);
}

StatementPtr Parser::parse_script_statement() {
    count_statement();
    const std::size_t first = current_;
    try {
        if (match(TokenType::semicolon))
            return nullptr;
        if ((contextual("let") || contextual("var")) && peek_at(1U).type == TokenType::identifier)
            return parse_let_statement(advance());
        if (function_start()) {
            const bool asynchronous = contextual("async");
            if (asynchronous)
                static_cast<void>(advance());
            const Token& keyword = advance();
            FunctionDeclaration function = parse_function(keyword, asynchronous);
            const SourceSpan span = covering(keyword.span, previous_or_peek().span);
            return make_statement(next_node_id_, FunctionStatement{std::move(function)}, span);
        }
        if (match(TokenType::if_keyword)) return parse_if_statement(previous_token());
        if (match(TokenType::when_keyword)) return parse_when_statement(previous_token());
        if (match(TokenType::for_keyword)) return parse_for_statement(previous_token());
        if (contextual("while") && peek_at(1U).type != TokenType::equal &&
            peek_at(1U).type != TokenType::dot) {
            const Token& keyword = advance();
            ExpressionPtr condition = parse_condition();
            BlockPtr block = parse_block();
            const SourceSpan span = covering(keyword.span, block->span);
            return make_statement(next_node_id_, WhileStatement{std::move(condition), std::move(block)},
                                  span);
        }
        if (contextual("break") || contextual("continue")) {
            const Token& keyword = advance();
            static_cast<void>(match(TokenType::semicolon));
            if (keyword.lexeme == "break")
                return make_statement(next_node_id_, BreakStatement{}, keyword.span);
            return make_statement(next_node_id_, ContinueStatement{}, keyword.span);
        }
        if (contextual("return")) {
            const Token& keyword = advance();
            ExpressionPtr value;
            if (!check(TokenType::semicolon) && !check(TokenType::right_brace))
                value = expression();
            static_cast<void>(match(TokenType::semicolon));
            const SourceSpan span = value != nullptr ? covering(keyword.span, value->span) : keyword.span;
            return make_statement(next_node_id_, ReturnStatement{std::move(value)}, span);
        }
        if (contextual("throw")) {
            const Token& keyword = advance();
            ExpressionPtr value = expression();
            static_cast<void>(match(TokenType::semicolon));
            const SourceSpan span = covering(keyword.span, value->span);
            return make_statement(next_node_id_, ThrowStatement{std::move(value)}, span);
        }
        if (contextual("try") && peek_at(1U).type == TokenType::left_brace) {
            const Token& keyword = advance();
            BlockPtr block = parse_block();
            if (!contextual("catch")) {
                throw error(peek(), "STRATA.DSL.PARSE_TRY_CATCH", "Expected 'catch' after the try block.");
            }
            static_cast<void>(advance());
            std::string catch_name = "error";
            if (identifier_like() && !check(TokenType::left_brace))
                catch_name = advance().lexeme;
            BlockPtr catch_block = parse_block();
            const SourceSpan span = covering(keyword.span, catch_block->span);
            return make_statement(next_node_id_,
                                  TryStatement{std::move(block), std::move(catch_name),
                                               std::move(catch_block)},
                                  span);
        }
        if (match(TokenType::error)) {
            record_unparsed(first);
            return make_statement(next_node_id_, ErrorStatement{}, previous_token().span);
        }
        ExpressionPtr target = expression();
        std::optional<AssignmentOperator> operation;
        if (match(TokenType::equal)) operation = AssignmentOperator::assign;
        else if (match(TokenType::plus_equal)) operation = AssignmentOperator::add;
        else if (match(TokenType::minus_equal)) operation = AssignmentOperator::subtract;
        else if (match(TokenType::star_equal)) operation = AssignmentOperator::multiply;
        else if (match(TokenType::slash_equal)) operation = AssignmentOperator::divide;
        else if (match(TokenType::percent_equal)) operation = AssignmentOperator::modulo;
        if (operation.has_value()) {
            ExpressionPtr value = expression();
            static_cast<void>(match(TokenType::semicolon));
            const SourceSpan span = covering(target->span, value->span);
            return make_statement(next_node_id_,
                                  AssignmentStatement{std::move(target), *operation, std::move(value)},
                                  span);
        }
        static_cast<void>(match(TokenType::semicolon));
        const SourceSpan span = target->span;
        return make_statement(next_node_id_, ExpressionStatement{std::move(target)}, span);
    } catch (const ParsePanic&) {
        const SourceSpan span = previous_or_peek().span;
        synchronize_block();
        record_unparsed(first);
        return make_statement(next_node_id_, ErrorStatement{}, span);
    }
}

} // namespace strata::compiler
