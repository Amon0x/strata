#include "compiler/lexer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace strata::compiler {
namespace {

struct Symbol final {
    std::string_view lexeme;
    TokenType type;
};

constexpr std::array symbols{
    Symbol{"->", TokenType::arrow},
    Symbol{"&&", TokenType::amp_amp},
    Symbol{"||", TokenType::pipe_pipe},
    Symbol{"==", TokenType::equal_equal},
    Symbol{"!=", TokenType::bang_equal},
    Symbol{"<=", TokenType::less_equal},
    Symbol{">=", TokenType::greater_equal},
    Symbol{"??", TokenType::question_question},
    Symbol{"(", TokenType::left_parenthesis},
    Symbol{")", TokenType::right_parenthesis},
    Symbol{"{", TokenType::left_brace},
    Symbol{"}", TokenType::right_brace},
    Symbol{"[", TokenType::left_bracket},
    Symbol{"]", TokenType::right_bracket},
    Symbol{",", TokenType::comma},
    Symbol{".", TokenType::dot},
    Symbol{":", TokenType::colon},
    Symbol{";", TokenType::semicolon},
    Symbol{"?", TokenType::question},
    Symbol{"+", TokenType::plus},
    Symbol{"-", TokenType::minus},
    Symbol{"*", TokenType::star},
    Symbol{"/", TokenType::slash},
    Symbol{"%", TokenType::percent},
    Symbol{"!", TokenType::bang},
    Symbol{"=", TokenType::equal},
    Symbol{"<", TokenType::less},
    Symbol{">", TokenType::greater},
};

[[nodiscard]] bool ascii_digit(const std::uint32_t code_point) noexcept {
    return code_point >= static_cast<std::uint32_t>('0') &&
           code_point <= static_cast<std::uint32_t>('9');
}

[[nodiscard]] bool ascii_hex_digit(const std::uint32_t code_point) noexcept {
    return ascii_digit(code_point) ||
           (code_point >= static_cast<std::uint32_t>('a') &&
            code_point <= static_cast<std::uint32_t>('f')) ||
           (code_point >= static_cast<std::uint32_t>('A') &&
            code_point <= static_cast<std::uint32_t>('F'));
}

[[nodiscard]] bool identifier_start(const std::uint32_t code_point) noexcept {
    return code_point == static_cast<std::uint32_t>('_') ||
           (code_point >= static_cast<std::uint32_t>('a') &&
            code_point <= static_cast<std::uint32_t>('z')) ||
           (code_point >= static_cast<std::uint32_t>('A') &&
            code_point <= static_cast<std::uint32_t>('Z')) ||
           code_point >= 0x80U;
}

[[nodiscard]] bool identifier_part(const std::uint32_t code_point) noexcept {
    return identifier_start(code_point) || ascii_digit(code_point);
}

[[nodiscard]] bool whitespace(const std::uint32_t code_point) noexcept {
    return code_point == static_cast<std::uint32_t>(' ') ||
           code_point == static_cast<std::uint32_t>('\r') ||
           code_point == static_cast<std::uint32_t>('\t') ||
           code_point == static_cast<std::uint32_t>('\n');
}

[[nodiscard]] TokenType keyword(const std::string_view value) noexcept {
    if (value == "import") return TokenType::import_keyword;
    if (value == "screen") return TokenType::screen;
    if (value == "overlay") return TokenType::overlay;
    if (value == "component") return TokenType::component;
    if (value == "style") return TokenType::style;
    if (value == "animation") return TokenType::animation;
    if (value == "extends") return TokenType::extends_keyword;
    if (value == "state") return TokenType::state;
    if (value == "derived") return TokenType::derived;
    if (value == "root") return TokenType::root;
    if (value == "if") return TokenType::if_keyword;
    if (value == "when") return TokenType::when_keyword;
    if (value == "else") return TokenType::else_keyword;
    if (value == "for") return TokenType::for_keyword;
    if (value == "in") return TokenType::in_keyword;
    if (value == "where") return TokenType::where;
    if (value == "true") return TokenType::true_keyword;
    if (value == "false") return TokenType::false_keyword;
    if (value == "null") return TokenType::null_keyword;
    if (value == "from") return TokenType::from;
    if (value == "to") return TokenType::to;
    return TokenType::identifier;
}

[[nodiscard]] bool is_symbol_initial(const std::uint32_t code_point) noexcept {
    for (const Symbol& symbol : symbols) {
        if (code_point == static_cast<std::uint32_t>(symbol.lexeme.front())) {
            return true;
        }
    }
    return false;
}

} // namespace

