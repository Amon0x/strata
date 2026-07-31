#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui/layout.hpp"
#include "ui/notification.hpp"
#include "ui/quick_pick.hpp"
#include "ui/text.hpp"

namespace strata::ui {

class CommandIndex;
class RetainedNode;

inline constexpr std::string_view tooltip_shown_state = "strata.tooltip.shown";
inline constexpr std::int64_t tooltip_default_show_delay_nanos = 400'000'000;
inline constexpr std::int64_t tooltip_default_hide_delay_nanos = 80'000'000;
inline constexpr std::string_view banner_action_subtarget = "$action";
inline constexpr std::string_view banner_dismiss_subtarget = "$dismiss";
inline constexpr std::string_view banner_active_subtarget_state = "$bannerActiveTarget";

struct BannerProjection final {
    bool active = true;
    bool has_action = false;
    bool dismissible = false;
};

/** Shared retained participation and conditional-control projection for Banner. */
[[nodiscard]] BannerProjection project_banner(const RetainedNode& node) noexcept;
[[nodiscard]] std::optional<bool> tooltip_controlled_visible(
    const RetainedNode& node
) noexcept;
[[nodiscard]] bool tooltip_disclosure_visible(const RetainedNode& node) noexcept;
[[nodiscard]] std::int64_t tooltip_show_delay_nanos(const RetainedNode& node) noexcept;
[[nodiscard]] std::int64_t tooltip_hide_delay_nanos(const RetainedNode& node) noexcept;

struct TooltipProjection final {
    Rect anchor;
    Rect bounds;
    Point text_origin;
    TextLayout text_layout;
};

using ShellTextLayoutResolver = std::function<TextLayout(
    const RetainedNode&,
    std::string_view,
    const TextLayoutOptions&
)>;

/** Shared retained-tooltip geometry used by detached hit roots and presentation. */
[[nodiscard]] std::optional<TooltipProjection> project_tooltip(
    const RetainedNode& node,
    const LayoutResult& layout,
    const ShellTextLayoutResolver& text_layout
);

struct PaletteEntryModel final {
    std::string id;
    std::string label;
    std::string detail;
    std::string searchable_detail;
    std::string command_id;
    std::size_t source_index = 0U;
    std::int64_t recent_rank = std::numeric_limits<std::int64_t>::min();
    int score = 0;
    std::vector<QuickPickMatchSpan> label_spans;
};

struct PaletteProjection final {
    Rect bounds;
    Rect input_bounds;
    std::vector<PaletteEntryModel> matches;
    std::size_t active_index = 0U;
    std::size_t window_start = 0U;
    std::size_t max_visible_rows = 10U;
    double row_height = 32.0;

    [[nodiscard]] std::size_t visible_count() const noexcept;
    [[nodiscard]] Rect row_bounds(std::size_t local_index) const noexcept;
    [[nodiscard]] const PaletteEntryModel* active() const noexcept;
};

[[nodiscard]] PaletteProjection project_command_palette(
    const RetainedNode& node,
    const LayoutResult& layout,
    const CommandIndex* commands,
    std::string_view query
);

struct ToastCardModel final {
    Notification notification;
    Rect bounds;
    Rect message_bounds;
    Point message_origin;
    TextLayoutOptions message_options;
    TextLayout message_layout;
    Rect dismiss_bounds;
    Rect action_bounds;
    Point action_origin;
    TextLayoutOptions action_options;
    TextLayout action_layout;
};

struct ToastProjection final {
    std::vector<ToastCardModel> cards;
    std::size_t overflow_count = 0U;
};

using ToastTextLayoutResolver = ShellTextLayoutResolver;

[[nodiscard]] ToastProjection project_toasts(
    const RetainedNode& node,
    const LayoutResult& layout,
    const NotificationService& notifications,
    const ToastTextLayoutResolver& text_layout = {}
);

} // namespace strata::ui
