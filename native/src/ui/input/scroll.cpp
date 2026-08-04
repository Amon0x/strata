#include "ui/input.hpp"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "runtime/value.hpp"
#include "ui/behavior/registry.hpp"
#include "ui/input/detail.hpp"
#include "ui/motion.hpp"

namespace strata::ui {
using namespace input_detail;

namespace {

[[nodiscard]] bool contains(const Rect& bounds, const Point point) noexcept {
    return point.x >= bounds.x && point.y >= bounds.y &&
           point.x <= bounds.right() && point.y <= bounds.bottom();
}

[[nodiscard]] bool input_enabled(
    const RetainedNode& node,
    const BehaviorRegistry& behaviors
) noexcept {
    const runtime::Value* enabled = scalar_property(node, "enabled");
    if (enabled != nullptr && enabled->boolean() != nullptr && !*enabled->boolean()) return false;
    for (const DescriptionBehavior& attachment : node.description().behaviors) {
        if (!attachment.enabled) continue;
        const BehaviorLifecycle* lifecycle = behaviors.find(attachment.id);
        if (lifecycle != nullptr && lifecycle->input.disabled) return false;
    }
    return motion_input_eligible(node);
}

[[nodiscard]] runtime::Value scroll_value(const Point offset) {
    return runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
        {"x", runtime::Value(offset.x)},
        {"y", runtime::Value(offset.y)},
    });
}

} // namespace

std::optional<Point> InputRouter::scroll_limits(const RetainedNode& node) const {
    const LayoutRecord* layout = layout_ != nullptr ? layout_->find(node.identity()) : nullptr;
    if (layout == nullptr || layout->kind != LayoutKind::scroll || !layout->viewport.has_value()) {
        return std::nullopt;
    }
    const LayoutStyle style = layout_style(node.description());
    return Point{
        style.scroll_horizontal
            ? std::max(0.0, layout->content_size.width - layout->content_bounds.width)
            : 0.0,
        style.scroll_vertical
            ? std::max(0.0, layout->content_size.height - layout->content_bounds.height)
            : 0.0,
    };
}

bool InputRouter::set_scroll_offset(
    const RetainedNode& node,
    Point offset,
    InputOperationResult& result
) {
    const LayoutRecord* layout = layout_ != nullptr ? layout_->find(node.identity()) : nullptr;
    const std::optional<Point> limits = scroll_limits(node);
    if (layout == nullptr || !limits.has_value()) return false;
    offset.x = std::clamp(offset.x, 0.0, limits->x);
    offset.y = std::clamp(offset.y, 0.0, limits->y);
    if (offset.x == layout->scroll_offset.x && offset.y == layout->scroll_offset.y) return false;
    const bool changed = tree_->set_arrangement_value(
        node.identity(),
        "strata.scroll.offset",
        scroll_value(offset)
    );
    if (!changed) return false;
    if (frame_invalidator_ != nullptr) frame_invalidator_();
    if (scroll_mutation_observer_ != nullptr && node.description().key.has_value()) {
        scroll_mutation_observer_(*node.description().key);
    }

    const std::shared_ptr<const runtime::ActionValue> action = activation_action(node, "onScroll");
    if (action == nullptr || action->action == nullptr) return true;
    JsonValue event_json = object({
        {"action", canonical_action(*action, node)},
        {"offset", object({{"x", JsonValue(offset.x)}, {"y", JsonValue(offset.y)}})},
        {"source", source(node)},
        {"type", JsonValue("scroll-changed")},
    });
    emit(std::move(event_json), action, node, scroll_value(offset), result);
    return true;
}

std::optional<Point> InputRouter::scroll_offset(const std::string_view key) const noexcept {
    const RetainedNode* node = tree_ != nullptr ? tree_->find_key(key) : nullptr;
    const LayoutRecord* layout = node != nullptr && layout_ != nullptr
                                     ? layout_->find(node->identity())
                                     : nullptr;
    return layout != nullptr && scroll_limits(*node).has_value()
               ? std::optional<Point>(layout->scroll_offset)
               : std::nullopt;
}

