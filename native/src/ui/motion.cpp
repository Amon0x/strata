#include "ui/motion.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "ui/input.hpp"
#include "ui/motion/config.hpp"
#include "ui/motion/runtime_state.hpp"
#include "ui/theme.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] bool checked(const RetainedNode& node) noexcept {
    const runtime::Value* value = motion_detail::node_property(node, "checked");
    if (value == nullptr || value->boolean() == nullptr) value = node.retained_value("$checked");
    if (value == nullptr || value->boolean() == nullptr) {
        value = motion_detail::node_property(node, "defaultChecked");
    }
    return value != nullptr && value->boolean() != nullptr && *value->boolean();
}

[[nodiscard]] bool trigger_active(
    const MotionTrigger trigger,
    const RetainedNode& node,
    const InputRouter& input
) noexcept {
    switch (trigger) {
    case MotionTrigger::enter: return node.lifecycle() == RetainedLifecycle::attached;
    case MotionTrigger::exit: return node.lifecycle() == RetainedLifecycle::exiting;
    case MotionTrigger::hover:
        return node.lifecycle() == RetainedLifecycle::attached && input.hovered(node.identity());
    case MotionTrigger::pressed:
        return node.lifecycle() == RetainedLifecycle::attached && input.active(node.identity());
    case MotionTrigger::focus:
        return node.lifecycle() == RetainedLifecycle::attached && input.focused(node.identity());
    case MotionTrigger::focus_visible:
        return node.lifecycle() == RetainedLifecycle::attached &&
            input.focus_visible(node.identity());
    case MotionTrigger::checked:
        return node.lifecycle() == RetainedLifecycle::attached && checked(node);
    case MotionTrigger::move: return false;
    case MotionTrigger::animate: return node.lifecycle() == RetainedLifecycle::attached;
    }
    return false;
}

[[nodiscard]] bool channel_active(
    const motion_detail::TimelineBinding& channel,
    const RetainedNode& node,
    const InputRouter& input
) noexcept {
    if (channel.state_target.has_value()) return *channel.state_target;
    switch (*channel.interaction) {
    case MotionInteraction::hover: return input.hovered(node.identity());
    case MotionInteraction::pressed: return input.active(node.identity());
    case MotionInteraction::focus: return input.focused(node.identity());
    case MotionInteraction::focus_visible: return input.focus_visible(node.identity());
    }
    return false;
}

[[nodiscard]] std::vector<std::string> affected(const CompiledMotion& animation) {
    std::vector<std::string> result;
    result.reserve(animation.tracks.size());
    for (const MotionTrack& track : animation.tracks) {
        result.emplace_back(motion_property_name(track.property));
    }
    return result;
}

[[nodiscard]] bool schedules_motion(const DirtySet& dirty) noexcept {
    return dirty.contains(DirtyReason::structure) ||
           dirty.contains(DirtyReason::properties) ||
           dirty.contains(DirtyReason::layout) ||
           dirty.contains(DirtyReason::text) ||
           dirty.contains(DirtyReason::style) ||
           dirty.contains(DirtyReason::input) ||
           dirty.contains(DirtyReason::scale) ||
           dirty.contains(DirtyReason::animation);
}

[[nodiscard]] bool motion_generation_changed(
    const DirtyGenerationSnapshot& observed,
    const DirtyGenerationSnapshot& current
) noexcept {
    return observed.properties != current.properties ||
           observed.layout != current.layout ||
           observed.style != current.style ||
           observed.input != current.input ||
           observed.scale != current.scale ||
           observed.animation != current.animation;
}

[[nodiscard]] bool has_motion(const motion_detail::NodeMotionConfig& config) noexcept {
    return !config.triggers.empty() || !config.timelines.empty() ||
           !config.motion_timelines.empty() || !config.targets.empty();
}

} // namespace

MotionFrameCounters MotionRuntime::advance(
    RetainedTree& tree,
    const std::shared_ptr<const runtime::RuntimeUnit>& unit,
    const InputRouter& input,
    const std::int64_t frame_time_nanos,
    const bool reduced_motion
) {
    return evaluate(tree, unit, input, frame_time_nanos, reduced_motion, true);
}

