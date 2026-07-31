#include "ui/behavior/input.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "runtime/action.hpp"
#include "runtime/value.hpp"
#include "ui/widget/input.hpp"

namespace strata::ui {
namespace {

using data::JsonValue;

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> entries) {
    return JsonValue(JsonValue::Object(entries));
}

} // namespace

BehaviorInputScope::BehaviorInputScope(
    InputDispatchContext& dispatch,
    const DescriptionBehavior& attachment,
    InputOperationResult& result
) noexcept : router_(dispatch.router_),
    node_(dispatch.node()),
    attachment_(attachment),
    event_(dispatch.pointer()),
    key_(dispatch.key() != nullptr ? std::string_view(dispatch.key()->key) : std::string_view{}),
    modifiers_(dispatch.key() != nullptr ? dispatch.key()->modifiers : KeyModifiers{}),
    phase_(dispatch.phase()),
    target_(&dispatch.node() == dispatch.target()),
    pointer_target_(dispatch.pointer_target()),
    result_(result),
    dispatch_(&dispatch) { ++router_.behavior_dispatch_count_; }

BehaviorInputScope::BehaviorInputScope(
    InputRouter& router,
    RetainedNode& node,
    const DescriptionBehavior& attachment,
    const PointerInputEvent& event,
    const BehaviorInputEventPhase phase,
    const bool target,
    RetainedNode* const pointer_target,
    InputOperationResult& result
) noexcept : router_(router),
    node_(node),
    attachment_(attachment),
    event_(&event),
    phase_(phase),
    target_(target),
    pointer_target_(pointer_target),
    result_(result) { ++router_.behavior_dispatch_count_; }

BehaviorInputScope::BehaviorInputScope(
    InputRouter& router,
    RetainedNode& node,
    const DescriptionBehavior& attachment,
    const std::string_view key,
    const KeyModifiers modifiers,
    const BehaviorInputEventPhase phase,
    const bool target,
    InputOperationResult& result
) noexcept : router_(router),
    node_(node),
    attachment_(attachment),
    key_(key),
    modifiers_(modifiers),
    phase_(phase),
    target_(target),
    result_(result) { ++router_.behavior_dispatch_count_; }

BehaviorInputScope::BehaviorInputScope(
    InputRouter& router,
    RetainedNode& node,
    const DescriptionBehavior& attachment,
    const BehaviorInputEventPhase phase,
    InputOperationResult& result
) noexcept : router_(router),
    node_(node),
    attachment_(attachment),
    phase_(phase),
    result_(result) { ++router_.behavior_dispatch_count_; }

RetainedNode& BehaviorInputScope::node() noexcept { return node_; }
const RetainedNode& BehaviorInputScope::node() const noexcept { return node_; }
const DescriptionBehavior& BehaviorInputScope::attachment() const noexcept { return attachment_; }
const PointerInputEvent& BehaviorInputScope::event() const noexcept { return *event_; }
const PointerInputEvent* BehaviorInputScope::pointer() const noexcept { return event_; }
std::string_view BehaviorInputScope::key() const noexcept { return key_; }
const KeyModifiers& BehaviorInputScope::modifiers() const noexcept {
    return event_ != nullptr ? event_->modifiers : modifiers_;
}
BehaviorInputEventPhase BehaviorInputScope::phase() const noexcept { return phase_; }
bool BehaviorInputScope::target() const noexcept { return target_; }
InputDispatchContext* BehaviorInputScope::dispatch() const noexcept { return dispatch_; }

WidgetInputScope BehaviorInputScope::widget_scope() const noexcept {
    return WidgetInputScope(
        router_,
        node_,
        result_,
        {},
        event_ != nullptr ? event_->modifiers : KeyModifiers{},
        event_,
        pointer_target_,
        0U,
        std::nullopt,
        {},
        dispatch_
    );
}

const runtime::Value* BehaviorInputScope::option(const std::string_view name) const noexcept {
    return attachment_.options.field(name);
}

const runtime::Value* BehaviorInputScope::retained(const std::string_view name) const noexcept {
    return node_.retained_value(name);
}

const LayoutRecord* BehaviorInputScope::layout() const noexcept {
    return router_.layout_ != nullptr ? router_.layout_->find(node_.identity()) : nullptr;
}

const LayoutRecord* BehaviorInputScope::layout(const RetainedNode& node) const noexcept {
    return router_.layout_ != nullptr ? router_.layout_->find(node.identity()) : nullptr;
}

RetainedNode* BehaviorInputScope::find_key(const std::string_view key) const noexcept {
    return router_.tree_ != nullptr ? router_.tree_->find_key(key) : nullptr;
}

bool BehaviorInputScope::passive_pointer_descendant() const noexcept {
    return pointer_target_ != nullptr && pointer_target_ != &node_ &&
        router_.passive_pointer_path(node_, pointer_target_);
}

