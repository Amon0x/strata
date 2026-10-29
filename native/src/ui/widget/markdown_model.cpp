#include "ui/widget/markdown_model.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <utility>

#include "runtime/expression.hpp"
#include "ui/theme.hpp"
#include "ui/tree.hpp"
#include "ui/widget/description.hpp"
#include "ui/widget/presentation.hpp"
#include "ui/widget/registry.hpp"
#include "ui/widget/semantics.hpp"

namespace strata::ui {
namespace {

// ---------------------------------------------------------------------------------------------
// Inline syntax

[[nodiscard]] bool space(const char character) noexcept {
    return character == ' ' || character == '\t' || character == '\n';
}

[[nodiscard]] bool word(const char character) noexcept {
    const auto byte = static_cast<unsigned char>(character);
    return std::isalnum(byte) != 0 || byte >= 0x80U;
}

[[nodiscard]] bool punctuation(const char character) noexcept {
    return std::ispunct(static_cast<unsigned char>(character)) != 0;
}

/** The length of the run of `character` starting at `at`. */
[[nodiscard]] std::size_t run_length(const std::string_view text, const std::size_t at,
                                     const char character) noexcept {
    std::size_t end = at;
    while (end < text.size() && text[end] == character)
        ++end;
    return end - at;
}

class InlineParser final {
  public:
    explicit InlineParser(std::vector<MarkdownSpan>& output) : output_(output) {}

    void parse(const std::string_view text, const MarkdownSpan& style) {
        std::size_t index = 0U;
        while (index < text.size()) {
            const char character = text[index];
            if (character == '\\' && index + 1U < text.size() && punctuation(text[index + 1U])) {
                emit(std::string(1U, text[index + 1U]), style);
                index += 2U;
                continue;
            }
            if (character == '`') {
                const std::size_t ticks = run_length(text, index, '`');
                const std::size_t close = closing_ticks(text, index + ticks, ticks);
                if (close != std::string_view::npos) {
                    std::string_view content = text.substr(index + ticks, close - index - ticks);
                    if (content.size() >= 2U && content.front() == ' ' && content.back() == ' ')
                        content = content.substr(1U, content.size() - 2U);
                    MarkdownSpan code = style;
                    code.code = true;
                    emit(std::string(content), code);
                    index = close + ticks;
                    continue;
                }
                emit(std::string(ticks, '`'), style);
                index += ticks;
                continue;
            }
            if (character == '!' && index + 1U < text.size() && text[index + 1U] == '[') {
                if (const auto link = bracketed_link(text, index + 1U); link.has_value()) {
                    // An image inline shows its alternative text.
                    MarkdownSpan alternative = style;
                    alternative.emphasis = true;
                    parse(link->label, alternative);
                    index = link->end;
                    continue;
                }
            }
            if (character == '[') {
                if (const auto link = bracketed_link(text, index); link.has_value()) {
                    MarkdownSpan linked = style;
                    linked.href = link->destination;
                    parse(link->label, linked);
                    index = link->end;
                    continue;
                }
            }
            if (character == '<') {
                const std::size_t close = text.find('>', index + 1U);
                if (close != std::string_view::npos) {
                    const std::string_view target = text.substr(index + 1U, close - index - 1U);
                    if (url_like(target) && target.find(' ') == std::string_view::npos) {
                        MarkdownSpan linked = style;
                        linked.href = std::string(target);
                        emit(std::string(target), linked);
                        index = close + 1U;
                        continue;
                    }
                }
            }
            if ((character == 'h' || character == 'w') && style.href.empty() &&
                (index == 0U || !word(text[index - 1U])) && url_like(text.substr(index))) {
                std::size_t end = index;
                while (end < text.size() && !space(text[end]) && text[end] != '<')
                    ++end;
                while (end > index && std::string_view(".,;:!?)'\"").find(text[end - 1U]) !=
                                          std::string_view::npos)
                    --end;
                const std::string target(text.substr(index, end - index));
                MarkdownSpan linked = style;
                linked.href = target.starts_with("www.") ? "https://" + target : target;
                emit(target, linked);
                index = end;
                continue;
            }
            if (character == '*' || character == '_' || character == '~') {
                if (const std::optional<std::size_t> next = emphasis(text, index, style);
                    next.has_value()) {
                    index = *next;
                    continue;
                }
                const std::size_t length = run_length(text, index, character);
                emit(std::string(length, character), style);
                index += length;
                continue;
            }
            std::size_t end = index + 1U;
            while (end < text.size() && std::string_view("\\`![<*_~hw").find(text[end]) ==
                                            std::string_view::npos)
                ++end;
            emit(std::string(text.substr(index, end - index)), style);
            index = end;
        }
    }

  private:
    struct Link final {
        std::string_view label;
        std::string destination;
        std::size_t end = 0U;
    };

    [[nodiscard]] static bool url_like(const std::string_view text) noexcept {
        return text.starts_with("https://") || text.starts_with("http://") ||
               text.starts_with("mailto:") || text.starts_with("www.");
    }

