#include "ui/input.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "runtime/value.hpp"
#include "ui/behavior/backdrop.hpp"
#include "ui/behavior/input.hpp"
#include "ui/behavior/registry.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {
namespace {

using Fields = std::vector<std::pair<std::string, runtime::Value>>;

/** A scroll that has not moved for this long has stopped: its zero velocity is reported. */
constexpr std::int64_t scroll_rest_nanos = 90'000'000;
/**
 * A size that returns to the one before it this soon after changing is the node answering its own
 * report (narrow layout makes it wide, wide makes it narrow): it is held, not reported again.
 */
constexpr std::int64_t size_flip_window_nanos = 50'000'000;

[[nodiscard]] const DescriptionBehavior* attachment(const RetainedNode& node,
                                                    const std::string_view id) noexcept {
    for (const DescriptionBehavior& behavior : node.description().behaviors) {
        if (behavior.enabled && behavior.id == id)
            return &behavior;
    }
    return nullptr;
}

[[nodiscard]] bool same(const double left, const double right) noexcept {
    return std::abs(left - right) < 1.0e-6;
}

} // namespace

bool InputRouter::request_behavior_frame(const RetainedNode& node) {
    if (tree_ == nullptr || node.lifecycle() != RetainedLifecycle::attached)
        return false;
    requested_behavior_frames_.insert(node.identity());
    if (frame_invalidator_ != nullptr)
        frame_invalidator_();
    return true;
}

void InputRouter::advance_behavior_frames(InputOperationResult& result) {
    if (requested_behavior_frames_.empty())
        return;
    // Each request is for one frame: a hook that wants another asks again while it runs.
    const std::set<std::uint64_t> requested = std::exchange(requested_behavior_frames_, {});
    for (const std::uint64_t identity : requested) {
        RetainedNode* const node = tree_ != nullptr ? tree_->find_identity(identity) : nullptr;
        if (node == nullptr || node->lifecycle() != RetainedLifecycle::attached)
            continue;
        // Copied: a hook's action may describe the node again.
        const std::vector<DescriptionBehavior> attachments = node->description().behaviors;
        for (const DescriptionBehavior& behavior : attachments) {
            const BehaviorLifecycle* const lifecycle =
                behavior.enabled ? behaviors_.find(behavior.id) : nullptr;
            if (lifecycle == nullptr || lifecycle->input.advance == nullptr)
                continue;
            BehaviorInputScope scope(*this, *node, behavior, BehaviorInputEventPhase::advance,
                                     result);
            static_cast<void>(lifecycle->input.advance(scope));
        }
    }
}

void InputRouter::synchronize_widgets(InputOperationResult& result) {
    if (tree_ == nullptr || widgets_.synchronized_types().empty())
        return;
    if (synchronized_tree_generation_ == tree_->generation())
        return;
    synchronized_tree_generation_ = tree_->generation();
    for (RetainedNode* const node : tree_->nodes_of_types(widgets_.synchronized_types())) {
        if (node->lifecycle() != RetainedLifecycle::attached)
            continue;
        const WidgetLifecycle* const lifecycle = widgets_.find(node->description().type);
        if (lifecycle == nullptr || lifecycle->input.synchronize == nullptr)
            continue;
        WidgetInputScope scope(*this, *node, result, {}, {});
        static_cast<void>(lifecycle->input.synchronize(scope));
    }
}

