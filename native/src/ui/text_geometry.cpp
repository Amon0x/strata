#include "ui/text_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "ui/text.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] std::size_t utf8_width(const unsigned char lead) noexcept {
    if ((lead & 0x80U) == 0U) return 1U;
    if ((lead & 0xE0U) == 0xC0U) return 2U;
    if ((lead & 0xF0U) == 0xE0U) return 3U;
    return 4U;
}

[[nodiscard]] std::size_t utf16_width(const unsigned char lead) noexcept {
    return (lead & 0xF8U) == 0xF0U ? 2U : 1U;
}

} // namespace

std::size_t utf16_offset_for_utf8_byte(
    const std::string_view text,
    const std::size_t byte_offset
) noexcept {
    const std::size_t target = std::min(byte_offset, text.size());
    std::size_t bytes = 0U;
    std::size_t utf16 = 0U;
    while (bytes < target) {
        const auto lead = static_cast<unsigned char>(text[bytes]);
        const std::size_t width = utf8_width(lead);
        if (bytes + width > target) break;
        bytes += width;
        utf16 += utf16_width(lead);
    }
    return utf16;
}

std::size_t utf8_byte_for_utf16_offset(
    const std::string_view text,
    const std::size_t utf16_offset
) noexcept {
    std::size_t bytes = 0U;
    std::size_t utf16 = 0U;
    while (bytes < text.size()) {
        const auto lead = static_cast<unsigned char>(text[bytes]);
        const std::size_t next_utf16 = utf16 + utf16_width(lead);
        if (next_utf16 > utf16_offset) break;
        bytes += utf8_width(lead);
        utf16 = next_utf16;
    }
    return std::min(bytes, text.size());
}

std::vector<TextLineRange> text_line_ranges(const std::string_view text) {
    std::vector<TextLineRange> result;
    std::size_t line_start = 0U;
    std::size_t utf16 = 0U;
    std::size_t bytes = 0U;
    while (bytes < text.size()) {
        const auto lead = static_cast<unsigned char>(text[bytes]);
        const std::size_t width = utf8_width(lead);
        if (lead == '\n') {
            result.push_back(TextLineRange{line_start, utf16});
            ++utf16;
            line_start = utf16;
        } else {
            utf16 += utf16_width(lead);
        }
        bytes += width;
    }
    result.push_back(TextLineRange{line_start, utf16});
    return result;
}

double shaped_text_line_height(const font::ShapedText& shaped) noexcept {
    return shaped.metrics.line_count == 0U
               ? shaped.metrics.natural_line_height
               : shaped.metrics.height / static_cast<double>(shaped.metrics.line_count);
}

std::size_t text_line_for_offset(
    const std::vector<TextLineRange>& lines,
    const std::size_t utf16_offset
) noexcept {
    for (std::size_t line = 0U; line < lines.size(); ++line) {
        if (utf16_offset <= lines[line].end) return line;
    }
    return lines.empty() ? 0U : lines.size() - 1U;
}

double shaped_caret_x(
    const font::ShapedText& shaped,
    const std::size_t line,
    const std::size_t utf16_offset
) noexcept {
    double result = 0.0;
    for (const font::ShapedCluster& cluster : shaped.clusters) {
        if (cluster.line_index != line) continue;
        if (utf16_offset <= cluster.text_start_offset) return cluster.x;
        result = cluster.x + cluster.advance;
        if (utf16_offset < cluster.text_end_offset) return result;
    }
    return result;
}

std::size_t shaped_text_hit_offset(
    const font::ShapedText& shaped,
    const std::vector<TextLineRange>& lines,
    const Point relative_position
) noexcept {
    if (lines.empty()) return 0U;
    const double height = std::max(shaped_text_line_height(shaped), 1.0);
    const auto raw_line = static_cast<std::int64_t>(std::floor(relative_position.y / height));
    const std::size_t line = static_cast<std::size_t>(std::clamp<std::int64_t>(
        raw_line, 0, static_cast<std::int64_t>(lines.size() - 1U)
    ));
    std::size_t result = lines[line].start;
    for (const font::ShapedCluster& cluster : shaped.clusters) {
        if (cluster.line_index != line) continue;
        const double midpoint = cluster.x + cluster.advance * 0.5;
        if (relative_position.x < midpoint) return cluster.text_start_offset;
        result = cluster.text_end_offset;
    }
    return std::min(result, lines[line].end);
}

