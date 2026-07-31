#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "font/opentype.hpp"
#include "ui/layout.hpp"

namespace strata::ui {

class TextLayout;

struct TextLineRange final {
    std::size_t start = 0U;
    std::size_t end = 0U;
};

[[nodiscard]] std::size_t utf16_offset_for_utf8_byte(
    std::string_view text,
    std::size_t byte_offset
) noexcept;
[[nodiscard]] std::size_t utf8_byte_for_utf16_offset(
    std::string_view text,
    std::size_t utf16_offset
) noexcept;
[[nodiscard]] std::vector<TextLineRange> text_line_ranges(std::string_view text);
[[nodiscard]] double shaped_text_line_height(const font::ShapedText& shaped) noexcept;
[[nodiscard]] std::size_t text_line_for_offset(
    const std::vector<TextLineRange>& lines,
    std::size_t utf16_offset
) noexcept;
[[nodiscard]] double shaped_caret_x(
    const font::ShapedText& shaped,
    std::size_t line,
    std::size_t utf16_offset
) noexcept;
[[nodiscard]] std::size_t shaped_text_hit_offset(
    const font::ShapedText& shaped,
    const std::vector<TextLineRange>& lines,
    Point relative_position
) noexcept;
[[nodiscard]] Point text_input_origin(
    const LayoutRecord& layout,
    const font::ShapedText& shaped,
    bool multiline
) noexcept;
[[nodiscard]] std::vector<Rect> shaped_selection_rects(
    const font::ShapedText& shaped,
    const std::vector<TextLineRange>& lines,
    Point origin,
    std::size_t first,
    std::size_t second
);
[[nodiscard]] std::vector<TextLineRange> text_layout_line_ranges(
    const TextLayout& layout
);
[[nodiscard]] std::size_t text_layout_hit_offset(
    const TextLayout& layout,
    Point relative_position
) noexcept;
/** Visual line containing a y coordinate relative to the immutable layout origin. */
[[nodiscard]] std::size_t text_layout_line_at_y(
    const TextLayout& layout,
    double y
) noexcept;
/** Resolves the visual line containing a UTF-16 caret, retaining affinity at shared wrap edges. */
[[nodiscard]] std::size_t text_layout_caret_line(
    const TextLayout& layout,
    std::size_t utf16_offset,
    std::optional<std::size_t> preferred_line = std::nullopt
) noexcept;
/** Cluster-safe UTF-16 offset at a visual line edge (soft-wrap gaps remain outside the line). */
[[nodiscard]] std::size_t text_layout_line_edge_offset(
    const TextLayout& layout,
    std::size_t line,
    bool end
) noexcept;
/** Cluster-safe UTF-16 caret nearest the requested x on one immutable visual line. */
[[nodiscard]] std::size_t text_layout_line_offset_at_x(
    const TextLayout& layout,
    std::size_t line,
    double x
) noexcept;
[[nodiscard]] Point text_input_origin(
    const LayoutRecord& layout,
    const TextLayout& text,
    bool multiline
) noexcept;
/** Editable text origin inside a presenter-owned composite viewport. */
[[nodiscard]] Point text_input_origin(
    Rect viewport,
    const TextLayout& text,
    bool multiline
) noexcept;
[[nodiscard]] std::vector<Rect> text_layout_selection_rects(
    const TextLayout& layout,
    Point origin,
    std::size_t first,
    std::size_t second
);
/** Exact caret rectangle shared by editor presentation and platform IME publication. */
[[nodiscard]] Rect text_layout_caret_rect(
    const TextLayout& layout,
    Point origin,
    std::string_view source_text,
    std::size_t caret_byte_offset
) noexcept;

} // namespace strata::ui
