#include "ui/widget/code_view_model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "data/json.hpp"
#include "runtime/expression.hpp"
#include "ui/code_highlight.hpp"
#include "ui/theme.hpp"
#include "ui/tree.hpp"
#include "ui/widget/description.hpp"
#include "ui/widget/presentation.hpp"
#include "ui/widget/registry.hpp"
#include "ui/widget/semantics.hpp"

namespace strata::ui {
namespace {

/**
 * Lines per text node. A CodeView lays out and shapes its text once per change, a chunk at a
 * time, so a long file is many modest layouts rather than one enormous one, and a frame that
 * scrolls culls whole chunks it cannot see.
 */
constexpr std::size_t chunk_lines = 80U;
/** Expansions kept for reuse: a page re-described by a ticking timer keeps its code as it was. */
constexpr std::size_t cached_expansions = 32U;

using Fields = std::vector<std::pair<std::string, runtime::Value>>;

[[nodiscard]] runtime::Value object(Fields fields) {
    return runtime::Value(std::move(fields));
}

[[nodiscard]] runtime::Value token(const std::string_view name) {
    return runtime::Value(runtime::ThemeTokenValue{std::string(name)});
}

[[nodiscard]] runtime::Value edges(const double top, const double right, const double bottom,
                                   const double left) {
    return object({{"bottom", runtime::Value(bottom)},
                   {"left", runtime::Value(left)},
                   {"right", runtime::Value(right)},
                   {"top", runtime::Value(top)}});
}

using Properties = DescriptionNode::Properties;

[[nodiscard]] const runtime::Value* property(const Properties& properties,
                                             const std::string_view name) {
    const auto found = properties.find(name);
    return found != properties.end() ? found->second.value() : nullptr;
}

[[nodiscard]] double number(const Properties& properties, const std::string_view name,
                            const double fallback) {
    const runtime::Value* value = property(properties, name);
    return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
               ? *value->number()
               : fallback;
}

[[nodiscard]] bool boolean(const Properties& properties, const std::string_view name,
                           const bool fallback) {
    const runtime::Value* value = property(properties, name);
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

[[nodiscard]] std::string text(const Properties& properties, const std::string_view name) {
    const runtime::Value* value = property(properties, name);
    return value != nullptr && value->string() != nullptr ? *value->string() : std::string();
}

/** A field the author gave the node itself: its style, then its resolved layout. */
[[nodiscard]] const runtime::Value* authored(const Properties& properties,
                                             const std::string_view name) {
    for (const std::string_view container : {"style", "$layout", "visualStyle", "textStyle"}) {
        const runtime::Value* fields = property(properties, container);
        const runtime::Value* value =
            fields != nullptr && fields->object() != nullptr ? fields->field(name) : nullptr;
        if (value != nullptr && value->kind() != runtime::ValueKind::null_value)
            return value;
    }
    return nullptr;
}

/** A colour the author may give as a colour or a theme token. */
[[nodiscard]] bool colour_like(const runtime::Value* value) noexcept {
    return value != nullptr && (value->color() != nullptr || value->theme_token() != nullptr);
}

/**
 * What a CodeView paints its text and chrome with: each kind's colour (an authored `syntax` entry,
 * or the theme's palette token for it), the line numbers, a highlighted line's band and the
 * gutter behind the numbers.
 */
struct Palette final {
    std::array<runtime::Value, syntax_kind_count> kinds{};
    runtime::Value line_number;
    runtime::Value line_highlight;
    runtime::Value gutter;
};

[[nodiscard]] Palette palette(const Properties& properties) {
    const runtime::Value* syntax = authored(properties, "syntax");
    const auto entry = [syntax](const std::string_view name) -> const runtime::Value* {
        const runtime::Value* value =
            syntax != nullptr && syntax->object() != nullptr ? syntax->field(name) : nullptr;
        return colour_like(value) ? value : nullptr;
    };
    Palette result;
    for (std::size_t index = 1U; index < syntax_kind_count; ++index) {
        const auto kind = static_cast<SyntaxKind>(index);
        const runtime::Value* own = entry(syntax_kind_name(kind));
        result.kinds[index] = own != nullptr ? *own : token(syntax_palette_token(kind));
    }
    if (const runtime::Value* plain = entry("plain"); plain != nullptr)
        result.kinds[0] = *plain;
    const runtime::Value* number = entry("lineNumber");
    result.line_number = number != nullptr ? *number : token("codeLineNumber");
    const runtime::Value* highlight = entry("lineHighlight");
    result.line_highlight = highlight != nullptr ? *highlight : token("codeLineHighlight");
    // The gutter stays over the code as it scrolls sideways, so it is opaque: the code's own
    // background unless the author gives it one.
    const runtime::Value* gutter = entry("gutter");
    const runtime::Value* background = authored(properties, "background");
    result.gutter = gutter != nullptr       ? *gutter
                    : background != nullptr ? *background
                                            : token("codeBackground");
    return result;
}

struct Options final {
    std::string text;
    std::string language;
    bool line_numbers = true;
    bool wrap = false;
    bool selectable = true;
    /** Whether the author sized the height, so the view scrolls vertically within it. */
    bool bounded = false;
    std::size_t first_line = 1U;
    std::size_t tab_size = 4U;
    /** Highlighted line numbers, as displayed (first_line based), sorted. */
    std::vector<std::size_t> highlighted;
    /** Text style every chunk and the gutter share. */
    Fields text_style;
};

[[nodiscard]] Options options(const Properties& properties) {
    Options result;
    result.text = text(properties, "text");
    result.language = text(properties, "language");
    result.line_numbers = boolean(properties, "lineNumbers", true);
    result.wrap = boolean(properties, "wrap", false);
    result.selectable = boolean(properties, "selectable", true);
    result.first_line =
        static_cast<std::size_t>(std::clamp(number(properties, "firstLine", 1.0), 0.0, 1.0e9));
    result.tab_size =
        static_cast<std::size_t>(std::clamp(number(properties, "tabSize", 4.0), 1.0, 16.0));
    if (const runtime::Value* lines = property(properties, "highlightLines");
        lines != nullptr && lines->list() != nullptr) {
        for (const runtime::Value& value : lines->list()->values) {
            if (value.number() != nullptr && *value.number() >= 0.0)
                result.highlighted.push_back(static_cast<std::size_t>(*value.number()));
        }
    }
    std::ranges::sort(result.highlighted);
    const runtime::Value* height = authored(properties, "height");
    result.bounded =
        height != nullptr && !(height->string() != nullptr &&
                               (*height->string() == "content" || *height->string() == "auto"));
    for (const std::string_view name :
         {"font", "pixelSize", "color", "lineHeightMultiplier", "letterSpacing", "fontWeight"}) {
        if (const runtime::Value* value = authored(properties, name); value != nullptr)
            result.text_style.emplace_back(std::string(name), *value);
    }
    if (std::ranges::none_of(result.text_style,
                             [](const auto& field) { return field.first == "font"; }))
        result.text_style.emplace_back("font", runtime::Value("strata:fonts/mono"));
    if (std::ranges::none_of(result.text_style,
                             [](const auto& field) { return field.first == "pixelSize"; }))
        result.text_style.emplace_back("pixelSize", runtime::Value(13.0));
    if (std::ranges::none_of(result.text_style, [](const auto& field) {
            return field.first == "lineHeightMultiplier";
        }))
        result.text_style.emplace_back("lineHeightMultiplier", runtime::Value(1.4));
    return result;
}

/** The expansion of one CodeView, kept so an unchanged one is not tokenized or built again. */
struct CachedExpansion final {
    std::string identity;
    std::vector<std::shared_ptr<const DescriptionNode>> children;
    std::size_t synthesized = 0U;
    std::uint64_t used = 0U;
};

struct ExpansionCache final {
    std::mutex mutex;
    std::vector<CachedExpansion> entries;
    std::uint64_t clock = 0U;
};

[[nodiscard]] ExpansionCache& expansion_cache() {
    static ExpansionCache cache;
    return cache;
}

/** Everything an expansion depends on, spelled out: equal identities expand identically. */
[[nodiscard]] std::string identity(const WidgetDescriptionScope& scope, const std::string& key,
                                   const Options& options, const Palette& colours) {
    std::string result;
    result.reserve(options.text.size() + 512U);
    const auto field = [&result](const std::string_view part) {
        result += part;
        result.push_back('\x1F');
    };
    field(key);
    // The scope's state namespace is part of every node it makes.
    field(scope.state_scope());
    field(options.language);
    field(options.line_numbers ? "n" : "-");
    field(options.wrap ? "w" : "-");
    field(options.selectable ? "s" : "-");
    field(options.bounded ? "b" : "-");
    field(std::to_string(options.first_line));
    field(std::to_string(options.tab_size));
    for (const std::size_t line : options.highlighted)
        field(std::to_string(line));
    field(data::encode_canonical_json(runtime::value_to_json(object(options.text_style))));
    for (const runtime::Value& colour : colours.kinds)
        field(data::encode_canonical_json(runtime::value_to_json(colour)));
    field(data::encode_canonical_json(runtime::value_to_json(colours.line_number)));
    field(data::encode_canonical_json(runtime::value_to_json(colours.line_highlight)));
    field(data::encode_canonical_json(runtime::value_to_json(colours.gutter)));
    result += options.text;
    return result;
}

class CodeViewBuilder final {
  public:
    CodeViewBuilder(WidgetDescriptionScope& scope, std::string key, const Options& options,
                    const Palette& colours)
        : scope_(scope), key_(std::move(key)), options_(options), colours_(colours),
          source_(code_view_source(options.text, options.tab_size)),
          spans_(highlight_syntax(source_.text, options.language)) {}

    [[nodiscard]] std::shared_ptr<const DescriptionNode> build() {
        // Each row is one chunk of lines (or, wrapping, one line), and the scroll materializes
        // only the rows in and near its viewport: a long file lays out, shapes and draws what is
        // on screen, whatever its length.
        std::vector<std::shared_ptr<const DescriptionNode>> rows;
        const std::vector<std::pair<std::size_t, std::size_t>> parts =
            options_.wrap ? single_lines() : segments();
        rows.reserve(parts.size());
        for (std::size_t index = 0U; index < parts.size(); ++index)
            rows.push_back(row(parts[index].first, parts[index].second, index == 0U,
                               index + 1U == parts.size()));
        double pixel_size = 13.0;
        double multiplier = 1.4;
        for (const auto& [name, value] : options_.text_style) {
            if (name == "pixelSize" && value.number() != nullptr)
                pixel_size = *value.number();
            if (name == "lineHeightMultiplier" && value.number() != nullptr)
                multiplier = *value.number();
        }
        // A line's height is about 1.32 em of the font times the authored multiplier; the scroll
        // measures each row it materializes and uses this only for rows it has not.
        const double line_height = pixel_size * 1.32 * multiplier;
        const double lines_per_row = parts.empty() ? 1.0
                                                   : static_cast<double>(source_.lines.size()) /
                                                         static_cast<double>(parts.size());
        DescriptionNode::Properties scroll{
            {"horizontal", runtime::ExpressionValue(runtime::Value(!options_.wrap))},
            {"vertical", runtime::ExpressionValue(runtime::Value(options_.bounded))},
            // Its layout is authored here, so the theme's list style does not replace it.
            {"style", runtime::ExpressionValue(object({}))},
        };
        // A view with a height of its own scrolls a virtual list of its rows; one sized to its
        // content shows every row anyway, so it lays them all out.
        const std::string_view type = options_.bounded ? "VirtualList" : "Scroll";
        scope_.apply_layout_defaults(type, scroll);
        scope_.set_layout(scroll, "height",
                          options_.bounded ? widget_fill() : runtime::Value("content"));
        scope_.set_layout(scroll, "width", widget_fill());
        // The rows carry the view's padding, so a view sized to its content measures all of it.
        scope_.set_layout(scroll, "contentPadding", edges(0.0, 0.0, 0.0, 0.0));
        scope_.set_layout(scroll, "viewportInsets", edges(0.0, 0.0, 0.0, 0.0));
        scope_.set_layout(scroll, "scrollbarGutter", runtime::Value(0.0));
        if (!options_.bounded) {
            scope_.synthesized();
            return scope_.node(std::string(type), key_ + ".scroll", std::move(scroll),
                               std::move(rows));
        }
        scope_.set_layout(scroll, "virtualItemCount",
                          runtime::Value(static_cast<double>(rows.size())));
        scope_.set_layout(scroll, "virtualItemExtent",
                          runtime::Value(std::max(1.0, line_height * lines_per_row)));
        scope_.set_layout(scroll, "virtualOverscan", runtime::Value(1.0));
        // Rows are measured as they are realized, keyed as the list knows them.
        scope_.set_layout(scroll, "virtualMeasureItemExtents", runtime::Value(true));
        std::vector<runtime::Value> keys;
        keys.reserve(rows.size());
        for (const std::shared_ptr<const DescriptionNode>& row : rows)
            keys.emplace_back(runtime::KeyValue{row->key.value_or(std::string())});
        scope_.set_layout(scroll, "virtualItemKeys", runtime::Value(std::move(keys)));
        scope_.synthesized();
        // The list starts with nothing realized: layout publishes the rows in view, and only
        // those are realized, laid out and drawn.
        auto list = std::make_shared<DescriptionNode>(
            *scope_.node(std::string(type), key_ + ".scroll", std::move(scroll), std::move(rows)));
        list->materialization = MaterializationRange{};
        return list;
    }

  private:
    [[nodiscard]] bool highlighted(const std::size_t line) const {
        return std::ranges::binary_search(options_.highlighted, options_.first_line + line);
    }

    /** Consecutive lines that draw as one text: up to chunk_lines, a highlighted line alone. */
    [[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> segments() const {
        std::vector<std::pair<std::size_t, std::size_t>> result;
        std::size_t start = 0U;
        const std::size_t count = source_.lines.size();
        while (start < count) {
            std::size_t end = start + 1U;
            if (!highlighted(start)) {
                while (end < count && end - start < chunk_lines && !highlighted(end))
                    ++end;
            }
            result.emplace_back(start, end);
            start = end;
        }
        return result;
    }

    /** The text of lines [first, last), joined by newlines. */
    [[nodiscard]] std::string_view lines_text(const std::size_t first,
                                              const std::size_t last) const {
        const std::size_t from = source_.lines[first].first;
        const std::size_t to = source_.lines[last - 1U].second;
        return std::string_view(source_.text).substr(from, to - from);
    }

    /** Coloured runs covering lines [first, last): one per token, plain text between them. */
    [[nodiscard]] runtime::Value spans(const std::size_t first, const std::size_t last) {
        const std::size_t from = source_.lines[first].first;
        const std::size_t to = source_.lines[last - 1U].second;
        std::vector<runtime::Value> result;
        std::size_t at = from;
        const auto run = [&](const std::size_t start, const std::size_t end,
                             const SyntaxKind kind) {
            if (end <= start)
                return;
            Fields fields{{"text", runtime::Value(source_.text.substr(start, end - start))}};
            const runtime::Value& colour = colours_.kinds[static_cast<std::size_t>(kind)];
            if (colour.kind() != runtime::ValueKind::null_value)
                fields.emplace_back("color", colour);
            if (kind == SyntaxKind::heading) {
                fields.emplace_back("style", object({{"fontWeight", runtime::Value("BOLD")}}));
            } else if (kind == SyntaxKind::emphasis) {
                fields.emplace_back("style", object({{"fontStyle", runtime::Value("ITALIC")}}));
            }
            result.emplace_back(std::move(fields));
        };
        // Spans are ordered, and chunks are built in order, so one cursor walks them all.
        while (cursor_ < spans_.size() && spans_[cursor_].start + spans_[cursor_].length <= from)
            ++cursor_;
        for (std::size_t index = cursor_; index < spans_.size(); ++index) {
            const SyntaxSpan& span = spans_[index];
            if (span.start >= to)
                break;
            const std::size_t start = std::max<std::size_t>(span.start, from);
            const std::size_t end = std::min<std::size_t>(span.start + span.length, to);
            if (start < at)
                continue;
            run(at, start, SyntaxKind::plain);
            run(start, end, span.kind);
            at = end;
        }
        run(at, to, SyntaxKind::plain);
        if (result.empty())
            result.emplace_back(Fields{{"text", runtime::Value(std::string())}});
        return runtime::Value(std::move(result));
    }

    [[nodiscard]] std::shared_ptr<const DescriptionNode> code_text(const std::string& key,
                                                                   const std::size_t first,
                                                                   const std::size_t last,
                                                                   const bool wrap) {
        Fields layout{
            {"height", runtime::Value("content")},
            {"width", wrap ? widget_fill() : runtime::Value("content")},
            {"wrapMode", runtime::Value(wrap ? "CHARACTER" : "NONE")},
        };
        // Beside numbers, the text keeps the first and last rows' padding level with theirs.
        if (options_.line_numbers) {
            layout.emplace_back("padding", edges(first == 0U ? 10.0 : 0.0, 0.0,
                                                 last == source_.lines.size() ? 12.0 : 0.0, 4.0));
        }
        DescriptionNode::Properties properties{
            {"text",
             runtime::ExpressionValue(runtime::Value(std::string(lines_text(first, last))))},
            {"spans", runtime::ExpressionValue(spans(first, last))},
            {"textStyle", runtime::ExpressionValue(object(options_.text_style))},
            {"$layout", runtime::ExpressionValue(object(std::move(layout)))},
        };
        if (options_.selectable) {
            properties.emplace("selectable", runtime::ExpressionValue(runtime::Value(true)));
            properties.emplace("selectionContainer",
                               runtime::ExpressionValue(runtime::Value(key_)));
        }
        scope_.synthesized();
        return scope_.node("RichText", key, std::move(properties));
    }

    /** Every line on its own: wrapped lines, so each keeps its number beside its first row. */
    [[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> single_lines() const {
        std::vector<std::pair<std::size_t, std::size_t>> result;
        result.reserve(source_.lines.size());
        for (std::size_t line = 0U; line < source_.lines.size(); ++line)
            result.emplace_back(line, line + 1U);
        return result;
    }

    /**
     * Lines [first, last) as numbers, one a line, right-aligned by padding them with spaces to
     * the widest number's digits: in a monospace font every row's numbers are then equally wide
     * without measuring the font.
     */
    [[nodiscard]] std::shared_ptr<const DescriptionNode>
    number_text(const std::string& key, const std::size_t first, const std::size_t last) {
        const std::size_t digits =
            std::to_string(options_.first_line + std::max<std::size_t>(source_.lines.size(), 1U) -
                           1U)
                .size();
        std::string numbers;
        for (std::size_t line = first; line < last; ++line) {
            if (line > first)
                numbers.push_back('\n');
            const std::string number = std::to_string(options_.first_line + line);
            numbers.append(digits > number.size() ? digits - number.size() : 0U, ' ');
            numbers += number;
        }
        Fields style = options_.text_style;
        std::erase_if(style, [](const auto& field) { return field.first == "color"; });
        style.emplace_back("color", colours_.line_number);
        DescriptionNode::Properties properties{
            {"text", runtime::ExpressionValue(runtime::Value(std::move(numbers)))},
            {"textStyle", runtime::ExpressionValue(object(std::move(style)))},
            {"$layout", runtime::ExpressionValue(object({{"height", runtime::Value("content")},
                                                         {"width", runtime::Value("content")},
                                                         {"wrapMode", runtime::Value("NONE")}}))},
            {"selectable", runtime::ExpressionValue(runtime::Value(false))},
        };
        scope_.synthesized();
        return scope_.node("Text", key, std::move(properties));
    }

    /**
     * One row of the view: lines [first, last) beside their numbers. The numbers' cell has the
     * gutter's opaque background and holds at the view's left edge while the code scrolls
     * sideways under it. A highlighted line's band spans the row, and the gutter shows it too.
     */
    [[nodiscard]] std::shared_ptr<const DescriptionNode>
    row(const std::size_t first, const std::size_t last, const bool top, const bool bottom) {
        const std::string key = key_ + ".r" + std::to_string(first);
        const bool marked = highlighted(first);
        const double above = top ? 10.0 : 0.0;
        const double below = bottom ? 12.0 : 0.0;
        std::vector<std::shared_ptr<const DescriptionNode>> cells;
        if (options_.line_numbers) {
            Fields band;
            if (marked)
                band.emplace_back("background", colours_.line_highlight);
            std::shared_ptr<const DescriptionNode> numbers =
                panel(key + ".b",
                      {{"height", widget_fill()},
                       {"kind", runtime::Value("COLUMN")},
                       {"padding", edges(above, 12.0, below, 14.0)},
                       {"width", runtime::Value("content")}},
                      std::move(band), {number_text(key + ".n", first, last)});
            cells.push_back(panel(key + ".g",
                                  {{"alignItems", runtime::Value("STRETCH")},
                                   {"height", widget_fill()},
                                   {"inset", object({{"left", runtime::Value(0.0)}})},
                                   {"kind", runtime::Value("COLUMN")},
                                   {"position", runtime::Value("STICKY")},
                                   {"width", runtime::Value("content")},
                                   {"zIndex", runtime::Value(1.0)}},
                                  {{"background", colours_.gutter}}, {std::move(numbers)}));
        }
        cells.push_back(code_text(key + ".t", first, last, options_.wrap));
        Fields visual;
        if (marked)
            visual.emplace_back("background", colours_.line_highlight);
        Fields layout{
            {"alignItems", runtime::Value("STRETCH")},
            {"height", runtime::Value("content")},
            {"kind", runtime::Value("ROW")},
            {"padding",
             edges(options_.line_numbers ? 0.0 : above, 14.0, options_.line_numbers ? 0.0 : below,
                   options_.line_numbers ? 0.0 : 14.0)},
        };
        if (options_.wrap) {
            layout.emplace_back("width", widget_fill());
        } else {
            // As wide as its lines, and at least as wide as the view, so a band reaches across.
            layout.emplace_back("width", runtime::Value("content"));
            layout.emplace_back("minWidth", object({{"fraction", runtime::Value(1.0)}}));
        }
        return panel(key, std::move(layout), std::move(visual), std::move(cells));
    }

    [[nodiscard]] std::shared_ptr<const DescriptionNode>
    panel(const std::string& key, Fields layout, Fields visual,
          std::vector<std::shared_ptr<const DescriptionNode>> children) {
        // Generated chrome is exactly what it says: no themed surface, its own colours set
        // directly so they outrank the theme's.
        DescriptionNode::Properties properties = widget_transparent_properties();
        for (auto& [name, value] : visual)
            properties.insert_or_assign(name, runtime::ExpressionValue(std::move(value)));
        properties.insert_or_assign("$layout", runtime::ExpressionValue(object(std::move(layout))));
        scope_.synthesized();
        return scope_.node("Panel", key, std::move(properties), std::move(children));
    }

    WidgetDescriptionScope& scope_;
    std::string key_;
    const Options& options_;
    const Palette& colours_;
    CodeViewSource source_;
    std::vector<SyntaxSpan> spans_;
    std::size_t cursor_ = 0U;
};

/**
 * The view's own chrome, unless the author styles it otherwise: the theme's code background,
 * rounded, clipping what scrolls inside it to the rounding.
 */
void code_view_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("background", token("codeBackground"));
    scope.set("clip", runtime::Value(true));
    scope.set("radius", runtime::Value(8.0));
    scope.set("height", runtime::Value("content"));
    scope.set("kind", runtime::Value("COLUMN"));
    scope.set("width", widget_fill());
}

void code_view_expand(WidgetDescriptionScope& scope) {
    WidgetDescriptionExpansion& description = scope.description();
    if (!description.key.has_value())
        throw std::logic_error("validated CodeView must have a key");
    description.children = expand_code_view(scope, *description.key, description.properties);
}

void code_view_content(WidgetRenderScope& scope) {
    if (scope.visual().background.has_value())
        scope.rounded_rect(scope.layout().bounds, *scope.visual().background);
}

void code_view_foreground(WidgetRenderScope& scope) {
    if (scope.visual().border.has_value())
        scope.border(scope.layout().bounds, *scope.visual().border);
}

void code_view_semantics(WidgetSemanticsScope& scope) {
    label_or_key(scope);
}

} // namespace

CodeViewSource code_view_source(const std::string_view text, const std::size_t tab_size) {
    CodeViewSource result;
    result.text.reserve(text.size() + text.size() / 16U);
    std::size_t column = 0U;
    std::size_t line_start = 0U;
    const std::size_t tab = std::max<std::size_t>(tab_size, 1U);
    for (std::size_t index = 0U; index < text.size(); ++index) {
        const char c = text[index];
        if (c == '\r' || c == '\n') {
            if (c == '\r' && index + 1U < text.size() && text[index + 1U] == '\n')
                ++index;
            result.lines.emplace_back(line_start, result.text.size());
            result.text.push_back('\n');
            line_start = result.text.size();
            column = 0U;
            continue;
        }
        if (c == '\t') {
            const std::size_t width = tab - column % tab;
            result.text.append(width, ' ');
            column += width;
            continue;
        }
        result.text.push_back(c);
        // A column per character: continuation bytes do not start one.
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U)
            ++column;
    }
    if (line_start < result.text.size() || result.lines.empty()) {
        result.lines.emplace_back(line_start, result.text.size());
    } else if (!result.text.empty()) {
        // A final newline ends the last line rather than starting another.
        result.text.pop_back();
    }
    return result;
}

std::vector<std::shared_ptr<const DescriptionNode>>
expand_code_view(WidgetDescriptionScope& scope, const std::string& key,
                 const DescriptionNode::Properties& properties) {
    const Options settings = options(properties);
    const Palette colours = palette(properties);
    std::string expansion_identity = identity(scope, key, settings, colours);
    ExpansionCache& cache = expansion_cache();
    {
        const std::scoped_lock lock(cache.mutex);
        const auto found =
            std::ranges::find(cache.entries, expansion_identity, &CachedExpansion::identity);
        if (found != cache.entries.end()) {
            found->used = ++cache.clock;
            scope.synthesized(found->synthesized);
            return found->children;
        }
    }
    const std::size_t before = scope.description().synthesized_nodes;
    CodeViewBuilder builder(scope, key, settings, colours);
    std::vector<std::shared_ptr<const DescriptionNode>> children{builder.build()};
    const std::size_t synthesized = scope.description().synthesized_nodes - before;
    const std::scoped_lock lock(cache.mutex);
    if (cache.entries.size() >= cached_expansions)
        cache.entries.erase(std::ranges::min_element(cache.entries, {}, &CachedExpansion::used));
    cache.entries.push_back(
        CachedExpansion{std::move(expansion_identity), children, synthesized, ++cache.clock});
    return children;
}

void register_code_view_widget(WidgetRegistry& registry) {
    WidgetDescribePhase describe{&code_view_defaults, &code_view_expand, {}, "CodeView", true};
    describe.implicit_key_prefix = "code";
    registry.register_describe_phase("CodeView", std::move(describe));
    registry.register_semantics_phase(
        "CodeView", WidgetSemanticsPhase{"document", {}, &code_view_semantics, false, false});
    WidgetPresentPhase present;
    present.content = &code_view_content;
    present.foreground = &code_view_foreground;
    registry.register_present_phase("CodeView", std::move(present));
    registry.register_style_schema("CodeView", ThemeStyleSchema{.chromeless = true});
}

} // namespace strata::ui
