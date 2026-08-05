#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <variant>

#include "data/json.hpp"
#include "runtime/application.hpp"
#include "runtime/expression.hpp"
#include "runtime/host_services.hpp"
#include "ui/input/editor.hpp"
#include "ui/layout.hpp"
#include "ui/notification.hpp"
#include "ui/render.hpp"
#include "ui/scroll_geometry.hpp"
#include "ui/tree.hpp"
#include "ui/widget/subtarget.hpp"

namespace strata::ui {

class CommandIndex;
class BehaviorRegistry;
class BehaviorInputScope;
class InputDispatchContext;
class InputRouter;
class MotionRuntime;
class StatusFeedbackService;
class WidgetInputScope;
class WidgetRegistry;

struct InputOperationResult final {
    std::vector<data::JsonValue> events;
    std::vector<data::JsonValue> action_outcomes;
    std::size_t injected_events = 0U;
    std::size_t processed_events = 0U;
};

struct InputProfilerCounters final {
    std::size_t dispatches = 0U;
    std::size_t coalesced_moves = 0U;
    std::size_t behavior_dispatches = 0U;
    std::size_t pointer_geometry_rebuilds = 0U;
};

struct InjectedActionResult final {
    InputOperationResult input;
    runtime::ActionDispatchOutcome outcome;
};

struct KeyModifiers final {
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool super_key = false;
};

enum class PointerEventType { move, press, release, cancel };
enum class KeyEventType { press, release, repeat };

struct PointerInputEvent final {
    PointerInputEvent() = default;
    PointerInputEvent(
        Point position,
        PointerEventType type,
        std::int32_t pointer_id = 0,
        std::int32_t button = 0,
        KeyModifiers modifiers = {},
        Point delta = {},
        std::int64_t timestamp_nanos = 0,
        Point coalesced_origin = {},
        bool has_coalesced_origin = false,
        bool coalesced_moved_beyond_slop = false
    ) : position(position),
        type(type),
        pointer_id(pointer_id),
        button(button),
        modifiers(modifiers),
        delta(delta),
        timestamp_nanos(timestamp_nanos),
        coalesced_origin(coalesced_origin),
        has_coalesced_origin(has_coalesced_origin),
        coalesced_moved_beyond_slop(coalesced_moved_beyond_slop) {}

    Point position;
    PointerEventType type = PointerEventType::move;
    std::int32_t pointer_id = 0;
    std::int32_t button = 0;
    KeyModifiers modifiers;
    Point delta;
    std::int64_t timestamp_nanos = 0;
    Point coalesced_origin;
    bool has_coalesced_origin = false;
    bool coalesced_moved_beyond_slop = false;
};

struct ScrollInputEvent final {
    Point position;
    double delta_x = 0.0;
    double delta_y = 0.0;
    KeyModifiers modifiers{};
    std::int64_t timestamp_nanos = 0;
};

struct KeyInputEvent final {
    std::string key;
    KeyModifiers modifiers;
    KeyEventType type = KeyEventType::press;
    std::int64_t timestamp_nanos = 0;
};

struct TextInputEvent final {
    std::string text;
    std::int64_t timestamp_nanos = 0;
};

struct ImePreeditInputEvent final {
    std::string text;
    std::size_t selection_start = 0U;
    std::size_t selection_end = 0U;
    std::int64_t timestamp_nanos = 0;
};

struct NavigationInputEvent final {
    std::string direction;
    KeyEventType type = KeyEventType::press;
    KeyModifiers modifiers{};
    std::int64_t timestamp_nanos = 0;
};

/** Stable propagation position shared by widget and behavior input lifecycles. */
enum class InputEventPhase { capture, target, bubble, advance, after_layout };

/** Logical event kind; pointer drag is derived once the press crosses the surface slop. */
enum class InputEventKind {
    pointer_move,
    pointer_press,
    pointer_release,
    pointer_drag,
    pointer_cancel,
    scroll,
    key,
    text,
    ime_preedit,
    focus,
    blur,
    advance,
    after_layout,
};

/** A claim is irreversible for one press, so returning to the origin cannot re-arm a click. */
enum class GestureClaimState { unclaimed, claimed, cancelled };

struct InputDispatchState final {
    bool consumed = false;
    bool propagation_stopped = false;
    std::size_t dispatches = 0U;
};

/**
 * One mutable, typed dispatch contract shared by widget and behavior hooks.
 *
 * A routed event owns one InputDispatchState across capture, target, and bubble. Hooks may consume
 * without stopping later phases, stop propagation explicitly, and claim or cancel the active
 * pointer gesture independently of either propagation choice.
 */
class InputDispatchContext final {
public:
    [[nodiscard]] RetainedNode& node() noexcept;
    [[nodiscard]] const RetainedNode& node() const noexcept;
    [[nodiscard]] RetainedNode* target() const noexcept;
    [[nodiscard]] RetainedNode* pointer_target() const noexcept;
    [[nodiscard]] InputEventPhase phase() const noexcept;
    [[nodiscard]] InputEventKind kind() const noexcept;
    [[nodiscard]] const PointerInputEvent* pointer() const noexcept;
    [[nodiscard]] const ScrollInputEvent* scroll() const noexcept;
    [[nodiscard]] const KeyInputEvent* key() const noexcept;
    [[nodiscard]] const TextInputEvent* text() const noexcept;
    [[nodiscard]] const ImePreeditInputEvent* ime_preedit() const noexcept;
    [[nodiscard]] bool consumed() const noexcept;
    [[nodiscard]] bool propagation_stopped() const noexcept;
    [[nodiscard]] std::optional<Point> press_origin() const noexcept;
    [[nodiscard]] std::optional<Point> press_last_position() const noexcept;
    [[nodiscard]] bool press_moved_beyond_slop() const noexcept;
    [[nodiscard]] bool long_press_emitted() const noexcept;
    [[nodiscard]] GestureClaimState gesture_claim_state() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> gesture_claim_owner() const noexcept;

