#pragma once

#include <utility>

#include "ui/path.hpp"
#include "ui/render.hpp"

namespace strata::ui {

enum class WidgetChevronDirection { right, down };

/** Framework-owned normalized chevron geometry; built-ins never depend on a font or host texture. */
[[nodiscard]] inline PathShape widget_chevron(
    const WidgetChevronDirection direction,
    const RenderColor color,
    const double stroke_width = 1.5
) {
    Path path;
    if (direction == WidgetChevronDirection::down) {
        path.move_to(Point{0.12, 0.32});
        path.line_to(Point{0.5, 0.68});
        path.line_to(Point{0.88, 0.32});
    } else {
        path.move_to(Point{0.32, 0.12});
        path.line_to(Point{0.68, 0.5});
        path.line_to(Point{0.32, 0.88});
    }
    return PathShape{
        std::move(path),
        std::nullopt,
        Paint(color),
        StrokeStyle{stroke_width, PathCap::round, PathJoin::round},
    };
}

} // namespace strata::ui
