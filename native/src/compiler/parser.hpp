#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compiler/ast.hpp"
#include "compiler/diagnostic.hpp"
#include "compiler/lexer.hpp"

namespace strata::compiler {

struct ParseResult final {
    File file;
    std::vector<Diagnostic> diagnostics;
};

struct ParserLimits final {
    std::size_t max_source_bytes = 8U * 1024U * 1024U;
    std::size_t max_tokens = Lexer::default_max_tokens;
    std::size_t max_nesting_depth = 256U;
    std::size_t max_declarations = 10'000U;
    std::size_t max_statements = 100'000U;
    std::size_t max_expressions = 500'000U;
    std::size_t max_collection_items = 100'000U;
};

class Parser final {
public:
    Parser(
        std::string source_id,
        std::vector<Token> tokens,
        std::vector<Diagnostic> diagnostics,
        ParserLimits limits = {},
        NodeId first_node_id = 0U
    );

    [[nodiscard]] ParseResult parse();
    [[nodiscard]] ExpressionPtr expression();

private:
    enum class DeclarationKind { screen, overlay };
    enum class BodyKind { general, animation };
    enum class Precedence {
        lowest,
        conditional,
        coalesce,
        logical_or,
        logical_and,
        equality,
        comparison,
        term,
        factor,
        unary,
        call,
        primary,
    };
    class ParsePanic final {};
    class LimitPanic final {};
    class NestingGuard final {
    public:
        explicit NestingGuard(Parser& parser);
        ~NestingGuard();
        NestingGuard(const NestingGuard&) = delete;
        NestingGuard& operator=(const NestingGuard&) = delete;

    private:
        Parser& parser_;
    };

    [[nodiscard]] Import parse_import(const Token& keyword);
    [[nodiscard]] std::optional<Declaration> parse_declaration();
    [[nodiscard]] Declaration parse_named_block_declaration(
        const Token& keyword,
        DeclarationKind kind
    );
    [[nodiscard]] Declaration parse_component_declaration(const Token& keyword);
    [[nodiscard]] Declaration parse_style_declaration(const Token& keyword);
    [[nodiscard]] Declaration parse_animation_declaration(const Token& keyword);
    /** `fn name(params)[: Type] { ... }` or `= expr`, after `fn` (and `async`). */
    [[nodiscard]] FunctionDeclaration parse_function(const Token& keyword, bool asynchronous);
    /** A StrataScript statement, inside a function or lambda block. */
    [[nodiscard]] StatementPtr parse_script_statement();
    [[nodiscard]] StatementPtr parse_let_statement(const Token& keyword);
    /** A script block: `{ statements }`, parsed as StrataScript. */
    [[nodiscard]] BlockPtr parse_script_block();
    /** After the parameters and `=>`: the lambda's expression or block body. */
    [[nodiscard]] ExpressionPtr finish_lambda(std::vector<LambdaParameter> parameters,
                                              SourceSpan start);
    /** At `(`: whether `( params ) =>` follows, so the parenthesis opens a lambda. */
    [[nodiscard]] bool lambda_parameters_ahead() const noexcept;
    /** At `{` after `=>`: whether it opens a record literal rather than a block. */
    [[nodiscard]] bool record_literal_ahead() const noexcept;
    [[nodiscard]] std::vector<LambdaParameter> parse_lambda_parameters();
    /** The current token is the identifier `word` (a contextual keyword). */
    [[nodiscard]] bool contextual(std::string_view word) const noexcept;
    /** `fn name` or `async fn name` starts here. */
    [[nodiscard]] bool function_start() const noexcept;
    [[nodiscard]] const Token& peek_at(std::size_t offset) const noexcept;
    [[nodiscard]] std::vector<Parameter> parse_parameters();
    [[nodiscard]] TypeReference parse_type_reference();
    /** One option of a union: a name, a string, a record or function type, or a grouping. */
    TypeReference parse_type_option();
    /** `type Name = Type` after its keyword. */
    TypeDeclaration parse_type_declaration(const Token& keyword);
    [[nodiscard]] bool type_declaration_start() const noexcept;
    /** A template string: its text and expressions joined as text. */
    ExpressionPtr parse_template(const Token& head);
    [[nodiscard]] std::string decode_template(const Token& token) const;
    [[nodiscard]] BlockPtr parse_block(BodyKind kind = BodyKind::general);
    [[nodiscard]] StatementPtr parse_statement(BodyKind kind);
    [[nodiscard]] StatementPtr parse_state_declaration(const Token& keyword);
    [[nodiscard]] StatementPtr parse_derived_declaration(const Token& keyword);
    [[nodiscard]] StatementPtr parse_root_statement(const Token& keyword);
    [[nodiscard]] StatementPtr parse_if_statement(const Token& keyword);
    [[nodiscard]] StatementPtr parse_when_statement(const Token& keyword);
    [[nodiscard]] StatementPtr parse_for_statement(const Token& keyword);
    [[nodiscard]] StatementPtr parse_animation_frame_statement(const Token& keyword);
    [[nodiscard]] std::pair<std::vector<Property>, SourceSpan> parse_property_block();
    [[nodiscard]] StatementPtr parse_property_statement();
    [[nodiscard]] Property parse_property();
    [[nodiscard]] StatementPtr parse_widget_statement();
    [[nodiscard]] WidgetCall parse_widget_call();
    [[nodiscard]] std::vector<Argument> parse_arguments(TokenType terminator);
    [[nodiscard]] Argument parse_argument();
    [[nodiscard]] ExpressionPtr parse_condition();
    [[nodiscard]] ExpressionPtr parse_expression(Precedence precedence);
    [[nodiscard]] ExpressionPtr parse_prefix();
    [[nodiscard]] ExpressionPtr parse_unary(const Token& token, UnaryOperator operation);
    [[nodiscard]] ExpressionPtr parse_grouping(const Token& left);
    [[nodiscard]] ExpressionPtr parse_list(const Token& left);
    [[nodiscard]] ExpressionPtr parse_map(const Token& left);
    [[nodiscard]] std::optional<MapEntry> parse_map_entry();
    [[nodiscard]] ExpressionPtr parse_infix(ExpressionPtr left);
    [[nodiscard]] ExpressionPtr finish_call(ExpressionPtr callee, const Token& left_parenthesis);
    [[nodiscard]] std::optional<CallTarget> call_target(const ExpressionPtr& expression) const;
    [[nodiscard]] ExpressionPtr finish_property_access(ExpressionPtr receiver, const Token& dot);
    [[nodiscard]] ExpressionPtr finish_index(ExpressionPtr receiver, const Token& left_bracket);
    [[nodiscard]] ExpressionPtr finish_conditional(ExpressionPtr condition, const Token& question);
    [[nodiscard]] ExpressionPtr finish_binary(ExpressionPtr left);

