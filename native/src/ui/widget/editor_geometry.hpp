#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "ui/layout.hpp"
#include "ui/text.hpp"

namespace strata::ui {

class RetainedNode;
class WidgetRegistry;
struct WidgetSubtarget;

/** Whether the widget edits multi-line text (wrapping, vertical scrolling) rather than a single line. */
[[nodiscard]] bool editable_text_multiline(
    const WidgetRegistry& widgets,
    const RetainedNode& node
) noexcept;

/**
 * Resolves the actual text viewport of an editable widget.
 *
 * Composite editors publish a `$editor` subtarget; presentation, pointer hit testing, and platform
 * IME placement all consume this function so their insets cannot drift independently.
 */
[[nodiscard]] std::optional<Rect> editable_text_viewport(
    const RetainedNode& node,
    const LayoutRecord& layout,
    std::span<const WidgetSubtarget> subtargets
) noexcept;

/**
 * Text layout contract of an editable widget. Single-line text never wraps; it scrolls horizontally
 * instead. Multi-line text wraps at the viewport width unless the author set `wrapWidth`, and
 * scrolls vertically. Presentation, hit testing, caret navigation and IME placement all lay the
 * text out with these options, so the scrolled geometry they share is identical.
 */
[[nodiscard]] TextLayoutOptions editable_text_layout_options(Rect viewport, bool multiline);

/** Width reserved past the last glyph so a caret at the end of overflowing text stays visible. */
inline constexpr double editable_text_caret_allowance = 2.0;

/** Largest scroll offset per axis that still shows text; zero when the text fits. */
[[nodiscard]] Point editable_text_scroll_limit(
    Rect viewport,
    const TextLayout& layout,
    bool multiline
) noexcept;

/** Where the laid-out text's origin lands for a scroll offset (see text_input_origin). */
[[nodiscard]] Point editable_text_origin(
    Rect viewport,
    const TextLayout& layout,
    bool multiline,
    Point scroll
) noexcept;

/**
 * The smallest change to `scroll` that shows the caret at `caret` (a UTF-8 byte offset into `text`)
 * entirely inside the viewport, clamped to what the text can scroll. A caret at the end of
 * overflowing text may scroll past the last glyph by its own width.
 */
[[nodiscard]] Point reveal_editable_caret(
    Rect viewport,
    const TextLayout& layout,
    bool multiline,
    std::string_view text,
    std::size_t caret,
    Point scroll
) noexcept;

/** Clamps a retained scroll offset after the text or viewport changed. */
[[nodiscard]] Point clamp_editable_scroll(
    Rect viewport,
    const TextLayout& layout,
    bool multiline,
    Point scroll
) noexcept;

} // namespace strata::ui