bool BehaviorInputScope::press_matches(const bool include_descendants) const noexcept {
    if (event_ == nullptr) return false;
    const auto found = router_.pressed_pointer_targets_.find(event_->pointer_id);
    if (found == router_.pressed_pointer_targets_.end() || router_.tree_ == nullptr) return false;
    const RetainedNode* pressed = router_.tree_->find_identity(found->second.identity);
    return pressed != nullptr && (pressed == &node_ ||
        (include_descendants && InputRouter::descendant_of(*pressed, node_)));
}

bool BehaviorInputScope::press_moved_beyond_slop() const noexcept {
    if (event_ != nullptr) {
        const auto found = router_.pressed_pointer_targets_.find(event_->pointer_id);
        return found != router_.pressed_pointer_targets_.end() && found->second.moved_beyond_slop;
    }
    if (router_.tree_ == nullptr) return false;
    return std::ranges::any_of(router_.pressed_pointer_targets_, [this](const auto& entry) {
        const RetainedNode* pressed = router_.tree_->find_identity(entry.second.identity);
        return entry.second.moved_beyond_slop && pressed != nullptr &&
            (pressed == &node_ || InputRouter::descendant_of(*pressed, node_));
    });
}

bool BehaviorInputScope::long_press_emitted() const noexcept {
    if (event_ == nullptr) return false;
    const auto found = router_.pressed_pointer_targets_.find(event_->pointer_id);
    return found != router_.pressed_pointer_targets_.end() && found->second.long_press_emitted;
}

GestureClaimState BehaviorInputScope::gesture_claim_state() const noexcept {
    if (dispatch_ != nullptr) return dispatch_->gesture_claim_state();
    if (router_.tree_ == nullptr) return GestureClaimState::unclaimed;
    for (const auto& [pointer_id, press] : router_.pressed_pointer_targets_) {
        static_cast<void>(pointer_id);
        const RetainedNode* pressed = router_.tree_->find_identity(press.identity);
        if (pressed != nullptr &&
            (pressed == &node_ || InputRouter::descendant_of(*pressed, node_))) {
            return press.gesture;
        }
    }
    return GestureClaimState::unclaimed;
}

void BehaviorInputScope::consume() noexcept {
    if (dispatch_ != nullptr) dispatch_->consume();
}

void BehaviorInputScope::stop_propagation() noexcept {
    if (dispatch_ != nullptr) dispatch_->stop_propagation();
}

bool BehaviorInputScope::claim_gesture() noexcept {
    return dispatch_ != nullptr && dispatch_->claim_gesture();
}

bool BehaviorInputScope::cancel_gesture() noexcept {
    return dispatch_ != nullptr && dispatch_->cancel_gesture();
}

void BehaviorInputScope::mark_long_press_emitted() {
    for (auto& [pointer_id, press] : router_.pressed_pointer_targets_) {
        if (event_ != nullptr && pointer_id != event_->pointer_id) continue;
        if (router_.tree_ == nullptr) continue;
        const RetainedNode* owner = router_.tree_->find_identity(press.identity);
        if (owner != nullptr && (owner == &node_ || InputRouter::descendant_of(*owner, node_))) {
            press.long_press_emitted = true;
        }
    }
}

void BehaviorInputScope::set_retained(
    std::string name,
    runtime::Value value,
    const DirtyReason reason
) {
    set_retained(node_, std::move(name), std::move(value), reason);
}

void BehaviorInputScope::set_retained(
    RetainedNode& node,
    std::string name,
    runtime::Value value,
    const DirtyReason reason
) {
    if (router_.tree_ == nullptr) return;
    const bool changed = router_.tree_->set_retained_value(
        node.identity(), std::move(name), std::move(value), reason
    );
    if (changed &&
        (reason == DirtyReason::structure || reason == DirtyReason::properties) &&
        router_.description_invalidator_) {
        router_.description_invalidator_();
    }
}

bool BehaviorInputScope::emit(
    const std::string_view event_kind,
    runtime::Value event_value
) {
    JsonValue emitted = object({
        {"action", attachment_.action != nullptr
            ? router_.canonical_action(*attachment_.action, node_)
            : JsonValue{}},
        {"source", router_.source(node_)},
        {"type", JsonValue(std::string(event_kind))},
        {"value", runtime::value_to_json(event_value)},
    });
    router_.emit(std::move(emitted), attachment_.action, node_, std::move(event_value), result_);
    return true;
}

bool BehaviorInputScope::emit_action(
    const std::string_view action_id,
    runtime::Value payload,
    const std::string_view event_kind,
    runtime::Value event_value
) {
    const std::shared_ptr<const runtime::ActionContract> contract =
        router_.application_.bundle()->action_registry().contract(action_id);
    if (contract == nullptr) return false;
    const auto action_value = std::make_shared<const runtime::Action>(
        contract,
        std::move(payload)
    );
    JsonValue emitted = object({
        {"action", router_.canonical_action(*action_value, node_)},
        {"source", router_.source(node_)},
        {"type", JsonValue(std::string(event_kind))},
        {"value", runtime::value_to_json(event_value)},
    });
    router_.emit(std::move(emitted), action_value, node_, std::move(event_value), result_);
    return true;
}

} // namespace strata::ui