    void consume() noexcept;
    void stop_propagation() noexcept;
    /** Claims this pointer's press for the current node. A different prior owner wins. */
    [[nodiscard]] bool claim_gesture() noexcept;
    /** Cancels this pointer's gesture and permanently suppresses its release activation. */
    [[nodiscard]] bool cancel_gesture() noexcept;

private:
    friend class BehaviorInputScope;
    friend class InputRouter;

    InputDispatchContext(
        InputRouter& router,
        RetainedNode& node,
        RetainedNode* target,
        RetainedNode* pointer_target,
        InputEventPhase phase,
        InputEventKind kind,
        InputDispatchState& state,
        const PointerInputEvent* pointer = nullptr,
        const ScrollInputEvent* scroll = nullptr,
        const KeyInputEvent* key = nullptr,
        const TextInputEvent* text = nullptr,
        const ImePreeditInputEvent* ime_preedit = nullptr
    ) noexcept;

    InputRouter& router_;
    RetainedNode& node_;
    RetainedNode* target_ = nullptr;
    RetainedNode* pointer_target_ = nullptr;
    InputEventPhase phase_ = InputEventPhase::target;
    InputEventKind kind_ = InputEventKind::pointer_move;
    InputDispatchState& state_;
    const PointerInputEvent* pointer_ = nullptr;
    const ScrollInputEvent* scroll_ = nullptr;
    const KeyInputEvent* key_ = nullptr;
    const TextInputEvent* text_ = nullptr;
    const ImePreeditInputEvent* ime_preedit_ = nullptr;
};

/** Platform-neutral input packet consumed atomically by one surface-owned queue. */
using SurfaceInputEvent = std::variant<
    PointerInputEvent,
    ScrollInputEvent,
    KeyInputEvent,
    TextInputEvent,
    ImePreeditInputEvent,
    NavigationInputEvent
>;

/** Surface-local input budgets and gesture thresholds shared by every platform adapter. */
struct InputProcessingConfig final {
    std::size_t max_events_per_frame = 128U;
    std::size_t max_dispatches_per_event = 128U;
    std::size_t max_queued_events = 512U;
    double scroll_step = 32.0;
    double pointer_drag_slop = 4.0;
    double drag_auto_scroll_edge = 24.0;
    double drag_auto_scroll_speed = 420.0;
    std::int64_t drag_auto_scroll_max_frame_nanos = 50'000'000;
    std::int64_t command_tooltip_delay_nanos = 450'000'000;
    std::int64_t multi_click_interval_nanos = 400'000'000;
    double multi_click_slop = 4.0;

    void validate() const;
};

struct TextEditorSnapshot final {
    std::string_view text;
    std::size_t caret = 0U;
    std::size_t selection_start = 0U;
    std::size_t selection_end = 0U;
    std::optional<std::string_view> preedit;
    std::size_t preedit_selection_start = 0U;
    std::size_t preedit_selection_end = 0U;
};

/** Read-only selection projected by selectable Text/RichText; never an editable draft or IME. */
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

/** Retained-identity input/focus router shared by deterministic and platform event adapters. */
class InputRouter final {
public:
    using SurfaceFrameworkExecutor =
        std::function<runtime::ActionDispatchOutcome(const runtime::Action&)>;
    using DescriptionInvalidator =
        std::function<void(const RetainedNode*, std::string_view)>;
    using FrameInvalidator = std::function<void()>;
    using HitBoundsResolver =
        std::function<Rect(const RetainedNode&, const LayoutRecord&)>;
    using TextOffsetResolver = std::function<std::optional<std::size_t>(
        const RetainedNode&,
        const LayoutRecord&,
        std::string_view,
        Point
    )>;
    using TextWidthResolver = WidgetTextWidthResolver;
    using TextLayoutResolver = WidgetTextLayoutResolver;
    using ImeCursorRectResolver = std::function<std::optional<runtime::HostServiceRect>(
        const RetainedNode&,
        const LayoutRecord&,
        const TextEditorSnapshot&
    )>;
    using DragPreviewResolver =
        std::function<std::vector<RenderCommand>(const RetainedNode&)>;
    using ScrollMutationObserver = std::function<void(std::string_view)>;

