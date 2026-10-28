#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "runtime/expression.hpp"
#include "ui/layout.hpp"
#include "ui/theme.hpp"
#include "ui/widget/subtarget.hpp"

namespace strata::runtime {
class IndexableSequence;
class RuntimeActionRegistry;
} // namespace strata::runtime

namespace strata::ui {

class InputRouter;
class WidgetInputScope;
class WidgetInspectionScope;
class WidgetRenderScope;
class WidgetSemanticsScope;
class WidgetDescriptionScope;
class WidgetLayoutDefaultsScope;

using WidgetGeneratedChildHook = std::function<std::shared_ptr<const DescriptionNode>(
    WidgetDescriptionScope& scope, std::size_t index)>;

struct WidgetGeneratedVirtualization final {
    std::shared_ptr<const runtime::IndexableSequence> sequence{};
    std::shared_ptr<const VirtualItemMembers> item_members{};
    std::shared_ptr<const collection::VirtualItemExtents> item_extents{};
};

struct WidgetGeneratedChildren final {
    std::size_t count = 0U;
    WidgetGeneratedChildHook factory;
    WidgetGeneratedVirtualization virtualization;
};

struct WidgetDescriptionExpansion final {
    std::string type;
    std::optional<std::string> key;
    DescriptionNode::Properties properties;
    std::vector<std::shared_ptr<const DescriptionNode>> children;
    std::vector<DescriptionBehavior> behaviors;
    std::size_t synthesized_nodes = 0U;
    /** Optional data-backed child source used by virtual collection descriptions. */
    std::shared_ptr<const DescriptionChildren> generated_children;
    /** Deferred widget row factory finalized by DescriptionBuilder into an owned evaluator. */
    std::shared_ptr<const WidgetGeneratedChildren> generated_widget_children;
};

using WidgetPresentHook = std::function<void(WidgetRenderScope& scope)>;
using WidgetClipHook = std::function<std::optional<Rect>(WidgetRenderScope& scope)>;
/** Whether a widget's overlay has anything to show now, decided without presenting it. */
using WidgetOverlayShown = bool (*)(const RetainedNode& node, const InputRouter& input);
using WidgetInputHook = std::function<bool(WidgetInputScope& scope)>;
using WidgetInspectionHook = std::function<void(WidgetInspectionScope& scope)>;
using WidgetSemanticsHook = std::function<void(WidgetSemanticsScope& scope)>;
struct WidgetFrameInfo final {
    std::int64_t time_nanos = 0;
    std::int64_t delta_nanos = 0;
    bool reduced_motion = false;
};
using WidgetFrameHook = std::function<void(WidgetInputScope& scope, const WidgetFrameInfo& frame)>;
enum class WidgetFrameCost { paint, layout };
using WidgetDescriptionHook = std::function<void(WidgetDescriptionScope& scope)>;
using WidgetParticipationHook = bool (*)(const RetainedNode& node) noexcept;
using WidgetSubtargetsHook =
    std::function<std::vector<WidgetSubtarget>(const WidgetSubtargetContext& context)>;
using WidgetLayoutDefaultsHook = std::function<void(WidgetLayoutDefaultsScope& scope)>;
/** Resolves a default action from the attached retained node, including its final key. */
using WidgetDefaultActionFactory = std::function<std::shared_ptr<const runtime::ActionValue>(
    const RetainedNode& node, const runtime::RuntimeActionRegistry& actions)>;
using WidgetTemplateArguments = std::map<std::string, runtime::ExpressionValue, std::less<>>;
using WidgetTemplateInstantiator = std::function<std::shared_ptr<const DescriptionNode>(
    std::string_view component, std::string key, WidgetTemplateArguments arguments)>;
using WidgetRetainedDependencyObserver =
    std::function<void(std::string_view name, const runtime::Value* value)>;

enum class WidgetTextEditMode { none, single_line, multi_line, numeric, static_text };
enum class WidgetPointerFocusPolicy { automatic, preserve };
/** Why an open popup is being closed by the framework. */
enum class WidgetDismissal { outside_press, escape };
/** Widget-owned cleanup around a framework dismissal. Returns whether it changed retained state
 * the description reads (via WidgetInputScope::exchange_retained), so the caller re-describes once.
 */
using WidgetDismissHook = bool (*)(WidgetInputScope& scope, WidgetDismissal reason);
using WidgetNodePredicate = bool (*)(const RetainedNode& node);
/** A widget's place in form validation: forms validate the fields beneath them. */
enum class WidgetFormRole { none, form, field };

/** Composite editors whose text sits inside one of their own subtargets, inset by a margin. */
struct WidgetEditorViewport final {
    std::string subtarget = "$editor";
    double horizontal_inset = 0.0;
    double vertical_inset = 0.0;
};

/** Item keys whose async loads a widget keeps alive; owner ids are formed `<surface>:<key>`. */
using WidgetAsyncOwnersHook = void (*)(const RetainedNode& node, std::vector<std::string>& owners);

struct WidgetDescribePhase final {
    WidgetLayoutDefaultsHook layout_defaults = nullptr;
    WidgetDescriptionHook expand = nullptr;
    std::string canonical_type;
    /** Static no-payload fallback; use default_action_factory when retained identity is required.
     */
    std::string default_action;
    bool layout_participates = true;
    std::string implicit_key_prefix{};
    bool starts_unmaterialized = false;
    WidgetDefaultActionFactory default_action_factory = nullptr;
    /** Template property whose authored subtree consumes projected control interaction state. */
    std::string authored_presentation_property;
};

struct WidgetInputPhase final {
    /** Generic capture/target/bubble hook run before built-in widget/control policy. */
    WidgetInputHook event = nullptr;
    /** Full pointer lifecycle hook for gestures that cannot be expressed as click activation. */
    WidgetInputHook pointer = nullptr;
    /** Active-pointer time hook; visited only while this widget owns a pressed pointer. */
    WidgetInputHook advance = nullptr;
    /** Active-pointer post-layout hook for geometry-dependent gesture synchronization. */
    WidgetInputHook after_layout = nullptr;
    WidgetInputHook click = nullptr;
    /** Committed text hook run before the shared editor mutates its draft buffer. */
    WidgetInputHook text = nullptr;
    /** Key hook for composite editors that own structural navigation around the draft. */
    WidgetInputHook editor_key = nullptr;
    WidgetInputHook key = nullptr;
    std::string action_property;
    std::string fallback_action;
    bool action_capability_requires_binding = false;
    bool focusable = false;
    /** Overrides pointer-press focus transfer for transient interaction that must not steal it. */
    WidgetPointerFocusPolicy pointer_focus = WidgetPointerFocusPolicy::automatic;
    /** Pointer focus remains possible when false, but traversal omits this lifecycle. */
    bool tabbable = true;
    std::string focusable_when;
    std::string popup_controlled;
    std::string popup_retained;
    std::string popup_initial;
    std::string popup_dismiss_action_property;
    WidgetTextEditMode text_edit_mode = WidgetTextEditMode::none;
    /** Scalar editors publish their draft through onChange; composite editors publish separately.
     */
    bool editor_emits_change = true;
    /** Text viewport of a composite editor; absent, the editor uses its content bounds. Pointer
     * text hits outside the composite viewport miss the editor. */
    std::optional<WidgetEditorViewport> editor_viewport;
    /** Static text that stays focusable and tabbable without selection (rich text links). */
    WidgetNodePredicate static_text_interactive = nullptr;