    [[nodiscard]] static std::size_t closing_ticks(const std::string_view text, std::size_t from,
                                                   const std::size_t ticks) noexcept {
        while (from < text.size()) {
            const std::size_t found = text.find('`', from);
            if (found == std::string_view::npos)
                return std::string_view::npos;
            const std::size_t length = run_length(text, found, '`');
            if (length == ticks)
                return found;
            from = found + length;
        }
        return std::string_view::npos;
    }

    /** `[label](destination)`, with `open` at the '['. */
    [[nodiscard]] static std::optional<Link> bracketed_link(const std::string_view text,
                                                            const std::size_t open) {
        int depth = 0;
        std::size_t close = std::string_view::npos;
        for (std::size_t index = open; index < text.size(); ++index) {
            if (text[index] == '\\') {
                ++index;
                continue;
            }
            if (text[index] == '[')
                ++depth;
            else if (text[index] == ']' && --depth == 0) {
                close = index;
                break;
            }
        }
        if (close == std::string_view::npos || close + 1U >= text.size() ||
            text[close + 1U] != '(')
            return std::nullopt;
        int parentheses = 0;
        for (std::size_t index = close + 1U; index < text.size(); ++index) {
            if (text[index] == '(')
                ++parentheses;
            else if (text[index] == ')' && --parentheses == 0) {
                std::string_view destination = text.substr(close + 2U, index - close - 2U);
                // A title ("...") after the destination is not part of it.
                if (const std::size_t gap = destination.find(' '); gap != std::string_view::npos)
                    destination = destination.substr(0U, gap);
                if (destination.size() >= 2U && destination.front() == '<' &&
                    destination.back() == '>')
                    destination = destination.substr(1U, destination.size() - 2U);
                return Link{text.substr(open + 1U, close - open - 1U), std::string(destination),
                            index + 1U};
            }
        }
        return std::nullopt;
    }

    /** Opens emphasis at `at` when a matching closer follows; returns where parsing resumes. */
    [[nodiscard]] std::optional<std::size_t> emphasis(const std::string_view text,
                                                      const std::size_t at,
                                                      const MarkdownSpan& style) {
        const char delimiter = text[at];
        const std::size_t length = run_length(text, at, delimiter);
        if (delimiter == '~' && length != 2U)
            return std::nullopt;
        if (delimiter != '~' && length > 3U)
            return std::nullopt;
        const std::size_t inner = at + length;
        if (inner >= text.size() || space(text[inner]))
            return std::nullopt;
        if (delimiter == '_' && at > 0U && word(text[at - 1U]))
            return std::nullopt;
        for (std::size_t index = inner + 1U; index < text.size(); ++index) {
            if (text[index] == '\\') {
                ++index;
                continue;
            }
            if (text[index] == '`') {
                const std::size_t ticks = run_length(text, index, '`');
                const std::size_t close = closing_ticks(text, index + ticks, ticks);
                if (close != std::string_view::npos)
                    index = close + ticks - 1U;
                continue;
            }
            if (text[index] != delimiter)
                continue;
            const std::size_t closing = run_length(text, index, delimiter);
            const bool after_word = !space(text[index - 1U]);
            const bool before_word =
                index + closing < text.size() && word(text[index + closing]);
            if (closing == length && after_word && !(delimiter == '_' && before_word)) {
                MarkdownSpan styled = style;
                if (delimiter == '~') {
                    styled.strike = true;
                } else {
                    styled.strong = styled.strong || length >= 2U;
                    styled.emphasis = styled.emphasis || length != 2U;
                }
                parse(text.substr(inner, index - inner), styled);
                return index + closing;
            }
            index += closing - 1U;
        }
        return std::nullopt;
    }

    void emit(std::string text, const MarkdownSpan& style) {
        if (text.empty())
            return;
        if (!output_.empty() && output_.back().same_style(style)) {
            output_.back().text += text;
            return;
        }
        MarkdownSpan span = style;
        span.text = std::move(text);
        output_.push_back(std::move(span));
    }

    std::vector<MarkdownSpan>& output_;
};

// ---------------------------------------------------------------------------------------------
// Block syntax

[[nodiscard]] std::size_t indentation(const std::string_view line) noexcept {
    std::size_t width = 0U;
    for (const char character : line) {
        if (character == ' ')
            ++width;
        else if (character == '\t')
            width += 4U - width % 4U;
        else
            break;
    }
    return width;
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1U);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1U);
    return text;
}

[[nodiscard]] bool blank(const std::string_view line) noexcept {
    return trim(line).empty();
}

/** Removes up to `width` columns of leading whitespace. */
[[nodiscard]] std::string dedent(const std::string_view line, const std::size_t width) {
    std::size_t column = 0U;
    std::size_t index = 0U;
    while (index < line.size() && column < width && (line[index] == ' ' || line[index] == '\t')) {
        column += line[index] == '\t' ? 4U - column % 4U : 1U;
        ++index;
    }
    return std::string(line.substr(index));
}

