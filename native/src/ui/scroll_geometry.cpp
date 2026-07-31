#include "ui/scroll_geometry.hpp"

#include <algorithm>

namespace strata::ui {
namespace {

constexpr double fallback_gutter = 8.0;
constexpr double thumb_thickness = 3.0;
constexpr double thumb_inset = 1.0;

} // namespace

std::optional<ScrollbarGeometry> scrollbar_geometry(
    const LayoutRecord& layout,
    const LayoutStyle& style,
    const ScrollbarAxis axis
) noexcept {
    if (!layout.viewport.has_value()) return std::nullopt;
    const Rect viewport = *layout.viewport;
    const Rect frame = layout.scroll_frame.value_or(viewport);
    const Size content{
        layout.content_size.width + style.scroll_content_padding.horizontal(),
        layout.content_size.height + style.scroll_content_padding.vertical(),
    };
    if (axis == ScrollbarAxis::vertical) {
        if (!style.scroll_vertical || content.height <= viewport.height || viewport.height <= 0.0) {
            return std::nullopt;
        }
        const double ratio = std::clamp(viewport.height / content.height, 0.05, 1.0);
        const double thumb_length = viewport.height * ratio;
        const double maximum = content.height - viewport.height;
        const double thumb_start = viewport.y +
            (viewport.height - thumb_length) * (layout.scroll_offset.y / maximum);
        const double reserved = std::max(0.0, frame.right() - viewport.right());
        const double hit_width = reserved > 0.0 ? reserved : fallback_gutter;
        const double hit_x = reserved > 0.0 ? viewport.right() : viewport.right() - hit_width;
        const double thumb_right = reserved > 0.0 ? frame.right() : viewport.right();
        return ScrollbarGeometry{
            axis,
            viewport.y,
            viewport.height,
            thumb_start,
            thumb_length,
            maximum,
            Rect{hit_x, viewport.y, hit_width, viewport.height},
            Rect{
                thumb_right - thumb_inset - thumb_thickness,
                thumb_start,
                thumb_thickness,
                thumb_length,
            },
        };
    }
    if (!style.scroll_horizontal || content.width <= viewport.width || viewport.width <= 0.0) {
        return std::nullopt;
    }
    const double ratio = std::clamp(viewport.width / content.width, 0.05, 1.0);
    const double thumb_length = viewport.width * ratio;
    const double maximum = content.width - viewport.width;
    const double thumb_start = viewport.x +
        (viewport.width - thumb_length) * (layout.scroll_offset.x / maximum);
    const double reserved = std::max(0.0, frame.bottom() - viewport.bottom());
    const double hit_height = reserved > 0.0 ? reserved : fallback_gutter;
    const double hit_y = reserved > 0.0 ? viewport.bottom() : viewport.bottom() - hit_height;
    const double thumb_bottom = reserved > 0.0 ? frame.bottom() : viewport.bottom();
    return ScrollbarGeometry{
        axis,
        viewport.x,
        viewport.width,
        thumb_start,
        thumb_length,
        maximum,
        Rect{viewport.x, hit_y, viewport.width, hit_height},
        Rect{
            thumb_start,
            thumb_bottom - thumb_inset - thumb_thickness,
            thumb_length,
            thumb_thickness,
        },
    };
}

} // namespace strata::ui
