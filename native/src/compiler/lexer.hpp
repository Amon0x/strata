#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>

#include "compiler/diagnostic.hpp"
#include "compiler/source.hpp"

namespace strata::compiler {

enum class TokenType {
    identifier,
    string,
    number,
    color,
    trivia,
    import_keyword,
    screen,
    overlay,
    component,
    style,
    animation,
    extends_keyword,
    state,
    derived,
    root,
    if_keyword,
    when_keyword,
    else_keyword,
    for_keyword,
    in_keyword,
    where,
    true_keyword,
    false_keyword,
    null_keyword,
    from,
    to,
    left_parenthesis,
    right_parenthesis,
    left_brace,
    right_brace,
    left_bracket,
    right_bracket,
    comma,
    dot,
    colon,
    semicolon,
    question,
    plus,
    minus,
    star,
    slash,
    percent,
    bang,
    equal,
    less,
    greater,
    amp_amp,
    pipe_pipe,
    equal_equal,
    bang_equal,
    less_equal,
    greater_equal,
    question_question,
    arrow,
    error,
    end_of_file,
};

struct Token final {
    TokenType type;
    std::string lexeme;
    SourceSpan span;

    [[nodiscard]] friend bool operator==(const Token&, const Token&) = default;
};

struct LexResult final {
    std::vector<Token> tokens;
    std::vector<Diagnostic> diagnostics;
};

class Lexer final {
public:
    static constexpr std::size_t default_max_tokens = 1'000'000U;

    Lexer(
        std::string source_id,
        std::string source,
        std::size_t max_tokens = default_max_tokens,
        std::pmr::memory_resource* scratch = std::pmr::get_default_resource()
    );

    [[nodiscard]] LexResult lex();

private:
    void scan_token();
    void scan_symbol(std::uint32_t first);
    void scan_slash();
    void scan_trivia();
    void scan_block_comment();
    void scan_string();
    void scan_color();
    void scan_number();
    void scan_digits();
    void scan_identifier();
    void unexpected_character();
    void add_token(TokenType type);
    void report(std::string code, std::string message, std::size_t start, std::size_t end);

    [[nodiscard]] bool match_ascii(char expected) noexcept;
    [[nodiscard]] std::uint32_t advance() noexcept;
    [[nodiscard]] std::uint32_t peek() const noexcept;
    [[nodiscard]] std::uint32_t peek_next() const noexcept;
    [[nodiscard]] bool at_end() const noexcept;
    [[nodiscard]] bool valid_number_body(std::size_t end) const noexcept;

    SourceBuffer buffer_;
    std::vector<Token> tokens_;
    std::vector<Diagnostic> diagnostics_;
    std::size_t max_tokens_;
    std::size_t start_ = 0U;
    std::size_t current_ = 0U;
};

[[nodiscard]] std::string_view token_type_name(TokenType type) noexcept;

} // namespace strata::compiler
