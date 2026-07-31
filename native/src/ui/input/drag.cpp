#include "ui/input.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/value.hpp"
#include "ui/input/detail.hpp"
#include "ui/motion.hpp"

namespace strata::ui {
using namespace input_detail;
namespace {

void append(InputOperationResult& destination, InputOperationResult source) {
    destination.injected_events += source.injected_events;
    destination.processed_events += source.processed_events;
    destination.events.insert(
        destination.events.end(),
        std::make_move_iterator(source.events.begin()),
        std::make_move_iterator(source.events.end())
    );
    destination.action_outcomes.insert(
        destination.action_outcomes.end(),
        std::make_move_iterator(source.action_outcomes.begin()),
        std::make_move_iterator(source.action_outcomes.end())
    );
}

} // namespace

RetainedNode* InputRouter::drag_source_ancestor(RetainedNode* node) noexcept {
    while (node != nullptr) {
        if (behavior(*node, "strata.drag-source") != nullptr) return node;
        node = node->parent();
    }
    return nullptr;
}

bool InputRouter::route_pointer_drag(
    const PointerInputEvent& event,
    InputOperationResult& result
) {
    if (tree_ == nullptr || layout_ == nullptr) return false;
    const auto pressed = pressed_pointer_targets_.find(event.pointer_id);
    auto session = drag_sessions_.find(event.pointer_id);

    const auto source_for_press = [this, pressed]() -> RetainedNode* {
        if (pressed == pressed_pointer_targets_.end() || pressed->second.button != 0) return nullptr;
        RetainedNode* current = tree_->find_identity(pressed->second.identity);
        while (current != nullptr) {
            if (behavior(*current, "strata.drag-source") != nullptr &&
                motion_input_eligible(*current)) {
                return current;
            }
            current = current->parent();
        }
        return nullptr;
    };

    if (session == drag_sessions_.end()) {
        if (event.type != PointerEventType::move) return false;
        RetainedNode* source_node = source_for_press();
        if (source_node == nullptr) return false;
        if (pressed->second.gesture == GestureClaimState::cancelled ||
            (pressed->second.gesture == GestureClaimState::claimed &&
             pressed->second.gesture_owner != source_node->identity())) {
            return false;
        }
        const DescriptionBehavior* attachment = behavior(*source_node, "strata.drag-source");
        const runtime::Value* descendant_type = attachment->options.field("descendantKeyPayloadType");
        const bool descendant_payload = descendant_type != nullptr;
        const runtime::Value* type_value = descendant_payload
            ? descendant_type
            : attachment->options.field("payloadType");
        if (type_value == nullptr || type_value->string() == nullptr ||
            type_value->string()->empty()) {
            return false;
        }
        const runtime::Value* payload = attachment->options.field("payload");
        RetainedNode* preview_node = source_node;
        runtime::Value resolved_payload = payload != nullptr ? *payload : runtime::Value{};
        if (descendant_payload) {
            RetainedNode* current = tree_->find_identity(pressed->second.identity);
            while (current != nullptr) {
                if (current->description().key.has_value()) {
                    preview_node = current;
                    resolved_payload = runtime::Value(runtime::KeyValue{
                        *current->description().key
                    });
                    break;
                }
                if (current == source_node) break;
                current = current->parent();
            }
            if (resolved_payload.key() == nullptr) return false;
        }
        double slop = 4.0;
        if (const runtime::Value* configured = attachment->options.field("slop");
            configured != nullptr && configured->number() != nullptr &&
            std::isfinite(*configured->number()) && *configured->number() >= 0.0) {
            slop = *configured->number();
        }
        DragSession created;
        created.source_identity = source_node->identity();
        created.start = pressed->second.position;
        created.position = pressed->second.position;
        created.payload = std::move(resolved_payload);
        created.payload_type = *type_value->string();
        created.allowed_operations = drag_operations(
            attachment->options.field("allowedOperations"), {"move"}
        );
        created.slop = std::max(input_config_.pointer_drag_slop, slop);
        if (const LayoutRecord* source_layout = layout_->find(preview_node->identity());
            source_layout != nullptr) {
            created.preview_bounds = source_layout->bounds;
        }
        if (drag_preview_resolver_) {
            created.preview_commands = drag_preview_resolver_(*preview_node);
        }
        session = drag_sessions_.emplace(event.pointer_id, std::move(created)).first;
    }

    DragSession& drag = session->second;
    RetainedNode* source_node = tree_->find_identity(drag.source_identity);
    const DescriptionBehavior* source_behavior = source_node != nullptr
        ? behavior(*source_node, "strata.drag-source")
        : nullptr;
    if (source_node == nullptr || source_behavior == nullptr) {
        drag_sessions_.erase(session);
        return false;
    }

    if (event.type == PointerEventType::move) {
        drag.position = event.position;
        if (!drag.active) {
            RetainedNode* hover = hit_test(event.position);
            if (RetainedNode* modal = active_modal(); modal != nullptr &&
                (hover == nullptr || !descendant_of(*hover, *modal))) {
                hover = modal;
            }
            hover_route(hover);
            const double distance = std::hypot(
                event.position.x - drag.start.x,
                event.position.y - drag.start.y
            );
            if (distance < drag.slop) return true;
            drag.active = true;
            if (pressed != pressed_pointer_targets_.end()) {
                pressed->second.gesture = GestureClaimState::claimed;
                pressed->second.gesture_owner = source_node->identity();
            }
            drag.last_auto_scroll_nanos = frame_time_nanos_;
            emit_drag_lifecycle(drag, "start", DragTarget{}, event.position, result);
            // Drop/reorder targets have their own retained presentation channel. Freezing the
            // ordinary hover route during an active drag keeps unrelated widget chrome stable
            // until release while target resolution continues independently below.
            hover_route(source_node);
        }

        const runtime::Value* emit_moves = source_behavior->options.field("emitMoves");
        synchronize_drag_target(
            drag,
            event.position,
            emit_moves != nullptr && emit_moves->boolean() != nullptr && *emit_moves->boolean(),
            result
        );
        if (frame_invalidator_) frame_invalidator_();
        return true;
    }

    if (event.type == PointerEventType::release || event.type == PointerEventType::cancel) {
        if (event.type == PointerEventType::cancel &&
            pressed != pressed_pointer_targets_.end()) {
            pressed->second.gesture = GestureClaimState::cancelled;
        }
        if (event.type == PointerEventType::release) hover_route(source_node);
        const DragTarget target = retained_drag_target(drag);
        const bool was_active = drag.active;
        if (was_active) {
            emit_drag_lifecycle(
                drag,
                event.type == PointerEventType::cancel || target.node == nullptr
                    ? "cancel"
                    : "drop",
                target,
                event.position,
                result
            );
            if (frame_invalidator_) frame_invalidator_();
        }
        drag_sessions_.erase(session);
        return was_active || event.type == PointerEventType::cancel;
    }
    return true;
}

bool InputRouter::cancel_active_drag(InputOperationResult& result) {
    if (tree_ == nullptr || drag_sessions_.empty()) return false;
    std::vector<std::int32_t> pointers;
    pointers.reserve(drag_sessions_.size());
    for (const auto& [pointer_id, session] : drag_sessions_) {
        static_cast<void>(session);
        pointers.push_back(pointer_id);
    }
    bool cancelled = false;
    for (const std::int32_t pointer_id : pointers) {
        const auto found = drag_sessions_.find(pointer_id);
        if (found == drag_sessions_.end()) continue;
        // Keyboard/blur cancellation has no pointer coordinate. The Kotlin core reports the
        // gesture origin in that case, while pointer cancellation is routed through
        // route_pointer_drag with its actual position.
        const Point position = found->second.start;
        cancelled = route_pointer_drag(PointerInputEvent{
            position, PointerEventType::cancel, pointer_id, 0,
        }, result) || cancelled;
    }
    return cancelled;
}

std::optional<DragPreviewPresentation> InputRouter::drag_preview(
    const std::uint64_t identity
) const noexcept {
    for (const auto& [pointer_id, session] : drag_sessions_) {
        static_cast<void>(pointer_id);
        if (!session.active || session.source_identity != identity) continue;
        return DragPreviewPresentation{
            session.position,
            session.preview_bounds,
            &session.preview_commands,
            session.target_identity.has_value(),
        };
    }
    return std::nullopt;
}

std::optional<DropTargetPresentation> InputRouter::drop_target(
    const std::uint64_t identity
) const noexcept {
    for (const auto& [pointer_id, session] : drag_sessions_) {
        static_cast<void>(pointer_id);
        if (!session.active || session.target_identity != identity) continue;
        return DropTargetPresentation{session.position, session.placement};
    }
    return std::nullopt;
}

InputOperationResult InputRouter::drag(
    const std::string_view from_key,
    const std::string_view to_key
) {
    if (tree_ == nullptr || layout_ == nullptr) {
        throw std::logic_error("input requires a completed surface frame");
    }
    RetainedNode* source_node = tree_->find_key(from_key);
    RetainedNode* destination_node = tree_->find_key(to_key);
    const LayoutRecord* source_layout = source_node != nullptr
        ? layout_->find(source_node->identity())
        : nullptr;
    const LayoutRecord* destination_layout = destination_node != nullptr
        ? layout_->find(destination_node->identity())
        : nullptr;
    if (source_node == nullptr || destination_node == nullptr ||
        source_layout == nullptr || destination_layout == nullptr) {
        throw std::invalid_argument("drag source and destination keys must be retained and arranged");
    }
    if (drag_source_ancestor(source_node) == nullptr) {
        throw std::invalid_argument("drag source has no enabled strata.drag-source behavior");
    }
    if (pressed_pointer_targets_.contains(0)) {
        throw std::logic_error("deterministic drag cannot reuse an active pointer");
    }
    const Point start{
        source_layout->bounds.x + source_layout->bounds.width * 0.5,
        source_layout->bounds.y + source_layout->bounds.height * 0.5,
    };
    const Point end{
        destination_layout->bounds.x + destination_layout->bounds.width * 0.5,
        destination_layout->bounds.y + destination_layout->bounds.height * 0.5,
    };
    InputOperationResult result;
    append(result, pointer(PointerInputEvent{start, PointerEventType::move, 0, 0}));
    append(result, pointer(PointerInputEvent{start, PointerEventType::press, 0, 0}));
    append(result, pointer(PointerInputEvent{end, PointerEventType::move, 0, 0}));
    append(result, pointer(PointerInputEvent{end, PointerEventType::release, 0, 0}));
    return result;
}

} // namespace strata::ui