[[nodiscard]] std::optional<std::pair<char, std::size_t>> fence(const std::string_view line) {
    if (indentation(line) > 3U)
        return std::nullopt;
    const std::string_view rest = trim(line);
    if (rest.empty() || (rest.front() != '`' && rest.front() != '~'))
        return std::nullopt;
    const std::size_t length = run_length(rest, 0U, rest.front());
    return length >= 3U ? std::optional(std::pair(rest.front(), length)) : std::nullopt;
}

[[nodiscard]] bool rule(const std::string_view line) noexcept {
    if (indentation(line) > 3U)
        return false;
    const std::string_view rest = trim(line);
    if (rest.empty() || (rest.front() != '-' && rest.front() != '*' && rest.front() != '_'))
        return false;
    std::size_t marks = 0U;
    for (const char character : rest) {
        if (character == rest.front())
            ++marks;
        else if (character != ' ' && character != '\t')
            return false;
    }
    return marks >= 3U;
}

[[nodiscard]] std::optional<int> heading(const std::string_view line) noexcept {
    if (indentation(line) > 3U)
        return std::nullopt;
    const std::string_view rest = trim(line);
    const std::size_t hashes = run_length(rest, 0U, '#');
    if (hashes == 0U || hashes > 6U || (hashes < rest.size() && rest[hashes] != ' '))
        return std::nullopt;
    return static_cast<int>(hashes);
}

struct ListMarker final {
    bool ordered = false;
    char delimiter = '-';
    int number = 1;
    /** Columns from the line's start to the item's content. */
    std::size_t content = 0U;
    std::size_t indent = 0U;
};

[[nodiscard]] std::optional<ListMarker> list_marker(const std::string_view line) {
    ListMarker marker;
    marker.indent = indentation(line);
    std::size_t index = 0U;
    while (index < line.size() && (line[index] == ' ' || line[index] == '\t'))
        ++index;
    if (index >= line.size())
        return std::nullopt;
    if (line[index] == '-' || line[index] == '*' || line[index] == '+') {
        marker.delimiter = line[index];
        ++index;
    } else {
        std::size_t digits = 0U;
        int number = 0;
        while (index < line.size() && std::isdigit(static_cast<unsigned char>(line[index])) &&
               digits < 9U) {
            number = number * 10 + (line[index] - '0');
            ++index;
            ++digits;
        }
        if (digits == 0U || index >= line.size() || (line[index] != '.' && line[index] != ')'))
            return std::nullopt;
        marker.ordered = true;
        marker.number = number;
        marker.delimiter = line[index];
        ++index;
    }
    if (index < line.size() && line[index] != ' ' && line[index] != '\t')
        return std::nullopt;
    const std::size_t after = index;
    while (index < line.size() && (line[index] == ' ' || line[index] == '\t'))
        ++index;
    // A marker followed by five or more spaces, or by nothing, starts its content after one.
    if (index - after > 4U || index >= line.size())
        marker.content = after + 1U;
    else
        marker.content = index;
    return marker;
}

[[nodiscard]] std::vector<std::string_view> table_cells(std::string_view line) {
    line = trim(line);
    if (line.starts_with('|'))
        line.remove_prefix(1U);
    if (line.ends_with('|') && !line.ends_with("\\|"))
        line.remove_suffix(1U);
    std::vector<std::string_view> cells;
    std::size_t start = 0U;
    for (std::size_t index = 0U; index <= line.size(); ++index) {
        if (index < line.size() && line[index] == '\\') {
            ++index;
            continue;
        }
        if (index == line.size() || line[index] == '|') {
            cells.push_back(trim(line.substr(start, index - start)));
            start = index + 1U;
        }
    }
    return cells;
}

[[nodiscard]] bool table_separator(const std::string_view line) {
    if (line.find('-') == std::string_view::npos)
        return false;
    for (const std::string_view cell : table_cells(line)) {
        if (cell.empty())
            return false;
        for (const char character : cell) {
            if (character != '-' && character != ':' && character != ' ')
                return false;
        }
    }
    return true;
}

[[nodiscard]] bool starts_block(const std::string_view line) {
    return fence(line).has_value() || rule(line) || heading(line).has_value() ||
           trim(line).starts_with('>') || list_marker(line).has_value();
}

/** Joins a paragraph's lines: a line ending in two spaces or a backslash breaks hard. */
[[nodiscard]] std::string joined(const std::vector<std::string>& lines) {
    std::string text;
    for (std::size_t index = 0U; index < lines.size(); ++index) {
        std::string_view line = lines[index];
        const bool hard = index + 1U < lines.size() &&
                          (line.ends_with("  ") || (line.ends_with('\\') && !line.ends_with("\\\\")));
        line = trim(line);
        if (hard && line.ends_with('\\'))
            line.remove_suffix(1U);
        text += line;
        if (index + 1U < lines.size())
            text += hard ? "\n" : " ";
    }
    return text;
}

[[nodiscard]] std::vector<MarkdownBlock> parse_lines(const std::vector<std::string>& lines,
                                                     std::size_t depth);

