#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ui/input/editor.hpp"
#include "ui/layout.hpp"
#include "ui/render.hpp"
#include "ui/tree.hpp"
#include "ui/widget/subtarget.hpp"

namespace strata::resource {
class SvgImageRegistry;
}

namespace strata::ui::lottie {
class SceneCache;
}

namespace strata::ui {

class BehaviorRegistry;
class CommandIndex;
class InputRouter;
class MotionRuntime;
class NotificationService;
class StatusFeedbackService;
class WidgetRegistry;
enum class WidgetTextEditMode;

/** The platform cursor values match the public STRATA_POINTER_CURSOR constants. */
enum class PointerCursor : std::uint8_t {
    standard, pointer, text, grab, grabbing, move, not_allowed, resize_ew, resize_ns,
    resize_nesw, resize_nwse, crosshair, wait, progress, help, none,
};

[[nodiscard]] std::string_view pointer_cursor_name(PointerCursor cursor) noexcept;
[[nodiscard]] std::optional<PointerCursor> pointer_cursor_from_name(std::string_view name) noexcept;

struct TextEditorSnapshot final {
    std::string_view text;
    std::size_t caret = 0U;
    std::size_t selection_start = 0U;
    std::size_t selection_end = 0U;
    std::optional<std::string_view> preedit;
    std::size_t preedit_selection_start = 0U;
    std::size_t preedit_selection_end = 0U;
};

/** A read-only selection, never an editable draft or an IME owner. */
struct StaticTextSelectionSnapshot final {
    std::string_view text;
    std::size_t caret = 0U;
    std::size_t selection_start = 0U;
    std::size_t selection_end = 0U;
};

struct DragPreviewPresentation final {
    Point position;
    Rect source_bounds;
    const std::vector<RenderCommand>* commands = nullptr;
    bool accepted = false;
};

struct DropTargetPresentation final {
    Point position;
    std::string_view placement;
};

/**
 * Tree-bound presentation storage, separate from fixed service references and live input effects.
 * Swapping it transfers geometry caches with their tree, without copying editors or owning IME.
 * Both the live router and an owned candidate use this storage on the serialized Surface owner.
 */
class InputPresentationState {
    friend class InputRouter;
    friend class WidgetInputScope;
    friend class BehaviorInputScope;

  public:
    InputPresentationState(InputPresentationState&&) = delete;
    InputPresentationState& operator=(InputPresentationState&&) = delete;
    InputPresentationState(const InputPresentationState&) = delete;
    InputPresentationState& operator=(const InputPresentationState&) = delete;
    ~InputPresentationState() = default;

  protected:
    InputPresentationState() = default;
    /** Container swap avoids the sentinel allocation of MSVC's map move constructor. */