void InputRouter::observe_layout(InputOperationResult& result) {
    if (tree_ == nullptr || layout_ == nullptr)
        return;
    std::vector<std::uint64_t> scrolls;
    std::vector<std::uint64_t> sizes;
    for (const RetainedNode* node : tree_->behavior_nodes()) {
        if (node->lifecycle() != RetainedLifecycle::attached)
            continue;
        if (attachment(*node, scroll_behavior_id) != nullptr)
            scrolls.push_back(node->identity());
        if (attachment(*node, size_behavior_id) != nullptr)
            sizes.push_back(node->identity());
    }
    // Readings of nodes that are gone or no longer observe go with them.
    std::erase_if(scroll_observations_, [&scrolls](const auto& entry) {
        return !std::ranges::contains(scrolls, entry.first);
    });
    std::erase_if(size_observations_, [&sizes](const auto& entry) {
        return !std::ranges::contains(sizes, entry.first);
    });

    for (const std::uint64_t identity : sizes) {
        RetainedNode* const node = tree_->find_identity(identity);
        const LayoutRecord* const record = node != nullptr ? layout_->find(identity) : nullptr;
        const DescriptionBehavior* const behavior =
            node != nullptr ? attachment(*node, size_behavior_id) : nullptr;
        if (record == nullptr || behavior == nullptr)
            continue;
        const Size size{record->bounds.width, record->bounds.height};
        const auto found = size_observations_.find(identity);
        if (found != size_observations_.end()) {
            SizeObservation& observed = found->second;
            if (same(observed.last.width, size.width) && same(observed.last.height, size.height))
                continue;
            if (observed.previous.has_value() && same(observed.previous->width, size.width) &&
                same(observed.previous->height, size.height) &&
                frame_time_nanos_ - observed.time_nanos <= size_flip_window_nanos) {
                continue;
            }
            observed.previous = observed.last;
            observed.last = size;
            observed.time_nanos = frame_time_nanos_;
        } else {
            size_observations_.emplace(identity,
                                       SizeObservation{size, std::nullopt, frame_time_nanos_});
        }
        BehaviorInputScope scope(*this, *node, *behavior, InputEventPhase::target, result);
        static_cast<void>(scope.emit("size-changed", runtime::Value(Fields{
                                                         {"width", runtime::Value(size.width)},
                                                         {"height", runtime::Value(size.height)},
                                                     })));
    }

    for (const std::uint64_t identity : scrolls) {
        RetainedNode* const node = tree_->find_identity(identity);
        const LayoutRecord* const record = node != nullptr ? layout_->find(identity) : nullptr;
        const DescriptionBehavior* const behavior =
            node != nullptr ? attachment(*node, scroll_behavior_id) : nullptr;
        const std::optional<Point> limits = record != nullptr ? scroll_limits(*node) : std::nullopt;
        if (behavior == nullptr || !limits.has_value() || !record->viewport.has_value())
            continue;
        const Point offset = record->scroll_offset;
        const Size content = record->content_size;
        const Size viewport{record->viewport->width, record->viewport->height};
        Point velocity;
        bool moving = false;
        const auto found = scroll_observations_.find(identity);
        if (found != scroll_observations_.end()) {
            const ScrollObservation& observed = found->second;
            const bool moved =
                !same(observed.offset.x, offset.x) || !same(observed.offset.y, offset.y);
            const bool resized = !same(observed.content.width, content.width) ||
                                 !same(observed.content.height, content.height) ||
                                 !same(observed.viewport.width, viewport.width) ||
                                 !same(observed.viewport.height, viewport.height);
            const std::int64_t elapsed = frame_time_nanos_ - observed.time_nanos;
            if (moved) {
                if (elapsed > 0) {
                    const double seconds = static_cast<double>(elapsed) * 1.0e-9;
                    const Point instant{(offset.x - observed.offset.x) / seconds,
                                        (offset.y - observed.offset.y) / seconds};
                    // Smoothed over consecutive frames; a scroll starting after a rest has no
                    // earlier velocity to blend with.
                    const bool continuing = observed.moving && elapsed < scroll_rest_nanos;
                    velocity = continuing ? Point{(instant.x + observed.velocity.x) * 0.5,
                                                  (instant.y + observed.velocity.y) * 0.5}
                                          : instant;
                } else {
                    // A second layout in the same frame: the frame's velocity stands.
                    velocity = observed.velocity;
                }
                moving = true;
            } else if (observed.moving && elapsed < scroll_rest_nanos) {
                // Still within a gesture's pause: nothing new to say yet.
                if (!resized)
                    continue;
                velocity = observed.velocity;
                moving = true;
            } else if (!resized && !observed.moving) {
                continue;
            }
        }
        const double progress_x =
            limits->x > 0.0 ? std::clamp(offset.x / limits->x, 0.0, 1.0) : 0.0;
        const double progress_y =
            limits->y > 0.0 ? std::clamp(offset.y / limits->y, 0.0, 1.0) : 0.0;
        // The reading keeps the time it last moved: a rest is measured from there.
        const bool unmoved = found != scroll_observations_.end() &&
                             same(found->second.offset.x, offset.x) &&
                             same(found->second.offset.y, offset.y);
        const std::int64_t stamp = unmoved ? found->second.time_nanos : frame_time_nanos_;
        scroll_observations_.insert_or_assign(
            identity, ScrollObservation{offset, content, viewport, velocity, stamp, moving});
        BehaviorInputScope scope(*this, *node, *behavior, InputEventPhase::target, result);
        static_cast<void>(
            scope.emit("scroll-changed", runtime::Value(Fields{
                                             {"x", runtime::Value(offset.x)},
                                             {"y", runtime::Value(offset.y)},
                                             {"progressX", runtime::Value(progress_x)},
                                             {"progressY", runtime::Value(progress_y)},
                                             {"velocityX", runtime::Value(velocity.x)},
                                             {"velocityY", runtime::Value(velocity.y)},
                                             {"contentWidth", runtime::Value(content.width)},
                                             {"contentHeight", runtime::Value(content.height)},
                                             {"viewportWidth", runtime::Value(viewport.width)},
                                             {"viewportHeight", runtime::Value(viewport.height)},
                                         })));
    }
}

} // namespace strata::ui