    InputRouter(
        std::string public_surface_id,
        std::string host_service_owner,
        runtime::ApplicationContext& application,
        const WidgetRegistry& widgets,
        const BehaviorRegistry& behaviors,
        StatusFeedbackService& status_feedback,
        NotificationService& notifications,
        SurfaceFrameworkExecutor surface_framework_executor = {},
        DescriptionInvalidator description_invalidator = {},
        HitBoundsResolver hit_bounds_resolver = {},
        TextOffsetResolver text_offset_resolver = {},
        TextWidthResolver text_width_resolver = {},
        ImeCursorRectResolver ime_cursor_rect_resolver = {},
        DragPreviewResolver drag_preview_resolver = {},
        InputProcessingConfig input_config = {},
        runtime::HostServices* host_services = nullptr,
        TextLayoutResolver text_layout_resolver = {},
        ScrollMutationObserver scroll_mutation_observer = {},
        FrameInvalidator frame_invalidator = {}
    );
    ~InputRouter();

    /** Captures interaction sources before a reconciliation may detach their identities. */
    void begin_tree_update();
    [[nodiscard]] InputOperationResult prepare(
        RetainedTree& tree,
        std::optional<std::string_view> restore_focus_key = std::nullopt
    );
    /** Advances time-owned input state before layout, including stationary drag auto-scroll. */
    [[nodiscard]] InputOperationResult advance_frame();
    [[nodiscard]] InputOperationResult after_layout();
    void publish_layout(const LayoutResult& layout) noexcept;
    void publish_motion(const MotionRuntime& motion) noexcept;
    void publish_commands(CommandIndex& commands) noexcept;
    void publish_frame_time(std::int64_t frame_time_nanos);
    /** Invalidates cached platform geometry while retaining logical focus/editor state. */
    void invalidate_host_geometry() noexcept;
    [[nodiscard]] InputOperationResult click(std::string_view key);
    [[nodiscard]] InputOperationResult drag(std::string_view from_key, std::string_view to_key);
    [[nodiscard]] InputOperationResult key(std::string_view key, KeyModifiers modifiers = {});
    [[nodiscard]] InputOperationResult text(std::string text);
    [[nodiscard]] InputOperationResult ime_preedit(
        std::string text,
        std::size_t selection_start,
        std::size_t selection_end
    );
    [[nodiscard]] InputOperationResult enqueue_click(std::string key);
    [[nodiscard]] InputOperationResult enqueue_pointer(PointerInputEvent event);
    [[nodiscard]] InputOperationResult enqueue_scroll(ScrollInputEvent event);
    [[nodiscard]] InputOperationResult enqueue_scroll(
        std::string key,
        double delta_x,
        double delta_y,
        KeyModifiers modifiers = {}
    );
    [[nodiscard]] InputOperationResult enqueue_drag(std::string from_key, std::string to_key);
    [[nodiscard]] InputOperationResult enqueue_key(std::string key, KeyModifiers modifiers = {});
    [[nodiscard]] InputOperationResult enqueue_text(std::string text);
    [[nodiscard]] InputOperationResult enqueue_ime_preedit(
        std::string text,
        std::size_t selection_start,
        std::size_t selection_end
    );
    /** Enqueues a platform batch atomically; rejection never leaves a partial batch queued. */
    [[nodiscard]] InputOperationResult enqueue(std::vector<SurfaceInputEvent> events);
    /** Cancels all surface-owned focus, capture, hover, press, drag, and queued input state. */
    [[nodiscard]] InputOperationResult cancel_interactions();
    /** Drains queued platform events at the surface input phase. */
    [[nodiscard]] InputOperationResult process_queued();
    /** Injects a typed or explicitly dynamic action through the surface event pipeline. */
    [[nodiscard]] InjectedActionResult dispatch_action(
        std::string action_id,
        runtime::Value payload,
        std::string event_kind,
        std::optional<std::string> source_key,
        runtime::Value event_value,
        bool dynamic = false
    );
    [[nodiscard]] std::size_t queued_event_count() const noexcept;
    /** Drains operation counts accumulated since the previous profiler publication. */
    [[nodiscard]] InputProfilerCounters take_profiler_counters() noexcept;
    /** True when capture/drag/reveal or diagnostics require a time-advancing frame. */
    [[nodiscard]] bool requires_frame_advance() const noexcept;
    void set_clipboard(std::optional<std::string> text);
    [[nodiscard]] std::optional<std::string_view> clipboard_text() const noexcept;
    [[nodiscard]] std::vector<runtime::RuntimeDiagnostic> take_diagnostics();
    void clear_diagnostics() noexcept;

