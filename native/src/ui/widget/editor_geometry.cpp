#include "ui/widget/editor_geometry.hpp"

#include <algorithm>
#include <ranges>
#include <string_view>

#include "ui/text_geometry.hpp"
#include "ui/tree.hpp"
#include "ui/widget/registry.hpp"
#include "ui/widget/subtarget.hpp"

namespace strata::ui {

bool editable_text_multiline(const WidgetRegistry& widgets, const RetainedNode& node) noexcept {
    const WidgetLifecycle* lifecycle = widgets.find(node.description().type);
    return lifecycle != nullptr && lifecycle->input.text_edit_mode == WidgetTextEditMode::multi_line;
}

std::optional<Rect> editable_text_viewport(
    const RetainedNode& node,
    const LayoutRecord& layout,
    const std::span<const WidgetSubtarget> subtargets
) noexcept {
    const std::string_view type = node.description().type;
    if (type != "ChipInput" && type != "CommandPalette") {
        return layout.content_bounds;
    }
    const auto editor = std::ranges::find(subtargets, "$editor", &WidgetSubtarget::id);
    if (editor == subtargets.end()) return std::nullopt;
    const double horizontal_inset = type == "ChipInput" ? 6.0 : 10.0;
    const double vertical_inset = type == "ChipInput" ? 4.0 : 3.0;
    return Rect{
        editor->bounds.x + horizontal_inset,
        editor->bounds.y + vertical_inset,
        std::max(0.0, editor->bounds.width - horizontal_inset * 2.0),
        std::max(0.0, editor->bounds.height - vertical_inset * 2.0),
    };
}

TextLayoutOptions editable_text_layout_options(const Rect viewport, const bool multiline) {
    TextLayoutOptions options;
    if (multiline) {
        options.fallback_wrap_width = viewport.width;
    } else {
        options.wrap_mode = "NONE";
    }
    return options;
}

Point editable_text_scroll_limit(
    const Rect viewport,
    const TextLayout& layout,
    const bool multiline
) noexcept {
    const font::TextMetrics& metrics = layout.shaped.metrics;
    return Point{
        std::max(0.0, metrics.width + editable_text_caret_allowance - viewport.width),
        multiline ? std::max(0.0, metrics.height - viewport.height) : 0.0,
    };
}

Point editable_text_origin(
    const Rect viewport,
    const TextLayout& layout,
    const bool multiline,
    const Point scroll
) noexcept {
    const Point origin = text_input_origin(viewport, layout, multiline);
    return Point{origin.x - scroll.x, origin.y - scroll.y};
}

Point clamp_editable_scroll(
    const Rect viewport,
    const TextLayout& layout,
    const bool multiline,
    const Point scroll
) noexcept {
    const Point limit = editable_text_scroll_limit(viewport, layout, multiline);
    return Point{std::clamp(scroll.x, 0.0, limit.x), std::clamp(scroll.y, 0.0, limit.y)};
}

Point reveal_editable_caret(
    const Rect viewport,
    const TextLayout& layout,
    const bool multiline,
    const std::string_view text,
    const std::size_t caret,
    Point scroll
) noexcept {
    if (layout.lines.empty()) return Point{};
    // Caret geometry in the unscrolled frame; the viewport must contain it after subtracting scroll.
    const Rect rect = text_layout_caret_rect(
        layout, text_input_origin(viewport, layout, multiline), text, caret
    );
    if (rect.x - scroll.x < viewport.x) scroll.x = rect.x - viewport.x;
    if (rect.right() - scroll.x > viewport.right()) scroll.x = rect.right() - viewport.right();
    if (multiline) {
        if (rect.y - scroll.y < viewport.y) scroll.y = rect.y - viewport.y;
        if (rect.bottom() - scroll.y > viewport.bottom()) scroll.y = rect.bottom() - viewport.bottom();
    }
    Point limit = editable_text_scroll_limit(viewport, layout, multiline);
    // Trailing whitespace can put the caret past the measured text; never clamp it back out of view.
    limit.x = std::max(limit.x, rect.right() - viewport.right());
    return Point{std::clamp(scroll.x, 0.0, limit.x), std::clamp(scroll.y, 0.0, limit.y)};
}

} // namespace strata::ui
