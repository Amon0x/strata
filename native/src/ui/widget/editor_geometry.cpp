#include "ui/widget/editor_geometry.hpp"

#include <algorithm>
#include <ranges>
#include <string_view>

#include "ui/tree.hpp"
#include "ui/widget/subtarget.hpp"

namespace strata::ui {

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

} // namespace strata::ui