    [[nodiscard]] Precedence current_precedence() const noexcept;
    [[nodiscard]] static Precedence precedence(TokenType type) noexcept;
    [[nodiscard]] static Precedence previous(Precedence value) noexcept;
    [[nodiscard]] static BinaryOperator binary_operator(TokenType type);
    [[nodiscard]] static NumberLiteral number_literal(const Token& token);
    [[nodiscard]] static std::string decode_string(const Token& token);

    [[nodiscard]] const Token& consume_identifier(std::string code, std::string message);
    [[nodiscard]] const Token& consume_identifier_like(std::string code, std::string message);
    [[nodiscard]] const Token& consume_widget_name(std::string code, std::string message);
    [[nodiscard]] const Token& consume_name_token(std::string code, std::string message);
    [[nodiscard]] const Token& consume(TokenType type, std::string code, std::string message);
    [[nodiscard]] ParsePanic error(const Token& token, std::string code, std::string message);
    [[noreturn]] void limit(std::string code, std::string message);
    void count_declaration();
    void count_statement();
    void count_expression();
    void check_collection_size(std::size_t size);
    void synchronize_top_level();
    void synchronize_block();
    /** Records the names in tokens `first` up to the current one, which produced no node. */
    void record_unparsed(std::size_t first);

    [[nodiscard]] bool declaration_start() const noexcept;
    [[nodiscard]] bool name_token() const noexcept;
    [[nodiscard]] bool identifier_like() const noexcept;
    [[nodiscard]] bool widget_name() const noexcept;
    [[nodiscard]] static bool expression_recovery_token(TokenType type) noexcept;
    [[nodiscard]] bool match(TokenType type);
    [[nodiscard]] bool match(TokenType first, TokenType second);
    [[nodiscard]] bool check(TokenType type) const noexcept;
    [[nodiscard]] bool check_next(TokenType type) const noexcept;
    /** The message for a missing '}', naming the opening line when the file ended first. */
    [[nodiscard]] std::string unclosed_message(std::string_view what, const Token& open) const;
    [[nodiscard]] const Token& advance();
    [[nodiscard]] bool at_end() const noexcept;
    [[nodiscard]] const Token& peek() const noexcept;
    [[nodiscard]] const Token& previous_token() const noexcept;
    [[nodiscard]] const Token& previous_or_peek() const noexcept;
    [[nodiscard]] static std::string_view close_lexeme(TokenType type) noexcept;

    std::string source_id_;
    std::vector<Token> tokens_;
    std::vector<Diagnostic> diagnostics_;
    std::vector<std::string> unparsed_names_;
    bool truncated_ = false;
    ParserLimits limits_;
    std::size_t current_ = 0U;
    std::size_t nesting_depth_ = 0U;
    std::size_t declaration_count_ = 0U;
    std::size_t statement_count_ = 0U;
    std::size_t expression_count_ = 0U;
    NodeId next_node_id_ = 0U;
    /** Inside a function or lambda block: statements are StrataScript. */
    bool in_script_ = false;
};

[[nodiscard]] ParseResult parse_source(
    std::string source_id,
    std::string source,
    ParserLimits limits = {},
    std::pmr::memory_resource* scratch = std::pmr::get_default_resource(),
    NodeId first_node_id = 0U
);

} // namespace strata::compiler