    /** An ancestor's clip shape in its laid-out coordinate space. */
    struct ClipOutline final {
        std::uint64_t node = 0U;
        std::vector<std::vector<Point>> outlines;
    };
    struct PointerHitEntry final {
        RetainedNode* node = nullptr;
        /** Bounds remain in the anchor's space when scrolling moves that anchor. */
        Rect bounds;
        Rect local_bounds;
        std::int32_t anchor = -1;
        /** An anchor's own node receives only the clips around the anchor. */
        bool anchor_root = false;
        MotionTransform relative;
        std::optional<Rect> relative_clip;
        /** Even-odd hit outlines in the same space as local_bounds. */
        std::vector<std::vector<Point>> shape;
        std::shared_ptr<const std::vector<ClipOutline>> clips;
    };
    /**
     * A presentation root, kept relative to its parent. Moving it inverts the pointer into that
     * space rather than rewriting every descendant hit entry.
     */
    struct PointerAnchor final {
        std::uint64_t identity = 0U;
        std::int32_t parent = -1;
        MotionTransform base;
        MotionTransform local;
        std::optional<Rect> clip;
        std::optional<Rect> own_clip;
        MotionTransform world;
        std::optional<Rect> world_clip;
        std::optional<Rect> world_inner_clip;
        double inherited_opacity = 1.0;
        bool visible = true;
        /** Scrolling content, distinct from stationary viewport/chrome. */
        bool scroll_content = false;
    };
    struct PointerPlacement final {
        std::int32_t anchor = -1;
        MotionTransform relative;
    };
    /** A broad-phase region; first_entry preserves painter-order priority. */
    struct PointerHitBranch final {
        Rect bounds;
        std::size_t first_entry = 0U;
        std::size_t begin = 0U;
        std::size_t end = 0U;
        /** Zero for a leaf; otherwise the left branch immediately follows this one. */
        std::size_t right = 0U;
        /** Overflowing regions retain exact linear tests instead of invalid sorting. */
        bool bounded = true;
    };
    struct PointerHitRoot final {
        std::size_t begin = 0U;
        std::size_t end = 0U;
        std::size_t branch = 0U;
    };
    struct EditorScroll final {
        Point offset;
        /** Changes to these inputs cause the caret to be revealed again. */
        std::size_t revealed_caret = static_cast<std::size_t>(-1);
        std::size_t revealed_text = 0U;
        Rect revealed_viewport;
    };
    struct StaticTextRange final {
        std::size_t anchor = 0U;
        std::size_t focus = 0U;
    };
    /** The authored template controls and interaction values last projected into their tree. */
    struct AuthoredPresentationInputs final {
        const RetainedTree* tree = nullptr;
        std::uint64_t generation = 0U;
        std::uint64_t structure_generation = 0U;
        std::uint64_t render_generation = 0U;
        std::set<std::uint64_t> candidates;
        std::optional<std::uint64_t> focused;
        bool focus_visible = false;
        std::optional<std::uint64_t> active;
        std::set<std::uint64_t> hovered;

    };

    RetainedTree* tree_ = nullptr;
    const LayoutResult* layout_ = nullptr;
    const MotionRuntime* motion_ = nullptr;
    CommandIndex* commands_ = nullptr;
    std::optional<std::uint64_t> focused_;
    bool focused_by_pointer_ = false;
    bool focus_highlight_visible_ = true;
    std::optional<std::string> focus_containment_key_;
    std::set<std::uint64_t> hovered_;
    std::optional<Point> hover_position_;
    bool hover_present_ = false;
    std::set<std::uint64_t> matured_command_tooltips_;
    std::optional<std::uint64_t> active_;
    std::optional<std::pair<std::uint64_t, std::string>> hovered_subtarget_;
    std::optional<WidgetSubtargetKind> hovered_subtarget_kind_;
    std::optional<std::uint64_t> hover_target_;
    std::optional<std::pair<std::uint64_t, std::string>> active_subtarget_;
    std::map<std::uint64_t, EditorScroll> editor_scroll_;
    std::map<std::uint64_t, StaticTextRange> static_text_ranges_;
    const resource::SvgImageRegistry* images_ = nullptr;
    lottie::SceneCache* lottie_scenes_ = nullptr;
    bool reduced_motion_ = false;
    std::int64_t frame_time_nanos_ = 0;
    AuthoredPresentationInputs authored_presentation_inputs_;

