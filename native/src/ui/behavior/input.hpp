#pragma once

#include <string_view>

#include "ui/input.hpp"

namespace strata::ui {

class WidgetInputScope;

using BehaviorInputEventPhase = InputEventPhase;

/** Behavior-local capabilities layered over the shared phased input dispatch context. */
class BehaviorInputScope final {
public:
    BehaviorInputScope(
        InputDispatchContext& dispatch,
        const DescriptionBehavior& attachment,
        InputOperationResult& result
    ) noexcept;
    BehaviorInputScope(
        InputRouter& router,
        RetainedNode& node,
        const DescriptionBehavior& attachment,
        const PointerInputEvent& event,
        BehaviorInputEventPhase phase,
        bool target,
        RetainedNode* pointer_target,
        InputOperationResult& result
    ) noexcept;
    BehaviorInputScope(
        InputRouter& router,
        RetainedNode& node,
        const DescriptionBehavior& attachment,
        std::string_view key,
        KeyModifiers modifiers,
        BehaviorInputEventPhase phase,
        bool target,
        InputOperationResult& result
    ) noexcept;
    BehaviorInputScope(
        InputRouter& router,
        RetainedNode& node,
        const DescriptionBehavior& attachment,
        BehaviorInputEventPhase phase,
        InputOperationResult& result
    ) noexcept;

    [[nodiscard]] RetainedNode& node() noexcept;
    [[nodiscard]] const RetainedNode& node() const noexcept;
    [[nodiscard]] const DescriptionBehavior& attachment() const noexcept;
    [[nodiscard]] const PointerInputEvent& event() const noexcept;
    [[nodiscard]] const PointerInputEvent* pointer() const noexcept;
    [[nodiscard]] std::string_view key() const noexcept;
    [[nodiscard]] const KeyModifiers& modifiers() const noexcept;
    [[nodiscard]] BehaviorInputEventPhase phase() const noexcept;
    [[nodiscard]] bool target() const noexcept;
    [[nodiscard]] InputDispatchContext* dispatch() const noexcept;
    [[nodiscard]] WidgetInputScope widget_scope() const noexcept;
    [[nodiscard]] const runtime::Value* option(std::string_view name) const noexcept;
    [[nodiscard]] const runtime::Value* retained(std::string_view name) const noexcept;
    [[nodiscard]] const LayoutRecord* layout() const noexcept;
    [[nodiscard]] const LayoutRecord* layout(const RetainedNode& node) const noexcept;
    [[nodiscard]] RetainedNode* find_key(std::string_view key) const noexcept;
    [[nodiscard]] bool passive_pointer_descendant() const noexcept;
    [[nodiscard]] bool press_matches(bool include_descendants = false) const noexcept;
    [[nodiscard]] bool press_moved_beyond_slop() const noexcept;
    [[nodiscard]] bool long_press_emitted() const noexcept;
    [[nodiscard]] GestureClaimState gesture_claim_state() const noexcept;
    void consume() noexcept;
    void stop_propagation() noexcept;
    [[nodiscard]] bool claim_gesture() noexcept;
    [[nodiscard]] bool cancel_gesture() noexcept;
    void mark_long_press_emitted();
    void set_retained(std::string name, runtime::Value value, DirtyReason reason);
    void set_retained(
        RetainedNode& node,
        std::string name,
        runtime::Value value,
        DirtyReason reason
    );
    [[nodiscard]] bool emit(
        std::string_view event_kind,
        runtime::Value event_value = runtime::Value{}
    );
    [[nodiscard]] bool emit_action(
        std::string_view action_id,
        runtime::Value payload,
        std::string_view event_kind,
        runtime::Value event_value = runtime::Value{}
    );

private:
    InputRouter& router_;
    RetainedNode& node_;
    const DescriptionBehavior& attachment_;
    const PointerInputEvent* event_ = nullptr;
    std::string_view key_;
    KeyModifiers modifiers_;
    BehaviorInputEventPhase phase_;
    bool target_ = false;
    RetainedNode* pointer_target_ = nullptr;
    InputOperationResult& result_;
    InputDispatchContext* dispatch_ = nullptr;
};

} // namespace strata::ui
