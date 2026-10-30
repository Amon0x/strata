#include "ui/code_highlight.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <span>
#include <string>
#include <string_view>

namespace strata::ui {
namespace {

using Words = std::span<const std::string_view>;

[[nodiscard]] bool digit(const char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] bool alpha(const char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80U;
}

[[nodiscard]] bool word_char(const char c) noexcept {
    return alpha(c) || digit(c);
}

[[nodiscard]] bool hex(const char c) noexcept {
    return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

[[nodiscard]] bool space(const char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

[[nodiscard]] char lower(const char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool equal_folded(const std::string_view left,
                                const std::string_view right) noexcept {
    return left.size() == right.size() &&
           std::ranges::equal(left, right,
                              [](const char a, const char b) { return lower(a) == lower(b); });
}

[[nodiscard]] bool contains(const Words words, const std::string_view word,
                            const bool folded = false) noexcept {
    if (folded) {
        return std::ranges::any_of(words, [word](const std::string_view candidate) {
            return equal_folded(candidate, word);
        });
    }
    return std::ranges::find(words, word) != words.end();
}

/**
 * What a C-family lexer knows about one language. Everything else (numbers, identifiers, calls,
 * operators, punctuation) is shared.
 */
struct Rules final {
    Words keywords;
    Words types;
    Words builtins;
    Words constants;
    /** Keywords after which the next identifier names a function or a type being declared. */
    Words function_introducers;
    Words type_introducers;
    std::array<std::string_view, 2U> line_comments{};
    std::string_view block_open;
    std::string_view block_close;
    bool nested_blocks = false;
    /** Characters that open a string (or a character literal) closed by the same character. */
    std::string_view quotes = "\"'";
    /** Quotes whose strings may span lines (Go's raw backticks); the others end at a newline. */
    std::string_view multiline_quotes;
    bool triple_quotes = false;
    bool template_strings = false;
    bool regex_literals = false;
    bool preprocessor = false;
    bool decorators = false;
    bool pascal_types = false;
    bool upper_constants = false;
    /** `name:` is a property (Strata's arguments and style fields, JSON keys). */
    bool property_keys = false;
    bool hash_colors = false;
    bool folded_keywords = false;
    bool dollar_identifiers = false;
    bool rust = false;
    bool python_prefixes = false;
    bool csharp_strings = false;
    bool cpp_strings = false;
    /** A double-quoted run is a quoted identifier, not a string (SQL). */
    bool quoted_identifiers = false;
};

constexpr Words none{};

class Output final {
  public:
    explicit Output(std::vector<SyntaxSpan>& spans, const std::size_t base = 0U) noexcept
        : spans_(spans), base_(base) {}

    void add(const std::size_t start, const std::size_t end, const SyntaxKind kind) {
        if (end <= start || kind == SyntaxKind::plain)
            return;
        const auto from = static_cast<std::uint32_t>(base_ + start);
        const auto length = static_cast<std::uint32_t>(end - start);
        if (!spans_.empty() && spans_.back().kind == kind &&
            spans_.back().start + spans_.back().length == from) {
            spans_.back().length += length;
            return;
        }
        spans_.push_back(SyntaxSpan{from, length, kind});
    }

    [[nodiscard]] std::size_t base() const noexcept {
        return base_;
    }
    [[nodiscard]] std::vector<SyntaxSpan>& spans() noexcept {
        return spans_;
    }

  private:
    std::vector<SyntaxSpan>& spans_;
    std::size_t base_ = 0U;
};

void highlight_into(std::string_view source, std::string_view language, Output& output);

/** The end of the line `at` is on (the newline's index, or the end). */
[[nodiscard]] std::size_t line_end(const std::string_view text, const std::size_t at) noexcept {
    const std::size_t found = text.find('\n', at);
    return found == std::string_view::npos ? text.size() : found;
}

/** Whether only blanks precede `at` on its line. */
[[nodiscard]] bool line_start(const std::string_view text, std::size_t at) noexcept {
    while (at > 0U) {
        const char c = text[at - 1U];
        if (c == '\n')
            return true;
        if (c != ' ' && c != '\t')
            return false;
        --at;
    }
    return true;
}

[[nodiscard]] std::size_t skip_blanks(const std::string_view text, std::size_t at) noexcept {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t'))
        ++at;
    return at;
}

/** A number literal from `at`: digits, a radix prefix, a fraction, an exponent and a suffix. */
[[nodiscard]] std::size_t number_end(const std::string_view text, std::size_t at) noexcept {
    const bool radix = text[at] == '0' && at + 1U < text.size() &&
                       std::string_view("xXbBoO").find(text[at + 1U]) != std::string_view::npos;
    while (at < text.size()) {
        const char c = text[at];
        if (word_char(c)) {
            ++at;
            // An exponent's sign: 1e-5, but never inside a hexadecimal literal.
            if (!radix && (c == 'e' || c == 'E') && at < text.size() &&
                (text[at] == '+' || text[at] == '-') && at + 1U < text.size() &&
                digit(text[at + 1U])) {
                ++at;
            }
            continue;
        }
        // One point between digits; `0..10` is a range, not a fraction.
        if (c == '.' && at + 1U < text.size() && text[at + 1U] != '.' &&
            (digit(text[at + 1U]) || !radix)) {
            if (at + 1U < text.size() && alpha(text[at + 1U]) && !digit(text[at + 1U]))
                break;
            ++at;
            continue;
        }
        break;
    }
    return at;
}

/** The end of a string opened at `at` by `quote`, after any escapes; at a newline unless allowed.
 */
[[nodiscard]] std::size_t quoted_end(const std::string_view text, std::size_t at,
                                     const std::string_view quote, const bool multiline,
                                     const bool escapes = true) noexcept {
    at += quote.size();
    while (at < text.size()) {
        if (escapes && text[at] == '\\') {
            at += 2U;
            continue;
        }
        if (text.substr(at).starts_with(quote))
            return at + quote.size();
        if (text[at] == '\n' && !multiline)
            return at;
        ++at;
    }
    return text.size();
}

/** The index of the brace that closes the one before `at`, skipping nested braces and strings. */
[[nodiscard]] std::size_t closing_brace(const std::string_view text, std::size_t at) noexcept {
    std::size_t depth = 1U;
    while (at < text.size()) {
        const char c = text[at];
        if (c == '"' || c == '\'' || c == '`') {
            at = quoted_end(text, at, text.substr(at, 1U), true);
            continue;
        }
        if (c == '{') {
            ++depth;
        } else if (c == '}' && --depth == 0U) {
            return at;
        }
        ++at;
    }
    return text.size();
}

class CLexer final {
  public:
    CLexer(const std::string_view text, const Rules& rules, Output& output) noexcept
        : text_(text), rules_(rules), output_(output) {}

    void run() {
        while (at_ < text_.size()) {
            const std::size_t start = at_;
            if (!step()) {
                // Nothing claimed it: one character (a whole UTF-8 sequence) of plain text.
                ++at_;
                while (at_ < text_.size() &&
                       (static_cast<unsigned char>(text_[at_]) & 0xC0U) == 0x80U)
                    ++at_;
            }
            if (at_ == start)
                ++at_;
        }
    }

  private:
    void emit(const std::size_t start, const std::size_t end, const SyntaxKind kind) {
        output_.add(start, end, kind);
        if (kind != SyntaxKind::comment)
            previous_ = kind;
    }

    [[nodiscard]] bool starts(const std::string_view token) const noexcept {
        return !token.empty() && text_.substr(at_).starts_with(token);
    }

    [[nodiscard]] char peek(const std::size_t offset = 0U) const noexcept {
        return at_ + offset < text_.size() ? text_[at_ + offset] : '\0';
    }

    /** Where a regex literal may begin: after an operator, an opening bracket, or a keyword. */
    [[nodiscard]] bool regex_allowed() const noexcept {
        return previous_ == SyntaxKind::plain || previous_ == SyntaxKind::operator_symbol ||
               previous_ == SyntaxKind::keyword ||
               (previous_ == SyntaxKind::punctuation && last_punctuation_ != ')' &&
                last_punctuation_ != ']' && last_punctuation_ != '}');
    }

    bool step() {
        const char c = text_[at_];
        if (space(c)) {
            ++at_;
            return true;
        }
        if (rules_.preprocessor && c == '#' && line_start(text_, at_)) {
            // A directive runs to its line's end, and on over escaped newlines.
            std::size_t end = line_end(text_, at_);
            while (end > at_ && end < text_.size() && text_[end - 1U] == '\\')
                end = line_end(text_, end + 1U);
            emit(at_, end, SyntaxKind::meta);
            at_ = end;
            return true;
        }
        if (rules_.rust && c == '#' && (peek(1U) == '[' || (peek(1U) == '!' && peek(2U) == '['))) {
            std::size_t end = text_.find(']', at_);
            end = end == std::string_view::npos ? text_.size() : end + 1U;
            emit(at_, end, SyntaxKind::meta);
            at_ = end;
            return true;
        }
        for (const std::string_view comment : rules_.line_comments) {
            if (starts(comment)) {
                const std::size_t end = line_end(text_, at_);
                emit(at_, end, SyntaxKind::comment);
                at_ = end;
                return true;
            }
        }
        if (starts(rules_.block_open)) {
            std::size_t depth = 1U;
            std::size_t end = at_ + rules_.block_open.size();
            while (end < text_.size() && depth > 0U) {
                if (rules_.nested_blocks && text_.substr(end).starts_with(rules_.block_open)) {
                    ++depth;
                    end += rules_.block_open.size();
                } else if (text_.substr(end).starts_with(rules_.block_close)) {
                    --depth;
                    end += rules_.block_close.size();
                } else {
                    ++end;
                }
            }
            emit(at_, end, SyntaxKind::comment);
            at_ = end;
            return true;
        }
        if (rules_.hash_colors && c == '#' && hex(peek(1U))) {
            std::size_t end = at_ + 1U;
            while (end < text_.size() && hex(text_[end]))
                ++end;
            emit(at_, end, SyntaxKind::constant);
            at_ = end;
            return true;
        }
        if (string_literal())
            return true;
        if (digit(c) || (c == '.' && digit(peek(1U)))) {
            const std::size_t end = number_end(text_, at_);
            emit(at_, end, SyntaxKind::number);
            at_ = end;
            return true;
        }
        if (alpha(c) || (rules_.dollar_identifiers && c == '$')) {
            identifier();
            return true;
        }
        if (rules_.decorators && c == '@' && alpha(peek(1U))) {
            std::size_t end = at_ + 1U;
            while (end < text_.size() && (word_char(text_[end]) || text_[end] == '.'))
                ++end;
            emit(at_, end, SyntaxKind::meta);
            at_ = end;
            return true;
        }
        if (rules_.regex_literals && c == '/' && peek(1U) != '/' && peek(1U) != '*' &&
            regex_allowed()) {
            if (regex())
                return true;
        }
        if (std::string_view("+-*/%=<>!&|^~?:").find(c) != std::string_view::npos) {
            std::size_t end = at_ + 1U;
            // A run of operator characters, stopping where a comment begins (`a=//x`).
            const auto comment_at = [this](const std::size_t index) {
                const std::string_view rest = text_.substr(index);
                return (!rules_.line_comments[0].empty() &&
                        rest.starts_with(rules_.line_comments[0])) ||
                       (!rules_.block_open.empty() && rest.starts_with(rules_.block_open));
            };
            while (end < text_.size() &&
                   std::string_view("+-*/%=<>!&|^~?:").find(text_[end]) != std::string_view::npos &&
                   !comment_at(end)) {
                ++end;
            }
            emit(at_, end, SyntaxKind::operator_symbol);
            at_ = end;
            return true;
        }
        if (std::string_view("(){}[];,.@#").find(c) != std::string_view::npos) {
            last_punctuation_ = c;
            emit(at_, at_ + 1U, SyntaxKind::punctuation);
            ++at_;
            return true;
        }
        return false;
    }

    /** Emits a string from `start` to `end`, a property instead when it is a key (`"name":`). */
    void string_span(const std::size_t start, const std::size_t end) {
        const std::size_t after = skip_blanks(text_, end);
        const bool key = rules_.property_keys && after < text_.size() && text_[after] == ':' &&
                         (after + 1U >= text_.size() || text_[after + 1U] != ':');
        emit(start, end, key ? SyntaxKind::property : SyntaxKind::string);
        at_ = end;
    }

    bool string_literal() {
        const char c = text_[at_];
        std::size_t prefix = 0U;
        // Prefixed strings: Python's r"", f"", b"" (and pairs), C++'s L"", u8"", R"(...)" and
        // C#'s @"" and $"".
        if (rules_.python_prefixes) {
            while (prefix < 2U &&
                   std::string_view("rRbBfFuU").find(peek(prefix)) != std::string_view::npos)
                ++prefix;
            if (prefix > 0U && peek(prefix) != '"' && peek(prefix) != '\'')
                prefix = 0U;
        }
        if (rules_.cpp_strings) {
            for (const std::string_view candidate :
                 {"u8R", "LR", "uR", "UR", "R", "u8", "L", "u", "U"}) {
                if (starts(candidate) && peek(candidate.size()) == '"') {
                    prefix = candidate.size();
                    break;
                }
            }
            if (prefix > 0U && text_[at_ + prefix - 1U] == 'R') {
                // A raw string: R"delimiter( ... )delimiter".
                const std::size_t open = at_ + prefix + 1U;
                const std::size_t paren = text_.find('(', open);
                if (paren != std::string_view::npos && paren - open <= 16U) {
                    const std::string close =
                        ")" + std::string(text_.substr(open, paren - open)) + "\"";
                    const std::size_t found = text_.find(close, paren);
                    const std::size_t end =
                        found == std::string_view::npos ? text_.size() : found + close.size();
                    emit(at_, end, SyntaxKind::string);
                    at_ = end;
                    return true;
                }
            }
        }
        if (rules_.csharp_strings && (c == '@' || c == '$')) {
            std::size_t index = 0U;
            bool verbatim = false;
            while (index < 2U && (peek(index) == '@' || peek(index) == '$')) {
                verbatim = verbatim || peek(index) == '@';
                ++index;
            }
            if (peek(index) == '"') {
                std::size_t end = at_ + index + 1U;
                while (end < text_.size()) {
                    if (!verbatim && text_[end] == '\\') {
                        end += 2U;
                        continue;
                    }
                    if (text_[end] == '"') {
                        if (verbatim && end + 1U < text_.size() && text_[end + 1U] == '"') {
                            end += 2U;
                            continue;
                        }
                        ++end;
                        break;
                    }
                    if (text_[end] == '\n' && !verbatim)
                        break;
                    ++end;
                }
                emit(at_, end, SyntaxKind::string);
                at_ = end;
                return true;
            }
        }
        if (rules_.rust) {
            // Raw strings r"..", r#".."#, byte strings b"..", br#".."#.
            std::size_t index = 0U;
            if (peek(index) == 'b')
                ++index;
            if (peek(index) == 'r') {
                ++index;
                std::size_t hashes = 0U;
                while (peek(index + hashes) == '#')
                    ++hashes;
                if (peek(index + hashes) == '"') {
                    const std::string close = "\"" + std::string(hashes, '#');
                    const std::size_t found = text_.find(close, at_ + index + hashes + 1U);
                    const std::size_t end =
                        found == std::string_view::npos ? text_.size() : found + close.size();
                    emit(at_, end, SyntaxKind::string);
                    at_ = end;
                    return true;
                }
            } else if (index == 1U && (peek(1U) == '"' || peek(1U) == '\'')) {
                prefix = 1U;
            }
            // A lifetime ('a, 'static) is not a character literal.
            if (c == '\'' && alpha(peek(1U)) && peek(2U) != '\'' && !(peek(1U) == '\\')) {
                std::size_t end = at_ + 1U;
                while (end < text_.size() && word_char(text_[end]))
                    ++end;
                if (end >= text_.size() || text_[end] != '\'') {
                    emit(at_, end, SyntaxKind::meta);
                    at_ = end;
                    return true;
                }
            }
        }
        const char quote = peek(prefix);
        if (rules_.quotes.find(quote) == std::string_view::npos || quote == '\0')
            return false;
        const std::size_t open = at_ + prefix;
        if (rules_.triple_quotes && text_.substr(open).starts_with(std::string(3U, quote))) {
            const std::size_t end = quoted_end(text_, open, std::string(3U, quote), true);
            emit(at_, end, SyntaxKind::string);
            at_ = end;
            return true;
        }
        if (rules_.template_strings && quote == '`') {
            template_string(open);
            return true;
        }
        const bool multiline = rules_.multiline_quotes.find(quote) != std::string_view::npos;
        const std::size_t end =
            quoted_end(text_, open, text_.substr(open, 1U), multiline, quote != '`' || !multiline);
        if (rules_.quoted_identifiers && quote == '"') {
            emit(at_, end, SyntaxKind::property);
            at_ = end;
            return true;
        }
        string_span(at_, end);
        return true;
    }

    /** A template literal: its text is a string, each `${...}` inside it code. */
    void template_string(std::size_t at) {
        std::size_t run = at;
        ++at;
        while (at < text_.size()) {
            if (text_[at] == '\\') {
                at += 2U;
                continue;
            }
            if (text_[at] == '`') {
                ++at;
                break;
            }
            if (text_[at] == '$' && at + 1U < text_.size() && text_[at + 1U] == '{') {
                emit(run, at, SyntaxKind::string);
                emit(at, at + 2U, SyntaxKind::punctuation);
                const std::size_t close = closing_brace(text_, at + 2U);
                Output inner(output_.spans(), output_.base() + at + 2U);
                CLexer nested(text_.substr(at + 2U, close - at - 2U), rules_, inner);
                nested.run();
                if (close < text_.size())
                    emit(close, close + 1U, SyntaxKind::punctuation);
                at = std::min(close + 1U, text_.size());
                run = at;
                continue;
            }
            ++at;
        }
        at = std::min(at, text_.size());
        emit(run, at, SyntaxKind::string);
        at_ = at;
    }

    bool regex() {
        std::size_t end = at_ + 1U;
        bool in_class = false;
        while (end < text_.size() && text_[end] != '\n') {
            const char c = text_[end];
            if (c == '\\') {
                end += 2U;
                continue;
            }
            if (c == '[')
                in_class = true;
            else if (c == ']')
                in_class = false;
            else if (c == '/' && !in_class)
                break;
            ++end;
        }
        if (end >= text_.size() || text_[end] != '/')
            return false;
        ++end;
        while (end < text_.size() && alpha(text_[end]))
            ++end;
        emit(at_, end, SyntaxKind::string);
        at_ = end;
        return true;
    }

    void identifier() {
        const std::size_t start = at_;
        std::size_t end = at_ + 1U;
        while (end < text_.size() &&
               (word_char(text_[end]) || (rules_.dollar_identifiers && text_[end] == '$')))
            ++end;
        const std::string_view word = text_.substr(start, end - start);
        at_ = end;
        const std::size_t after = skip_blanks(text_, end);
        const char next = after < text_.size() ? text_[after] : '\0';
        const SyntaxKind introduced = introduced_;
        introduced_ = SyntaxKind::plain;
        const bool folded = rules_.folded_keywords;
        SyntaxKind kind = SyntaxKind::plain;
        if (rules_.rust && end < text_.size() && text_[end] == '!' &&
            (end + 1U >= text_.size() || text_[end + 1U] != '=')) {
            // A macro call: name!(...).
            emit(start, end + 1U, SyntaxKind::function);
            at_ = end + 1U;
            return;
        }
        if (rules_.property_keys && next == ':' &&
            (after + 1U >= text_.size() || text_[after + 1U] != ':')) {
            kind = SyntaxKind::property;
        } else if (contains(rules_.constants, word, folded)) {
            kind = SyntaxKind::constant;
        } else if (contains(rules_.keywords, word, folded)) {
            kind = SyntaxKind::keyword;
            if (contains(rules_.function_introducers, word))
                introduced_ = SyntaxKind::function;
            else if (contains(rules_.type_introducers, word))
                introduced_ = SyntaxKind::type;
        } else if (contains(rules_.types, word, folded)) {
            kind = SyntaxKind::type;
        } else if (introduced != SyntaxKind::plain) {
            kind = introduced;
        } else if (contains(rules_.builtins, word, folded)) {
            kind = next == '(' ? SyntaxKind::function : SyntaxKind::builtin;
        } else if (next == '(') {
            kind = SyntaxKind::function;
        } else if (rules_.upper_constants && word.size() > 1U &&
                   std::ranges::all_of(word,
                                       [](const char c) {
                                           return (c >= 'A' && c <= 'Z') || digit(c) || c == '_';
                                       }) &&
                   std::ranges::any_of(word, [](const char c) { return c >= 'A' && c <= 'Z'; })) {
            kind = SyntaxKind::constant;
        } else if (rules_.pascal_types && word.front() >= 'A' && word.front() <= 'Z') {
            kind = SyntaxKind::type;
        }
        if (kind == SyntaxKind::plain) {
            // A plain name is still a value: a `/` after it divides rather than opening a regex.
            previous_ = SyntaxKind::variable;
            return;
        }
        emit(start, end, kind);
    }

    std::string_view text_;
    const Rules& rules_;
    Output& output_;
    std::size_t at_ = 0U;
    SyntaxKind previous_ = SyntaxKind::plain;
    char last_punctuation_ = '\0';
    SyntaxKind introduced_ = SyntaxKind::plain;
};

// ---------------------------------------------------------------------------------------------
// Languages built on the C-family lexer

constexpr std::string_view json_constants[] = {"true", "false", "null", "NaN"};

constexpr std::string_view js_keywords[] = {
    "async",    "await",      "break",      "case",    "catch",    "class", "const",
    "continue", "debugger",   "default",    "delete",  "do",       "else",  "export",
    "extends",  "finally",    "for",        "from",    "function", "get",   "if",
    "import",   "in",         "instanceof", "let",     "new",      "of",    "return",
    "set",      "static",     "super",      "switch",  "this",     "throw", "try",
    "typeof",   "var",        "void",       "while",   "with",     "yield", "as",
    "enum",     "implements", "interface",  "package",
};
constexpr std::string_view ts_extra_keywords[] = {
    "abstract",  "declare", "is",       "keyof", "module", "namespace", "private",
    "protected", "public",  "readonly", "type",  "infer",  "satisfies", "override",
    "asserts",   "unique",  "accessor", "out",   "global", "require",   "constructor",
};
constexpr std::string_view ts_types[] = {
    "any",    "boolean", "never",  "number", "object",    "string",
    "symbol", "unknown", "bigint", "void",   "undefined", "null",
};
constexpr std::string_view js_constants[] = {"true", "false", "null", "undefined", "NaN"};
constexpr std::string_view js_builtins[] = {
    "console", "window", "document", "globalThis", "Math",   "JSON",  "Object", "Array",
    "Promise", "Number", "String",   "Boolean",    "Symbol", "Error", "Map",    "Set",
};
constexpr std::string_view js_functions[] = {"function"};
constexpr std::string_view js_types_introducers[] = {"class", "interface", "type", "enum"};

constexpr std::string_view python_keywords[] = {
    "and",  "as",     "assert", "async", "await",  "break",   "case",     "class", "continue",
    "def",  "del",    "elif",   "else",  "except", "finally", "for",      "from",  "global",
    "if",   "import", "in",     "is",    "lambda", "match",   "nonlocal", "not",   "or",
    "pass", "raise",  "return", "try",   "while",  "with",    "yield",    "type",
};
constexpr std::string_view python_constants[] = {"True", "False", "None", "NotImplemented"};
constexpr std::string_view python_builtins[] = {
    "abs",    "all",    "any",     "bool",    "dict",  "enumerate", "filter",
    "float",  "format", "getattr", "hasattr", "input", "int",       "isinstance",
    "iter",   "len",    "list",    "map",     "max",   "min",       "next",
    "object", "open",   "print",   "range",   "repr",  "reversed",  "round",
    "self",   "set",    "sorted",  "str",     "sum",   "super",
};
constexpr std::string_view python_functions[] = {"def"};
constexpr std::string_view python_type_introducers[] = {"class"};

constexpr std::string_view c_keywords[] = {
    "auto",     "break",     "case",      "const",     "continue",  "default",   "do",
    "else",     "enum",      "extern",    "for",       "goto",      "if",        "inline",
    "register", "restrict",  "return",    "sizeof",    "static",    "struct",    "switch",
    "typedef",  "union",     "volatile",  "while",     "alignas",   "alignof",   "class",
    "concept",  "constexpr", "consteval", "constinit", "co_await",  "co_return", "co_yield",
    "decltype", "delete",    "explicit",  "export",    "friend",    "mutable",   "namespace",
    "new",      "noexcept",  "operator",  "private",   "protected", "public",    "requires",
    "template",
};
constexpr std::string_view cpp_extra_keywords[] = {
    "this",       "throw",         "try",   "catch",       "typename",     "using",
    "virtual",    "override",      "final", "static_cast", "dynamic_cast", "reinterpret_cast",
    "const_cast", "static_assert",
};
constexpr std::string_view c_types[] = {
    "bool",     "char",     "double",    "float",    "int",      "long",     "short",
    "signed",   "unsigned", "void",      "size_t",   "int8_t",   "int16_t",  "int32_t",
    "int64_t",  "uint8_t",  "uint16_t",  "uint32_t", "uint64_t", "wchar_t",  "char8_t",
    "char16_t", "char32_t", "ptrdiff_t", "string",   "vector",   "optional", "string_view",
};
constexpr std::string_view c_constants[] = {"true", "false", "nullptr", "NULL", "this"};
constexpr std::string_view c_type_introducers[] = {"struct", "class", "union", "enum", "namespace"};

constexpr std::string_view csharp_keywords[] = {
    "abstract", "as",       "async",      "await",    "base",     "break",   "case",
    "catch",    "checked",  "class",      "const",    "continue", "default", "delegate",
    "do",       "else",     "enum",       "event",    "explicit", "extern",  "finally",
    "fixed",    "for",      "foreach",    "get",      "goto",     "if",      "implicit",
    "in",       "init",     "interface",  "internal", "is",       "lock",    "namespace",
    "new",      "operator", "out",        "override", "params",   "private", "protected",
    "public",   "readonly", "record",     "ref",      "required", "return",  "sealed",
    "set",      "sizeof",   "stackalloc", "static",   "struct",   "switch",  "throw",
    "try",      "typeof",   "unchecked",  "unsafe",   "using",    "var",     "virtual",
    "while",
};
constexpr std::string_view csharp_types[] = {
    "bool", "byte",   "char",  "decimal", "double", "dynamic", "float", "int",    "long",
    "nint", "object", "sbyte", "short",   "string", "uint",    "ulong", "ushort",
};
constexpr std::string_view csharp_constants[] = {"true", "false", "null", "this", "value"};
constexpr std::string_view csharp_type_introducers[] = {"class", "struct", "interface", "enum",
                                                        "record"};

constexpr std::string_view rust_keywords[] = {
    "as",     "async",  "await",  "break",       "const", "continue", "crate",  "dyn",
    "else",   "enum",   "extern", "fn",          "for",   "if",       "impl",   "in",
    "let",    "loop",   "match",  "mod",         "move",  "mut",      "pub",    "ref",
    "return", "static", "struct", "super",       "trait", "type",     "unsafe", "use",
    "where",  "while",  "yield",  "macro_rules", "union", "default",
};
constexpr std::string_view rust_types[] = {
    "bool", "char", "f32", "f64", "i8",   "i16",   "i32",    "i64", "i128",   "isize",  "str",
    "u8",   "u16",  "u32", "u64", "u128", "usize", "String", "Vec", "Option", "Result",
};
constexpr std::string_view rust_constants[] = {"true", "false", "self", "Self",
                                               "None", "Some",  "Ok"};
constexpr std::string_view rust_builtins[] = {"Err"};
constexpr std::string_view rust_functions[] = {"fn"};
constexpr std::string_view rust_type_introducers[] = {"struct", "enum", "trait", "type", "union"};

constexpr std::string_view go_keywords[] = {
    "break",  "case",        "chan", "const",   "continue", "default", "defer",
    "else",   "fallthrough", "for",  "func",    "go",       "goto",    "if",
    "import", "interface",   "map",  "package", "range",    "return",  "select",
    "struct", "switch",      "type", "var",
};
constexpr std::string_view go_types[] = {
    "bool", "byte",  "complex64", "complex128", "error",  "float32", "float64",
    "int",  "int8",  "int16",     "int32",      "int64",  "rune",    "string",
    "uint", "uint8", "uint16",    "uint32",     "uint64", "uintptr",
};
constexpr std::string_view go_constants[] = {"true", "false", "nil", "iota"};
constexpr std::string_view go_builtins[] = {
    "append", "cap",  "clear", "close", "complex", "copy",  "delete", "imag",
    "len",    "make", "max",   "min",   "new",     "panic", "print",
};
constexpr std::string_view go_functions[] = {"func"};
constexpr std::string_view go_type_introducers[] = {"type"};

constexpr std::string_view sql_keywords[] = {
    "add",       "all",     "alter",      "and",         "as",      "asc",       "begin",
    "between",   "by",      "case",       "check",       "column",  "commit",    "constraint",
    "create",    "cross",   "database",   "default",     "delete",  "desc",      "distinct",
    "drop",      "else",    "end",        "exists",      "foreign", "from",      "full",
    "group",     "having",  "if",         "in",          "index",   "inner",     "insert",
    "into",      "is",      "join",       "key",         "left",    "like",      "limit",
    "not",       "offset",  "on",         "or",          "order",   "outer",     "over",
    "partition", "primary", "references", "returning",   "right",   "rollback",  "select",
    "set",       "table",   "then",       "transaction", "union",   "unique",    "update",
    "values",    "view",    "when",       "where",       "with",    "recursive", "using",
};
constexpr std::string_view sql_types[] = {
    "bigint",  "binary", "blob",     "boolean", "char",    "date",      "datetime",
    "decimal", "double", "float",    "int",     "integer", "json",      "numeric",
    "real",    "serial", "smallint", "text",    "time",    "timestamp", "varchar",
};
constexpr std::string_view sql_constants[] = {"true", "false", "null"};
constexpr std::string_view sql_builtins[] = {
    "avg", "coalesce", "count", "lower", "max",    "min",
    "now", "round",    "sum",   "upper", "length", "cast",
};

constexpr std::string_view strata_keywords[] = {
    "import", "from",   "as",    "screen",   "overlay", "component", "style",  "animation",
    "extends", "state", "derived", "root",   "if",      "when",      "else",   "for",
    "in",     "where",  "to",    "fn",       "async",   "await",     "let",    "var",
    "while",  "break",  "continue", "return", "throw",  "try",       "catch",
};
constexpr std::string_view strata_constants[] = {"true", "false", "null"};
constexpr std::string_view strata_builtins[] = {"theme", "env", "item"};
constexpr std::string_view strata_type_introducers[] = {"component", "overlay"};

/** One rule set per language, built once. */
[[nodiscard]] Rules json_rules() {
    Rules rules;
    rules.constants = json_constants;
    rules.line_comments = {"//", ""};
    rules.block_open = "/*";
    rules.block_close = "*/";
    rules.quotes = "\"";
    rules.property_keys = true;
    return rules;
}

[[nodiscard]] Rules script_rules(const bool typescript) {
    static const std::vector<std::string_view> ts_keywords = [] {
        std::vector<std::string_view> words(std::begin(js_keywords), std::end(js_keywords));
        words.insert(words.end(), std::begin(ts_extra_keywords), std::end(ts_extra_keywords));
        return words;
    }();
    Rules rules;
    rules.keywords = typescript ? Words(ts_keywords) : Words(js_keywords);
    rules.types = typescript ? Words(ts_types) : none;
    rules.constants = js_constants;
    rules.builtins = js_builtins;
    rules.function_introducers = js_functions;
    rules.type_introducers = js_types_introducers;
    rules.line_comments = {"//", ""};
    rules.block_open = "/*";
    rules.block_close = "*/";
    rules.quotes = "\"'`";
    rules.template_strings = true;
    rules.regex_literals = true;
    rules.decorators = true;
    rules.pascal_types = true;
    rules.upper_constants = true;
    rules.dollar_identifiers = true;
    return rules;
}

[[nodiscard]] Rules python_rules() {
    Rules rules;
    rules.keywords = python_keywords;
    rules.constants = python_constants;
    rules.builtins = python_builtins;
    rules.function_introducers = python_functions;
    rules.type_introducers = python_type_introducers;
    rules.line_comments = {"#", ""};
    rules.quotes = "\"'";
    rules.triple_quotes = true;
    rules.decorators = true;
    rules.python_prefixes = true;
    rules.upper_constants = true;
    rules.pascal_types = true;
    return rules;
}

[[nodiscard]] Rules c_rules(const bool cpp) {
    static const std::vector<std::string_view> cpp_keywords = [] {
        std::vector<std::string_view> words(std::begin(c_keywords), std::end(c_keywords));
        words.insert(words.end(), std::begin(cpp_extra_keywords), std::end(cpp_extra_keywords));
        return words;
    }();
    Rules rules;
    rules.keywords = cpp ? Words(cpp_keywords) : Words(c_keywords);
    rules.types = c_types;
    rules.constants = c_constants;
    rules.type_introducers = c_type_introducers;
    rules.line_comments = {"//", ""};
    rules.block_open = "/*";
    rules.block_close = "*/";
    rules.preprocessor = true;
    rules.cpp_strings = true;
    rules.upper_constants = true;
    rules.pascal_types = cpp;
    return rules;
}

[[nodiscard]] Rules csharp_rules() {
    Rules rules;
    rules.keywords = csharp_keywords;
    rules.types = csharp_types;
    rules.constants = csharp_constants;
    rules.type_introducers = csharp_type_introducers;
    rules.line_comments = {"//", ""};
    rules.block_open = "/*";
    rules.block_close = "*/";
    rules.preprocessor = true;
    rules.csharp_strings = true;
    rules.pascal_types = true;
    return rules;
}

[[nodiscard]] Rules rust_rules() {
    Rules rules;
    rules.keywords = rust_keywords;
    rules.types = rust_types;
    rules.constants = rust_constants;
    rules.builtins = rust_builtins;
    rules.function_introducers = rust_functions;
    rules.type_introducers = rust_type_introducers;
    rules.line_comments = {"//", ""};
    rules.block_open = "/*";
    rules.block_close = "*/";
    rules.nested_blocks = true;
    rules.quotes = "\"'";
    rules.multiline_quotes = "\"";
    rules.rust = true;
    rules.pascal_types = true;
    rules.upper_constants = true;
    return rules;
}

[[nodiscard]] Rules go_rules() {
    Rules rules;
    rules.keywords = go_keywords;
    rules.types = go_types;
    rules.constants = go_constants;
    rules.builtins = go_builtins;
    rules.function_introducers = go_functions;
    rules.type_introducers = go_type_introducers;
    rules.line_comments = {"//", ""};
    rules.block_open = "/*";
    rules.block_close = "*/";
    rules.quotes = "\"'`";
    rules.multiline_quotes = "`";
    rules.pascal_types = true;
    return rules;
}

[[nodiscard]] Rules sql_rules() {
    Rules rules;
    rules.keywords = sql_keywords;
    rules.types = sql_types;
    rules.constants = sql_constants;
    rules.builtins = sql_builtins;
    rules.line_comments = {"--", ""};
    rules.block_open = "/*";
    rules.block_close = "*/";
    rules.quotes = "'\"`";
    rules.multiline_quotes = "'";
    rules.folded_keywords = true;
    rules.quoted_identifiers = true;
    return rules;
}

[[nodiscard]] Rules strata_rules() {
    Rules rules;
    rules.keywords = strata_keywords;
    rules.constants = strata_constants;
    rules.builtins = strata_builtins;
    rules.type_introducers = strata_type_introducers;
    rules.line_comments = {"//", ""};
    rules.block_open = "/*";
    rules.block_close = "*/";
    rules.quotes = "\"";
    rules.property_keys = true;
    rules.hash_colors = true;
    rules.pascal_types = true;
    return rules;
}

void lex(const std::string_view text, const Rules& rules, Output& output) {
    CLexer lexer(text, rules, output);
    lexer.run();
}

// ---------------------------------------------------------------------------------------------
// CSS

constexpr std::string_view css_important[] = {"important", "inherit"};

void lex_css(const std::string_view text, Output& output) {
    std::size_t at = 0U;
    std::size_t depth = 0U;
    // Inside a declaration block, before a property's colon (a name) or after it (a value).
    bool value = false;
    // Whether the last token was a property name, so a colon now starts its value rather than a
    // pseudo-class (`a:hover` inside a nested or @media rule).
    bool after_property = false;
    while (at < text.size()) {
        const char c = text[at];
        if (space(c)) {
            ++at;
            continue;
        }
        if (text.substr(at).starts_with("/*")) {
            const std::size_t found = text.find("*/", at + 2U);
            const std::size_t end = found == std::string_view::npos ? text.size() : found + 2U;
            output.add(at, end, SyntaxKind::comment);
            at = end;
            continue;
        }
        if (c == '"' || c == '\'') {
            const std::size_t end = quoted_end(text, at, text.substr(at, 1U), false);
            output.add(at, end, SyntaxKind::string);
            at = end;
            continue;
        }
        if (c == '@') {
            std::size_t end = at + 1U;
            while (end < text.size() && (word_char(text[end]) || text[end] == '-'))
                ++end;
            output.add(at, end, SyntaxKind::keyword);
            at = end;
            continue;
        }
        if (c == '{' || c == '}' || c == ';' || c == ':' || c == ',' || c == '(' || c == ')') {
            if (c == '{') {
                ++depth;
                value = false;
            } else if (c == '}') {
                depth = depth > 0U ? depth - 1U : 0U;
                value = false;
            } else if (c == ';') {
                value = false;
            }
            const bool starts_value = c == ':' && depth > 0U && !value && after_property;
            if (starts_value)
                value = true;
            after_property = false;
            output.add(at, at + 1U,
                       c == ':' && !starts_value ? SyntaxKind::keyword : SyntaxKind::punctuation);
            ++at;
            continue;
        }
        after_property = false;
        if (digit(c) && !(depth > 0U && value)) {
            // A number outside a value: a media query's width, a keyframe's percentage.
            std::size_t end = at + 1U;
            while (end < text.size() &&
                   (word_char(text[end]) || text[end] == '.' || text[end] == '%'))
                ++end;
            output.add(at, end, SyntaxKind::number);
            at = end;
            continue;
        }
        const bool declarations = depth > 0U;
        if (declarations && value) {
            if (c == '#') {
                std::size_t end = at + 1U;
                while (end < text.size() && word_char(text[end]))
                    ++end;
                output.add(at, end, SyntaxKind::number);
                at = end;
                continue;
            }
            if (digit(c) ||
                ((c == '.' || c == '-') && at + 1U < text.size() && digit(text[at + 1U]))) {
                std::size_t end = at + 1U;
                while (end < text.size() &&
                       (word_char(text[end]) || text[end] == '.' || text[end] == '%'))
                    ++end;
                output.add(at, end, SyntaxKind::number);
                at = end;
                continue;
            }
            if (c == '!') {
                std::size_t end = at + 1U;
                while (end < text.size() && word_char(text[end]))
                    ++end;
                output.add(at, end, SyntaxKind::keyword);
                at = end;
                continue;
            }
            if (alpha(c) || c == '-') {
                std::size_t end = at + 1U;
                while (end < text.size() && (word_char(text[end]) || text[end] == '-'))
                    ++end;
                const bool call = end < text.size() && text[end] == '(';
                const std::string_view word = text.substr(at, end - at);
                output.add(at, end,
                           call                            ? SyntaxKind::function
                           : contains(css_important, word) ? SyntaxKind::keyword
                                                           : SyntaxKind::constant);
                at = end;
                continue;
            }
        } else if (declarations && (alpha(c) || c == '-')) {
            // A property name, or a nested rule's selector (which has no colon before its brace).
            std::size_t end = at + 1U;
            while (end < text.size() && (word_char(text[end]) || text[end] == '-'))
                ++end;
            const std::size_t after = skip_blanks(text, end);
            const bool pseudo = at > 0U && text[at - 1U] == ':';
            const bool property = !pseudo && after < text.size() && text[after] == ':';
            output.add(at, end,
                       pseudo     ? SyntaxKind::keyword
                       : property ? SyntaxKind::property
                                  : SyntaxKind::tag);
            after_property = property;
            at = end;
            continue;
        } else {
            // A selector: tags, .classes, #ids, :pseudo-classes and [attributes].
            if (c == '.' || c == '#') {
                std::size_t end = at + 1U;
                while (end < text.size() && (word_char(text[end]) || text[end] == '-'))
                    ++end;
                output.add(at, end, c == '.' ? SyntaxKind::type : SyntaxKind::constant);
                at = end;
                continue;
            }
            if (c == '[') {
                const std::size_t found = text.find(']', at);
                const std::size_t end = found == std::string_view::npos ? text.size() : found + 1U;
                output.add(at, end, SyntaxKind::attribute);
                at = end;
                continue;
            }
            if (alpha(c) || c == '-' || c == '*') {
                std::size_t end = at + 1U;
                while (end < text.size() && (word_char(text[end]) || text[end] == '-'))
                    ++end;
                // After a selector colon, a pseudo-class.
                const bool pseudo = at > 0U && text[at - 1U] == ':';
                output.add(at, end, pseudo ? SyntaxKind::keyword : SyntaxKind::tag);
                at = end;
                continue;
            }
        }
        if (std::string_view("+>~*/=").find(c) != std::string_view::npos) {
            output.add(at, at + 1U, SyntaxKind::operator_symbol);
            ++at;
            continue;
        }
        ++at;
    }
}

// ---------------------------------------------------------------------------------------------
// HTML and XML

[[nodiscard]] std::size_t find_folded(const std::string_view text, const std::string_view needle,
                                      std::size_t from) noexcept {
    while (from + needle.size() <= text.size()) {
        if (equal_folded(text.substr(from, needle.size()), needle))
            return from;
        ++from;
    }
    return std::string_view::npos;
}

void lex_markup(const std::string_view text, Output& output, const bool html) {
    std::size_t at = 0U;
    while (at < text.size()) {
        const char c = text[at];
        if (c == '&') {
            std::size_t end = at + 1U;
            while (end < text.size() && end - at < 12U &&
                   (word_char(text[end]) || text[end] == '#'))
                ++end;
            if (end < text.size() && text[end] == ';') {
                output.add(at, end + 1U, SyntaxKind::constant);
                at = end + 1U;
                continue;
            }
            ++at;
            continue;
        }
        if (c != '<') {
            ++at;
            continue;
        }
        const std::string_view rest = text.substr(at);
        const auto block = [&](const std::string_view close, const SyntaxKind kind) {
            const std::size_t found = text.find(close, at);
            const std::size_t end =
                found == std::string_view::npos ? text.size() : found + close.size();
            output.add(at, end, kind);
            at = end;
        };
        if (rest.starts_with("<!--")) {
            block("-->", SyntaxKind::comment);
            continue;
        }
        if (rest.starts_with("<![CDATA[")) {
            block("]]>", SyntaxKind::string);
            continue;
        }
        if (rest.starts_with("<!") || rest.starts_with("<?")) {
            block(rest[1] == '?' ? "?>" : ">", SyntaxKind::meta);
            continue;
        }
        // A tag: its name, then attributes, then its end.
        std::size_t index = at + 1U;
        const bool closing = index < text.size() && text[index] == '/';
        if (closing)
            ++index;
        if (index >= text.size() || !(alpha(text[index]))) {
            ++at;
            continue;
        }
        output.add(at, index, SyntaxKind::punctuation);
        std::size_t end = index;
        while (end < text.size() &&
               (word_char(text[end]) || text[end] == '-' || text[end] == ':' || text[end] == '.'))
            ++end;
        const std::string_view name = text.substr(index, end - index);
        output.add(index, end, SyntaxKind::tag);
        at = end;
        while (at < text.size() && text[at] != '>') {
            const char d = text[at];
            if (space(d)) {
                ++at;
                continue;
            }
            if (d == '/') {
                output.add(at, at + 1U, SyntaxKind::punctuation);
                ++at;
                continue;
            }
            if (d == '=') {
                output.add(at, at + 1U, SyntaxKind::operator_symbol);
                ++at;
                continue;
            }
            if (d == '"' || d == '\'') {
                const std::size_t close = text.find(d, at + 1U);
                const std::size_t stop = close == std::string_view::npos ? text.size() : close + 1U;
                output.add(at, stop, SyntaxKind::string);
                at = stop;
                continue;
            }
            std::size_t stop = at;
            while (stop < text.size() && !space(text[stop]) && text[stop] != '=' &&
                   text[stop] != '>' && text[stop] != '/' && text[stop] != '"' &&
                   text[stop] != '\'')
                ++stop;
            if (stop == at)
                stop = at + 1U;
            output.add(at, stop, SyntaxKind::attribute);
            at = stop;
        }
        if (at < text.size()) {
            output.add(at, at + 1U, SyntaxKind::punctuation);
            ++at;
        }
        // An HTML script or style element's content is code of its own language.
        if (html && !closing && (equal_folded(name, "script") || equal_folded(name, "style"))) {
            const bool script = equal_folded(name, "script");
            const std::size_t close = find_folded(text, script ? "</script" : "</style", at);
            const std::size_t stop = close == std::string_view::npos ? text.size() : close;
            Output inner(output.spans(), output.base() + at);
            highlight_into(text.substr(at, stop - at), script ? "javascript" : "css", inner);
            at = stop;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Shell

constexpr std::string_view shell_keywords[] = {
    "if",     "then",  "else",   "elif",     "fi",      "for",    "while",
    "until",  "do",    "done",   "case",     "esac",    "in",     "function",
    "return", "local", "export", "readonly", "declare", "select", "time",
};
constexpr std::string_view shell_builtins[] = {
    "alias", "cd",    "echo",   "eval", "exec", "exit",  "printf", "pwd",  "read",
    "set",   "shift", "source", "test", "trap", "unset", "cat",    "grep", "sed",
    "awk",   "ls",    "mkdir",  "rm",   "cp",   "mv",    "sudo",
};

/** A variable reference at `at` ($NAME, ${...}, $(...), $1, $@): its end, or `at` when none. */
[[nodiscard]] std::size_t shell_variable(const std::string_view text,
                                         const std::size_t at) noexcept {
    if (at + 1U >= text.size())
        return at;
    const char next = text[at + 1U];
    if (next == '{' || next == '(') {
        const char close = next == '{' ? '}' : ')';
        std::size_t depth = 0U;
        for (std::size_t index = at + 1U; index < text.size(); ++index) {
            if (text[index] == next)
                ++depth;
            else if (text[index] == close && --depth == 0U)
                return index + 1U;
        }
        return text.size();
    }
    if (alpha(next)) {
        std::size_t end = at + 2U;
        while (end < text.size() && word_char(text[end]))
            ++end;
        return end;
    }
    if (digit(next) || std::string_view("@*#?$!-").find(next) != std::string_view::npos)
        return at + 2U;
    return at;
}

void lex_shell(const std::string_view text, Output& output) {
    std::size_t at = 0U;
    bool command = true;
    while (at < text.size()) {
        const char c = text[at];
        if (c == '\n' || c == ';' || c == '|' || c == '&') {
            command = true;
            if (c != '\n')
                output.add(at, at + 1U, SyntaxKind::operator_symbol);
            ++at;
            continue;
        }
        if (space(c)) {
            ++at;
            continue;
        }
        if (c == '#' && (at == 0U || space(text[at - 1U]))) {
            const std::size_t end = line_end(text, at);
            output.add(at, end, SyntaxKind::comment);
            at = end;
            continue;
        }
        if (c == '\'') {
            const std::size_t found = text.find('\'', at + 1U);
            const std::size_t end = found == std::string_view::npos ? text.size() : found + 1U;
            output.add(at, end, SyntaxKind::string);
            at = end;
            command = false;
            continue;
        }
        if (c == '"') {
            // A double-quoted string interpolates variables, which keep their own colour.
            std::size_t run = at;
            std::size_t index = at + 1U;
            while (index < text.size() && text[index] != '"') {
                if (text[index] == '\\') {
                    index += 2U;
                    continue;
                }
                if (text[index] == '$') {
                    const std::size_t end = shell_variable(text, index);
                    if (end > index) {
                        output.add(run, index, SyntaxKind::string);
                        output.add(index, end, SyntaxKind::variable);
                        index = end;
                        run = end;
                        continue;
                    }
                }
                ++index;
            }
            index = std::min(index + 1U, text.size());
            output.add(run, index, SyntaxKind::string);
            at = index;
            command = false;
            continue;
        }
        if (c == '$') {
            const std::size_t end = shell_variable(text, at);
            if (end > at) {
                output.add(at, end, SyntaxKind::variable);
                at = end;
                command = false;
                continue;
            }
        }
        if (c == '>' || c == '<') {
            output.add(at, at + 1U, SyntaxKind::operator_symbol);
            ++at;
            continue;
        }
        // A word: a keyword, the command it starts, an option, an assignment or an argument.
        std::size_t end = at;
        while (end < text.size() && !space(text[end]) &&
               std::string_view(";|&<>\"'$#()").find(text[end]) == std::string_view::npos)
            ++end;
        if (end == at) {
            output.add(at, at + 1U, SyntaxKind::punctuation);
            ++at;
            continue;
        }
        const std::string_view word = text.substr(at, end - at);
        if (contains(shell_keywords, word)) {
            output.add(at, end, SyntaxKind::keyword);
            command = word != "in";
        } else if (const std::size_t equals = word.find('=');
                   command && equals != std::string_view::npos && equals > 0U &&
                   std::ranges::all_of(word.substr(0U, equals), word_char)) {
            output.add(at, at + equals, SyntaxKind::variable);
            output.add(at + equals, at + equals + 1U, SyntaxKind::operator_symbol);
        } else if (word.front() == '-') {
            output.add(at, end, SyntaxKind::attribute);
        } else if (std::ranges::all_of(word, [](const char d) { return digit(d) || d == '.'; })) {
            output.add(at, end, SyntaxKind::number);
        } else if (command) {
            output.add(at, end,
                       contains(shell_builtins, word) ? SyntaxKind::builtin : SyntaxKind::function);
            command = false;
        }
        at = end;
    }
}

// ---------------------------------------------------------------------------------------------
// Markdown

/** Inline Markdown in one line: code spans, emphasis, links and autolinks. */
void lex_markdown_inline(const std::string_view text, const std::size_t from, const std::size_t to,
                         Output& output) {
    std::size_t at = from;
    while (at < to) {
        const char c = text[at];
        if (c == '\\' && at + 1U < to) {
            at += 2U;
            continue;
        }
        if (c == '`') {
            std::size_t ticks = 0U;
            while (at + ticks < to && text[at + ticks] == '`')
                ++ticks;
            const std::size_t close = text.substr(0U, to).find(std::string(ticks, '`'), at + ticks);
            if (close != std::string_view::npos) {
                output.add(at, close + ticks, SyntaxKind::string);
                at = close + ticks;
                continue;
            }
            at += ticks;
            continue;
        }
        if (c == '*' || c == '_') {
            std::size_t marks = 0U;
            while (at + marks < to && text[at + marks] == c && marks < 3U)
                ++marks;
            const std::string closing(marks, c);
            const std::size_t close = text.substr(0U, to).find(closing, at + marks);
            if (close != std::string_view::npos && close > at + marks && !space(text[at + marks])) {
                output.add(at, close + marks,
                           marks >= 2U ? SyntaxKind::heading : SyntaxKind::emphasis);
                at = close + marks;
                continue;
            }
            at += marks;
            continue;
        }
        if (c == '!' && at + 1U < to && text[at + 1U] == '[') {
            ++at;
            continue;
        }
        if (c == '[') {
            const std::size_t label = text.substr(0U, to).find(']', at + 1U);
            if (label != std::string_view::npos && label + 1U < to && text[label + 1U] == '(') {
                const std::size_t target = text.substr(0U, to).find(')', label + 2U);
                if (target != std::string_view::npos) {
                    output.add(at, at + 1U, SyntaxKind::punctuation);
                    output.add(at + 1U, label, SyntaxKind::link);
                    output.add(label, label + 2U, SyntaxKind::punctuation);
                    output.add(label + 2U, target, SyntaxKind::string);
                    output.add(target, target + 1U, SyntaxKind::punctuation);
                    at = target + 1U;
                    continue;
                }
            }
        }
        if (c == '<') {
            const std::size_t close = text.substr(0U, to).find('>', at + 1U);
            if (close != std::string_view::npos && (text.substr(at + 1U).starts_with("http") ||
                                                    text.substr(at + 1U).starts_with("mailto:"))) {
                output.add(at, close + 1U, SyntaxKind::link);
                at = close + 1U;
                continue;
            }
        }
        ++at;
    }
}

void lex_markdown(const std::string_view text, Output& output) {
    std::size_t at = 0U;
    while (at < text.size()) {
        const std::size_t end = line_end(text, at);
        const std::size_t first = skip_blanks(text, at);
        const std::string_view line = text.substr(first, end - first);
        if (line.starts_with("```") || line.starts_with("~~~")) {
            // A fenced block: its fences are meta, its body is the language its info names.
            const char fence = line.front();
            std::size_t ticks = 0U;
            while (ticks < line.size() && line[ticks] == fence)
                ++ticks;
            std::string_view info = line.substr(ticks);
            while (!info.empty() && space(info.front()))
                info.remove_prefix(1U);
            const std::size_t word = info.find_first_of(" \t{");
            const std::string_view language = info.substr(0U, word);
            output.add(first, end, SyntaxKind::meta);
            std::size_t body = end < text.size() ? end + 1U : end;
            std::size_t close = body;
            while (close < text.size()) {
                const std::size_t close_end = line_end(text, close);
                const std::size_t start = skip_blanks(text, close);
                if (text.substr(start, close_end - start).starts_with(std::string(ticks, fence)))
                    break;
                close = close_end < text.size() ? close_end + 1U : close_end;
            }
            if (close > body) {
                Output inner(output.spans(), output.base() + body);
                highlight_into(text.substr(body, close - body), language, inner);
            }
            if (close < text.size()) {
                const std::size_t close_end = line_end(text, close);
                output.add(close, close_end, SyntaxKind::meta);
                at = close_end < text.size() ? close_end + 1U : close_end;
            } else {
                at = text.size();
            }
            continue;
        }
        if (line.starts_with('#')) {
            output.add(first, end, SyntaxKind::heading);
        } else if (line.starts_with('>')) {
            output.add(first, first + 1U, SyntaxKind::comment);
            lex_markdown_inline(text, first + 1U, end, output);
        } else if ((line.starts_with("---") || line.starts_with("***") ||
                    line.starts_with("___")) &&
                   std::ranges::all_of(
                       line, [&line](const char c) { return c == line.front() || c == ' '; })) {
            output.add(first, end, SyntaxKind::punctuation);
        } else {
            std::size_t body = first;
            // A list item's marker: -, *, + or a number with a point.
            if (!line.empty() &&
                (line.front() == '-' || line.front() == '*' || line.front() == '+') &&
                line.size() > 1U && line[1] == ' ') {
                output.add(first, first + 1U, SyntaxKind::keyword);
                body = first + 1U;
            } else {
                std::size_t digits = 0U;
                while (digits < line.size() && digit(line[digits]))
                    ++digits;
                if (digits > 0U && digits + 1U < line.size() &&
                    (line[digits] == '.' || line[digits] == ')') && line[digits + 1U] == ' ') {
                    output.add(first, first + digits + 1U, SyntaxKind::keyword);
                    body = first + digits + 1U;
                }
            }
            lex_markdown_inline(text, body, end, output);
        }
        at = end < text.size() ? end + 1U : end;
    }
}

// ---------------------------------------------------------------------------------------------

struct Alias final {
    std::string_view name;
    std::string_view language;
};

constexpr std::array aliases{
    Alias{"json", "json"},
    Alias{"jsonc", "json"},
    Alias{"json5", "json"},
    Alias{"javascript", "javascript"},
    Alias{"js", "javascript"},
    Alias{"mjs", "javascript"},
    Alias{"cjs", "javascript"},
    Alias{"jsx", "javascript"},
    Alias{"node", "javascript"},
    Alias{"typescript", "typescript"},
    Alias{"ts", "typescript"},
    Alias{"tsx", "typescript"},
    Alias{"mts", "typescript"},
    Alias{"python", "python"},
    Alias{"py", "python"},
    Alias{"python3", "python"},
    Alias{"c", "c"},
    Alias{"h", "c"},
    Alias{"cpp", "cpp"},
    Alias{"c++", "cpp"},
    Alias{"cxx", "cpp"},
    Alias{"cc", "cpp"},
    Alias{"hpp", "cpp"},
    Alias{"hxx", "cpp"},
    Alias{"hh", "cpp"},
    Alias{"csharp", "csharp"},
    Alias{"c#", "csharp"},
    Alias{"cs", "csharp"},
    Alias{"rust", "rust"},
    Alias{"rs", "rust"},
    Alias{"go", "go"},
    Alias{"golang", "go"},
    Alias{"css", "css"},
    Alias{"scss", "css"},
    Alias{"less", "css"},
    Alias{"html", "html"},
    Alias{"htm", "html"},
    Alias{"xhtml", "html"},
    Alias{"xml", "xml"},
    Alias{"svg", "xml"},
    Alias{"xaml", "xml"},
    Alias{"plist", "xml"},
    Alias{"markdown", "markdown"},
    Alias{"md", "markdown"},
    Alias{"shell", "shell"},
    Alias{"sh", "shell"},
    Alias{"bash", "shell"},
    Alias{"zsh", "shell"},
    Alias{"console", "shell"},
    Alias{"sql", "sql"},
    Alias{"mysql", "sql"},
    Alias{"postgresql", "sql"},
    Alias{"postgres", "sql"},
    Alias{"sqlite", "sql"},
    Alias{"strata", "strata"},
};

void highlight_into(const std::string_view source, const std::string_view language,
                    Output& output) {
    const std::string_view canonical = syntax_language(language);
    if (canonical.empty())
        return;
    if (canonical == "css") {
        lex_css(source, output);
        return;
    }
    if (canonical == "html" || canonical == "xml") {
        lex_markup(source, output, canonical == "html");
        return;
    }
    if (canonical == "markdown") {
        lex_markdown(source, output);
        return;
    }
    if (canonical == "shell") {
        lex_shell(source, output);
        return;
    }
    static const Rules json = json_rules();
    static const Rules javascript = script_rules(false);
    static const Rules typescript = script_rules(true);
    static const Rules python = python_rules();
    static const Rules c = c_rules(false);
    static const Rules cpp = c_rules(true);
    static const Rules csharp = csharp_rules();
    static const Rules rust = rust_rules();
    static const Rules go = go_rules();
    static const Rules sql = sql_rules();
    static const Rules strata = strata_rules();
    const Rules& rules = canonical == "json"         ? json
                         : canonical == "javascript" ? javascript
                         : canonical == "typescript" ? typescript
                         : canonical == "python"     ? python
                         : canonical == "c"          ? c
                         : canonical == "cpp"        ? cpp
                         : canonical == "csharp"     ? csharp
                         : canonical == "rust"       ? rust
                         : canonical == "go"         ? go
                         : canonical == "sql"        ? sql
                                                     : strata;
    lex(source, rules, output);
}

} // namespace

std::string_view syntax_language(const std::string_view name) noexcept {
    std::string_view trimmed = name;
    while (!trimmed.empty() && space(trimmed.front()))
        trimmed.remove_prefix(1U);
    while (!trimmed.empty() && space(trimmed.back()))
        trimmed.remove_suffix(1U);
    if (trimmed.starts_with('.'))
        trimmed.remove_prefix(1U);
    for (const Alias& alias : aliases) {
        if (equal_folded(alias.name, trimmed))
            return alias.language;
    }
    return {};
}

std::vector<SyntaxSpan> highlight_syntax(const std::string_view source,
                                         const std::string_view language) {
    std::vector<SyntaxSpan> spans;
    spans.reserve(source.size() / 6U);
    Output output(spans);
    highlight_into(source, language, output);
    // Nested languages (template code, fenced blocks, script elements) append in place, so the
    // spans are already in order; a stable sort keeps them so if one ever is not.
    if (!std::ranges::is_sorted(spans, {}, &SyntaxSpan::start))
        std::ranges::stable_sort(spans, {}, &SyntaxSpan::start);
    return spans;
}

std::string_view syntax_kind_name(const SyntaxKind kind) noexcept {
    switch (kind) {
    case SyntaxKind::plain:
        return "plain";
    case SyntaxKind::keyword:
        return "keyword";
    case SyntaxKind::type:
        return "type";
    case SyntaxKind::builtin:
        return "builtin";
    case SyntaxKind::function:
        return "function";
    case SyntaxKind::string:
        return "string";
    case SyntaxKind::number:
        return "number";
    case SyntaxKind::comment:
        return "comment";
    case SyntaxKind::operator_symbol:
        return "operator";
    case SyntaxKind::punctuation:
        return "punctuation";
    case SyntaxKind::constant:
        return "constant";
    case SyntaxKind::property:
        return "property";
    case SyntaxKind::tag:
        return "tag";
    case SyntaxKind::attribute:
        return "attribute";
    case SyntaxKind::variable:
        return "variable";
    case SyntaxKind::heading:
        return "heading";
    case SyntaxKind::emphasis:
        return "emphasis";
    case SyntaxKind::link:
        return "link";
    case SyntaxKind::meta:
        return "meta";
    }
    return "plain";
}

std::string_view syntax_palette_token(const SyntaxKind kind) noexcept {
    switch (kind) {
    case SyntaxKind::plain:
        return {};
    case SyntaxKind::keyword:
        return "syntaxKeyword";
    case SyntaxKind::type:
        return "syntaxType";
    case SyntaxKind::builtin:
        return "syntaxBuiltin";
    case SyntaxKind::function:
        return "syntaxFunction";
    case SyntaxKind::string:
        return "syntaxString";
    case SyntaxKind::number:
        return "syntaxNumber";
    case SyntaxKind::comment:
        return "syntaxComment";
    case SyntaxKind::operator_symbol:
        return "syntaxOperator";
    case SyntaxKind::punctuation:
        return "syntaxPunctuation";
    case SyntaxKind::constant:
        return "syntaxConstant";
    case SyntaxKind::property:
        return "syntaxProperty";
    case SyntaxKind::tag:
        return "syntaxTag";
    case SyntaxKind::attribute:
        return "syntaxAttribute";
    case SyntaxKind::variable:
        return "syntaxVariable";
    case SyntaxKind::heading:
        return "syntaxHeading";
    case SyntaxKind::emphasis:
        return "syntaxEmphasis";
    case SyntaxKind::link:
        return "syntaxLink";
    case SyntaxKind::meta:
        return "syntaxMeta";
    }
    return {};
}

} // namespace strata::ui