    mutable std::size_t pointer_geometry_rebuild_count_ = 0U;
    mutable std::size_t subtarget_projection_count_ = 0U;
    mutable bool preparing_pointer_geometry_ = false;
    mutable const RetainedTree* pointer_geometry_tree_ = nullptr;
    mutable const LayoutResult* pointer_geometry_layout_ = nullptr;
    mutable const MotionRuntime* pointer_geometry_motion_ = nullptr;
    mutable std::uint64_t pointer_geometry_tree_generation_ = 0U;
    mutable std::uint64_t pointer_geometry_layout_generation_ = 0U;
    mutable std::uint64_t pointer_geometry_generation_ = 0U;
    mutable std::uint64_t pointer_geometry_render_generation_ = 0U;
    mutable std::uint64_t pointer_geometry_notification_generation_ = 0U;
    mutable std::uint64_t pointer_geometry_command_generation_ = 0U;
    mutable DirtyGenerationSnapshot pointer_geometry_dirty_generations_;
    mutable std::vector<PointerHitEntry> pointer_hit_entries_;
    mutable std::map<std::uint64_t, std::size_t> pointer_hit_index_;
    mutable std::vector<PointerAnchor> pointer_anchors_;
    mutable std::unordered_map<std::uint64_t, PointerPlacement> pointer_placements_;
    /** Root zero is surface space; subsequent roots belong to anchor index root - 1. */
    mutable std::vector<PointerHitRoot> pointer_hit_roots_;
    mutable std::vector<PointerHitBranch> pointer_hit_branches_;
    mutable std::vector<std::size_t> pointer_hit_order_;
    mutable std::map<std::uint64_t, std::vector<WidgetSubtarget>> projected_subtargets_;
    mutable std::vector<WidgetSubtarget> detached_subtargets_;
    mutable bool pointer_geometry_ready_ = false;
    mutable bool detached_subtargets_ready_ = false;
};

/**
 * The read side of input, shared by rendering, motion, hit geometry and semantics. It neither
 * dispatches events nor owns platform effects.
 */
class InputPresentation : protected InputPresentationState {
    friend class InputRouter;
    friend class WidgetInputScope;
    friend class BehaviorInputScope;

  public:
    using HitBoundsResolver = std::function<Rect(
        const RetainedNode&, const LayoutRecord&, const LayoutResult&, const InputPresentation&)>;
    using TextWidthResolver = WidgetTextWidthResolver;
    using TextLayoutResolver = WidgetTextLayoutResolver;

    [[nodiscard]] std::optional<std::string_view> focused_key() const noexcept;
    [[nodiscard]] bool modal_active() const { return active_modal() != nullptr; }
    [[nodiscard]] bool focus_contained() const noexcept;
    [[nodiscard]] bool focused(std::uint64_t identity) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> focused_identity() const noexcept;
    [[nodiscard]] bool focus_visible(std::uint64_t identity) const noexcept;
    [[nodiscard]] bool focus_indicated() const noexcept;
    [[nodiscard]] bool hovered(std::uint64_t identity) const noexcept;
    [[nodiscard]] PointerCursor cursor() const;
    [[nodiscard]] bool pointer_held() const noexcept;
    [[nodiscard]] bool command_tooltip_ready(std::uint64_t identity) const noexcept;
    [[nodiscard]] bool active(std::uint64_t identity) const noexcept;
    [[nodiscard]] std::vector<WidgetSubtarget> subtargets(std::uint64_t identity) const;
    [[nodiscard]] const std::vector<WidgetSubtarget>&
    projected_subtargets(const RetainedNode& node) const;
    [[nodiscard]] const WidgetRegistry& widget_registry() const noexcept { return widgets_; }
    [[nodiscard]] MotionTransform presentation_transform(const RetainedNode& node) const;
    [[nodiscard]] Point logical_pointer_position(const RetainedNode& node, Point position) const;
    [[nodiscard]] const LayoutResult* layout_result() const noexcept { return layout_; }
    [[nodiscard]] const CommandIndex* command_index() const noexcept { return commands_; }
    [[nodiscard]] Rect presented_bounds(const RetainedNode& node, Rect bounds) const;
    [[nodiscard]] Rect relative_bounds(const RetainedNode& source, const RetainedNode& target,
                                       Rect bounds) const;
    [[nodiscard]] std::optional<Rect> presented_clip(const RetainedNode& node) const;
    [[nodiscard]] std::vector<WidgetSubtarget> project_subtargets(const RetainedNode& node) const;
    [[nodiscard]] bool subtarget_hovered(std::uint64_t identity, std::string_view id) const noexcept;
    [[nodiscard]] std::optional<std::string_view>
    hovered_subtarget(std::uint64_t identity) const noexcept;
    [[nodiscard]] bool subtarget_active(std::uint64_t identity, std::string_view id) const noexcept;
    [[nodiscard]] std::optional<DragPreviewPresentation>
    drag_preview(std::uint64_t identity) const noexcept;
    [[nodiscard]] std::optional<DropTargetPresentation>
    drop_target(std::uint64_t identity) const noexcept;
    [[nodiscard]] const std::string* edited_text(std::uint64_t identity) const noexcept;
    [[nodiscard]] std::optional<TextEditorSnapshot>
    editor_snapshot(std::uint64_t identity) const noexcept;
    [[nodiscard]] Point editor_scroll(std::uint64_t identity) const noexcept;
    [[nodiscard]] std::optional<StaticTextSelectionSnapshot>
    static_text_selection_snapshot(std::uint64_t identity) const noexcept;
    [[nodiscard]] const StatusFeedbackService& status_feedback() const noexcept;
    [[nodiscard]] const NotificationService& notifications() const noexcept;
    [[nodiscard]] const RetainedNode* inspection_target(Point position) const noexcept;
    [[nodiscard]] std::optional<Point> scroll_offset(std::string_view key) const noexcept;
    [[nodiscard]] std::optional<Point> constrained_scroll_target(std::string_view key,
                                                                 Point target) const noexcept;
    /** Settles moved anchors while this frame's dirty marks still describe what changed. */
    void settle_pointer_geometry() const;
    [[nodiscard]] std::int64_t frame_time_nanos() const noexcept { return frame_time_nanos_; }
    [[nodiscard]] const resource::SvgImageRegistry* images() const noexcept { return images_; }
    [[nodiscard]] lottie::SceneCache* lottie_scenes() const noexcept { return lottie_scenes_; }