MotionFrameCounters MotionRuntime::discover(
    RetainedTree& tree,
    const std::shared_ptr<const runtime::RuntimeUnit>& unit,
    const InputRouter& input,
    const std::int64_t frame_time_nanos,
    const bool reduced_motion
) {
    return evaluate(tree, unit, input, frame_time_nanos, reduced_motion, false);
}

MotionFrameCounters MotionRuntime::evaluate(
    RetainedTree& tree,
    const std::shared_ptr<const runtime::RuntimeUnit>& unit,
    const InputRouter& input,
    const std::int64_t frame_time_nanos,
    const bool reduced_motion,
    const bool advance_active_players
) {
    if (frame_time_nanos < 0) throw std::invalid_argument("motion frame time must be non-negative");
    bind(unit);
    MotionFrameCounters result;
    std::set<std::uint64_t> scheduled = advance_active_players
        ? implementation_->active_nodes
        : std::set<std::uint64_t>{};
    const bool reduced_motion_changed = !implementation_->reduced_motion.has_value() ||
                                        *implementation_->reduced_motion != reduced_motion;
    implementation_->reduced_motion = reduced_motion;
    if (reduced_motion_changed) {
        // Paused property-neutral timelines are deliberately absent from active_nodes. A policy
        // transition is their wake-up edge, so schedule only retained nodes that own such clocks.
        for (const auto& [identity, state] : implementation_->nodes) {
            if (!state.motion_timeline_players.empty()) scheduled.insert(identity);
        }
    }
    std::vector<RetainedNode*> dirty_nodes = tree.dirty_nodes();
    std::vector<std::uint64_t> lifecycle_boundaries;
    for (RetainedNode* node : dirty_nodes) {
        if (node == nullptr || !schedules_motion(node->dirty())) continue;
        if (node->lifecycle() == RetainedLifecycle::exiting &&
            node->dirty().contains(DirtyReason::animation)) {
            lifecycle_boundaries.push_back(node->identity());
        }
        const auto existing = implementation_->nodes.find(node->identity());
        const DirtyGenerationSnapshot current = node->dirty_generations();
        const motion_detail::NodeMotionConfig config =
            motion_detail::node_motion_config(*node, implementation_->catalog);
        if ((has_motion(config) || disclosure_motion(*node).has_value() ||
             existing != implementation_->nodes.end()) &&
            (existing == implementation_->nodes.end() ||
             motion_generation_changed(existing->second.observed_dirty, current))) {
            scheduled.insert(node->identity());
        }
    }
    // Reconciliation marks a retained lifecycle boundary once. Index the executable EXIT
    // attachments below that boundary at transition time; unrelated descendants are deliberately
    // not scheduled merely because an ancestor is leaving.
    for (const std::uint64_t identity : lifecycle_boundaries) {
        RetainedNode* boundary = tree.find_identity(identity);
        if (boundary == nullptr || boundary->lifecycle() != RetainedLifecycle::exiting ||
            !boundary->dirty().contains(DirtyReason::animation)) {
            continue;
        }
        const auto index_exits = [this, &scheduled](
                                     const auto& self,
                                     RetainedNode& candidate
                                 ) -> void {
            const motion_detail::NodeMotionConfig config =
                motion_detail::node_motion_config(candidate, implementation_->catalog);
            if (std::ranges::any_of(config.triggers, [](const auto& binding) {
                    return binding.trigger == MotionTrigger::exit && binding.animation != nullptr &&
                           binding.cancel_on_detach;
                })) {
                scheduled.insert(candidate.identity());
            }
            for (const auto& child : candidate.children()) self(self, *child);
        };
        index_exits(index_exits, *boundary);
    }
    std::set<std::uint64_t> next_active = advance_active_players
        ? std::set<std::uint64_t>{}
        : implementation_->active_nodes;
    for (const std::uint64_t identity : scheduled) {
        RetainedNode* scheduled_node = tree.find_identity(identity);
        if (scheduled_node == nullptr) continue;
        next_active.erase(identity);
        ++result.evaluated_nodes;
        RetainedNode& node = *scheduled_node;
            const bool node_reduced_motion = theme_motion_reduced(
                node.description(), reduced_motion
            );
            Impl::NodeState& state = implementation_->nodes[node.identity()];
            const motion_detail::NodeMotionConfig config =
                motion_detail::node_motion_config(node, implementation_->catalog);
            const auto move_binding = std::ranges::find(
                config.triggers,
                MotionTrigger::move,
                &motion_detail::TriggerBinding::trigger
            );
            if (move_binding != config.triggers.end() &&
                move_binding->animation != nullptr) {
                state.move_template = *move_binding->animation;
            } else {
                state.move_template.reset();
            }
            std::set<MotionTrigger> retained_triggers;
            std::set<std::string, std::less<>> retained_timelines;
            std::set<std::string, std::less<>> retained_motion_timelines;
            std::set<std::string, std::less<>> retained_targets;
            MotionComputedValues next;
            bool running = false;
            bool motion_timeline_affects_layout = false;
            if (node.lifecycle() == RetainedLifecycle::exiting) {
                state.trigger_players.erase(MotionTrigger::move);
                state.move_animation.reset();
                state.move_requested = false;
            }

            for (const motion_detail::TriggerBinding& binding : config.triggers) {
                if (node.lifecycle() == RetainedLifecycle::exiting &&
                    (binding.trigger != MotionTrigger::exit || !binding.cancel_on_detach)) {
                    continue;
                }
                retained_triggers.insert(binding.trigger);
                const CompiledMotion* animation =
                    binding.trigger == MotionTrigger::move && state.move_animation.has_value()
                        ? &*state.move_animation
                        : binding.animation;
                const bool active = binding.trigger == MotionTrigger::move
                                        ? state.move_requested
                                        : trigger_active(binding.trigger, node, input);
                auto player = state.trigger_players.find(binding.trigger);
                if (!active && player == state.trigger_players.end()) continue;
                if (player == state.trigger_players.end()) {
                    if (active && binding.continuity_from.has_value()) {
                        const auto source = state.trigger_players.find(*binding.continuity_from);
                        if (source != state.trigger_players.end()) {
                            player = state.trigger_players.emplace(
                                binding.trigger, source->second.continued()
                            ).first;
                        }
                    }
                    if (player == state.trigger_players.end()) {
                        player = state.trigger_players.emplace(
                            binding.trigger, motion_detail::TimelinePlayer{}
                        ).first;
                    }
                }
                const motion_detail::TimelineSample sample = player->second.advance(
                    *animation,
                    active,
                    binding.active_direction,
                    frame_time_nanos,
                    node_reduced_motion
                );
                const bool completed_move = binding.trigger == MotionTrigger::move && active &&
                                            !sample.running;
                if (completed_move) {
                    // MOVE is a one-shot FLIP timeline. Once it reaches the identity
                    // transform, release the generated override instead of allowing the
                    // authored placeholder timeline to replace or reverse it.
                    state.trigger_players.erase(MotionTrigger::move);
                    state.move_animation.reset();
                    state.move_requested = false;
                } else {
                    next.progress = std::max(next.progress, sample.computed.progress);
                    for (const auto& [property, value] : sample.computed.values) {
                        next.values.insert_or_assign(property, value);
                    }
                }
                running = running || sample.running;
            }
            std::erase_if(state.trigger_players, [&retained_triggers](const auto& entry) {
                return !retained_triggers.contains(entry.first);
            });

            for (const motion_detail::TimelineBinding& binding : config.timelines) {
                if (node.lifecycle() == RetainedLifecycle::exiting) continue;
                retained_timelines.insert(binding.id);
                const bool active = channel_active(binding, node, input);
                auto [player, inserted] = state.timeline_players.try_emplace(binding.id);
                const motion_detail::TimelineSample sample = inserted
                    ? player->second.snap(
                          *binding.animation,
                          active,
                          MotionDirection::forward,
                          frame_time_nanos
                      )
                    : player->second.advance(
                          *binding.animation,
                          active,
                          MotionDirection::forward,
                          frame_time_nanos,
                          node_reduced_motion
                      );
                next.progress = std::max(next.progress, sample.computed.progress);
                for (const auto& [property, value] : sample.computed.values) {
                    next.values.insert_or_assign(property, value);
                }
                running = running || sample.running;
            }
            std::erase_if(state.timeline_players, [&retained_timelines](const auto& entry) {
                return !retained_timelines.contains(entry.first);
            });

            for (const motion_detail::MotionTimelineBinding& binding : config.motion_timelines) {
                if (node.lifecycle() == RetainedLifecycle::exiting) continue;
                retained_motion_timelines.insert(binding.id);
                motion_detail::MotionTimelinePlayer& player =
                    state.motion_timeline_players[binding.id];
                const motion_detail::MotionTimelineSample sample = player.advance(
                    binding.spec, frame_time_nanos, node_reduced_motion
                );
                next.progress = std::max(next.progress, sample.progress);
                running = running || sample.running;
                motion_timeline_affects_layout = motion_timeline_affects_layout ||
                                                 binding.spec.affects_layout;
            }
            std::erase_if(
                state.motion_timeline_players,
                [&retained_motion_timelines](const auto& entry) {
                    return !retained_motion_timelines.contains(entry.first);
                }
            );

            for (const motion_detail::TargetBinding& binding : config.targets) {
                if (node.lifecycle() == RetainedLifecycle::exiting) continue;
                retained_targets.insert(binding.id);
                motion_detail::TargetPlayer& player = state.target_players[binding.id];
                const motion_detail::TargetSample sample = player.advance(
                    binding.target,
                    theme_motion_timing(node.description(), binding.timing),
                    frame_time_nanos,
                    node_reduced_motion
                );
                next.progress = std::max(next.progress, sample.progress);
                next.values.insert_or_assign(binding.property, sample.value);
                running = running || sample.running;
            }
            std::erase_if(state.target_players, [&retained_targets](const auto& entry) {
                return !retained_targets.contains(entry.first);
            });

            bool mutated = state.computed != next;
            if (mutated) {
                const bool layout_motion = state.computed.affects_layout() || next.affects_layout() ||
                                           motion_timeline_affects_layout;
                static_cast<void>(tree.mark(
                    node.identity(), layout_motion ? DirtyReason::layout : DirtyReason::animation
                ));
                state.computed = std::move(next);
            }

            if (const std::optional<DisclosureMotionSpec> disclosure = disclosure_motion(node);
                disclosure.has_value() && node.lifecycle() == RetainedLifecycle::attached) {
                retained_targets.insert("strata.disclosure");
                motion_detail::TargetPlayer& player =
                    implementation_->disclosure_players[node.identity()];
                const motion_detail::TargetSample sample = player.advance(
                    MotionValue(disclosure->expanded ? 1.0 : 0.0),
                    disclosure->timing,
                    frame_time_nanos,
                    node_reduced_motion
                );
                const double current = std::get<double>(sample.value);
                NormalizedMotionSample normalized{
                    current,
                    disclosure->expanded ? 1.0 : 0.0,
                    sample.progress,
                    sample.running,
                    sample.snapped_by_reduced_motion,
                };
                const auto previous = implementation_->disclosure_samples.find(node.identity());
                if (previous == implementation_->disclosure_samples.end() ||
                    previous->second.current != normalized.current ||
                    previous->second.target != normalized.target ||
                    previous->second.progress != normalized.progress ||
                    previous->second.running != normalized.running ||
                    previous->second.snapped_by_reduced_motion !=
                        normalized.snapped_by_reduced_motion) {
                    static_cast<void>(tree.mark(node.identity(), DirtyReason::layout));
                    mutated = true;
                }
                implementation_->disclosure_samples.insert_or_assign(node.identity(), normalized);
                running = running || sample.running;
            }

            state.inspection.clear();
            state.inspection.reserve(
                config.triggers.size() + config.timelines.size() +
                config.motion_timelines.size() + config.targets.size()
            );
            for (const motion_detail::TriggerBinding& binding : config.triggers) {
                const auto player = state.trigger_players.find(binding.trigger);
                state.inspection.push_back(MotionInspectionChannel{
                    "strata.trigger." + std::string(motion_trigger_name(binding.trigger)),
                    "trigger",
                    player != state.trigger_players.end()
                        ? player->second.direction()
                        : binding.active_direction,
                    player != state.trigger_players.end() ? player->second.progress() : 0.0,
                    std::nullopt,
                    std::nullopt,
                    // Inspection describes the authored channel contract. A MOVE player may use
                    // a runtime-generated FLIP subset without changing that public metadata.
                    affected(*binding.animation),
                    player != state.trigger_players.end() && player->second.running(),
                    false,
                    binding.trigger,
                    std::nullopt,
                });
            }
            for (const motion_detail::TimelineBinding& binding : config.timelines) {
                if (node.lifecycle() == RetainedLifecycle::exiting) continue;
                const bool active = channel_active(binding, node, input);
                const auto player = state.timeline_players.find(binding.id);
                state.inspection.push_back(MotionInspectionChannel{
                    binding.id,
                    "timeline",
                    player != state.timeline_players.end()
                        ? player->second.direction()
                        : active ? MotionDirection::forward : MotionDirection::reverse,
                    player != state.timeline_players.end()
                        ? player->second.progress()
                        : active ? 1.0 : 0.0,
                    std::nullopt,
                    active ? std::optional<std::string>("true")
                           : std::optional<std::string>("false"),
                    affected(*binding.animation),
                    player != state.timeline_players.end() && player->second.running(),
                    false,
                    std::nullopt,
                    binding.interaction,
                });
            }
            for (const motion_detail::MotionTimelineBinding& binding : config.motion_timelines) {
                if (node.lifecycle() == RetainedLifecycle::exiting) continue;
                const auto player = state.motion_timeline_players.find(binding.id);
                const motion_detail::MotionTimelineSample* sample =
                    player != state.motion_timeline_players.end()
                        ? &player->second.sample()
                        : nullptr;
                state.inspection.push_back(MotionInspectionChannel{
                    binding.id,
                    "timeline",
                    MotionDirection::forward,
                    sample != nullptr ? sample->progress : 0.0,
                    std::nullopt,
                    std::nullopt,
                    {},
                    sample != nullptr && sample->running,
                    false,
                    std::nullopt,
                    std::nullopt,
                });
            }
            for (const motion_detail::TargetBinding& binding : config.targets) {
                if (node.lifecycle() == RetainedLifecycle::exiting) continue;
                const auto player = state.target_players.find(binding.id);
                const motion_detail::TargetSample* sample =
                    player != state.target_players.end() ? &player->second.sample() : nullptr;
                state.inspection.push_back(MotionInspectionChannel{
                    binding.id,
                    binding.resolved_style ? "resolved-style" : "value",
                    MotionDirection::to_target,
                    sample != nullptr ? sample->progress : 1.0,
                    sample != nullptr
                        ? std::optional<std::string>(motion_detail::motion_value_text(sample->value))
                        : std::nullopt,
                    std::optional<std::string>(motion_detail::motion_value_text(binding.target)),
                    {std::string(motion_property_name(binding.property))},
                    sample != nullptr && sample->running,
                    sample != nullptr && sample->snapped_by_reduced_motion,
                    std::nullopt,
                    std::nullopt,
                });
            }
            if (mutated) ++result.mutated_nodes;
            if (running) {
                next_active.insert(node.identity());
            }
            state.observed_dirty = node.dirty_generations();
    }
    std::erase_if(next_active, [&tree](const std::uint64_t identity) {
        return tree.find_identity(identity) == nullptr;
    });
    result.running_players = next_active.size();
    implementation_->active_nodes = std::move(next_active);
    std::erase_if(implementation_->nodes, [&tree](const auto& entry) {
        return tree.find_identity(entry.first) == nullptr;
    });
    std::erase_if(implementation_->disclosure_players, [&tree](const auto& entry) {
        return tree.find_identity(entry.first) == nullptr;
    });
    std::erase_if(implementation_->disclosure_samples, [&tree](const auto& entry) {
        return tree.find_identity(entry.first) == nullptr;
    });
    return result;
}

const MotionComputedValues* MotionRuntime::computed_values(
    const std::uint64_t identity
) const noexcept {
    const auto found = implementation_->nodes.find(identity);
    return found != implementation_->nodes.end() ? &found->second.computed : nullptr;
}

const std::vector<MotionInspectionChannel>* MotionRuntime::inspection_channels(
    const std::uint64_t identity
) const noexcept {
    const auto found = implementation_->nodes.find(identity);
    return found != implementation_->nodes.end() ? &found->second.inspection : nullptr;
}

const NormalizedMotionSample* MotionRuntime::disclosure_sample(
    const std::uint64_t identity
) const noexcept {
    const auto found = implementation_->disclosure_samples.find(identity);
    return found != implementation_->disclosure_samples.end() ? &found->second : nullptr;
}

std::size_t MotionRuntime::active_count() const noexcept {
    return implementation_->active_nodes.size();
}

} // namespace strata::ui