/** A list starting at `index`; advances `index` past it. */
[[nodiscard]] MarkdownBlock parse_list(const std::vector<std::string>& lines, std::size_t& index,
                                       const ListMarker& first, const std::size_t depth) {
    MarkdownBlock list;
    list.kind = MarkdownBlockKind::list;
    list.ordered = first.ordered;
    list.start = first.number;
    while (index < lines.size()) {
        const std::optional<ListMarker> marker = list_marker(lines[index]);
        if (!marker.has_value() || marker->ordered != first.ordered ||
            marker->delimiter != first.delimiter || marker->indent >= first.content)
            break;
        // The item's first line is what follows its marker.
        std::vector<std::string> content{
            marker->content < lines[index].size() ? lines[index].substr(marker->content)
                                                  : std::string()};
        ++index;
        bool loose_gap = false;
        while (index < lines.size()) {
            const std::string& line = lines[index];
            if (blank(line)) {
                loose_gap = true;
                content.emplace_back();
                ++index;
                continue;
            }
            if (indentation(line) >= marker->content) {
                content.push_back(dedent(line, marker->content));
                loose_gap = false;
                ++index;
                continue;
            }
            // A lazy continuation line extends the item's paragraph.
            if (!loose_gap && !starts_block(line)) {
                content.push_back(std::string(trim(line)));
                ++index;
                continue;
            }
            break;
        }
        while (!content.empty() && blank(content.back()))
            content.pop_back();
        MarkdownItem item;
        if (!content.empty()) {
            std::string_view head = content.front();
            if (head.starts_with("[ ] ") || head.starts_with("[x] ") || head.starts_with("[X] ")) {
                item.checked = head[1] != ' ';
                content.front() = std::string(head.substr(4U));
            }
        }
        item.blocks = parse_lines(content, depth + 1U);
        list.items.push_back(std::move(item));
        // A blank line between items keeps the list going; anything else ends it.
        std::size_t next = index;
        while (next < lines.size() && blank(lines[next]))
            ++next;
        if (next < lines.size() && list_marker(lines[next]).has_value() &&
            list_marker(lines[next])->indent < first.content)
            index = next;
    }
    return list;
}

[[nodiscard]] std::vector<MarkdownBlock> parse_lines(const std::vector<std::string>& lines,
                                                     const std::size_t depth) {
    std::vector<MarkdownBlock> blocks;
    if (depth > 16U) {
        // Deeper nesting reads as plain text rather than recursing further.
        MarkdownBlock paragraph;
        paragraph.spans = parse_markdown_inline(joined(lines));
        blocks.push_back(std::move(paragraph));
        return blocks;
    }
    std::size_t index = 0U;
    while (index < lines.size()) {
        const std::string& line = lines[index];
        if (blank(line)) {
            ++index;
            continue;
        }
        if (const auto opened = fence(line); opened.has_value()) {
            MarkdownBlock code;
            code.kind = MarkdownBlockKind::code;
            const std::size_t indent = indentation(line);
            ++index;
            std::vector<std::string> body;
            while (index < lines.size()) {
                const std::string_view candidate = trim(lines[index]);
                if (!candidate.empty() && candidate.front() == opened->first &&
                    run_length(candidate, 0U, opened->first) >= opened->second &&
                    trim(candidate.substr(run_length(candidate, 0U, opened->first))).empty()) {
                    ++index;
                    break;
                }
                body.push_back(dedent(lines[index], indent));
                ++index;
            }
            for (std::size_t row = 0U; row < body.size(); ++row)
                code.code += (row > 0U ? "\n" : "") + body[row];
            blocks.push_back(std::move(code));
            continue;
        }
        if (indentation(line) >= 4U) {
            MarkdownBlock code;
            code.kind = MarkdownBlockKind::code;
            std::vector<std::string> body;
            while (index < lines.size() &&
                   (blank(lines[index]) || indentation(lines[index]) >= 4U)) {
                body.push_back(dedent(lines[index], 4U));
                ++index;
            }
            while (!body.empty() && blank(body.back()))
                body.pop_back();
            for (std::size_t row = 0U; row < body.size(); ++row)
                code.code += (row > 0U ? "\n" : "") + body[row];
            blocks.push_back(std::move(code));
            continue;
        }
        if (const std::optional<int> level = heading(line); level.has_value()) {
            std::string_view text = trim(trim(line).substr(static_cast<std::size_t>(*level)));
            while (text.ends_with('#'))
                text.remove_suffix(1U);
            MarkdownBlock block;
            block.kind = MarkdownBlockKind::heading;
            block.level = *level;
            block.spans = parse_markdown_inline(trim(text));
            blocks.push_back(std::move(block));
            ++index;
            continue;
        }
        if (rule(line) && !(list_marker(line).has_value() && trim(line).size() < 3U)) {
            MarkdownBlock block;
            block.kind = MarkdownBlockKind::rule;
            blocks.push_back(std::move(block));
            ++index;
            continue;
        }
        if (trim(line).starts_with('>')) {
            std::vector<std::string> quoted;
            while (index < lines.size() && !blank(lines[index])) {
                std::string_view current = trim(lines[index]);
                if (current.starts_with('>')) {
                    current.remove_prefix(1U);
                    if (current.starts_with(' '))
                        current.remove_prefix(1U);
                } else if (starts_block(lines[index])) {
                    break;
                }
                quoted.emplace_back(current);
                ++index;
            }
            MarkdownBlock block;
            block.kind = MarkdownBlockKind::quote;
            block.children = parse_lines(quoted, depth + 1U);
            blocks.push_back(std::move(block));
            continue;
        }
        if (const std::optional<ListMarker> marker = list_marker(line); marker.has_value()) {
            blocks.push_back(parse_list(lines, index, *marker, depth));
            continue;
        }
        if (line.find('|') != std::string::npos && index + 1U < lines.size() &&
            table_separator(lines[index + 1U])) {
            MarkdownBlock table;
            table.kind = MarkdownBlockKind::table;
            const std::size_t columns = table_cells(line).size();
            const auto row_of = [columns](const std::string_view source) {
                std::vector<std::vector<MarkdownSpan>> row;
                for (const std::string_view cell : table_cells(source))
                    row.push_back(parse_markdown_inline(cell));
                row.resize(columns);
                return row;
            };
            table.rows.push_back(row_of(line));
            index += 2U;
            while (index < lines.size() && !blank(lines[index]) &&
                   lines[index].find('|') != std::string::npos) {
                table.rows.push_back(row_of(lines[index]));
                ++index;
            }
            blocks.push_back(std::move(table));
            continue;
        }
        std::vector<std::string> paragraph{line};
        ++index;
        while (index < lines.size() && !blank(lines[index]) && !starts_block(lines[index]) &&
               !(lines[index].find('|') != std::string::npos && index + 1U < lines.size() &&
                 table_separator(lines[index + 1U]))) {
            paragraph.push_back(lines[index]);
            ++index;
        }
        MarkdownBlock block;
        block.spans = parse_markdown_inline(joined(paragraph));
        blocks.push_back(std::move(block));
    }
    return blocks;
}