std::optional<Point> InputRouter::constrained_scroll_target(
    const std::string_view key,
    Point target
) const noexcept {
    const RetainedNode* node = tree_ != nullptr ? tree_->find_key(key) : nullptr;
    const std::optional<Point> limits = node != nullptr ? scroll_limits(*node) : std::nullopt;
    if (!limits.has_value()) return std::nullopt;
    target.x = std::clamp(target.x, 0.0, limits->x);
    target.y = std::clamp(target.y, 0.0, limits->y);
    return target;
}

bool InputRouter::scroll_to(
    const std::string_view key,
    const Point target,
    InputOperationResult& result
) {
    RetainedNode* node = tree_ != nullptr ? tree_->find_key(key) : nullptr;
    return node != nullptr && set_scroll_offset(*node, target, result);
}

bool InputRouter::route_scrollbar_pointer(
    const PointerInputEvent& event,
    InputOperationResult& result
) {
    const auto update_drag = [this, &result](
                                 const ScrollbarDrag& drag,
                                 const Point point
                             ) {
        RetainedNode* owner = tree_ != nullptr ? tree_->find_identity(drag.identity) : nullptr;
        const LayoutRecord* layout = owner != nullptr && layout_ != nullptr
                                         ? layout_->find(owner->identity())
                                         : nullptr;
        if (owner == nullptr || layout == nullptr) return;
        const std::optional<ScrollbarGeometry> geometry = scrollbar_geometry(
            *layout, layout_style(owner->description()), drag.axis
        );
        if (!geometry.has_value()) return;
        const double coordinate = drag.axis == ScrollbarAxis::vertical ? point.y : point.x;
        const double travel = std::max(
            1.0, geometry->track_length - geometry->thumb_length
        );
        const double thumb_start = std::clamp(
            coordinate - drag.grab_offset,
            geometry->track_start,
            geometry->track_start + travel
        );
        const double next = std::clamp(
            (thumb_start - geometry->track_start) / travel * geometry->maximum_offset,
            0.0,
            geometry->maximum_offset
        );
        Point offset = layout->scroll_offset;
        if (drag.axis == ScrollbarAxis::vertical) offset.y = next;
        else offset.x = next;
        static_cast<void>(set_scroll_offset(*owner, offset, result));
    };

    if (scrollbar_drag_.has_value() && scrollbar_drag_->pointer_id == event.pointer_id) {
        RetainedNode* owner = tree_->find_identity(scrollbar_drag_->identity);
        if (owner == nullptr) {
            scrollbar_drag_.reset();
            return false;
        }
        hover_route(owner);
        if (event.type == PointerEventType::move) update_drag(*scrollbar_drag_, event.position);
        if (event.type == PointerEventType::release || event.type == PointerEventType::cancel) {
            scrollbar_drag_.reset();
        }
        return true;
    }

    struct Hit final {
        RetainedNode* owner = nullptr;
        ScrollbarGeometry geometry;
    };
    std::optional<Hit> hit;
    RetainedNode* raw_target = widget_native_input_owner(hit_test(event.position));
    RetainedNode* modal = active_modal();
    for (RetainedNode* current = raw_target; current != nullptr; current = current->parent()) {
        if (modal != nullptr && !descendant_of(*current, *modal)) continue;
        if (!input_enabled(*current, behaviors_)) continue;
        const LayoutRecord* layout = layout_->find(current->identity());
        if (layout == nullptr) continue;
        const LayoutStyle style = layout_style(current->description());
        for (const ScrollbarAxis axis : {ScrollbarAxis::vertical, ScrollbarAxis::horizontal}) {
            const std::optional<ScrollbarGeometry> geometry = scrollbar_geometry(
                *layout, style, axis
            );
            if (geometry.has_value() && contains(geometry->hit_bounds, event.position)) {
                hit = Hit{current, *geometry};
                break;
            }
        }
        if (hit.has_value()) break;
    }
    if (!hit.has_value()) return false;

    hover_route(hit->owner);
    if (event.type == PointerEventType::press && event.button == 0) {
        const double coordinate = hit->geometry.axis == ScrollbarAxis::vertical
                                      ? event.position.y
                                      : event.position.x;
        const bool within_thumb = coordinate >= hit->geometry.thumb_start &&
            coordinate <= hit->geometry.thumb_start + hit->geometry.thumb_length;
        scrollbar_drag_ = ScrollbarDrag{
            hit->owner->identity(),
            hit->geometry.axis,
            within_thumb ? coordinate - hit->geometry.thumb_start
                         : hit->geometry.thumb_length * 0.5,
            event.pointer_id,
        };
        update_drag(*scrollbar_drag_, event.position);
    }
    return true;
}