Lexer::Lexer(
    std::string source_id,
    std::string source,
    const std::size_t max_tokens,
    std::pmr::memory_resource* const scratch
)
    : buffer_(std::move(source_id), std::move(source), scratch), max_tokens_(max_tokens) {
    if (max_tokens_ == 0U) {
        throw std::invalid_argument("lexer token limit must be positive");
    }
}

LexResult Lexer::lex() {
    while (!at_end()) {
        if (tokens_.size() >= max_tokens_) {
            report(
                "STRATA.DSL.LIMIT_TOKENS",
                "Source exceeds the compiler token limit.",
                current_,
                current_
            );
            current_ = buffer_.size_bytes();
            break;
        }
        start_ = current_;
        scan_token();
    }
    tokens_.push_back(Token{
        TokenType::end_of_file,
        "",
        buffer_.span(current_, current_),
    });
    return LexResult{std::move(tokens_), std::move(diagnostics_)};
}

void Lexer::scan_token() {
    const std::uint32_t code_point = advance();
    if (whitespace(code_point)) {
        scan_trivia();
    } else if (code_point == static_cast<std::uint32_t>('/')) {
        scan_slash();
    } else if (code_point == static_cast<std::uint32_t>('"')) {
        scan_string();
    } else if (code_point == static_cast<std::uint32_t>('#')) {
        scan_color();
    } else if (ascii_digit(code_point)) {
        scan_number();
    } else if (identifier_start(code_point)) {
        scan_identifier();
    } else if (is_symbol_initial(code_point)) {
        scan_symbol(code_point);
    } else {
        unexpected_character();
    }
}

void Lexer::scan_symbol(const std::uint32_t first) {
    for (const Symbol& symbol : symbols) {
        if (first == static_cast<std::uint32_t>(symbol.lexeme.front()) &&
            buffer_.starts_with(start_, symbol.lexeme)) {
            current_ = start_ + symbol.lexeme.size();
            add_token(symbol.type);
            return;
        }
    }
    unexpected_character();
}

void Lexer::scan_slash() {
    if (match_ascii('/')) {
        while (peek() != static_cast<std::uint32_t>('\n') && !at_end()) {
            static_cast<void>(advance());
        }
        add_token(TokenType::trivia);
    } else if (match_ascii('*')) {
        scan_block_comment();
    } else {
        scan_symbol(static_cast<std::uint32_t>('/'));
    }
}

void Lexer::scan_trivia() {
    while (!at_end() && whitespace(peek())) {
        static_cast<void>(advance());
    }
    add_token(TokenType::trivia);
}

void Lexer::scan_block_comment() {
    bool closed = false;
    while (!at_end()) {
        if (peek() == static_cast<std::uint32_t>('*') &&
            peek_next() == static_cast<std::uint32_t>('/')) {
            static_cast<void>(advance());
            static_cast<void>(advance());
            closed = true;
            break;
        }
        static_cast<void>(advance());
    }
    if (!closed) {
        report(
            "STRATA.DSL.LEX_UNTERMINATED_COMMENT",
            "Unterminated block comment.",
            start_,
            current_
        );
    }
    add_token(TokenType::trivia);
}

void Lexer::scan_string() {
    bool escaped = false;
    while (!at_end()) {
        const std::uint32_t code_point = advance();
        if (escaped) {
            escaped = false;
        } else if (code_point == static_cast<std::uint32_t>('\\')) {
            escaped = true;
        } else if (code_point == static_cast<std::uint32_t>('"')) {
            add_token(TokenType::string);
            return;
        }
    }
    report(
        "STRATA.DSL.LEX_UNTERMINATED_STRING",
        "Unterminated string literal.",
        start_,
        current_
    );
    add_token(TokenType::error);
}

void Lexer::scan_color() {
    while (ascii_hex_digit(peek())) {
        static_cast<void>(advance());
    }
    const std::size_t digit_count = current_ - start_ - 1U;
    if (digit_count == 3U || digit_count == 4U || digit_count == 6U || digit_count == 8U) {
        add_token(TokenType::color);
        return;
    }
    report(
        "STRATA.DSL.LEX_INVALID_COLOR",
        "Color literals must use 3, 4, 6, or 8 hexadecimal digits.",
        start_,
        current_
    );
    add_token(TokenType::error);
}

