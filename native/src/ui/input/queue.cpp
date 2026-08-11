#include "ui/input.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <type_traits>
#include <stdexcept>
#include <utility>

#include "ui/motion.hpp"
#include "ui/presentation_geometry.hpp"

namespace strata::ui {
namespace {

void append(InputOperationResult& destination, InputOperationResult source) {
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

void InputRouter::report_input_queue_overflow() {
    if (input_queue_overflow_reported_) return;
    input_queue_overflow_reported_ = true;
    pending_diagnostics_.push_back(runtime::RuntimeDiagnostic{
        "STRATA.UI.INPUT_QUEUE_OVERFLOW",
        "The bounded surface input queue rejected events after reaching " +
            std::to_string(input_config_.max_queued_events) + " pending entries.",
        {},
        "platform adapter backpressure or input drained every frame",
        runtime::DiagnosticSeverity::warning,
        std::nullopt,
    });
}

bool InputRouter::enqueue_input(SurfaceInputEvent input) {
    if (auto* pointer = std::get_if<PointerInputEvent>(&input);
        pointer != nullptr && pointer->type == PointerEventType::move) {
        if (!pointer->has_coalesced_origin) {
            if (const auto pressed = pressed_pointer_targets_.find(pointer->pointer_id);
                pressed != pressed_pointer_targets_.end()) {
                pointer->coalesced_origin = pressed->second.position;
                pointer->has_coalesced_origin = true;
            } else {
                const auto queued_press = std::find_if(
                    queued_inputs_.rbegin(), queued_inputs_.rend(),
                    [pointer](const SurfaceInputEvent& queued) {
                        const auto* candidate = std::get_if<PointerInputEvent>(&queued);
                        return candidate != nullptr &&
                            candidate->pointer_id == pointer->pointer_id &&
                            (candidate->type == PointerEventType::press ||
                             candidate->type == PointerEventType::release ||
                             candidate->type == PointerEventType::cancel);
                    }
                );
                if (queued_press != queued_inputs_.rend()) {
                    const auto& candidate = std::get<PointerInputEvent>(*queued_press);
                    if (candidate.type == PointerEventType::press) {
                        pointer->coalesced_origin = candidate.position;
                        pointer->has_coalesced_origin = true;
                    }
                }
            }
        }
        if (pointer->has_coalesced_origin) {
            const double x = pointer->position.x - pointer->coalesced_origin.x;
            const double y = pointer->position.y - pointer->coalesced_origin.y;
            const double slop = input_config_.pointer_drag_slop;
            pointer->coalesced_moved_beyond_slop =
                pointer->coalesced_moved_beyond_slop || x * x + y * y > slop * slop;
        }
        if (!queued_inputs_.empty()) {
        if (auto* previous = std::get_if<PointerInputEvent>(&queued_inputs_.back());
            previous != nullptr && previous->type == PointerEventType::move &&
            previous->pointer_id == pointer->pointer_id &&
            previous->modifiers.shift == pointer->modifiers.shift &&
            previous->modifiers.control == pointer->modifiers.control &&
            previous->modifiers.alt == pointer->modifiers.alt &&
            previous->modifiers.super_key == pointer->modifiers.super_key) {
            const Point accumulated{
                previous->delta.x + pointer->delta.x,
                previous->delta.y + pointer->delta.y,
            };
            const Point origin = previous->coalesced_origin;
            const bool has_origin = previous->has_coalesced_origin;
            const bool crossed_slop = previous->coalesced_moved_beyond_slop ||
                pointer->coalesced_moved_beyond_slop;
            *previous = *pointer;
            previous->delta = accumulated;
            previous->coalesced_origin = origin;
            previous->has_coalesced_origin = has_origin;
            previous->coalesced_moved_beyond_slop = crossed_slop;
            ++coalesced_move_count_;
            return true;
        }
        }
    }
    if (const auto* scroll = std::get_if<ScrollInputEvent>(&input);
        scroll != nullptr && !queued_inputs_.empty()) {
        if (auto* previous = std::get_if<ScrollInputEvent>(&queued_inputs_.back());
            previous != nullptr &&
            previous->position.x == scroll->position.x &&
            previous->position.y == scroll->position.y &&
            previous->modifiers.shift == scroll->modifiers.shift &&
            previous->modifiers.control == scroll->modifiers.control &&
            previous->modifiers.alt == scroll->modifiers.alt &&
            previous->modifiers.super_key == scroll->modifiers.super_key) {
            previous->position = scroll->position;
            previous->delta_x += scroll->delta_x;
            previous->delta_y += scroll->delta_y;
            previous->timestamp_nanos = scroll->timestamp_nanos;
            return true;
        }
    }
    if (queued_inputs_.size() >= input_config_.max_queued_events) {
        const bool incoming_move = std::holds_alternative<PointerInputEvent>(input) &&
            std::get<PointerInputEvent>(input).type == PointerEventType::move;
        if (!incoming_move) {
            const auto disposable = std::find_if(
                queued_inputs_.rbegin(),
                queued_inputs_.rend(),
                [](const SurfaceInputEvent& queued) {
                    const auto* pointer = std::get_if<PointerInputEvent>(&queued);
                    return pointer != nullptr && pointer->type == PointerEventType::move;
                }
            );
            if (disposable != queued_inputs_.rend()) {
                queued_inputs_.erase(std::prev(disposable.base()));
                queued_inputs_.push_back(std::move(input));
                return true;
            }
        }
        report_input_queue_overflow();
        return false;
    }
    queued_inputs_.push_back(std::move(input));
    return true;
}

bool InputRouter::enqueue_inputs(std::vector<SurfaceInputEvent> inputs) {
    const std::deque<SurfaceInputEvent> previous = queued_inputs_;
    const std::size_t previous_coalesced_moves = coalesced_move_count_;
    for (SurfaceInputEvent& input : inputs) {
        if (enqueue_input(std::move(input))) continue;
        queued_inputs_ = previous;
        coalesced_move_count_ = previous_coalesced_moves;
        return false;
    }
    return true;
}

Point InputRouter::injection_point(const std::string_view key) const {
    if (tree_ == nullptr || layout_ == nullptr) {
        throw std::logic_error("keyed input injection requires a completed surface frame");
    }
    const RetainedNode* node = tree_->find_key(key);
    const LayoutRecord* record = node != nullptr ? layout_->find(node->identity()) : nullptr;
    if (node == nullptr || record == nullptr) {
        throw std::invalid_argument("input target key is not retained and arranged");
    }
    Rect hit = hit_bounds_resolver_ ? hit_bounds_resolver_(*node, *record) : record->hit_bounds;
    if (motion_ != nullptr) {
        MotionTransform transform;
        std::vector<const RetainedNode*> route;
        for (const RetainedNode* current = node; current != nullptr; current = current->parent()) {
            route.push_back(current);
        }
        for (auto current = route.rbegin(); current != route.rend(); ++current) {
            const LayoutRecord* current_layout = layout_->find((*current)->identity());
            if (current_layout == nullptr) continue;
            transform = concatenate_presentation_transform(
                transform,
                local_presentation_transform(
                    **current,
                    *motion_,
                    current_layout->bounds
                )
            );
        }
        hit = transform_presentation_bounds(hit, transform);
    }
    return Point{hit.x + hit.width * 0.5, hit.y + hit.height * 0.5};
}

InputOperationResult InputRouter::enqueue_click(std::string key) {
    const Point center = injection_point(key);
    const bool pressed = enqueue_input(PointerInputEvent{
        center, PointerEventType::press, 0, 0
    });
    const bool released = pressed && enqueue_input(PointerInputEvent{
        center, PointerEventType::release, 0, 0
    });
    if (pressed && !released && !queued_inputs_.empty()) queued_inputs_.pop_back();
    return InputOperationResult{{}, {}, released ? 2U : 0U, 0U};
}

InputOperationResult InputRouter::enqueue_pointer(PointerInputEvent event) {
    const bool accepted = enqueue_input(event);
    return InputOperationResult{{}, {}, accepted ? 1U : 0U, 0U};
}

InputOperationResult InputRouter::enqueue_scroll(ScrollInputEvent event) {
    if (!std::isfinite(event.position.x) || !std::isfinite(event.position.y) ||
        !std::isfinite(event.delta_x) || !std::isfinite(event.delta_y)) {
        throw std::invalid_argument("scroll input coordinates and deltas must be finite");
    }
    const bool accepted = enqueue_input(event);
    return InputOperationResult{{}, {}, accepted ? 1U : 0U, 0U};
}

InputOperationResult InputRouter::enqueue_scroll(
    std::string key,
    const double delta_x,
    const double delta_y,
    const KeyModifiers modifiers
) {
    return enqueue_scroll(ScrollInputEvent{
        injection_point(key), delta_x, delta_y, modifiers,
    });
}

InputOperationResult InputRouter::enqueue_drag(std::string from_key, std::string to_key) {
    if (tree_ == nullptr || layout_ == nullptr) {
        throw std::logic_error("drag injection requires a completed surface frame");
    }
    RetainedNode* source = tree_->find_key(from_key);
    RetainedNode* destination = tree_->find_key(to_key);
    const LayoutRecord* source_layout = source != nullptr
        ? layout_->find(source->identity())
        : nullptr;
    const LayoutRecord* destination_layout = destination != nullptr
        ? layout_->find(destination->identity())
        : nullptr;
    if (source == nullptr || destination == nullptr || source_layout == nullptr ||
        destination_layout == nullptr) {
        throw std::invalid_argument(
            "drag source and destination keys must be retained and arranged"
        );
    }
    if (drag_source_ancestor(source) == nullptr) {
        throw std::invalid_argument("drag source has no enabled strata.drag-source behavior");
    }
    const Point start{
        source_layout->bounds.x + source_layout->bounds.width * 0.5,
        source_layout->bounds.y + source_layout->bounds.height * 0.5,
    };
    const Point end{
        destination_layout->bounds.x + destination_layout->bounds.width * 0.5,
        destination_layout->bounds.y + destination_layout->bounds.height * 0.5,
    };
    std::vector<SurfaceInputEvent> inputs;
    inputs.emplace_back(PointerInputEvent{start, PointerEventType::move, 0, 0});
    inputs.emplace_back(PointerInputEvent{start, PointerEventType::press, 0, 0});
    inputs.emplace_back(PointerInputEvent{end, PointerEventType::move, 0, 0});
    inputs.emplace_back(PointerInputEvent{end, PointerEventType::release, 0, 0});
    const bool accepted = enqueue_inputs(std::move(inputs));
    return InputOperationResult{{}, {}, accepted ? 4U : 0U, 0U};
}

InputOperationResult InputRouter::enqueue_key(std::string key, const KeyModifiers modifiers) {
    const bool accepted = enqueue_input(KeyInputEvent{std::move(key), modifiers});
    return InputOperationResult{{}, {}, accepted ? 1U : 0U, 0U};
}

InputOperationResult InputRouter::enqueue_text(std::string text) {
    const bool accepted = enqueue_input(TextInputEvent{std::move(text)});
    return InputOperationResult{{}, {}, accepted ? 1U : 0U, 0U};
}

InputOperationResult InputRouter::enqueue_ime_preedit(
    std::string text,
    const std::size_t selection_start,
    const std::size_t selection_end
) {
    const bool accepted = enqueue_input(ImePreeditInputEvent{
        std::move(text), selection_start, selection_end,
    });
    return InputOperationResult{{}, {}, accepted ? 1U : 0U, 0U};
}

InputOperationResult InputRouter::process_queued() {
    InputOperationResult result;
    while (!queued_inputs_.empty() &&
           result.processed_events < input_config_.max_events_per_frame) {
        SurfaceInputEvent input = std::move(queued_inputs_.front());
        queued_inputs_.pop_front();
        append(result, std::visit([this](auto&& value) -> InputOperationResult {
            using Event = std::remove_cvref_t<decltype(value)>;
            const std::int64_t saved_frame_time = frame_time_nanos_;
            if (value.timestamp_nanos > 0) frame_time_nanos_ = value.timestamp_nanos;
            const auto restore_time = [this, saved_frame_time](InputOperationResult routed) {
                frame_time_nanos_ = saved_frame_time;
                return routed;
            };
            if constexpr (std::is_same_v<Event, PointerInputEvent>) {
                return restore_time(pointer(value));
            } else if constexpr (std::is_same_v<Event, ScrollInputEvent>) {
                return restore_time(scroll(value));
            } else if constexpr (std::is_same_v<Event, KeyInputEvent>) {
                return restore_time(key(std::move(value), false));
            } else if constexpr (std::is_same_v<Event, TextInputEvent>) {
                return restore_time(text(std::move(value)));
            } else if constexpr (std::is_same_v<Event, ImePreeditInputEvent>) {
                return restore_time(ime_preedit(std::move(value)));
            } else {
                KeyModifiers modifiers = value.modifiers;
                std::string key = value.direction;
                const bool traversal = key == "next" || key == "previous";
                if (key == "next") key = "tab";
                else if (key == "previous") {
                    key = "tab";
                    modifiers.shift = true;
                } else if (key == "activate") key = "enter";
                else if (key == "cancel" || key == "back") key = "escape";
                return restore_time(this->key(KeyInputEvent{
                    std::move(key), modifiers, value.type, value.timestamp_nanos,
                }, traversal));
            }
        }, std::move(input)));
    }
    if (queued_inputs_.empty()) input_queue_overflow_reported_ = false;
    synchronize_authored_presentations();
    return result;
}

std::size_t InputRouter::queued_event_count() const noexcept { return queued_inputs_.size(); }

InputProfilerCounters InputRouter::take_profiler_counters() noexcept {
    return InputProfilerCounters{
        std::exchange(dispatch_count_, 0U),
        std::exchange(coalesced_move_count_, 0U),
        std::exchange(behavior_dispatch_count_, 0U),
        std::exchange(pointer_geometry_rebuild_count_, 0U),
    };
}

bool InputRouter::requires_frame_advance() const noexcept {
    const bool pending_command_tooltip = std::ranges::any_of(
        hover_started_nanos_,
        [this](const auto& entry) {
            return !matured_command_tooltips_.contains(entry.first) &&
                   command_tooltip_candidate(entry.first);
        }
    );
    return pending_command_tooltip || tooltip_disclosures_need_frame() ||
           !pressed_pointer_targets_.empty() || !drag_sessions_.empty() ||
           scrollbar_drag_.has_value() || pending_focus_.has_value() ||
           !pending_reveals_.empty() || !pending_diagnostics_.empty() ||
           !requested_widget_frames_.empty();
}

InputOperationResult InputRouter::enqueue(std::vector<SurfaceInputEvent> events) {
    const std::size_t count = events.size();
    const bool accepted = enqueue_inputs(std::move(events));
    return InputOperationResult{{}, {}, accepted ? count : 0U, 0U};
}

} // namespace strata::ui