// ---------------------------------------------------------------------------------------------
// Expansion into retained widgets

/** Translucent grey reads as a quiet tint on light and dark surfaces alike. */
[[nodiscard]] runtime::Value tint(const std::uint8_t alpha) {
    return runtime::Value(runtime::ColorValue{128U, 128U, 128U, alpha});
}

[[nodiscard]] runtime::Value object(std::vector<std::pair<std::string, runtime::Value>> fields) {
    return runtime::Value(std::move(fields));
}

[[nodiscard]] runtime::Value spacing(const double top, const double right, const double bottom,
                                     const double left) {
    return object({{"bottom", runtime::Value(bottom)},
                   {"left", runtime::Value(left)},
                   {"right", runtime::Value(right)},
                   {"top", runtime::Value(top)}});
}

class MarkdownBuilder final {
  public:
    MarkdownBuilder(WidgetDescriptionScope& scope, std::string key)
        : scope_(scope), key_(std::move(key)) {
        // Text style the author gave the Markdown carries into every block.
        for (const std::string_view name :
             {"font", "pixelSize", "color", "lineHeightMultiplier", "letterSpacing"}) {
            for (const std::string_view container : {"textStyle", "style", "$layout"}) {
                const runtime::Value* style = scope.property(container);
                const runtime::Value* value =
                    style != nullptr && style->object() != nullptr ? style->field(name) : nullptr;
                if (value != nullptr && value->kind() != runtime::ValueKind::null_value) {
                    text_style_.emplace_back(std::string(name), *value);
                    break;
                }
            }
        }
        for (const auto& [name, value] : text_style_) {
            if (name == "pixelSize" && value.number() != nullptr && *value.number() > 0.0)
                base_size_ = *value.number();
        }
        const runtime::Value* code_font = scope.property("codeFont");
        code_font_ = code_font != nullptr && code_font->string() != nullptr ? *code_font->string()
                                                                              : "strata:fonts/mono";
        const auto link = scope.description().properties.find("onLink");
        link_action_ = link != scope.description().properties.end() ? link->second
                                                                     : scope.action("Markdown");
    }

    [[nodiscard]] std::vector<std::shared_ptr<const DescriptionNode>>
    blocks(const std::vector<MarkdownBlock>& blocks, const std::string& prefix) {
        std::vector<std::shared_ptr<const DescriptionNode>> nodes;
        for (std::size_t index = 0U; index < blocks.size(); ++index) {
            if (std::shared_ptr<const DescriptionNode> node =
                    block(blocks[index], prefix + "." + std::to_string(index));
                node != nullptr)
                nodes.push_back(std::move(node));
        }
        return nodes;
    }

