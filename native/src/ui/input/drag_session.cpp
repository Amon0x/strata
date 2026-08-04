#include "ui/input.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/value.hpp"
#include "ui/input/detail.hpp"
#include "ui/motion.hpp"
#include "ui/reorder.hpp"

namespace strata::ui {
using namespace input_detail;
namespace {

[[nodiscard]] bool contains(const Rect& bounds, const Point point) noexcept {
    return point.x >= bounds.x && point.y >= bounds.y &&
           point.x <= bounds.right() && point.y <= bounds.bottom();
}

[[nodiscard]] double edge_velocity(
    const double position,
    const double start,
    const double end,
    const InputProcessingConfig& config
) noexcept {
    double strength = 0.0;
    if (position < start + config.drag_auto_scroll_edge) {
        strength = -std::clamp(
            (start + config.drag_auto_scroll_edge - position) /
                config.drag_auto_scroll_edge,
            0.0,
            1.0
        );
    } else if (position > end - config.drag_auto_scroll_edge) {
        strength = std::clamp(
            (position - (end - config.drag_auto_scroll_edge)) /
                config.drag_auto_scroll_edge,
            0.0,
            1.0
        );
    }
    return strength * config.drag_auto_scroll_speed;
}

} // namespace

std::vector<std::string> InputRouter::drag_operations(
    const runtime::Value* value,
    std::vector<std::string> fallback
) {
    if (value == nullptr || value->list() == nullptr) return fallback;
    std::vector<std::string> parsed;
    for (const runtime::Value& entry : value->list()->values) {
        if (entry.string() == nullptr) continue;
        std::string operation = lower_ascii(*entry.string());
        if (operation != "move" && operation != "copy" && operation != "link") continue;
        if (!std::ranges::contains(parsed, operation)) parsed.push_back(std::move(operation));
    }
    return parsed.empty() ? std::move(fallback) : std::move(parsed);
}

InputRouter::DragTarget InputRouter::resolve_drag_target(
    const DragSession& session,
    const Point position
) const {
    if (tree_ == nullptr || layout_ == nullptr) return {};
    RetainedNode* node = widget_native_input_owner(hit_test(position));
    RetainedNode* modal = active_modal();
    while (node != nullptr) {
        const LayoutRecord* record = layout_->find(node->identity());
        if (record != nullptr && contains(record->bounds, position) &&
            motion_input_eligible(*node) &&
            (modal == nullptr || descendant_of(*node, *modal))) {
            if (const DescriptionBehavior* attachment = behavior(*node, "strata.drop-target");
                attachment != nullptr) {
                const runtime::Value* accepted = attachment->options.field("acceptedTypes");
                const bool accepts_type = accepted != nullptr && accepted->list() != nullptr &&
                    std::ranges::any_of(
                        accepted->list()->values,
                        [&session](const runtime::Value& value) {
                            return value.string() != nullptr &&
                                *value.string() == session.payload_type;
                        }
                    );
                if (accepts_type) {
                    const std::vector<std::string> target_operations = drag_operations(
                        attachment->options.field("allowedOperations"),
                        {"move", "copy", "link"}
                    );
                    const auto selected = std::ranges::find_first_of(
                        session.allowed_operations,
                        target_operations
                    );
                    if (selected != session.allowed_operations.end()) {
                        std::string placement = "on";
                        const runtime::Value* placement_mode = attachment->options.field("placementMode");
                        if (placement_mode != nullptr && placement_mode->string() != nullptr &&
                            (*placement_mode->string() == "TREE" ||
                             *placement_mode->string() == "tree")) {
                            const double fraction = record->bounds.height > 0.0
                                ? (position.y - record->bounds.y) / record->bounds.height
                                : 0.5;
                            placement = fraction < 0.25 ? "before"
                                : fraction > 0.75 ? "after"
                                : "on";
                        }
                        const runtime::Value* axis = attachment->options.field("insertionAxis");
                        if (placement == "on" && axis != nullptr && axis->string() != nullptr) {
                            const std::string normalized = lower_ascii(*axis->string());
                            if (normalized == "horizontal") {
                                placement = position.x < record->bounds.x + record->bounds.width * 0.5
                                    ? "before"
                                    : "after";
                            } else if (normalized == "vertical") {
                                placement = position.y < record->bounds.y + record->bounds.height * 0.5
                                    ? "before"
                                    : "after";
                            }
                        }
                        return DragTarget{node, attachment, *selected, std::move(placement)};
                    }
                }
            }
            if (const DescriptionBehavior* attachment = behavior(*node, "strata.reorder-target");
                attachment != nullptr) {
                const runtime::Value* accepted = attachment->options.field("payloadType");
                if (accepted != nullptr && accepted->string() != nullptr &&
                    *accepted->string() == session.payload_type &&
                    std::ranges::contains(session.allowed_operations, std::string("move"))) {
                    return DragTarget{node, attachment, "move", "on"};
                }
            }
        }
        node = node->parent();
    }
    return {};
}

InputRouter::DragTarget InputRouter::retained_drag_target(
    const DragSession& session
) const {
    if (tree_ == nullptr || !session.target_identity.has_value() ||
        !session.operation.has_value()) {
        return {};
    }
    RetainedNode* target = tree_->find_identity(*session.target_identity);
    if (target == nullptr) return {};
    const DescriptionBehavior* attachment = behavior(*target, "strata.drop-target");
    if (attachment == nullptr) attachment = behavior(*target, "strata.reorder-target");
    return attachment != nullptr
        ? DragTarget{target, attachment, *session.operation, session.placement}
        : DragTarget{};
}

void InputRouter::emit_drag_lifecycle(
    DragSession& session,
    const std::string_view phase,
    const DragTarget& target,
    const Point position,
    InputOperationResult& result
) {
    if (tree_ == nullptr) return;
    RetainedNode* source_node = tree_->find_identity(session.source_identity);
    const DescriptionBehavior* source_behavior = source_node != nullptr
        ? behavior(*source_node, "strata.drag-source")
        : nullptr;
    if (source_node == nullptr || source_behavior == nullptr) return;

    std::vector<JsonValue> encoded_operations;
    encoded_operations.reserve(session.allowed_operations.size());
    for (const std::string& operation : session.allowed_operations) {
        encoded_operations.emplace_back(operation);
    }
    const JsonValue allowed = array(std::move(encoded_operations));
    const JsonValue payload = object({
        {"type", JsonValue(session.payload_type)},
        {"value", runtime::value_to_json(session.payload)},
    });
    const JsonValue drag_source = source(*source_node);
    const bool has_target = target.node != nullptr && target.behavior != nullptr;
    const bool reorder = has_target && target.behavior->id == "strata.reorder-target";
    std::optional<ReorderInsertion> insertion;
    if (reorder && layout_ != nullptr) {
        const runtime::Value* axis = target.behavior->options.field("axis");
        const bool horizontal = axis != nullptr && axis->string() != nullptr &&
            (*axis->string() == "HORIZONTAL" || *axis->string() == "horizontal");
        insertion = resolve_reorder_insertion(*target.node, *layout_, position, horizontal);
    }
    const auto event = [this, &allowed, &drag_source, &payload, phase, position,
                         &target, &insertion, has_target](
                           const RetainedNode& recipient,
                           const DescriptionBehavior& attachment,
                           const std::string_view recipient_name
                       ) {
        return object({
            {"action", attachment.action != nullptr
                ? canonical_action(*attachment.action, recipient)
                : JsonValue{}},
            {"allowedOperations", allowed},
            {"afterKey", insertion.has_value() && insertion->after_key.has_value()
                ? JsonValue(*insertion->after_key)
                : JsonValue{}},
            {"beforeKey", insertion.has_value() && insertion->before_key.has_value()
                ? JsonValue(*insertion->before_key)
                : JsonValue{}},
            {"dragSource", drag_source},
            {"operation", has_target ? JsonValue(target.operation) : JsonValue{}},
            {"insertionIndex", insertion.has_value()
                ? JsonValue(static_cast<std::int64_t>(insertion->index))
                : JsonValue{}},
            {"payload", payload},
            {"phase", JsonValue(std::string(phase))},
            {"placement", JsonValue(has_target ? target.placement : "on")},
            {"position", object({
                {"x", JsonValue(position.x)},
                {"y", JsonValue(position.y)},
            })},
            {"recipient", JsonValue(std::string(recipient_name))},
            {"source", source(recipient)},
            {"targetKey", has_target && target.node->description().key.has_value()
                ? JsonValue(*target.node->description().key)
                : JsonValue{}},
            {"type", JsonValue("drag")},
        });
    };
    std::vector<runtime::Value> value_operations;
    value_operations.reserve(session.allowed_operations.size());
    for (const std::string& operation : session.allowed_operations) {
        value_operations.emplace_back(operation);
    }
    const runtime::Value event_value(std::vector<std::pair<std::string, runtime::Value>>{
        {"afterKey", insertion.has_value() && insertion->after_key.has_value()
            ? runtime::Value(runtime::KeyValue{*insertion->after_key})
            : runtime::Value{}},
        {"allowedOperations", runtime::Value(std::move(value_operations))},
        {"beforeKey", insertion.has_value() && insertion->before_key.has_value()
            ? runtime::Value(runtime::KeyValue{*insertion->before_key})
            : runtime::Value{}},
        {"insertionIndex", insertion.has_value()
            ? runtime::Value(static_cast<double>(insertion->index))
            : runtime::Value{}},
        {"operation", has_target ? runtime::Value(target.operation) : runtime::Value{}},
        {"payload", runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
            {"type", runtime::Value(session.payload_type)},
            {"value", session.payload},
        })},
        {"phase", runtime::Value(std::string(phase))},
        {"placement", runtime::Value(has_target ? target.placement : "on")},
        {"position", runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
            {"x", runtime::Value(position.x)},
            {"y", runtime::Value(position.y)},
        })},
        {"targetKey", has_target && target.node->description().key.has_value()
            ? runtime::Value(runtime::KeyValue{*target.node->description().key})
            : runtime::Value{}},
    });
    emit(
        event(*source_node, *source_behavior, "source"),
        source_behavior->action,
        *source_node,
        event_value,
        result
    );
    if (has_target) {
        emit(
            event(*target.node, *target.behavior, "target"),
            target.behavior->action,
            *target.node,
            event_value,
            result
        );
    }
}

