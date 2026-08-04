#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/expression.hpp"
#include "ui/layout.hpp"

namespace strata::runtime {
class IndexableSequence;
class RuntimeActionRegistry;
}

namespace strata::ui {

class WidgetInputScope;
class WidgetInspectionScope;
class WidgetRenderScope;
class WidgetSemanticsScope;
class WidgetDescriptionScope;
class WidgetLayoutDefaultsScope;

using WidgetGeneratedChildHook = std::function<std::shared_ptr<const DescriptionNode>(
    WidgetDescriptionScope& scope,
    std::size_t index
)>;

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
using WidgetInputHook = std::function<bool(WidgetInputScope& scope)>;
using WidgetInspectionHook = std::function<void(WidgetInspectionScope& scope)>;
using WidgetSemanticsHook = std::function<void(WidgetSemanticsScope& scope)>;
using WidgetDescriptionHook = std::function<void(WidgetDescriptionScope& scope)>;
using WidgetParticipationHook = bool (*)(const RetainedNode& node) noexcept;
using WidgetLayoutDefaultsHook = std::function<void(WidgetLayoutDefaultsScope& scope)>;
/** Resolves a default action from the attached retained node, including its final key. */
using WidgetDefaultActionFactory = std::function<std::shared_ptr<const runtime::ActionValue>(
    const RetainedNode& node,
    const runtime::RuntimeActionRegistry& actions
)>;
using WidgetTemplateArguments =
    std::map<std::string, runtime::ExpressionValue, std::less<>>;
using WidgetTemplateInstantiator = std::function<std::shared_ptr<const DescriptionNode>(
    std::string_view component,
    std::string key,
    WidgetTemplateArguments arguments
)>;
using WidgetRetainedDependencyObserver = std::function<void(
    std::string_view name,
    const runtime::Value* value
)>;

enum class WidgetTextEditMode { none, single_line, multi_line, numeric, static_text };
enum class WidgetPointerFocusPolicy { automatic, preserve };

struct WidgetDescribePhase final {
    WidgetLayoutDefaultsHook layout_defaults = nullptr;
    WidgetDescriptionHook expand = nullptr;
    std::string canonical_type;
    /** Static no-payload fallback; use default_action_factory when retained identity is required. */
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
    /** Scalar editors publish their draft through onChange; composite editors publish separately. */
    bool editor_emits_change = true;
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

using WidgetPersistenceValidator = bool (*)(
    std::string_view field,
    const runtime::Value& value
) noexcept;

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
    bool depends_on_status_feedback = false;
    WidgetVisualProfile visual{};
    bool depends_on_motion_progress = false;
};

struct WidgetLifecycle final {
    std::string type;
    WidgetParticipationHook participates = nullptr;
    WidgetDescribePhase describe;
    WidgetInputPhase input;
    WidgetSemanticsPhase semantics;
    WidgetInspectionPhase inspection;
    WidgetCommandPhase command;
    WidgetPersistencePhase persistence;
    WidgetPresentPhase present;
};

/**
 * Surface-owned built-in/extension lifecycle table. Generic tree engines know lifecycle phases,
 * never widget names; widget modules own their description, input, semantics, and presentation.
 */
class WidgetRegistry final {
public:
    WidgetRegistry();

    [[nodiscard]] const WidgetLifecycle* find(std::string_view type) const noexcept;
    void register_lifecycle(WidgetLifecycle lifecycle);
    void register_participation(std::string type, WidgetParticipationHook participates);
    void register_describe_phase(std::string type, WidgetDescribePhase phase);
    void register_input_phase(std::string type, WidgetInputPhase phase);
    void register_semantics_phase(std::string type, WidgetSemanticsPhase phase);
    void register_inspection_phase(std::string type, WidgetInspectionPhase phase);
    void register_command_phase(std::string type, WidgetCommandPhase phase);
    void register_persistence_phase(std::string type, WidgetPersistencePhase phase);
    void register_present_phase(std::string type, WidgetPresentPhase phase);
    [[nodiscard]] std::vector<std::string> text_editable_types() const;

    void apply_layout_defaults(
        std::string_view type,
        DescriptionNode::Properties& properties
    ) const;
    [[nodiscard]] WidgetDescriptionExpansion expand_description(
        WidgetDescriptionExpansion description,
        std::string_view state_scope,
        const runtime::RuntimeActionRegistry& actions,
        const RetainedDescriptionSnapshot::Node* retained = nullptr,
        WidgetTemplateInstantiator instantiate_template = {},
        WidgetRetainedDependencyObserver observe_retained = {}
    ) const;

private:
    [[nodiscard]] WidgetLifecycle& lifecycle(std::string type);
    std::map<std::string, WidgetLifecycle, std::less<>> lifecycles_;
};

void register_builtin_widget_presenters(WidgetRegistry& registry);

} // namespace strata::ui
