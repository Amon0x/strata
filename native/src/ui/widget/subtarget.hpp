#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
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
class WidgetRegistry;

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

/** The detached plane a node's overlay and detached subtargets occupy, from its lifecycle. */
[[nodiscard]] int detached_overlay_z_index(
    const WidgetRegistry& widgets,
    const RetainedNode& node
) noexcept;
/**
 * Ascending paint order; reverse iteration is topmost-first hit order. Only nodes of the candidate
 * types, and nodes with behaviors when asked, can participate, so the tree is not walked.
 */
[[nodiscard]] std::vector<DetachedOverlayRoot> detached_overlay_roots(
    const RetainedTree& tree,
    const LayoutResult& layout,
    const WidgetRegistry& widgets,
    std::span<const std::string_view> candidate_types,
    bool behavior_candidates,
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
    /** The span's `href`, carried to its action as the event value (null when it has none). */
    runtime::Value href{};
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
    /** Retained node whose animated presentation determines whether this region can be hit. */
    std::optional<std::uint64_t> presentation_identity = std::nullopt;
    /** Presented ancestor clip resolved for an authored detached target. */
    std::optional<Rect> presentation_clip = std::nullopt;
    /**
     * A polygon, in the same space as `bounds` and inside them, for a region that is not a
     * rectangle (a chart's pie slice): the target is hit only inside it. Empty means the bounds.
     */
    std::vector<Point> shape{};
};

/**
 * Everything one lifecycle `inspection.subtargets` hook may read to project its regions. Built-in
 * and extension widgets receive the same context; the hook owns all widget-specific geometry.
 */
struct WidgetSubtargetContext final {
    const RetainedNode& node;
    const LayoutResult& layout;
    /** The node's own layout record. */
    const LayoutRecord& record;
    const CommandIndex* commands = nullptr;
    const WidgetTextWidthResolver* text_width = nullptr;
    const WidgetTextLayoutResolver* text_layout = nullptr;
    const NotificationService* notifications = nullptr;
    /** The node's live editor draft, when it has one. */
    std::string_view edited_text{};
};

/** Built-in widgets' `inspection.subtargets` hooks, registered with each widget type. */
void register_builtin_widget_subtargets(WidgetRegistry& registry);

} // namespace strata::ui
