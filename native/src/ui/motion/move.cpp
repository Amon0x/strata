#include "ui/motion.hpp"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <stdexcept>

#include "ui/layout.hpp"
#include "ui/motion/runtime_state.hpp"
#include "ui/presentation_geometry.hpp"
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

/**
 * The transform the node's ancestors present it under, root first. With `self`, the node's own
 * presentation is included: where it shows, rather than where its own transforms start from.
 */
[[nodiscard]] MotionTransform presented_transform(const RetainedNode& node,
                                                  const LayoutResult& layout,
                                                  const MotionRuntime& motion, const bool self) {
    std::vector<const RetainedNode*> chain;
    for (const RetainedNode* current = self ? &node : node.parent(); current != nullptr;
         current = current->parent()) {
        chain.push_back(current);
    }
    MotionTransform transform;
    for (auto current = chain.rbegin(); current != chain.rend(); ++current) {
        const LayoutRecord* const record = layout.find((*current)->identity());
        if (record == nullptr)
            continue;
        transform = concatenate_presentation_transform(
            transform, local_presentation_transform(**current, motion, record->bounds));
    }
    return transform;
}

} // namespace

void MotionRuntime::apply_shared_transitions(RetainedTree& tree, const LayoutResult& layout,
                                             const std::int64_t frame_time_nanos,
                                             const bool reduced_motion) {
    if (tree.root() == nullptr || layout.root_identity != tree.root()->identity())
        return;
    std::map<std::string, Impl::SharedPlace, std::less<>>& places = implementation_->shared_places;
    std::map<std::string, Impl::SharedPlace, std::less<>> next;
    for (auto& [identity, state] : implementation_->nodes) {
        if (!state.shared_key.has_value())
            continue;
        RetainedNode* const node = tree.find_identity(identity);
        const LayoutRecord* const record = layout.find(identity);
        if (node == nullptr || node->lifecycle() != RetainedLifecycle::attached ||
            record == nullptr || record->bounds.empty()) {
            continue;
        }
        const auto previous = places.find(*state.shared_key);
        const RetainedNode* const predecessor =
            previous != places.end() && previous->second.identity != identity
                ? tree.find_identity(previous->second.identity)
                : nullptr;
        // The key passes to this node only when the node that showed it is gone (or leaving).
        const bool taken_over =
            previous != places.end() && previous->second.identity != identity &&
            (predecessor == nullptr || predecessor->lifecycle() != RetainedLifecycle::attached);
        if (taken_over && !theme_motion_reduced(node->description(), reduced_motion)) {
            // The flight is laid out where the node's ancestors present it: the predecessor's
            // rectangle is taken into that space, so a scaled or moving parent is accounted for.
            const MotionTransform parent = presented_transform(*node, layout, *this, false);
            const std::optional<MotionTransform> inverse = inverse_presentation_transform(parent);
            const Rect from = inverse.has_value()
                                  ? transform_presentation_bounds(previous->second.bounds, *inverse)
                                  : previous->second.bounds;
            const Rect& to = record->bounds;
            if (from.width > 0.0 && from.height > 0.0) {
                const PresentationStatics& statics = presentation_statics(*node);
                const double scale_x = from.width / to.width;
                const double scale_y = from.height / to.height;
                // The translation that, with the scale about the node's transform origin, puts the
                // laid-out box exactly on the predecessor's.
                const double translate_x =
                    from.x - to.x - statics.origin_x * (to.width - from.width);
                const double translate_y =
                    from.y - to.y - statics.origin_y * (to.height - from.height);
                const double rest_x = statics.translate_x.value_or(0.0);
                const double rest_y = statics.translate_y.value_or(0.0);
                const double uniform = statics.scale.value_or(1.0);
                const double rest_scale_x = statics.scale_x.value_or(1.0) * uniform;
                const double rest_scale_y = statics.scale_y.value_or(1.0) * uniform;
                CompiledMotion flight;
                flight.name = "strata.shared";
                flight.trigger = MotionTrigger::move;
                flight.timing = theme_motion_timing(node->description(), state.shared_timing);
                flight.timing.delay_nanos = 0;
                flight.timing.repeat = MotionRepeat{};
                flight.timing.reverse = false;
                flight.timing.fill_mode = MotionFillMode::both;
                const auto track = [&flight](const MotionProperty property, const double from_value,
                                             const double to_value) {
                    flight.tracks.push_back(MotionTrack{
                        property,
                        {
                            MotionKeyframe{0.0, MotionValue(from_value), std::nullopt},
                            MotionKeyframe{1.0, MotionValue(to_value), std::nullopt},
                        },
                    });
                };
                track(MotionProperty::translate_x, rest_x + translate_x, rest_x);
                track(MotionProperty::translate_y, rest_y + translate_y, rest_y);
                track(MotionProperty::scale_x, rest_scale_x * scale_x, rest_scale_x);
                track(MotionProperty::scale_y, rest_scale_y * scale_y, rest_scale_y);
                state.shared_animation = std::move(flight);
                state.shared_player = motion_detail::TimelinePlayer{};
                const motion_detail::TimelineSample sample =
                    state.shared_player.advance(*state.shared_animation, true,
                                                MotionDirection::forward, frame_time_nanos, false);
                for (const auto& [property, value] : sample.computed.values)
                    state.computed.values.insert_or_assign(property, value);
                implementation_->active_nodes.insert(identity);
                static_cast<void>(tree.mark(identity, DirtyReason::animation));
                // In flight it paints above its siblings, which the parent orders.
                if (node->parent() != nullptr)
                    static_cast<void>(
                        tree.mark(node->parent()->identity(), DirtyReason::animation));
            }
        }
        // Where the node shows now, its own presentation included, for whatever takes it over.
        const MotionTransform shown = presented_transform(*node, layout, *this, true);
        next.try_emplace(
            *state.shared_key,
            Impl::SharedPlace{identity, transform_presentation_bounds(record->bounds, shown)});
    }
    // A key no attached node shows any more is forgotten: only a node appearing in the same
    // rebuild takes a leaving node's place.
    places = std::move(next);
}

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