void Lexer::scan_number() {
    scan_digits();
    if (peek() == static_cast<std::uint32_t>('.') && ascii_digit(peek_next())) {
        static_cast<void>(advance());
        scan_digits();
    }
    if (peek() == static_cast<std::uint32_t>('e') ||
        peek() == static_cast<std::uint32_t>('E')) {
        static_cast<void>(advance());
        if (peek() == static_cast<std::uint32_t>('+') ||
            peek() == static_cast<std::uint32_t>('-')) {
            static_cast<void>(advance());
        }
        if (ascii_digit(peek())) {
            scan_digits();
        } else {
            while (identifier_part(peek()) || peek() == static_cast<std::uint32_t>('+') ||
                   peek() == static_cast<std::uint32_t>('-')) {
                static_cast<void>(advance());
            }
            report(
                "STRATA.DSL.LEX_INVALID_NUMBER",
                "Malformed numeric exponent.",
                start_,
                current_
            );
            add_token(TokenType::error);
            return;
        }
    }
    if (identifier_start(peek())) {
        const std::size_t suffix_start = current_;
        while (identifier_part(peek())) {
            static_cast<void>(advance());
        }
        const std::string_view suffix = buffer_.slice(suffix_start, current_);
        if (suffix == "ms" || suffix == "s") {
            if (!valid_number_body(suffix_start)) {
                report(
                    "STRATA.DSL.LEX_INVALID_NUMBER",
                    "Numeric separators must appear between digits.",
                    start_,
                    current_
                );
                add_token(TokenType::error);
                return;
            }
            add_token(TokenType::number);
            return;
        }
        report(
            "STRATA.DSL.LEX_INVALID_NUMBER_SUFFIX",
            "Number literals cannot have identifier suffixes.",
            start_,
            current_
        );
        add_token(TokenType::error);
        return;
    }
    if (!valid_number_body(current_)) {
        report(
            "STRATA.DSL.LEX_INVALID_NUMBER",
            "Numeric separators must appear between digits.",
            start_,
            current_
        );
        add_token(TokenType::error);
        return;
    }
    add_token(TokenType::number);
}

void Lexer::scan_digits() {
    while (ascii_digit(peek()) || peek() == static_cast<std::uint32_t>('_')) {
        static_cast<void>(advance());
    }
}

void Lexer::scan_identifier() {
    while (identifier_part(peek())) {
        static_cast<void>(advance());
    }
    add_token(keyword(buffer_.slice(start_, current_)));
}

void Lexer::unexpected_character() {
    report(
        "STRATA.DSL.LEX_UNEXPECTED_CHAR",
        "Unexpected character '" + std::string(buffer_.slice(start_, current_)) + "'.",
        start_,
        current_
    );
    add_token(TokenType::error);
}

void Lexer::add_token(const TokenType type) {
    tokens_.push_back(Token{
        type,
        std::string(buffer_.slice(start_, current_)),
        buffer_.span(start_, current_),
    });
}

void Lexer::report(
    std::string code,
    std::string message,
    const std::size_t start,
    const std::size_t end
) {
    diagnostics_.push_back(Diagnostic{
        std::move(code),
        DiagnosticSeverity::error,
        std::move(message),
        buffer_.span(start, end).range(),
        std::nullopt,
        std::nullopt,
    });
}

bool Lexer::match_ascii(const char expected) noexcept {
    if (at_end() || buffer_.text()[current_] != expected) {
        return false;
    }
    ++current_;
    return true;
}

std::uint32_t Lexer::advance() noexcept {
    const std::uint32_t value = buffer_.code_point_at(current_).value_or(0U);
    current_ += buffer_.code_point_bytes_at(current_);
    return value;
}

std::uint32_t Lexer::peek() const noexcept {
    return buffer_.code_point_at(current_).value_or(0U);
}

std::uint32_t Lexer::peek_next() const noexcept {
    const std::size_t next = current_ + buffer_.code_point_bytes_at(current_);
    return buffer_.code_point_at(next).value_or(0U);
}

bool Lexer::at_end() const noexcept {
    return current_ >= buffer_.size_bytes();
}

