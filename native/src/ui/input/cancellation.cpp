#include "ui/input.hpp"

#include <cstdint>

namespace strata::ui {

InputOperationResult InputRouter::cancel_interactions() {
    InputOperationResult result;

    clear_focus("invalid_target", result);
    static_cast<void>(cancel_active_drag(result));

    if (tree_ != nullptr) {
        for (auto& [pointer_id, press] : pressed_pointer_targets_) {
            RetainedNode* owner = tree_->find_identity(press.identity);
            if (owner != nullptr) {
                press.gesture = GestureClaimState::cancelled;
                const PointerInputEvent cancellation{
                    press.last_position,
                    PointerEventType::cancel,
                    pointer_id,
                    press.button,
                    {},
                };
                InputDispatchState dispatch = route_pointer_event(
                    owner, owner, cancellation, result
                );
                if (!dispatch.consumed) {
                    static_cast<void>(route_widget_pointer(
                        owner, owner, cancellation, dispatch, result
                    ));
                }
            }
            static_cast<void>(tree_->mark(press.identity, DirtyReason::input));
        }
        if (scrollbar_drag_.has_value()) {
            static_cast<void>(tree_->mark(scrollbar_drag_->identity, DirtyReason::input));
        }
        if (active_.has_value()) {
            static_cast<void>(tree_->mark(*active_, DirtyReason::input));
        }
        hover_route(nullptr);
    } else {
        hovered_.clear();
        hover_started_nanos_.clear();
        matured_command_tooltips_.clear();
    }

    pressed_pointer_targets_.clear();
    drag_sessions_.clear();
    scrollbar_drag_.reset();
    active_.reset();
    routed_subtarget_.reset();
    if (hovered_notification_id_.has_value()) {
        static_cast<void>(notifications_.pause(*hovered_notification_id_, false));
        hovered_notification_id_.reset();
    }
    hovered_subtarget_.reset();
    active_subtarget_.reset();
    synchronized_modal_.reset();
    focus_before_modal_.reset();
    last_text_click_.reset();
    text_click_count_ = 0U;
    last_widget_click_.reset();
    last_widget_click_subtarget_.clear();
    widget_click_count_ = 0U;
    pending_focus_.reset();
    pending_reveals_.clear();
    queued_inputs_.clear();
    input_queue_overflow_reported_ = false;

    if (frame_invalidator_) frame_invalidator_();
    return result;
}

} // namespace strata::ui