    [[nodiscard]] std::optional<std::uint64_t> focused_identity() const noexcept;
    [[nodiscard]] std::optional<std::string_view> focused_key() const noexcept;
    /** Constrains all focus acquisition and traversal to one retained subtree; null clears it. */
    [[nodiscard]] bool set_focus_containment(
        std::optional<std::string_view> key,
        InputOperationResult& result
    );
    [[nodiscard]] bool focus_contained() const noexcept;
    /** Semantic focus remains active even when pointer modality suppresses its visual indicator. */
    [[nodiscard]] bool focused(std::uint64_t identity) const noexcept;
    /** True only for the focused node while keyboard/spatial focus indication is active. */
    [[nodiscard]] bool focus_visible(std::uint64_t identity) const noexcept;
    [[nodiscard]] bool hovered(std::uint64_t identity) const noexcept;
    /** True after a stationary command-bound hover crosses its one-shot disclosure deadline. */
    [[nodiscard]] bool command_tooltip_ready(std::uint64_t identity) const noexcept;
    [[nodiscard]] bool hover_ready(
        std::uint64_t identity,
        std::int64_t delay_nanos
    ) const noexcept;
    [[nodiscard]] bool active(std::uint64_t identity) const noexcept;
    [[nodiscard]] std::vector<WidgetSubtarget> subtargets(std::uint64_t identity) const;
    [[nodiscard]] bool subtarget_hovered(
        std::uint64_t identity,
        std::string_view id
    ) const noexcept;
    [[nodiscard]] bool subtarget_active(
        std::uint64_t identity,
        std::string_view id
    ) const noexcept;
    [[nodiscard]] std::optional<DragPreviewPresentation> drag_preview(
        std::uint64_t identity
    ) const noexcept;
    [[nodiscard]] std::optional<DropTargetPresentation> drop_target(
        std::uint64_t identity
    ) const noexcept;
    [[nodiscard]] const std::string* edited_text(std::uint64_t identity) const noexcept;
    [[nodiscard]] std::optional<TextEditorSnapshot> editor_snapshot(
        std::uint64_t identity
    ) const noexcept;
    [[nodiscard]] std::optional<StaticTextSelectionSnapshot> static_text_selection_snapshot(
        std::uint64_t identity
    ) const noexcept;
    [[nodiscard]] std::vector<std::string> pending_navigation_targets() const;
    [[nodiscard]] const StatusFeedbackService& status_feedback() const noexcept;
    [[nodiscard]] NotificationService& notifications() noexcept;
    [[nodiscard]] const NotificationService& notifications() const noexcept;
    /** Raw topmost retained hit used by the surface-owned inspector. */
    [[nodiscard]] const RetainedNode* inspection_target(Point position) const noexcept;
    [[nodiscard]] std::optional<Point> scroll_offset(std::string_view key) const noexcept;
    [[nodiscard]] std::optional<Point> constrained_scroll_target(
        std::string_view key,
        Point target
    ) const noexcept;
    [[nodiscard]] bool scroll_to(
        std::string_view key,
        Point target,
        InputOperationResult& result
    );

private:
    friend class BehaviorInputScope;
    friend class InputDispatchContext;
    friend class WidgetInputScope;

    struct DragSession;
    struct DragTarget;