Point text_input_origin(
    const LayoutRecord& layout,
    const font::ShapedText& shaped,
    const bool multiline
) noexcept {
    return Point{
        layout.content_bounds.x,
        multiline
            ? layout.content_bounds.y
            : layout.content_bounds.y +
                  std::max(0.0, layout.content_bounds.height - shaped.metrics.height) * 0.5,
    };
}

std::vector<Rect> shaped_selection_rects(
    const font::ShapedText& shaped,
    const std::vector<TextLineRange>& lines,
    const Point origin,
    const std::size_t first,
    const std::size_t second
) {
    const std::size_t start = std::min(first, second);
    const std::size_t end = std::max(first, second);
    std::vector<Rect> result;
    if (start == end) return result;
    const double line_height = shaped_text_line_height(shaped);
    for (std::size_t line = 0U; line < lines.size(); ++line) {
        const TextLineRange range = lines[line];
        const std::size_t from = std::max(start, range.start);
        const std::size_t to = std::min(end, range.end);
        if (from >= to) continue;
        const double left = shaped_caret_x(shaped, line, from);
        const double right = shaped_caret_x(shaped, line, to);
        result.push_back(Rect{
            origin.x + left,
            origin.y + static_cast<double>(line) * line_height,
            std::max(0.0, right - left),
            line_height,
        });
    }
    return result;
}

std::vector<TextLineRange> text_layout_line_ranges(const TextLayout& layout) {
    std::vector<TextLineRange> result;
    result.reserve(layout.lines.size());
    for (const TextLayoutLine& line : layout.lines) {
        result.push_back(TextLineRange{line.text_start_offset, line.text_end_offset});
    }
    return result;
}

std::size_t text_layout_hit_offset(
    const TextLayout& layout,
    const Point relative_position
) noexcept {
    if (layout.lines.empty()) return 0U;
    const std::size_t line = text_layout_line_at_y(layout, relative_position.y);
    std::size_t result = layout.lines[line].text_start_offset;
    for (const font::ShapedCluster& cluster : layout.shaped.clusters) {
        if (cluster.line_index != line) continue;
        if (cluster.soft_wrap_gap) continue;
        if (relative_position.x < cluster.x + cluster.advance * 0.5) {
            return cluster.text_start_offset;
        }
        result = cluster.text_end_offset;
    }
    return std::min(result, layout.lines[line].text_end_offset);
}

std::size_t text_layout_line_at_y(
    const TextLayout& layout,
    const double y
) noexcept {
    if (layout.lines.empty()) return 0U;
    for (std::size_t index = 0U; index < layout.lines.size(); ++index) {
        const TextLayoutLine& candidate = layout.lines[index];
        if (y < candidate.y + candidate.height) return index;
    }
    return layout.lines.size() - 1U;
}

std::size_t text_layout_caret_line(
    const TextLayout& layout,
    const std::size_t utf16_offset,
    const std::optional<std::size_t> preferred_line
) noexcept {
    if (layout.lines.empty()) return 0U;
    const auto contains = [&layout, utf16_offset](const std::size_t index) {
        const TextLayoutLine& line = layout.lines[index];
        const bool in_soft_gap = line.soft_wrap_gap_start_offset.has_value() &&
            line.soft_wrap_gap_end_offset.has_value() &&
            utf16_offset >= *line.soft_wrap_gap_start_offset &&
            utf16_offset < *line.soft_wrap_gap_end_offset;
        return (utf16_offset >= line.text_start_offset &&
                utf16_offset <= line.text_end_offset) || in_soft_gap;
    };
    if (preferred_line.has_value()) {
        const std::size_t preferred = std::min(*preferred_line, layout.lines.size() - 1U);
        if (contains(preferred)) return preferred;
    }
    for (std::size_t line = 0U; line < layout.lines.size(); ++line) {
        if (contains(line)) return line;
    }
    return utf16_offset < layout.lines.front().text_start_offset
        ? 0U
        : layout.lines.size() - 1U;
}