    /** A press outside this widget (it is not an ancestor of the target) runs this first. */
    WidgetDismissHook outside_press = nullptr;
    /** After the framework closed this widget's popup (popup_retained set to false). */
    WidgetDismissHook on_dismiss = nullptr;
    /** The popup is placed against its layout anchor: presses on the anchor count as inside, and
     * closing it returns focus to the anchor. */
    bool popup_anchored = false;
    /** Set for modal widgets: whether this node currently traps input. */
    WidgetNodePredicate modal_open = nullptr;
    /** Among open modals the highest priority wins; ties go to the last in tree order. A modal
     * popup is closed by outside presses only when the caller includes modals. */
    int modal_priority = 0;
    /** Whether Escape may close this open modal; absent, it always may. */
    WidgetNodePredicate escape_dismissible = nullptr;
    /** Disclosed while hovered after a delay (tooltips): the router keeps its shown state. */
    bool hover_disclosure = false;
    WidgetFormRole form_role = WidgetFormRole::none;
    /** Framework action namespace this widget is the target of (`<namespace>.<operation>`). */
    std::string framework_actions;
};

struct WidgetSemanticsPhase final {
    std::string role = "group";
    std::vector<std::string> actions;
    WidgetSemanticsHook derive = nullptr;
    bool hidden = false;
    bool transparent_when_single_child = false;
};

struct WidgetInspectionPhase final {
    WidgetInspectionHook derive = nullptr;
    /** Presenter-owned pointer regions; the one projection input routing and inspection share. */
    WidgetSubtargetsHook subtargets = nullptr;
    /** Some regions may lie outside the retained bounds (popup rows) and are hit above the tree. */
    bool detached_subtargets = false;
};

struct WidgetCommandPhase final {
    bool declaration = false;
    bool references_self = false;
    std::string references_property;
    /** Missing and empty lists select the complete CommandIndex for command-surface widgets. */
    bool all_when_unreferenced = false;
    std::string item_collection_property;
    std::string item_reference_property;
    /** A scalar command reference whose resolved command replaces ordinary widget activation. */
    std::string activation_reference_property;
};

using WidgetPersistenceValidator = bool (*)(std::string_view field,
                                            const runtime::Value& value) noexcept;

struct WidgetPersistencePhase final {
    /** Only these named retained values may cross a process restart. */
    std::vector<std::string> retained_fields;
    /** Rejects wrong-shaped durable data before it enters a retained widget. */
    WidgetPersistenceValidator accepts = nullptr;
};

struct WidgetVisualProfile final {
    bool transparent_chrome = false;
    bool raised_chrome = false;
    bool text_variant_foreground = false;
};

struct WidgetPresentPhase final {
    WidgetPresentHook content = nullptr;
    WidgetPresentHook foreground = nullptr;
    WidgetPresentHook overlay = nullptr;
    WidgetClipHook descendant_clip = nullptr;
    bool detached_overlay = false;
    /** Plane of this widget's detached overlay and detached subtargets; higher paints above. */
    int overlay_z = detached_overlay_menu_z;
    /** Presents the surface's notifications, so their changes repaint and re-describe it. */
    bool observes_notifications = false;
    /** Shape of the default themed style (chrome, raised surface, body text). */
    ThemeStyleSchema style{};
    /** When set, the overlay is presented only while this holds: a detached overlay that shows
     * rarely (a hover tooltip) costs nothing on the frames it does not. */
    WidgetOverlayShown overlay_shown = nullptr;
    bool depends_on_status_feedback = false;
    WidgetVisualProfile visual{};
    bool depends_on_motion_progress = false;
};

struct WidgetFramePhase final {
    WidgetFrameHook advance = nullptr;
};

struct WidgetLifecycle final {
    std::string type;
    WidgetParticipationHook participates = nullptr;
    /** Async loads this widget owns beyond its state scopes (a tree view's expanded rows). */
    WidgetAsyncOwnersHook async_owners = nullptr;
    /** Paint fields extension widgets may update across stable keyed peers. */
    std::vector<std::string> extension_paint_fields;
    WidgetDescribePhase describe;
    WidgetInputPhase input;
    WidgetSemanticsPhase semantics;
    WidgetInspectionPhase inspection;
    WidgetCommandPhase command;
    WidgetPersistencePhase persistence;
    WidgetPresentPhase present;
    WidgetFramePhase frame;
};

/**
 * Surface-owned built-in/extension lifecycle table. Generic tree engines know lifecycle phases,
 * never widget names; widget modules own their description, input, semantics, and presentation.
 */
class WidgetRegistry final {
  public:
    WidgetRegistry();
    // The trait type lists view the table's keys: a copy lists its own.
    WidgetRegistry(const WidgetRegistry& other);
    WidgetRegistry& operator=(const WidgetRegistry& other);
    WidgetRegistry(WidgetRegistry&& other) noexcept;
    WidgetRegistry& operator=(WidgetRegistry&& other) noexcept;