void InputRouter::synchronize_drag_target(
    DragSession& session,
    const Point position,
    const bool emit_move,
    InputOperationResult& result
) {
    if (!session.active || tree_ == nullptr) return;
    const DragTarget next = resolve_drag_target(session, position);
    const DragTarget previous = retained_drag_target(session);
    const bool identity_changed = next.node != previous.node ||
        next.operation != previous.operation;
    if (identity_changed && previous.node != nullptr) {
        emit_drag_lifecycle(session, "leave", previous, position, result);
    }
    if (identity_changed && next.node != nullptr) {
        emit_drag_lifecycle(session, "enter", next, position, result);
    }
    session.target_identity = next.node != nullptr
        ? std::optional(next.node->identity())
        : std::nullopt;
    session.operation = next.node != nullptr
        ? std::optional(next.operation)
        : std::nullopt;
    session.placement = next.node != nullptr ? next.placement : "on";
    if (emit_move) {
        emit_drag_lifecycle(session, "move", next, position, result);
    }
}

bool InputRouter::auto_scroll_drag(
    DragSession& session,
    InputOperationResult& result
) {
    if (!session.active || tree_ == nullptr || layout_ == nullptr) return false;
    const std::optional<std::int64_t> previous = session.last_auto_scroll_nanos;
    session.last_auto_scroll_nanos = frame_time_nanos_;
    if (!previous.has_value()) return false;
    const std::int64_t elapsed_nanos = std::clamp(
        frame_time_nanos_ - *previous,
        std::int64_t{0},
        input_config_.drag_auto_scroll_max_frame_nanos
    );
    if (elapsed_nanos <= 0) return false;
    const double elapsed_seconds = static_cast<double>(elapsed_nanos) / 1'000'000'000.0;

    for (RetainedNode* current = widget_native_input_owner(hit_test(session.position));
         current != nullptr;
         current = current->parent()) {
        const LayoutRecord* record = layout_->find(current->identity());
        const std::optional<Point> limits = scroll_limits(*current);
        if (record == nullptr || !record->viewport.has_value() || !limits.has_value() ||
            !contains(*record->viewport, session.position)) {
            continue;
        }
        const LayoutStyle style = layout_style(current->description());
        Point next = record->scroll_offset;
        if (style.scroll_horizontal) {
            next.x += edge_velocity(
                session.position.x,
                record->viewport->x,
                record->viewport->right(),
                input_config_
            ) * elapsed_seconds;
        }
        if (style.scroll_vertical) {
            next.y += edge_velocity(
                session.position.y,
                record->viewport->y,
                record->viewport->bottom(),
                input_config_
            ) * elapsed_seconds;
        }
        next.x = std::clamp(next.x, 0.0, limits->x);
        next.y = std::clamp(next.y, 0.0, limits->y);
        if (next.x == record->scroll_offset.x && next.y == record->scroll_offset.y) continue;
        if (!set_scroll_offset(*current, next, result)) continue;
        if (frame_invalidator_) frame_invalidator_();
        return true;
    }
    return false;
}

InputOperationResult InputRouter::advance_frame() {
    InputOperationResult result;
    for (auto& [pointer_id, session] : drag_sessions_) {
        static_cast<void>(pointer_id);
        static_cast<void>(auto_scroll_drag(session, result));
    }
    route_active_lifecycle_hooks(false, result);
    synchronize_authored_presentations();
    return result;
}

} // namespace strata::ui
