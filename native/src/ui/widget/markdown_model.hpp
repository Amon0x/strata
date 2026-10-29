#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace strata::ui {

class WidgetRegistry;

/** One run of inline text with the emphasis Markdown gave it. */
struct MarkdownSpan final {
    std::string text;
    bool strong = false;
    bool emphasis = false;
    bool code = false;
    bool strike = false;
    /** A link's destination; empty when the run is not a link. */
    std::string href;

    [[nodiscard]] bool same_style(const MarkdownSpan& other) const noexcept;
};

enum class MarkdownBlockKind {
    paragraph,
    heading,
    list,
    quote,
    code,
    rule,
    table,
};

struct MarkdownBlock;

/** One list item: its own blocks (usually a paragraph, perhaps a nested list). */
struct MarkdownItem final {
    std::vector<MarkdownBlock> blocks;
    /** A task list item's box: absent for an ordinary item. */
    std::optional<bool> checked;
};

struct MarkdownBlock final {
    MarkdownBlockKind kind = MarkdownBlockKind::paragraph;
    /** Paragraph and heading text, and table cells' text (row-major in `rows`). */
    std::vector<MarkdownSpan> spans;
    /** Heading level, 1 to 6. */
    int level = 0;
    /** A code block's text, verbatim. */
    std::string code;
    /** A fenced code block's language: the first word of its info string ("```rust"). */
    std::string language;
    bool ordered = false;
    /** An ordered list's first number. */
    int start = 1;
    std::vector<MarkdownItem> items;
    /** A quote's blocks. */
    std::vector<MarkdownBlock> children;
    /** A table's rows of cells, the header first. */
    std::vector<std::vector<std::vector<MarkdownSpan>>> rows;
};

/** Parses the inline syntax of one block's text: emphasis, code, strikethrough and links. */
[[nodiscard]] std::vector<MarkdownSpan> parse_markdown_inline(std::string_view text);

/** Parses Markdown into blocks: headings, paragraphs, lists, quotes, code, rules and tables. */
[[nodiscard]] std::vector<MarkdownBlock> parse_markdown(std::string_view text);

/** Registers Markdown's description, presentation and semantics phases. */
void register_markdown_widget(WidgetRegistry& registry);

} // namespace strata::ui
