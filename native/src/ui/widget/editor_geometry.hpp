#pragma once

#include <optional>
#include <span>

#include "ui/layout.hpp"

namespace strata::ui {

class RetainedNode;
struct WidgetSubtarget;

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

} // namespace strata::ui