bool Lexer::valid_number_body(const std::size_t end) const noexcept {
    const std::string_view body = std::string_view(buffer_.text()).substr(start_, end - start_);
    std::size_t offset = 0U;
    const auto digit_sequence = [&body, &offset]() {
        if (offset >= body.size() || body[offset] < '0' || body[offset] > '9') {
            return false;
        }
        ++offset;
        while (offset < body.size()) {
            if (body[offset] >= '0' && body[offset] <= '9') {
                ++offset;
            } else if (body[offset] == '_' && offset + 1U < body.size() &&
                       body[offset + 1U] >= '0' && body[offset + 1U] <= '9') {
                offset += 2U;
            } else {
                break;
            }
        }
        return true;
    };
    if (!digit_sequence()) {
        return false;
    }
    if (offset < body.size() && body[offset] == '.') {
        ++offset;
        if (!digit_sequence()) {
            return false;
        }
    }
    if (offset < body.size() && (body[offset] == 'e' || body[offset] == 'E')) {
        ++offset;
        if (offset < body.size() && (body[offset] == '+' || body[offset] == '-')) {
            ++offset;
        }
        if (!digit_sequence()) {
            return false;
        }
    }
    return offset == body.size();
}

std::string_view token_type_name(const TokenType type) noexcept {
    switch (type) {
    case TokenType::identifier: return "IDENTIFIER";
    case TokenType::string: return "STRING";
    case TokenType::number: return "NUMBER";
    case TokenType::color: return "COLOR";
    case TokenType::trivia: return "TRIVIA";
    case TokenType::import_keyword: return "IMPORT";
    case TokenType::screen: return "SCREEN";
    case TokenType::overlay: return "OVERLAY";
    case TokenType::component: return "COMPONENT";
    case TokenType::style: return "STYLE";
    case TokenType::animation: return "ANIMATION";
    case TokenType::extends_keyword: return "EXTENDS";
    case TokenType::state: return "STATE";
    case TokenType::derived: return "DERIVED";
    case TokenType::root: return "ROOT";
    case TokenType::if_keyword: return "IF";
    case TokenType::when_keyword: return "WHEN";
    case TokenType::else_keyword: return "ELSE";
    case TokenType::for_keyword: return "FOR";
    case TokenType::in_keyword: return "IN";
    case TokenType::where: return "WHERE";
    case TokenType::true_keyword: return "TRUE";
    case TokenType::false_keyword: return "FALSE";
    case TokenType::null_keyword: return "NULL";
    case TokenType::from: return "FROM";
    case TokenType::to: return "TO";
    case TokenType::left_parenthesis: return "LEFT_PAREN";
    case TokenType::right_parenthesis: return "RIGHT_PAREN";
    case TokenType::left_brace: return "LEFT_BRACE";
    case TokenType::right_brace: return "RIGHT_BRACE";
    case TokenType::left_bracket: return "LEFT_BRACKET";
    case TokenType::right_bracket: return "RIGHT_BRACKET";
    case TokenType::comma: return "COMMA";
    case TokenType::dot: return "DOT";
    case TokenType::colon: return "COLON";
    case TokenType::semicolon: return "SEMICOLON";
    case TokenType::question: return "QUESTION";
    case TokenType::plus: return "PLUS";
    case TokenType::minus: return "MINUS";
    case TokenType::star: return "STAR";
    case TokenType::slash: return "SLASH";
    case TokenType::percent: return "PERCENT";
    case TokenType::bang: return "BANG";
    case TokenType::equal: return "EQUAL";
    case TokenType::less: return "LESS";
    case TokenType::greater: return "GREATER";
    case TokenType::amp_amp: return "AMP_AMP";
    case TokenType::pipe_pipe: return "PIPE_PIPE";
    case TokenType::equal_equal: return "EQUAL_EQUAL";
    case TokenType::bang_equal: return "BANG_EQUAL";
    case TokenType::less_equal: return "LESS_EQUAL";
    case TokenType::greater_equal: return "GREATER_EQUAL";
    case TokenType::question_question: return "QUESTION_QUESTION";
    case TokenType::arrow: return "ARROW";
    case TokenType::error: return "ERROR";
    case TokenType::end_of_file: return "EOF";
    }
    return "UNKNOWN";
}

} // namespace strata::compiler