  protected:
    InputPresentation(const WidgetRegistry& widgets, const BehaviorRegistry& behaviors,
                      StatusFeedbackService& status_feedback, NotificationService& notifications,
                      HitBoundsResolver hit_bounds_resolver, TextWidthResolver text_width_resolver,
                      TextLayoutResolver text_layout_resolver);
    ~InputPresentation() = default;

    [[nodiscard]] bool node_participates(const RetainedNode& node) const noexcept;
    [[nodiscard]] bool node_input_enabled(const RetainedNode& node) const noexcept;
    [[nodiscard]] bool command_tooltip_candidate(std::uint64_t identity) const noexcept;
    [[nodiscard]] bool hover_disclosure(const RetainedNode& node) const noexcept;
    [[nodiscard]] bool focusable(const RetainedNode& node) const noexcept;
    [[nodiscard]] bool tabbable(const RetainedNode& node) const noexcept;
    [[nodiscard]] bool focus_request_allowed(const RetainedNode& node) const noexcept;
    [[nodiscard]] bool focus_valid(std::uint64_t identity) const;
    struct ModalFocusPlan final {
        std::optional<std::uint64_t> modal;
        std::optional<std::uint64_t> before;
        std::optional<std::uint64_t> target;
        bool request = false;
    };
    [[nodiscard]] ModalFocusPlan plan_modal_focus(
        std::optional<std::uint64_t> synchronized, std::optional<std::uint64_t> before) const;
    [[nodiscard]] RetainedNode* focus_boundary() const noexcept;
    [[nodiscard]] bool within_focus_containment(const RetainedNode& node) const noexcept;
    [[nodiscard]] RetainedNode* active_modal() const;
    [[nodiscard]] static bool descendant_of(const RetainedNode& node,
                                            const RetainedNode& ancestor) noexcept;
    [[nodiscard]] static const DescriptionBehavior* behavior(const RetainedNode& node,
                                                             std::string_view id) noexcept;
    [[nodiscard]] MotionTransform placed_transform(std::int32_t anchor,
                                                    MotionTransform relative) const noexcept;
    [[nodiscard]] MotionTransform entry_transform(const PointerHitEntry& entry) const noexcept;
    [[nodiscard]] std::optional<Rect> entry_clip(const PointerHitEntry& entry) const noexcept;
    [[nodiscard]] bool place_moved_anchors() const;
    void prepare_pointer_geometry() const;
    void index_pointer_geometry() const;
    void prepare_detached_subtargets() const;
    [[nodiscard]] RetainedNode* hit_test(Point position) const noexcept;
    [[nodiscard]] bool inside_clips(const PointerHitEntry& entry, Point position) const noexcept;
    [[nodiscard]] std::optional<WidgetSubtarget>
    hit_subtarget(Point position, const RetainedNode* ordinary_target) const;
    struct HoverTarget final {
        RetainedNode* node = nullptr;
        std::optional<WidgetSubtarget> subtarget;
    };
    [[nodiscard]] RetainedNode* subtarget_hover_target(
        Point position, RetainedNode* hit, std::optional<WidgetSubtarget>& subtarget) const;
    [[nodiscard]] HoverTarget resolve_hover(Point position) const;
    struct DragTarget final {
        RetainedNode* node = nullptr;
        const DescriptionBehavior* behavior = nullptr;
        std::string operation;
        std::string placement = "on";
    };
    [[nodiscard]] DragTarget resolve_drag_target(
        std::string_view payload_type, std::span<const std::string> allowed_operations,
        Point position) const;
    [[nodiscard]] static std::vector<std::string>
    drag_operations(const runtime::Value* value, std::vector<std::string> fallback);
    [[nodiscard]] bool static_text_node(const RetainedNode& node) const noexcept;
    [[nodiscard]] bool static_text_selectable(const RetainedNode& node) const noexcept;
    [[nodiscard]] std::optional<std::string_view>
    static_text_value(const RetainedNode& node) const noexcept;
    [[nodiscard]] std::optional<std::string_view>
    static_text_container(const RetainedNode& node) const noexcept;
    [[nodiscard]] std::vector<RetainedNode*> static_text_nodes(std::string_view container) const;
    struct StaticTextSource final {
        const RetainedNode* holder = nullptr;
        std::string_view text;
        std::size_t offset = 0U;
    };
    [[nodiscard]] std::optional<StaticTextSource>
    static_text_source(const RetainedNode& node) const noexcept;
    [[nodiscard]] std::vector<RetainedNode*>
    sourced_static_text_nodes(const RetainedNode& holder, std::string_view container) const;
    /** Projects a whole-source range onto realized rows without advancing its drag session. */
    [[nodiscard]] bool project_source_selection(std::uint64_t holder, std::string_view container,
                                                std::size_t& anchor, std::size_t& focus);
    [[nodiscard]] std::vector<RetainedNode*> focusable_nodes() const;
    [[nodiscard]] std::optional<Point> scroll_limits(const RetainedNode& node) const;
    [[nodiscard]] static runtime::Value scroll_value(Point offset);
    /** Resolves nested scrolling in order; the caller owns mutation and any resulting effects. */
    [[nodiscard]] bool visit_reveal_offsets(
        const RetainedNode& node, Rect revealed, double padding,
        const std::function<bool(const RetainedNode&, Point)>& apply) const;
    struct EditorView final {
        Rect viewport;
        TextLayout layout;
        bool multiline = false;
    };
    [[nodiscard]] std::optional<EditorView> editor_view(const RetainedNode& node,
                                                        std::string_view text) const;
    [[nodiscard]] static const runtime::Value*
    editor_controlled_value(const RetainedNode& node, WidgetTextEditMode mode) noexcept;
    [[nodiscard]] static bool editor_read_only(const RetainedNode& node) noexcept;
    /** Reconciles only caret/viewport projection; IME and frame effects remain with the router. */
    [[nodiscard]] bool project_editor_scroll();
    /** Removes presentation for absent members; true means its hovered subtarget was removed. */
    [[nodiscard]] bool prune_presentation();
    /** Updates only the tree's authored interaction values; its description owner handles dirt. */
    [[nodiscard]] bool project_authored_presentations();

    const WidgetRegistry& widgets_;
    const BehaviorRegistry& behaviors_;
    StatusFeedbackService& status_feedback_;
    NotificationService& notifications_;
    HitBoundsResolver hit_bounds_resolver_;
    TextWidthResolver text_width_resolver_;
    TextLayoutResolver text_layout_resolver_;
    /** Present only for the live view; this pointer never enters a preparation worker. */
    const InputRouter* input_owner_ = nullptr;
};

} // namespace strata::ui