    [[nodiscard]] const WidgetLifecycle* find(std::string_view type) const noexcept;
    /** Changes whenever the registry does, assignment included, and never repeats: what was
     * looked up in it stays good while this does. */
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    void register_lifecycle(WidgetLifecycle lifecycle);
    void register_participation(std::string type, WidgetParticipationHook participates);
    void register_describe_phase(std::string type, WidgetDescribePhase phase);
    void register_input_phase(std::string type, WidgetInputPhase phase);
    void register_semantics_phase(std::string type, WidgetSemanticsPhase phase);
    void register_inspection_phase(std::string type, WidgetInspectionPhase phase);
    void register_command_phase(std::string type, WidgetCommandPhase phase);
    void register_persistence_phase(std::string type, WidgetPersistencePhase phase);
    void register_present_phase(std::string type, WidgetPresentPhase phase);
    /** Types with a text edit mode, sorted; input preparation walks them every frame. */
    [[nodiscard]] std::span<const std::string_view> text_editable_types() const noexcept;
    /** Types whose nodes declare commands. */
    [[nodiscard]] std::span<const std::string_view> command_declaration_types() const noexcept;
    /** Types with an authored presentation property the input router keeps current. */
    [[nodiscard]] std::span<const std::string_view> authored_presentation_types() const noexcept;
    /** Types that present a detached overlay. */
    [[nodiscard]] std::span<const std::string_view> detached_overlay_types() const noexcept;
    /** Types whose subtargets can lie outside their retained bounds. */
    [[nodiscard]] std::span<const std::string_view> detached_subtarget_types() const noexcept;
    /** Modal types, highest modal_priority first. */
    [[nodiscard]] std::span<const std::string_view> modal_types() const noexcept;
    /** Types whose input phase is a hover disclosure. */
    [[nodiscard]] std::span<const std::string_view> hover_disclosure_types() const noexcept;
    /** Types that present notifications. */
    [[nodiscard]] std::span<const std::string_view> notification_types() const noexcept;
    /** Types that own async loads beyond their state scopes. */
    [[nodiscard]] std::span<const std::string_view> async_owner_types() const noexcept;
    /** Types that are the target of a framework action namespace, sorted. */
    [[nodiscard]] std::vector<std::string_view>
    framework_action_types(std::string_view action_namespace) const;
    /** Sets only the subtarget projection of a type, keeping the rest of its inspection phase. */
    void register_subtargets(std::string type, WidgetSubtargetsHook subtargets,
                             bool detached = false);
    void register_async_owners(std::string type, WidgetAsyncOwnersHook owners);
    /** Sets only the theme style schema of a type, keeping the rest of its present phase. */
    void register_style_schema(std::string type, ThemeStyleSchema schema);

