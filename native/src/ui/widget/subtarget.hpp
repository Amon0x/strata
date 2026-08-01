#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/value.hpp"
#include "ui/layout.hpp"
#include "ui/text.hpp"

namespace strata::runtime {
struct ActionValue;
}

namespace strata::ui {

class CommandIndex;
class NotificationService;
class RetainedNode;
class RetainedTree;

using WidgetTextWidthResolver = std::function<double(
    const RetainedNode& node,
    std::string_view text
)>;

using WidgetTextLayoutResolver = std::function<TextLayout(
    const RetainedNode& node,
    std::string_view text,
    const TextLayoutOptions& options
)>;

inline constexpr int detached_overlay_menu_z = 20'000;
inline constexpr int detached_overlay_tooltip_z = 22'000;
inline constexpr int detached_overlay_palette_z = 30'000;
inline constexpr int detached_overlay_toast_z = 31'000;

/** One shared detached-overlay ordering record used by paint and hit testing. */
struct DetachedOverlayRoot final {
    const RetainedNode* node = nullptr;
    int z_index = detached_overlay_menu_z;
    std::size_t author_order = 0U;
};

using DetachedOverlayPredicate = std::function<bool(const RetainedNode&)>;

[[nodiscard]] int detached_overlay_z_index(const RetainedNode& node) noexcept;
/** Ascending paint order; reverse iteration is topmost-first hit order. */
[[nodiscard]] std::vector<DetachedOverlayRoot> detached_overlay_roots(
    const RetainedTree& tree,
    const LayoutResult& layout,
    const DetachedOverlayPredicate& participates
);

enum class WidgetSubtargetKind {
    control,
    link,
    choice,
    command,
    action,
    dismiss,
    scrim,
    token,
    separator,
    notification,
};

struct RichTextLink final {
    std::size_t span_index = 0U;
    std::size_t byte_start = 0U;
    std::size_t byte_end = 0U;
    std::size_t utf16_start = 0U;
    std::size_t utf16_end = 0U;
    std::shared_ptr<const runtime::ActionValue> action;
};

/** Link spans and canonical UTF-8/UTF-16 ranges shared by input and virtual hit geometry. */
[[nodiscard]] std::vector<RichTextLink> rich_text_links(const RetainedNode& node);

/** Presenter-independent virtual hit region for controls that paint non-retained rows. */
struct WidgetSubtarget final {
    std::uint64_t owner_identity = 0U;
    std::string id;
    WidgetSubtargetKind kind = WidgetSubtargetKind::control;
    std::size_t index = 0U;
    Rect bounds;
    runtime::Value value;
    std::string label;
    std::string command_id;
    bool enabled = true;
    bool detached = false;
    int z_index = 0;
    std::vector<std::size_t> path{};
    std::string detail{};
    bool separator = false;
    bool has_children = false;
    std::optional<bool> checked = std::nullopt;
    std::optional<std::uint64_t> notification_id = std::nullopt;
};

/** True when this widget type can project presenter-owned pointer regions. */
[[nodiscard]] bool widget_projects_subtargets(std::string_view type) noexcept;
/** True when the widget can project pointer regions outside its retained layout bounds. */
[[nodiscard]] bool widget_projects_detached_subtargets(std::string_view type) noexcept;
/** Shared geometry projection consumed by both widget presenters and input hit testing. */
[[nodiscard]] std::vector<WidgetSubtarget> widget_subtargets(
    const RetainedNode& node,
    const LayoutResult& layout,
    const CommandIndex* commands,
    const WidgetTextWidthResolver& text_width = {},
    const WidgetTextLayoutResolver& text_layout = {},
    const NotificationService* notifications = nullptr,
    std::string_view edited_text = {}
);

} // namespace strata::ui