  private:
    [[nodiscard]] std::shared_ptr<const DescriptionNode>
    block(const MarkdownBlock& block, const std::string& key) {
        switch (block.kind) {
        case MarkdownBlockKind::paragraph:
            return text(key, block.spans, {});
        case MarkdownBlockKind::heading: {
            static constexpr double scale[] = {2.0, 1.6, 1.3, 1.15, 1.0, 0.9};
            const double size = base_size_ * scale[std::clamp(block.level, 1, 6) - 1];
            return text(key, block.spans,
                        {{"fontWeight", runtime::Value(block.level <= 2 ? "BOLD" : "SEMIBOLD")},
                         {"pixelSize", runtime::Value(size)}},
                        block.level <= 2 ? 4.0 : 2.0);
        }
        case MarkdownBlockKind::rule:
            return panel(key, {{"background", tint(90U)},
                               {"height", runtime::Value(1.0)},
                               {"margin", spacing(4.0, 0.0, 4.0, 0.0)},
                               {"width", widget_fill()}});
        case MarkdownBlockKind::code: {
            std::vector<std::pair<std::string, runtime::Value>> style = text_style_;
            std::erase_if(style, [](const auto& entry) { return entry.first == "font"; });
            style.emplace_back("font", runtime::Value(code_font_));
            DescriptionNode::Properties properties{
                {"text", runtime::ExpressionValue(runtime::Value(block.code))},
                {"textStyle", runtime::ExpressionValue(object(std::move(style)))},
                {"$layout", runtime::ExpressionValue(object({{"height", runtime::Value("content")},
                                                             {"width", widget_fill()},
                                                             {"wrapMode", runtime::Value("NONE")}}))},
            };
            scope_.synthesized();
            return panel(key,
                         {{"background", tint(36U)},
                          {"clip", runtime::Value(true)},
                          {"kind", runtime::Value("COLUMN")},
                          {"padding", spacing(10.0, 12.0, 10.0, 12.0)},
                          {"radius", runtime::Value(6.0)},
                          {"width", widget_fill()}},
                         {scope_.node("Text", key + ".code", std::move(properties))});
        }
        case MarkdownBlockKind::quote: {
            std::vector<std::shared_ptr<const DescriptionNode>> children = blocks(block.children, key);
            return panel(
                key,
                {{"alignItems", runtime::Value("STRETCH")},
                 {"kind", runtime::Value("ROW")},
                 {"gap", runtime::Value(12.0)},
                 {"width", widget_fill()}},
                {panel(key + ".bar", {{"background", tint(110U)},
                                      {"height", runtime::Value("auto")},
                                      {"radius", runtime::Value(1.5)},
                                      {"width", runtime::Value(3.0)}}),
                 panel(key + ".body", {{"gap", runtime::Value(8.0)},
                                       {"kind", runtime::Value("COLUMN")},
                                       {"opacity", runtime::Value(0.8)},
                                       {"width", widget_fill()}},
                       std::move(children))});
        }
        case MarkdownBlockKind::list:
            return list(block, key);
        case MarkdownBlockKind::table:
            return table(block, key);
        }
        return nullptr;
    }

    [[nodiscard]] std::shared_ptr<const DescriptionNode> list(const MarkdownBlock& block,
                                                              const std::string& key) {
        std::vector<std::shared_ptr<const DescriptionNode>> items;
        for (std::size_t index = 0U; index < block.items.size(); ++index) {
            const MarkdownItem& item = block.items[index];
            const std::string item_key = key + "." + std::to_string(index);
            std::shared_ptr<const DescriptionNode> marker;
            if (item.checked.has_value()) {
                marker = panel(item_key + ".box",
                               {{"background", *item.checked ? tint(150U) : runtime::Value{}},
                                {"border", object({{"color", tint(150U)},
                                                   {"width", runtime::Value(1.5)}})},
                                {"height", runtime::Value(base_size_)},
                                {"margin", spacing(base_size_ * 0.2, 0.0, 0.0, 0.0)},
                                {"radius", runtime::Value(3.0)},
                                {"width", runtime::Value(base_size_)}});
            } else if (!block.ordered) {
                marker = bullet(item_key + ".marker");
            } else {
                const std::string label = std::to_string(block.start + static_cast<int>(index)) + ".";
                DescriptionNode::Properties properties{
                    {"text", runtime::ExpressionValue(runtime::Value(label))},
                    {"textStyle", runtime::ExpressionValue(object(text_style_))},
                    {"$layout", runtime::ExpressionValue(object({
                                    {"alignment", runtime::Value("END")},
                                    {"height", runtime::Value("content")},
                                    {"minWidth", runtime::Value(base_size_ * 1.2)},
                                    {"width", runtime::Value("content")},
                                }))},
                };
                scope_.synthesized();
                marker = scope_.node("Text", item_key + ".marker", std::move(properties));
            }
            ++list_depth_;
            std::vector<std::shared_ptr<const DescriptionNode>> body = blocks(item.blocks, item_key);
            --list_depth_;
            items.push_back(panel(item_key,
                                  {{"gap", runtime::Value(8.0)},
                                   {"kind", runtime::Value("ROW")},
                                   {"width", widget_fill()}},
                                  {std::move(marker),
                                   panel(item_key + ".body", {{"gap", runtime::Value(6.0)},
                                                              {"kind", runtime::Value("COLUMN")},
                                                              {"width", widget_fill()}},
                                         std::move(body))}));
        }
        return panel(key,
                     {{"gap", runtime::Value(4.0)},
                      {"kind", runtime::Value("COLUMN")},
                      {"width", widget_fill()}},
                     std::move(items));
    }