    void apply_layout_defaults(std::string_view type,
                               DescriptionNode::Properties& properties) const;
    [[nodiscard]] WidgetDescriptionExpansion
    expand_description(WidgetDescriptionExpansion description, std::string_view state_scope,
                       const runtime::RuntimeActionRegistry& actions,
                       const RetainedDescriptionSnapshot::Node* retained = nullptr,
                       WidgetTemplateInstantiator instantiate_template = {},
                       WidgetRetainedDependencyObserver observe_retained = {}) const;

  private:
    struct TypeHash final {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(const std::string_view type) const noexcept {
            return std::hash<std::string_view>{}(type);
        }
    };
    [[nodiscard]] WidgetLifecycle& lifecycle(std::string type);
    void refresh_trait_types();
    /** Hashed: every retained node's presentation, input and command pass looks its type up. */
    std::unordered_map<std::string, WidgetLifecycle, TypeHash, std::equal_to<>> lifecycles_;
    // Views of the map's keys, which stay put across rehashing; sorted, so passes are ordered.
    std::vector<std::string_view> command_declaration_types_;
    std::vector<std::string_view> authored_presentation_types_;
    std::vector<std::string_view> detached_overlay_types_;
    std::vector<std::string_view> detached_subtarget_types_;
    std::vector<std::string_view> modal_types_;
    std::vector<std::string_view> hover_disclosure_types_;
    std::vector<std::string_view> notification_types_;
    std::vector<std::string_view> async_owner_types_;
    std::vector<std::string_view> text_editable_types_;
    std::uint64_t revision_ = 0U;
};

void register_builtin_widget_presenters(WidgetRegistry& registry);

} // namespace strata::ui