    [[nodiscard]] bool node_participates(const RetainedNode& node) const noexcept;
    [[nodiscard]] bool node_input_enabled(const RetainedNode& node) const noexcept;
    [[nodiscard]] bool command_tooltip_candidate(std::uint64_t identity) const noexcept;
    [[nodiscard]] bool tooltip_engaged(const RetainedNode& node) const noexcept;
    [[nodiscard]] bool tooltip_disclosures_need_frame() const noexcept;
    void update_tooltip_disclosures();
    void synchronize_authored_presentations();
    [[nodiscard]] bool focusable(const RetainedNode& node) const noexcept;
    [[nodiscard]] bool tabbable(const RetainedNode& node) const noexcept;
    [[nodiscard]] RetainedNode* focusable_ancestor(RetainedNode* node) const noexcept;
    [[nodiscard]] RetainedNode* pointer_focusable_ancestor(RetainedNode* node) const noexcept;
    void apply_pointer_focus_default(
        const PointerInputEvent& event,
        RetainedNode* pointer_target,
        const RetainedNode* interaction_target,
        InputOperationResult& result
    );
    [[nodiscard]] bool passive_pointer_path(
        const RetainedNode& owner,
        const RetainedNode* pointer_target
    ) const noexcept;
    [[nodiscard]] RetainedNode* focus_boundary() const noexcept;
    [[nodiscard]] bool within_focus_containment(const RetainedNode& node) const noexcept;
    void dismiss_transient_popups(
        const RetainedNode* target,
        InputOperationResult& result,
        bool include_modal = true,
        bool restore_modal_focus = true
    );
    [[nodiscard]] data::JsonValue source(const RetainedNode& node) const;
    void focus(
        const RetainedNode& node,
        std::string_view reason,
        InputOperationResult& result
    );
    void clear_focus(
        std::string_view reason,
        InputOperationResult& result,
        bool dismiss_popups = true
    );
    void hover_route(const RetainedNode* target);
    runtime::ActionDispatchOutcome emit(
        data::JsonValue event,
        const std::shared_ptr<const runtime::ActionValue>& action,
        const RetainedNode& node,
        runtime::Value event_value,
        InputOperationResult& result
    );
    runtime::ActionDispatchOutcome emit(
        data::JsonValue event,
        const std::shared_ptr<const runtime::Action>& action,
        const RetainedNode& node,
        runtime::Value event_value,
        InputOperationResult& result
    );
    void report_action_outcome(
        const runtime::Action& action,
        const runtime::ActionDispatchOutcome& outcome,
        const RetainedNode& node
    );
    [[nodiscard]] runtime::ActionDispatchOutcome execute_form_action(
        const runtime::Action& action,
        InputOperationResult& result
    );
    [[nodiscard]] bool validate_form(
        RetainedNode& form,
        bool focus_first_invalid,
        InputOperationResult& result
    );
    [[nodiscard]] bool validate_field(RetainedNode& field, bool mark_touched);
    void note_field_change(RetainedNode& node);
    void note_field_blur(RetainedNode& node);
    [[nodiscard]] static RetainedNode* ancestor(
        RetainedNode& node,
        std::string_view type
    ) noexcept;
    [[nodiscard]] std::shared_ptr<const runtime::ActionValue> activation_action(
        const RetainedNode& node,
        std::string_view property
    ) const;
    [[nodiscard]] data::JsonValue canonical_action(
        const runtime::ActionValue& action,
        const RetainedNode& node
    ) const;
    [[nodiscard]] data::JsonValue canonical_action(
        const runtime::Action& action,
        const RetainedNode& node,
        bool include_origin = true
    ) const;
    [[nodiscard]] runtime::ActionDispatchOutcome execute_action(
        const runtime::ActionEvent& event,
        const runtime::ActionValue& action,
        const RetainedNode& node,
        InputOperationResult& result
    );
    [[nodiscard]] runtime::ActionDispatchOutcome execute_action(
        const runtime::ActionEvent& event,
        const runtime::Action& action,
        const RetainedNode& node,
        InputOperationResult& result,
        const runtime::LexicalStateBinding* lexical_state_binding = nullptr
    );
    [[nodiscard]] std::optional<runtime::ActionDispatchOutcome> execute_surface_action(
        const runtime::ActionEvent& event,
        const runtime::Action& action,
        const RetainedNode& node,
        InputOperationResult& result
    );
    [[nodiscard]] runtime::ActionDispatchOutcome execute_composition(
        const runtime::ActionEvent& event,
        const runtime::ActionValue& composition,
        const RetainedNode& node,
        InputOperationResult& result
    );
    [[nodiscard]] runtime::ActionDispatchOutcome execute_tree_action(
        const runtime::Action& action,
        InputOperationResult& result
    );
    [[nodiscard]] runtime::ActionDispatchOutcome execute_command_action(
        const runtime::Action& action,
        const RetainedNode& source_node,
        InputOperationResult& result
    );
    struct CommandInvocation final {
        std::string status;
        std::string message;
        bool executed = false;
    };
    [[nodiscard]] CommandInvocation invoke_command(
        std::string_view id,
        const RetainedNode& source_node,
        bool invoked_from_active_modal,
        InputOperationResult& result
    );
    [[nodiscard]] bool route_command_shortcut(
        std::string_view key,
        KeyModifiers modifiers,
        KeyEventType type,
        bool editing_owns_key,
        InputOperationResult& result
    );
    [[nodiscard]] RetainedNode* active_modal() const;
    [[nodiscard]] static bool descendant_of(
        const RetainedNode& node,
        const RetainedNode& ancestor
    ) noexcept;
    [[nodiscard]] static const DescriptionBehavior* behavior(
        const RetainedNode& node,
        std::string_view id
    ) noexcept;
    [[nodiscard]] static RetainedNode* drag_source_ancestor(RetainedNode* node) noexcept;
    struct PointerHitEntry final {
        RetainedNode* node = nullptr;
        Rect bounds;
        std::optional<Rect> traversal_clip;
        MotionTransform transform;
    };
    void prepare_pointer_geometry() const;
    [[nodiscard]] const std::vector<WidgetSubtarget>& projected_subtargets(
        const RetainedNode& node
    ) const;
    void prepare_detached_subtargets() const;
    [[nodiscard]] RetainedNode* hit_test(Point position) const noexcept;
    [[nodiscard]] std::optional<WidgetSubtarget> hit_subtarget(
        Point position,
        const RetainedNode* ordinary_target
    ) const;
    void set_hovered_subtarget(const std::optional<WidgetSubtarget>& target);
    [[nodiscard]] RetainedNode* interactive_ancestor(RetainedNode* node) const noexcept;
    [[nodiscard]] InputDispatchState route_event(
        RetainedNode* target,
        RetainedNode* pointer_target,
        InputEventKind kind,
        const PointerInputEvent* pointer,
        const ScrollInputEvent* scroll,
        const KeyInputEvent* key,
        const TextInputEvent* text,
        const ImePreeditInputEvent* ime_preedit,
        InputOperationResult& result
    );
    [[nodiscard]] InputDispatchState route_pointer_event(
        RetainedNode* target,
        RetainedNode* pointer_target,
        const PointerInputEvent& event,
        InputOperationResult& result
    );
    [[nodiscard]] InputDispatchState route_key_event(
        RetainedNode* target,
        const KeyInputEvent& event,
        InputOperationResult& result
    );
    [[nodiscard]] bool route_widget_pointer(
        RetainedNode* target,
        RetainedNode* pointer_target,
        const PointerInputEvent& event,
        InputDispatchState& dispatch_state,
        InputOperationResult& result
    );
    void route_active_lifecycle_hooks(
        bool after_layout,
        InputOperationResult& result
    );
    [[nodiscard]] InputOperationResult pointer(PointerInputEvent event);
    [[nodiscard]] InputOperationResult key(
        KeyInputEvent event,
        bool navigation_traversal_repeat
    );
    [[nodiscard]] InputOperationResult text(TextInputEvent event);
    [[nodiscard]] InputOperationResult ime_preedit(ImePreeditInputEvent event);
    /** Routes the pending/captured pointer through the drag gesture state machine. */
    [[nodiscard]] bool route_pointer_drag(
        const PointerInputEvent& event,
        InputOperationResult& result
    );
    [[nodiscard]] bool cancel_active_drag(InputOperationResult& result);
    [[nodiscard]] DragTarget resolve_drag_target(
        const DragSession& session,
        Point position
    ) const;
    [[nodiscard]] DragTarget retained_drag_target(const DragSession& session) const;
    [[nodiscard]] static std::vector<std::string> drag_operations(
        const runtime::Value* value,
        std::vector<std::string> fallback
    );
    void synchronize_drag_target(
        DragSession& session,
        Point position,
        bool emit_move,
        InputOperationResult& result
    );
    void emit_drag_lifecycle(
        DragSession& session,
        std::string_view phase,
        const DragTarget& target,
        Point position,
        InputOperationResult& result
    );
    [[nodiscard]] bool auto_scroll_drag(
        DragSession& session,
        InputOperationResult& result
    );
    [[nodiscard]] bool route_scrollbar_pointer(
        const PointerInputEvent& event,
        InputOperationResult& result
    );
    [[nodiscard]] InputOperationResult scroll(ScrollInputEvent event);
    [[nodiscard]] std::optional<Point> scroll_limits(const RetainedNode& node) const;
    [[nodiscard]] bool set_scroll_offset(
        const RetainedNode& node,
        Point offset,
        InputOperationResult& result
    );
    void reveal_focus(const RetainedNode& node, InputOperationResult& result);
    [[nodiscard]] bool scroll_focused_ancestor(
        std::string_view key,
        InputOperationResult& result
    );
    void place_pointer_caret(
        RetainedNode& node,
        const PointerInputEvent& event,
        bool extend_selection,
        InputOperationResult& result
    );
    [[nodiscard]] std::optional<std::size_t> resolve_text_offset(
        const RetainedNode& node,
        std::string_view text,
        Point position
    ) const;
    [[nodiscard]] Point logical_pointer_position(
        const RetainedNode& node,
        Point position
    ) const;
    void seed_pointer_text_navigation(
        const RetainedNode& node,
        std::string_view text,
        std::size_t caret,
        Point position
    );
    [[nodiscard]] bool static_text_node(const RetainedNode& node) const noexcept;
    [[nodiscard]] bool static_text_selectable(const RetainedNode& node) const noexcept;
    [[nodiscard]] RetainedNode* selectable_static_text_owner(
        RetainedNode* hit
    ) const noexcept;
    [[nodiscard]] std::optional<std::string_view> static_text_value(
        const RetainedNode& node
    ) const noexcept;
    [[nodiscard]] std::optional<std::string_view> static_text_container(
        const RetainedNode& node
    ) const noexcept;
    [[nodiscard]] std::vector<RetainedNode*> static_text_nodes(
        std::string_view container
    ) const;
    void begin_static_text_selection(
        RetainedNode& node,
        InputOperationResult& result
    );
    void transition_static_text_selection_owner(
        RetainedNode* owner,
        InputOperationResult& result
    );
    [[nodiscard]] bool static_text_selection_owner_matches(
        const RetainedNode& node
    ) const noexcept;
    void extend_static_text_selection(
        RetainedNode& candidate,
        Point position,
        InputOperationResult& result
    );
    [[nodiscard]] bool move_static_text_selection(
        RetainedNode& node,
        std::string_view key,
        KeyModifiers modifiers,
        InputOperationResult& result
    );
    void set_static_text_selection(
        RetainedNode& node,
        std::size_t anchor,
        std::size_t focus,
        InputOperationResult& result
    );
    [[nodiscard]] std::optional<std::size_t> visual_text_navigation_offset(
        const RetainedNode& node,
        std::string_view text,
        std::size_t caret,
        std::string_view key
    );
    [[nodiscard]] bool copy_static_text_selection(RetainedNode& owner);
    [[nodiscard]] bool select_all_static_text(RetainedNode& owner, InputOperationResult& result);
    void activate(
        RetainedNode& target,
        InputOperationResult& result,
        const PointerInputEvent* pointer = nullptr,
        RetainedNode* pointer_target = nullptr,
        std::size_t click_count = 0U,
        std::optional<WidgetSubtarget> subtarget = std::nullopt
    );
    [[nodiscard]] std::vector<RetainedNode*> focusable_nodes() const;
    [[nodiscard]] bool traverse_focus(bool backwards, InputOperationResult& result);
    [[nodiscard]] bool move_focus_spatial(
        std::string_view direction,
        InputOperationResult& result
    );
    [[nodiscard]] bool dismiss_topmost(InputOperationResult& result);
    void sync_modal_focus(InputOperationResult& result);
    void sanitize_focus(InputOperationResult& result);
    void set_focus_visibility(bool visible);
    void record_editor_mutation(
        RetainedNode& node,
        const TextEditorMutation& mutation,
        InputOperationResult& result
    );
    [[nodiscard]] bool insert_editor_text(
        RetainedNode& node,
        std::string_view value,
        InputOperationResult& result
    );
    [[nodiscard]] bool clear_editor_text(
        RetainedNode& node,
        InputOperationResult& result
    );
    void synchronize_editor_text(
        RetainedNode& node,
        std::string_view value,
        bool move_caret_to_end
    );
    void commit_editor(RetainedNode& node, InputOperationResult& result);

