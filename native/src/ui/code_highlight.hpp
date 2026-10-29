#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace strata::ui {

/** What a run of source code is, for colouring it. */
enum class SyntaxKind : std::uint8_t {
    plain,
    keyword,
    type,
    builtin,
    function,
    string,
    number,
    comment,
    operator_symbol,
    punctuation,
    constant,
    property,
    tag,
    attribute,
    variable,
    heading,
    emphasis,
    link,
    meta,
};

inline constexpr std::size_t syntax_kind_count = 19U;

/** One coloured run: `length` bytes of the source from byte `start`. */
struct SyntaxSpan final {
    std::uint32_t start = 0U;
    std::uint32_t length = 0U;
    SyntaxKind kind = SyntaxKind::plain;
    [[nodiscard]] friend bool operator==(const SyntaxSpan&, const SyntaxSpan&) = default;
};

/**
 * The canonical name of a language a CodeView or a fenced Markdown block names, by any of its
 * usual spellings and file extensions ("js", "mjs" and "javascript" are "javascript"; "c++",
 * "hpp" and "cc" are "cpp"), or empty when it is none Strata highlights: json, javascript,
 * typescript, python, c, cpp, csharp, rust, go, css, html, xml, markdown, shell, sql and strata.
 */
[[nodiscard]] std::string_view syntax_language(std::string_view name) noexcept;

/**
 * Tokenizes `source` (UTF-8) as `language` (any spelling syntax_language accepts) in one pass
 * over the whole text, so block comments, multi-line strings and fenced code carry across lines.
 * Spans are in order and do not overlap; what they leave out is plain. An unknown language
 * yields no spans. Linear in the length of the source.
 */
[[nodiscard]] std::vector<SyntaxSpan> highlight_syntax(std::string_view source,
                                                       std::string_view language);

/** The name a kind's colour goes by in a CodeView's `syntax` style ("keyword"); "plain" too. */
[[nodiscard]] std::string_view syntax_kind_name(SyntaxKind kind) noexcept;

/** The theme palette token a kind is coloured by by default ("syntaxKeyword"); empty for plain. */
[[nodiscard]] std::string_view syntax_palette_token(SyntaxKind kind) noexcept;

} // namespace strata::ui
