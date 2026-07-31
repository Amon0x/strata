#include "ui/widget/input.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "ui/command.hpp"
#include "ui/input/detail.hpp"

namespace strata::ui {
namespace {

using data::JsonValue;

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> entries) {
    return JsonValue(JsonValue::Object(entries));
}

} // namespace

WidgetInputScope::WidgetInputScope(
    InputRouter& router,
    RetainedNode& node,
    InputOperationResult& result,
    const std::string_view key,
    const KeyModifiers modifiers,
    const PointerInputEvent* const pointer,
    RetainedNode* const pointer_target,
    const std::size_t click_count,
    std::optional<WidgetSubtarget> subtarget,
    const std::string_view input_text,
    InputDispatchContext* const dispatch
) noexcept : router_(router),
    node_(node),
    result_(result),
    key_(key),
    modifiers_(modifiers),
    pointer_(pointer),
    pointer_target_(pointer_target),
    click_count_(click_count),
    subtarget_(std::move(subtarget)),
    input_text_(input_text),
    dispatch_(dispatch) {}

RetainedNode& WidgetInputScope::node() noexcept { return node_; }
const RetainedNode& WidgetInputScope::node() const noexcept { return node_; }
std::string_view WidgetInputScope::key() const noexcept { return key_; }
std::string_view WidgetInputScope::input_text() const noexcept { return input_text_; }
const KeyModifiers& WidgetInputScope::modifiers() const noexcept { return modifiers_; }
const PointerInputEvent* WidgetInputScope::pointer() const noexcept { return pointer_; }
RetainedNode* WidgetInputScope::pointer_target() const noexcept { return pointer_target_; }
std::size_t WidgetInputScope::click_count() const noexcept { return click_count_; }
const WidgetSubtarget* WidgetInputScope::subtarget() const noexcept {
    return subtarget_.has_value() ? &*subtarget_ : nullptr;
}
InputDispatchContext* WidgetInputScope::dispatch() const noexcept { return dispatch_; }
InputEventPhase WidgetInputScope::phase() const noexcept {
    return dispatch_ != nullptr ? dispatch_->phase() : InputEventPhase::target;
}
InputEventKind WidgetInputScope::kind() const noexcept {
    if (dispatch_ != nullptr) return dispatch_->kind();
    if (pointer_ == nullptr) return key_.empty() ? InputEventKind::advance : InputEventKind::key;
    switch (pointer_->type) {
    case PointerEventType::move: return InputEventKind::pointer_move;
    case PointerEventType::press: return InputEventKind::pointer_press;
    case PointerEventType::release: return InputEventKind::pointer_release;
    case PointerEventType::cancel: return InputEventKind::pointer_cancel;
    }
    return InputEventKind::pointer_cancel;
}
bool WidgetInputScope::press_moved_beyond_slop() const noexcept {
    return dispatch_ != nullptr && dispatch_->press_moved_beyond_slop();
}
GestureClaimState WidgetInputScope::gesture_claim_state() const noexcept {
    return dispatch_ != nullptr
        ? dispatch_->gesture_claim_state()
        : GestureClaimState::unclaimed;
}
void WidgetInputScope::consume() noexcept {
    if (dispatch_ != nullptr) dispatch_->consume();
}
void WidgetInputScope::stop_propagation() noexcept {
    if (dispatch_ != nullptr) dispatch_->stop_propagation();
}
bool WidgetInputScope::claim_gesture() noexcept {
    return dispatch_ != nullptr && dispatch_->claim_gesture();
}
bool WidgetInputScope::cancel_gesture() noexcept {
    return dispatch_ != nullptr && dispatch_->cancel_gesture();
}
std::vector<WidgetSubtarget> WidgetInputScope::subtargets() const {
    return router_.subtargets(node_.identity());
}
std::optional<std::size_t> WidgetInputScope::text_offset(
    const std::string_view text
) const {
    return pointer_ != nullptr
        ? router_.resolve_text_offset(node_, text, pointer_->position)
        : std::nullopt;
}
std::int64_t WidgetInputScope::frame_time_nanos() const noexcept {
    return router_.frame_time_nanos_;
}

std::optional<WidgetDragInteraction> WidgetInputScope::active_drag() const {
    if (router_.tree_ == nullptr) return std::nullopt;
    for (const auto& [pointer_id, session] : router_.drag_sessions_) {
        static_cast<void>(pointer_id);
        if (!session.active || !session.target_identity.has_value() ||
            !session.operation.has_value()) {
            continue;
        }
        const RetainedNode* target = router_.tree_->find_identity(*session.target_identity);
        if (target == nullptr || !InputRouter::descendant_of(*target, node_)) continue;
        const RetainedNode* source = router_.tree_->find_identity(session.source_identity);
        return WidgetDragInteraction{
            source != nullptr ? source->description().key : std::nullopt,
            target->description().key,
            session.position,
            *session.operation,
            session.placement,
        };
    }
    return std::nullopt;
}