    [[nodiscard]] Point injection_point(std::string_view key) const;
    [[nodiscard]] bool enqueue_input(SurfaceInputEvent input);
    [[nodiscard]] bool enqueue_inputs(std::vector<SurfaceInputEvent> inputs);
    void report_input_queue_overflow();

    struct PendingFocus final {
        std::uint64_t identity = 0U;
        data::JsonValue source;
    };

    struct ScrollbarDrag final {
        std::uint64_t identity = 0U;
        ScrollbarAxis axis = ScrollbarAxis::vertical;
        double grab_offset = 0.0;
        std::int32_t pointer_id = 0;
    };

    struct PointerPress final {
        std::uint64_t identity = 0U;
        Point position;
        std::int32_t button = 0;
        std::int64_t timestamp_nanos = 0;
        Point last_position;
        bool moved_beyond_slop = false;
        bool long_press_emitted = false;
        GestureClaimState gesture = GestureClaimState::unclaimed;
        std::optional<std::uint64_t> gesture_owner = std::nullopt;
        std::string subtarget_id;
    };

    struct DragTarget final {
        RetainedNode* node = nullptr;
        const DescriptionBehavior* behavior = nullptr;
        std::string operation;
        std::string placement = "on";
    };

    struct DragSession final {
        std::uint64_t source_identity = 0U;
        Point start;
        Point position;
        runtime::Value payload;
        std::string payload_type;
        std::vector<std::string> allowed_operations;
        double slop = 4.0;
        bool active = false;
        std::optional<std::int64_t> last_auto_scroll_nanos;
        std::optional<std::uint64_t> target_identity;
        std::optional<std::string> operation;
        std::string placement = "on";
        Rect preview_bounds;
        std::vector<RenderCommand> preview_commands;
    };