std::size_t text_layout_line_edge_offset(
    const TextLayout& layout,
    const std::size_t line,
    const bool end
) noexcept {
    if (layout.lines.empty()) return 0U;
    const TextLayoutLine& selected = layout.lines[
        std::min(line, layout.lines.size() - 1U)
    ];
    return end ? selected.text_end_offset : selected.text_start_offset;
}

std::size_t text_layout_line_offset_at_x(
    const TextLayout& layout,
    const std::size_t line,
    const double x
) noexcept {
    if (layout.lines.empty()) return 0U;
    const std::size_t selected = std::min(line, layout.lines.size() - 1U);
    std::size_t result = layout.lines[selected].text_start_offset;
    for (const font::ShapedCluster& cluster : layout.shaped.clusters) {
        if (cluster.line_index != selected || cluster.soft_wrap_gap) continue;
        if (x < cluster.x + cluster.advance * 0.5) return cluster.text_start_offset;
        result = cluster.text_end_offset;
    }
    return std::min(result, layout.lines[selected].text_end_offset);
}

Point text_input_origin(
    const LayoutRecord& layout,
    const TextLayout& text,
    const bool multiline
) noexcept {
    return text_input_origin(layout.content_bounds, text, multiline);
}

Point text_input_origin(
    const Rect viewport,
    const TextLayout& text,
    const bool multiline
) noexcept {
    return Point{
        viewport.x,
        multiline
            ? viewport.y
            : viewport.y +
                  std::max(0.0, viewport.height - text.shaped.metrics.height) * 0.5,
    };
}

std::vector<Rect> text_layout_selection_rects(
    const TextLayout& layout,
    const Point origin,
    const std::size_t first,
    const std::size_t second
) {
    const std::size_t start = std::min(first, second);
    const std::size_t end = std::max(first, second);
    std::vector<Rect> result;
    if (start == end) return result;
    for (std::size_t index = 0U; index < layout.lines.size(); ++index) {
        const TextLayoutLine& line = layout.lines[index];
        const std::size_t source_end = line.soft_wrap_gap_end_offset.value_or(
            line.text_end_offset
        );
        const std::size_t from = std::max(start, line.text_start_offset);
        const std::size_t to = std::min(end, source_end);
        if (from >= to) continue;
        const double left = shaped_caret_x(layout.shaped, index, from);
        const double right = shaped_caret_x(layout.shaped, index, to);
        result.push_back(Rect{
            origin.x + left,
            origin.y + line.y,
            std::max(0.0, right - left),
            line.height,
        });
    }
    return result;
}

Rect text_layout_caret_rect(
    const TextLayout& layout,
    const Point origin,
    const std::string_view source_text,
    const std::size_t caret_byte_offset
) noexcept {
    if (layout.lines.empty()) return Rect{origin.x, origin.y, 1.0, 1.0};
    const std::size_t caret = utf16_offset_for_utf8_byte(source_text, caret_byte_offset);
    std::size_t line = layout.lines.size() - 1U;
    for (std::size_t index = 0U; index < layout.lines.size(); ++index) {
        const TextLayoutLine& candidate = layout.lines[index];
        const bool in_soft_gap = candidate.soft_wrap_gap_start_offset.has_value() &&
            candidate.soft_wrap_gap_end_offset.has_value() &&
            caret >= *candidate.soft_wrap_gap_start_offset &&
            caret < *candidate.soft_wrap_gap_end_offset;
        if (caret <= candidate.text_end_offset || in_soft_gap) {
            line = index;
            break;
        }
    }
    const TextLayoutLine& layout_line = layout.lines[line];
    return Rect{
        origin.x + shaped_caret_x(layout.shaped, line, caret),
        origin.y + layout_line.y,
        1.0,
        std::max(1.0, layout_line.height),
    };
}

} // namespace strata::ui