const LayoutRecord* WidgetInputScope::layout() const noexcept {
    return router_.layout_ != nullptr ? router_.layout_->find(node_.identity()) : nullptr;
}

const LayoutRecord* WidgetInputScope::layout(const RetainedNode& node) const noexcept {
    return router_.layout_ != nullptr ? router_.layout_->find(node.identity()) : nullptr;
}

const LayoutResult* WidgetInputScope::layout_result() const noexcept {
    return router_.layout_;
}

const CommandIndex* WidgetInputScope::command_index() const noexcept {
    return router_.commands_;
}

NotificationService& WidgetInputScope::notifications() noexcept {
    return router_.notifications_;
}

void WidgetInputScope::synchronize_modal_focus() {
    router_.sync_modal_focus(result_);
}

const runtime::Value* WidgetInputScope::property(const std::string_view name) const noexcept {
    const auto found = node_.description().properties.find(name);
    return found != node_.description().properties.end() ? found->second.data_value() : nullptr;
}

const runtime::Value* WidgetInputScope::retained(const std::string_view name) const noexcept {
    return node_.retained_value(name);
}

bool WidgetInputScope::effective_boolean(
    const std::string_view controlled,
    const std::string_view retained_name,
    const std::string_view initial,
    const bool fallback
) const noexcept {
    const runtime::Value* value = property(controlled);
    if (value == nullptr || value->boolean() == nullptr) value = retained(retained_name);
    if (value == nullptr || value->boolean() == nullptr) value = property(initial);
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

double WidgetInputScope::effective_number(
    const std::string_view controlled,
    const std::string_view retained_name,
    const std::string_view initial,
    const double fallback
) const noexcept {
    const runtime::Value* value = property(controlled);
    if (value == nullptr || value->number() == nullptr) value = retained(retained_name);
    if (value == nullptr || value->number() == nullptr) value = property(initial);
    return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
               ? *value->number()
               : fallback;
}

double WidgetInputScope::number(const std::string_view name, const double fallback) const noexcept {
    const runtime::Value* value = property(name);
    return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
               ? *value->number()
               : fallback;
}

std::int64_t WidgetInputScope::duration_nanos(
    const std::string_view name,
    const std::int64_t fallback
) const noexcept {
    const runtime::Value* value = property(name);
    return value != nullptr && value->duration() != nullptr
        ? value->duration()->nanoseconds
        : fallback;
}

std::string WidgetInputScope::string(const std::string_view name, std::string fallback) const {
    const runtime::Value* value = property(name);
    if (value != nullptr && value->string() != nullptr) return *value->string();
    if (value != nullptr && value->key() != nullptr) return value->key()->value;
    return fallback;
}

bool WidgetInputScope::reveal_vertical(
    const double item_start,
    const double item_end,
    const double sticky_leading_inset
) {
    const LayoutRecord* record = layout();
    if (record == nullptr || !record->viewport.has_value() ||
        !std::isfinite(item_start) || !std::isfinite(item_end) || item_end < item_start) {
        return false;
    }
    const Rect& viewport = *record->viewport;
    const double inset = std::clamp(sticky_leading_inset, 0.0, viewport.height);
    Point next = record->scroll_offset;
    const double unobscured_start = next.y + inset;
    if (item_start < unobscured_start) {
        next.y = std::max(0.0, item_start - inset);
    } else if (item_end > next.y + viewport.height) {
        next.y = std::max(0.0, item_end - viewport.height);
    }
    return router_.set_scroll_offset(node_, next, result_);
}

bool WidgetInputScope::scroll_by(const double delta_x, const double delta_y) {
    const LayoutRecord* record = layout();
    if (record == nullptr || !std::isfinite(delta_x) || !std::isfinite(delta_y)) return false;
    return router_.set_scroll_offset(
        node_,
        Point{record->scroll_offset.x + delta_x, record->scroll_offset.y + delta_y},
        result_
    );
}

const std::string* WidgetInputScope::editor_text() const noexcept {
    return router_.edited_text(node_.identity());
}

bool WidgetInputScope::insert_editor_text(const std::string_view value) {
    return router_.insert_editor_text(node_, value, result_);
}

bool WidgetInputScope::clear_editor_text() {
    return router_.clear_editor_text(node_, result_);
}

void WidgetInputScope::synchronize_editor_text(
    const std::string_view value,
    const bool move_caret_to_end
) {
    router_.synchronize_editor_text(node_, value, move_caret_to_end);
}

void WidgetInputScope::set_retained(
    std::string name,
    runtime::Value value,
    const DirtyReason reason
) {
    if (router_.tree_ == nullptr) return;
    const bool changed = router_.tree_->set_retained_value(
        node_.identity(), std::move(name), std::move(value), reason
    );
    if (changed &&
        (reason == DirtyReason::structure || reason == DirtyReason::properties) &&
        router_.description_invalidator_) {
        router_.description_invalidator_();
    }
}

void WidgetInputScope::set_presentation(std::string name, runtime::Value value) {
    if (router_.tree_ == nullptr) return;
    const bool changed = router_.tree_->set_presentation_value(
        node_.identity(), std::move(name), std::move(value)
    );
    if (changed && router_.frame_invalidator_) router_.frame_invalidator_();
}

void WidgetInputScope::set_event_count(const std::size_t count) noexcept {
    result_.injected_events = count;
    result_.processed_events = count;
}

std::shared_ptr<const runtime::ActionValue> WidgetInputScope::action(
    const std::string_view property_name
) const {
    return router_.activation_action(node_, property_name);
}

void WidgetInputScope::activated(const std::string_view action_property) {
    if (router_.commands_ != nullptr) {
        const std::optional<CommandActivationBinding> binding =
            router_.commands_->activation_binding(node_);
        if (binding.has_value()) {
            RetainedNode* modal = router_.active_modal();
            static_cast<void>(router_.invoke_command(
                binding->id,
                node_,
                modal != nullptr && router_.descendant_of(node_, *modal),
                result_
            ));
            return;
        }
    }
    const std::shared_ptr<const runtime::ActionValue> binding = action(action_property);
    JsonValue event = object({
        {"action", binding != nullptr ? router_.canonical_action(*binding, node_) : JsonValue{}},
        {"source", router_.source(node_)},
        {"type", JsonValue("activated")},
    });
    router_.emit(std::move(event), binding, node_, runtime::Value{}, result_);
}

void WidgetInputScope::boolean_changed(
    const std::string_view action_property,
    const bool value
) {
    const std::shared_ptr<const runtime::ActionValue> binding = action(action_property);
    JsonValue event = object({
        {"action", binding != nullptr ? router_.canonical_action(*binding, node_) : JsonValue{}},
        {"source", router_.source(node_)},
        {"type", JsonValue("boolean-changed")},
        {"value", JsonValue(value)},
    });
    router_.emit(std::move(event), binding, node_, runtime::Value(value), result_);
}

void WidgetInputScope::number_changed(
    const std::string_view action_property,
    const double value
) {
    const std::shared_ptr<const runtime::ActionValue> binding = action(action_property);
    JsonValue event = object({
        {"action", binding != nullptr ? router_.canonical_action(*binding, node_) : JsonValue{}},
        {"source", router_.source(node_)},
        {"type", JsonValue("number-changed")},
        {"value", JsonValue(value)},
    });
    router_.emit(std::move(event), binding, node_, runtime::Value(value), result_);
}

void WidgetInputScope::value_changed(
    const std::string_view action_property,
    const std::string_view event_kind,
    runtime::Value event_value
) {
    const std::shared_ptr<const runtime::ActionValue> binding = action(action_property);
    JsonValue event = object({
        {"action", binding != nullptr ? router_.canonical_action(*binding, node_) : JsonValue{}},
        {"source", router_.source(node_)},
        {"type", JsonValue(std::string(event_kind))},
        {"value", runtime::value_to_json(event_value)},
    });
    router_.emit(std::move(event), binding, node_, std::move(event_value), result_);
}

void WidgetInputScope::dispatch_action(
    std::shared_ptr<const runtime::ActionValue> binding,
    const std::string_view event_kind,
    runtime::Value event_value
) {
    JsonValue event = object({
        {"action", binding != nullptr ? router_.canonical_action(*binding, node_) : JsonValue{}},
        {"source", router_.source(node_)},
        {"type", JsonValue(std::string(event_kind))},
        {"value", runtime::value_to_json(event_value)},
    });
    router_.emit(std::move(event), binding, node_, std::move(event_value), result_);
}

bool WidgetInputScope::emit_action(
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
    JsonValue event = object({
        {"action", router_.canonical_action(*action_value, node_)},
        {"source", router_.source(node_)},
        {"type", JsonValue(std::string(event_kind))},
        {"value", runtime::value_to_json(event_value)},
    });
    router_.emit(std::move(event), action_value, node_, std::move(event_value), result_);
    return true;
}

bool WidgetInputScope::invoke_command(const std::string_view id) {
    if (router_.commands_ == nullptr || router_.commands_->find(id) == nullptr) return false;
    RetainedNode* modal = router_.active_modal();
    const InputRouter::CommandInvocation invocation = router_.invoke_command(
        id,
        node_,
        modal != nullptr && InputRouter::descendant_of(node_, *modal),
        result_
    );
    return invocation.executed;
}

void WidgetInputScope::emit_event(
    const std::string_view event_kind,
    runtime::Value event_value
) {
    JsonValue event = object({
        {"action", JsonValue{}},
        {"source", router_.source(node_)},
        {"type", JsonValue(std::string(event_kind))},
        {"value", runtime::value_to_json(event_value)},
    });
    result_.action_outcomes.push_back(input_detail::no_action_outcome(event));
    result_.events.push_back(std::move(event));
}

} // namespace strata::ui
