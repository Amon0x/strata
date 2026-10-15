#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "font/opentype.hpp"
#include "ui/layout.hpp"

namespace strata::ui {

class TextLayout;

[[nodiscard]] std::size_t utf16_offset_for_utf8_byte(
    std::string_view text,
    std::size_t byte_offset
) noexcept;
[[nodiscard]] std::size_t utf8_byte_for_utf16_offset(
    std::string_view text,
    std::size_t utf16_offset
) noexcept;
[[nodiscard]] double shaped_caret_x(
    const font::ShapedText& shaped,
    std::size_t line,
    std::size_t utf16_offset
) noexcept;
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