InputOperationResult InputRouter::scroll(const ScrollInputEvent event) {
    InputOperationResult result;
    result.injected_events = 1U;
    result.processed_events = 1U;
    if (tree_ == nullptr || layout_ == nullptr) return result;

    RetainedNode* target = widget_native_input_owner(hit_test(event.position));
    if (RetainedNode* modal = active_modal(); modal != nullptr &&
        (target == nullptr || !descendant_of(*target, *modal))) {
        target = modal;
    }
    hover_route(target);
    const InputDispatchState dispatch = route_event(
        target,
        target,
        InputEventKind::scroll,
        nullptr,
        &event,
        nullptr,
        nullptr,
        nullptr,
        result
    );
    if (dispatch.consumed) return result;

    for (RetainedNode* current = target; current != nullptr; current = current->parent()) {
        if (!input_enabled(*current, behaviors_)) continue;
        const LayoutRecord* layout = layout_->find(current->identity());
        const std::optional<Point> limits = scroll_limits(*current);
        if (layout == nullptr || !limits.has_value()) continue;
        const Point next{
            std::clamp(
                layout->scroll_offset.x - event.delta_x * input_config_.scroll_step,
                0.0,
                limits->x
            ),
            std::clamp(
                layout->scroll_offset.y - event.delta_y * input_config_.scroll_step,
                0.0,
                limits->y
            ),
        };
        if (set_scroll_offset(*current, next, result)) return result;
    }
    return result;
}

void InputRouter::reveal_focus(const RetainedNode& node, InputOperationResult& result) {
    if (layout_ == nullptr) return;
    const LayoutRecord* target = layout_->find(node.identity());
    if (target == nullptr) return;
    for (const RetainedNode* current = node.parent(); current != nullptr; current = current->parent()) {
        const LayoutRecord* scroll = layout_->find(current->identity());
        const std::optional<Point> limits = scroll_limits(*current);
        if (scroll == nullptr || !scroll->viewport.has_value() || !limits.has_value()) continue;
        Point next = scroll->scroll_offset;
        const Rect& viewport = *scroll->viewport;
        if (target->bounds.x < viewport.x) next.x += target->bounds.x - viewport.x;
        if (target->bounds.right() > viewport.right()) {
            next.x += target->bounds.right() - viewport.right();
        }
        if (target->bounds.y < viewport.y) next.y += target->bounds.y - viewport.y;
        if (target->bounds.bottom() > viewport.bottom()) {
            next.y += target->bounds.bottom() - viewport.bottom();
        }
        next.x = std::clamp(next.x, 0.0, limits->x);
        next.y = std::clamp(next.y, 0.0, limits->y);
        static_cast<void>(set_scroll_offset(*current, next, result));
    }
}

bool InputRouter::scroll_focused_ancestor(
    const std::string_view key,
    InputOperationResult& result
) {
    if (!focused_.has_value() || tree_ == nullptr || layout_ == nullptr) return false;
    RetainedNode* focused = tree_->find_identity(*focused_);
    if (focused == nullptr) return false;
    for (RetainedNode* current = focused; current != nullptr; current = current->parent()) {
        const LayoutRecord* layout = layout_->find(current->identity());
        const std::optional<Point> limits = scroll_limits(*current);
        if (layout == nullptr || !layout->viewport.has_value() || !limits.has_value()) continue;
        Point next = layout->scroll_offset;
        if (key == "page_up") next.y -= layout->viewport->height;
        else if (key == "page_down") next.y += layout->viewport->height;
        else if (key == "home") next.y = 0.0;
        else if (key == "end") next.y = limits->y;
        else return false;
        if (set_scroll_offset(*current, next, result)) return true;
    }
    return false;
}

} // namespace strata::ui