    struct PendingReveal final {
        std::string key;
        std::optional<std::string> scroll_key;
        bool focus = false;
        double padding = 6.0;
        std::size_t attempts = 0U;
    };

    /** Public identity retained in events, action context, and diagnostics. */
    std::string public_surface_id_;
    /** Private collision-free identity used only at the runtime HostServices boundary. */
    std::string host_service_owner_;
    runtime::ApplicationContext& application_;
    const WidgetRegistry& widgets_;
    const BehaviorRegistry& behaviors_;
    StatusFeedbackService& status_feedback_;
    NotificationService& notifications_;
    SurfaceFrameworkExecutor surface_framework_executor_;
    DescriptionInvalidator description_invalidator_;
    FrameInvalidator frame_invalidator_;
    HitBoundsResolver hit_bounds_resolver_;
    TextOffsetResolver text_offset_resolver_;
    TextWidthResolver text_width_resolver_;
    TextLayoutResolver text_layout_resolver_;
    ImeCursorRectResolver ime_cursor_rect_resolver_;
    DragPreviewResolver drag_preview_resolver_;
    ScrollMutationObserver scroll_mutation_observer_;
    InputProcessingConfig input_config_;
    runtime::HostServices fallback_host_services_;
    runtime::HostServices* host_services_ = nullptr;
    RetainedTree* tree_ = nullptr;
    const LayoutResult* layout_ = nullptr;
    const MotionRuntime* motion_ = nullptr;
    CommandIndex* commands_ = nullptr;
    std::optional<std::uint64_t> focused_;
    // Programmatic focus inherits the last real input modality. Initial focus remains visible.
    bool focus_highlight_visible_ = true;
    std::optional<std::uint64_t> synchronized_modal_;
    std::optional<std::uint64_t> focus_before_modal_;
    bool dismissing_transient_popups_ = false;
    std::optional<std::string> focus_containment_key_;
    std::optional<std::uint64_t> focus_before_containment_;
    std::set<std::uint64_t> hovered_;
    std::map<std::uint64_t, std::int64_t> hover_started_nanos_;
    std::set<std::uint64_t> matured_command_tooltips_;
    std::optional<std::uint64_t> active_;
    std::optional<WidgetSubtarget> routed_subtarget_;
    std::optional<std::pair<std::uint64_t, std::string>> hovered_subtarget_;
    std::optional<std::uint64_t> hovered_notification_id_;
    std::optional<std::pair<std::uint64_t, std::string>> active_subtarget_;
    std::map<std::int32_t, PointerPress> pressed_pointer_targets_;
    std::map<std::int32_t, DragSession> drag_sessions_;
    std::optional<ScrollbarDrag> scrollbar_drag_;
    std::optional<std::uint64_t> last_text_click_;
    Point last_text_click_position_;
    std::int64_t last_text_click_nanos_ = 0;
    std::size_t text_click_count_ = 0U;
    std::optional<std::uint64_t> last_widget_click_;
    std::string last_widget_click_subtarget_;
    Point last_widget_click_position_;
    std::int64_t last_widget_click_nanos_ = 0;
    std::size_t widget_click_count_ = 0U;
    std::optional<PendingFocus> pending_focus_;
    std::vector<PendingReveal> pending_reveals_;
    std::map<std::uint64_t, TextEditor> editors_;
    struct StaticTextRange final {
        std::size_t anchor = 0U;
        std::size_t focus = 0U;
    };
    std::map<std::uint64_t, StaticTextRange> static_text_ranges_;
    struct StaticTextSelectionSession final {
        std::uint64_t anchor_identity = 0U;
        std::size_t anchor_offset = 0U;
        std::optional<std::string> container;
    };
    std::optional<StaticTextSelectionSession> static_text_selection_;
    struct TextNavigationState final {
        std::size_t caret = 0U;
        std::size_t line = 0U;
        std::optional<double> vertical_x_goal;
    };
    std::map<std::uint64_t, TextNavigationState> text_navigation_;
    std::deque<SurfaceInputEvent> queued_inputs_;
    std::size_t dispatch_count_ = 0U;
    std::size_t coalesced_move_count_ = 0U;
    std::size_t behavior_dispatch_count_ = 0U;
    mutable std::size_t pointer_geometry_rebuild_count_ = 0U;
    mutable const RetainedTree* pointer_geometry_tree_ = nullptr;
    mutable const LayoutResult* pointer_geometry_layout_ = nullptr;
    mutable const MotionRuntime* pointer_geometry_motion_ = nullptr;
    mutable std::uint64_t pointer_geometry_tree_generation_ = 0U;
    mutable std::uint64_t pointer_geometry_layout_generation_ = 0U;
    mutable std::uint64_t pointer_geometry_notification_generation_ = 0U;
    mutable DirtyGenerationSnapshot pointer_geometry_dirty_generations_;
    mutable std::vector<PointerHitEntry> pointer_hit_entries_;
    mutable std::map<std::uint64_t, std::vector<WidgetSubtarget>> projected_subtargets_;
    mutable std::vector<WidgetSubtarget> detached_subtargets_;
    mutable bool pointer_geometry_ready_ = false;
    mutable bool detached_subtargets_ready_ = false;
    bool input_queue_overflow_reported_ = false;
    std::vector<runtime::RuntimeDiagnostic> pending_diagnostics_;
    std::int64_t frame_time_nanos_ = 0;
};

} // namespace strata::ui