    [[nodiscard]] std::shared_ptr<const DescriptionNode> table(const MarkdownBlock& block,
                                                               const std::string& key) {
        std::vector<std::shared_ptr<const DescriptionNode>> rows;
        for (std::size_t row = 0U; row < block.rows.size(); ++row) {
            const std::string row_key = key + "." + std::to_string(row);
            std::vector<std::shared_ptr<const DescriptionNode>> cells;
            for (std::size_t column = 0U; column < block.rows[row].size(); ++column) {
                const std::string cell_key = row_key + "." + std::to_string(column);
                std::vector<std::pair<std::string, runtime::Value>> extra;
                if (row == 0U)
                    extra.emplace_back("fontWeight", runtime::Value("SEMIBOLD"));
                cells.push_back(panel(cell_key,
                                      {{"kind", runtime::Value("COLUMN")},
                                       {"padding", spacing(6.0, 10.0, 6.0, 10.0)},
                                       {"width", object({{"weight", runtime::Value(1.0)}})}},
                                      {text(cell_key + ".text", block.rows[row][column],
                                            std::move(extra))}));
            }
            if (row > 0U) {
                rows.push_back(panel(row_key + ".divider", {{"background", tint(60U)},
                                                            {"height", runtime::Value(1.0)},
                                                            {"width", widget_fill()}}));
            }
            rows.push_back(panel(row_key,
                                 {{"background", row == 0U ? tint(28U) : runtime::Value{}},
                                  {"kind", runtime::Value("ROW")},
                                  {"width", widget_fill()}},
                                 std::move(cells)));
        }
        return panel(key,
                     {{"border", object({{"color", tint(70U)}, {"width", runtime::Value(1.0)}})},
                      {"clip", runtime::Value(true)},
                      {"kind", runtime::Value("COLUMN")},
                      {"radius", runtime::Value(6.0)},
                      {"width", widget_fill()}},
                     std::move(rows));
    }

    /**
     * A bullet drawn rather than typed, since not every font has bullet glyphs: a dot, then a ring,
     * then a square as lists nest, centred on the item's first line.
     */
    [[nodiscard]] std::shared_ptr<const DescriptionNode> bullet(const std::string& key) {
        const double size = std::max(4.0, std::round(base_size_ * 0.36));
        const double line = base_size_ * 1.2;
        const std::size_t style = list_depth_ % 3U;
        std::vector<std::pair<std::string, runtime::Value>> layout{
            {"height", runtime::Value(size)},
            {"margin", spacing(std::max(0.0, (line - size) * 0.5 + 1.0), base_size_ * 0.4, 0.0,
                               base_size_ * 0.4)},
            {"radius", runtime::Value(style == 2U ? 1.0 : size * 0.5)},
            {"width", runtime::Value(size)},
        };
        if (style == 1U)
            layout.emplace_back("border", object({{"color", tint(190U)}, {"width", runtime::Value(1.2)}}));
        else
            layout.emplace_back("background", tint(190U));
        return panel(key, std::move(layout));
    }

    /** A wrapping RichText of `spans`, with any extra text style on the whole block. */
    [[nodiscard]] std::shared_ptr<const DescriptionNode>
    text(const std::string& key, const std::vector<MarkdownSpan>& spans,
         std::vector<std::pair<std::string, runtime::Value>> extra, const double top = 0.0) {
        std::vector<runtime::ExpressionValue> values;
        values.reserve(spans.size());
        // Generated nodes skip their own describe step, so this carries what RichText's would:
        // the spans' text joined, which layout measures.
        std::string combined;
        for (const MarkdownSpan& span : spans) {
            combined += span.text;
            std::vector<std::pair<std::string, runtime::Value>> style;
            if (span.strong)
                style.emplace_back("fontWeight", runtime::Value("BOLD"));
            if (span.emphasis)
                style.emplace_back("fontStyle", runtime::Value("ITALIC"));
            if (span.code)
                style.emplace_back("font", runtime::Value(code_font_));
            if (span.strike)
                style.emplace_back("textDecoration", runtime::Value("LINE_THROUGH"));
            std::vector<std::pair<std::string, runtime::ExpressionValue>> fields{
                {"text", runtime::ExpressionValue(runtime::Value(span.text))},
            };
            if (!style.empty())
                fields.emplace_back("style", runtime::ExpressionValue(object(std::move(style))));
            if (!span.href.empty()) {
                fields.emplace_back("action", link_action_);
                fields.emplace_back("href", runtime::ExpressionValue(runtime::Value(span.href)));
            }
            values.emplace_back(
                std::make_shared<const runtime::ExpressionObjectValue>(std::move(fields)));
        }
        if (values.empty()) {
            values.emplace_back(std::make_shared<const runtime::ExpressionObjectValue>(
                std::vector<std::pair<std::string, runtime::ExpressionValue>>{
                    {"text", runtime::ExpressionValue(runtime::Value(std::string()))}}));
        }
        std::vector<std::pair<std::string, runtime::Value>> style = text_style_;
        for (auto& [name, value] : extra) {
            std::erase_if(style, [&name](const auto& entry) { return entry.first == name; });
            style.emplace_back(std::move(name), std::move(value));
        }
        std::vector<std::pair<std::string, runtime::Value>> layout{
            {"height", runtime::Value("content")},
            {"width", widget_fill()},
            {"wrapMode", runtime::Value("WORD")},
        };
        if (top > 0.0)
            layout.emplace_back("margin", spacing(top, 0.0, 0.0, 0.0));
        DescriptionNode::Properties properties{
            {"text", runtime::ExpressionValue(runtime::Value(std::move(combined)))},
            {"spans", runtime::ExpressionValue(
                          std::make_shared<const runtime::ExpressionListValue>(std::move(values)))},
            {"textStyle", runtime::ExpressionValue(object(std::move(style)))},
            {"$layout", runtime::ExpressionValue(object(std::move(layout)))},
        };
        scope_.synthesized();
        return scope_.node("RichText", key, std::move(properties));
    }

