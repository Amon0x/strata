#include "ui/motion.hpp"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <stdexcept>

#include "ui/layout.hpp"
#include "ui/motion/runtime_state.hpp"
#include "ui/theme.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] std::vector<std::string> affected(const CompiledMotion& animation) {
    std::vector<std::string> result;
    result.reserve(animation.tracks.size());
    for (const MotionTrack& track : animation.tracks) {
        result.emplace_back(motion_property_name(track.property));
    }
    return result;
}

struct ScrollContext final {
    double x = 0.0;
    double y = 0.0;
    std::optional<double> nearest_x;
    std::optional<double> nearest_y;
};

[[nodiscard]] Point scroll_influence(
    const RetainedNode& node,
    const LayoutResult& layout
) {
    ScrollContext context;
    std::vector<const RetainedNode*> ancestors;
    for (const RetainedNode* current = node.parent(); current != nullptr;
         current = current->parent()) {
        ancestors.push_back(current);
    }
    for (auto current = ancestors.rbegin(); current != ancestors.rend(); ++current) {
        const LayoutRecord* const record = layout.find((*current)->identity());
        if (record == nullptr) continue;
        if (record->scroll_horizontal) {
            context.x += record->scroll_offset.x;
            context.nearest_x = record->scroll_offset.x;
        }
        if (record->scroll_vertical) {
            context.y += record->scroll_offset.y;
            context.nearest_y = record->scroll_offset.y;
        }
    }
    const LayoutRecord* const record = layout.find(node.identity());
    return Point{
        context.x -
            (record != nullptr && record->pin_horizontal
                 ? context.nearest_x.value_or(0.0)
                 : 0.0),
        context.y -
            (record != nullptr && record->pin_vertical
                 ? context.nearest_y.value_or(0.0)
                 : 0.0),
    };
}

} // namespace

std::vector<MotionMoveOrigin> MotionRuntime::capture_move_origins(
    const RetainedTree& tree,
    const LayoutResult& layout
) {
    std::vector<MotionMoveOrigin> result;
    if (tree.root() == nullptr || layout.root_identity != tree.root()->identity()) return result;
    result.reserve(implementation_->nodes.size());
    for (const auto& [identity, state] : implementation_->nodes) {
        if (!state.move_template.has_value()) continue;
        const RetainedNode* const node = tree.find_identity(identity);
        const LayoutRecord* const record = layout.find(identity);
        if (node == nullptr || node->lifecycle() != RetainedLifecycle::attached ||
            record == nullptr || record->bounds.empty()) {
            continue;
        }
        const Point influence = scroll_influence(*node, layout);
        result.push_back(MotionMoveOrigin{
            identity,
            record->bounds.x,
            record->bounds.y,
            record->bounds.width,
            record->bounds.height,
            influence.x,
            influence.y,
        });
    }
    return result;
}

void MotionRuntime::apply_move_transitions(
    RetainedTree& tree,
    const std::vector<MotionMoveOrigin>& origins,
    const LayoutResult& after,
    const std::int64_t frame_time_nanos,
    const bool reduced_motion
) {
    if (frame_time_nanos < 0) throw std::invalid_argument("motion frame time must be non-negative");
    if (tree.root() == nullptr || origins.empty()) return;
    for (const MotionMoveOrigin& previous : origins) {
        RetainedNode* const node = tree.find_identity(previous.identity);
        const LayoutRecord* const current = after.find(previous.identity);
        const auto state_entry = implementation_->nodes.find(previous.identity);
        if (node == nullptr || node->lifecycle() != RetainedLifecycle::attached ||
            current == nullptr || current->bounds.empty() ||
            state_entry == implementation_->nodes.end() ||
            !state_entry->second.move_template.has_value()) {
            continue;
        }
        const Point influence = scroll_influence(*node, after);
        const double dx = previous.x - current->bounds.x -
            (influence.x - previous.ancestor_scroll_x);
        const double dy = previous.y - current->bounds.y -
            (influence.y - previous.ancestor_scroll_y);
        if (std::abs(dx) < 0.001 && std::abs(dy) < 0.001) continue;
        CompiledMotion animation = *state_entry->second.move_template;
        animation.timing.delay_nanos = 0;
        animation.tracks.clear();
        const auto add_track = [&animation](
                                   const MotionProperty property,
                                   const double offset
                               ) {
            animation.tracks.push_back(MotionTrack{
                property,
                {
                    MotionKeyframe{0.0, MotionValue(offset), std::nullopt},
                    MotionKeyframe{1.0, MotionValue(0.0), std::nullopt},
                },
            });
        };
        if (std::abs(dx) >= 0.001) add_track(MotionProperty::translate_x, dx);
        if (std::abs(dy) >= 0.001) add_track(MotionProperty::translate_y, dy);
        Impl::NodeState& state = state_entry->second;
        state.move_animation = std::move(animation);
        state.move_requested = true;
        implementation_->active_nodes.insert(node->identity());
        motion_detail::TimelinePlayer& player =
            state.trigger_players[MotionTrigger::move];
        const motion_detail::TimelineSample sample = player.advance(
            *state.move_animation,
            true,
            MotionDirection::forward,
            frame_time_nanos,
            theme_motion_reduced(node->description(), reduced_motion)
        );
        state.computed.progress = std::max(
            state.computed.progress, sample.computed.progress
        );
        state.computed.values.erase(MotionProperty::translate_x);
        state.computed.values.erase(MotionProperty::translate_y);
        for (const auto& [property, value] : sample.computed.values) {
            state.computed.values.insert_or_assign(property, value);
        }
        const auto channel = std::ranges::find(
            state.inspection,
            MotionTrigger::move,
            &MotionInspectionChannel::trigger
        );
        const MotionInspectionChannel inspection{
            "strata.trigger.move",
            "trigger",
            player.direction(),
            player.progress(),
            std::nullopt,
            std::nullopt,
            affected(*state.move_template),
            player.running(),
            theme_motion_reduced(node->description(), reduced_motion),
            MotionTrigger::move,
            std::nullopt,
        };
        if (channel != state.inspection.end()) *channel = inspection;
        else state.inspection.push_back(inspection);
        static_cast<void>(tree.mark(node->identity(), DirtyReason::animation));
    }
}

} // namespace strata::ui
