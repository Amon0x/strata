#pragma once

#include <optional>

#include "ui/layout.hpp"

namespace strata::ui {

enum class ScrollbarAxis { horizontal, vertical };

struct ScrollbarGeometry final {
    ScrollbarAxis axis = ScrollbarAxis::vertical;
    double track_start = 0.0;
    double track_length = 0.0;
    double thumb_start = 0.0;
    double thumb_length = 0.0;
    double maximum_offset = 0.0;
    Rect hit_bounds;
    Rect thumb_bounds;
};

/** Resolves the one scrollbar geometry contract shared by input and presentation. */
[[nodiscard]] std::optional<ScrollbarGeometry> scrollbar_geometry(
    const LayoutRecord& layout,
    const LayoutStyle& style,
    ScrollbarAxis axis
) noexcept;

} // namespace strata::ui