    [[nodiscard]] std::shared_ptr<const DescriptionNode>
    panel(const std::string& key, std::vector<std::pair<std::string, runtime::Value>> layout,
          std::vector<std::shared_ptr<const DescriptionNode>> children = {}) {
        // Generated chrome is exactly what it says here: no themed surface, and its own tint,
        // border and corners set directly so they outrank the theme's.
        DescriptionNode::Properties properties = widget_transparent_properties();
        std::vector<std::pair<std::string, runtime::Value>> merged{
            {"height", runtime::Value("content")}};
        for (auto& [name, value] : layout) {
            if (name == "background" || name == "border" || name == "radius" ||
                name == "opacity") {
                properties.insert_or_assign(name, runtime::ExpressionValue(std::move(value)));
                continue;
            }
            std::erase_if(merged, [&name](const auto& entry) { return entry.first == name; });
            merged.emplace_back(std::move(name), std::move(value));
        }
        properties.insert_or_assign("$layout", runtime::ExpressionValue(object(std::move(merged))));
        scope_.synthesized();
        return scope_.node("Panel", key, std::move(properties), std::move(children));
    }

    WidgetDescriptionScope& scope_;
    std::string key_;
    std::vector<std::pair<std::string, runtime::Value>> text_style_;
    double base_size_ = 12.0;
    std::string code_font_;
    runtime::ExpressionValue link_action_;
    std::size_t list_depth_ = 0U;
};

void markdown_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("gap", runtime::Value(10.0));
    scope.set("height", runtime::Value("content"));
    scope.set("kind", runtime::Value("COLUMN"));
    scope.set("width", widget_fill());
}

void markdown_expand(WidgetDescriptionScope& scope) {
    WidgetDescriptionExpansion& description = scope.description();
    if (!description.key.has_value())
        throw std::logic_error("validated Markdown must have a key");
    const std::vector<MarkdownBlock> blocks = parse_markdown(scope.string("text"));
    MarkdownBuilder builder(scope, *description.key);
    description.children = builder.blocks(blocks, *description.key);
}

void markdown_semantics(WidgetSemanticsScope& scope) {
    label_or_key(scope);
}

} // namespace

bool MarkdownSpan::same_style(const MarkdownSpan& other) const noexcept {
    return strong == other.strong && emphasis == other.emphasis && code == other.code &&
           strike == other.strike && href == other.href;
}

std::vector<MarkdownSpan> parse_markdown_inline(const std::string_view text) {
    std::vector<MarkdownSpan> spans;
    InlineParser parser(spans);
    parser.parse(text, MarkdownSpan{});
    return spans;
}

std::vector<MarkdownBlock> parse_markdown(const std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0U;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view line = text.substr(start, end - start);
        if (line.ends_with('\r'))
            line.remove_suffix(1U);
        lines.emplace_back(line);
        start = end + 1U;
    }
    return parse_lines(lines, 0U);
}

void register_markdown_widget(WidgetRegistry& registry) {
    WidgetDescribePhase describe{&markdown_defaults, &markdown_expand, {}, "Markdown", true};
    describe.implicit_key_prefix = "markdown";
    registry.register_describe_phase("Markdown", std::move(describe));
    registry.register_semantics_phase(
        "Markdown", WidgetSemanticsPhase{"document", {}, &markdown_semantics, false, false});
    registry.register_present_phase("Markdown", WidgetPresentPhase{});
    registry.register_style_schema("Markdown",
                                   ThemeStyleSchema{.chromeless = true, .body_text = true});
}

} // namespace strata::ui
