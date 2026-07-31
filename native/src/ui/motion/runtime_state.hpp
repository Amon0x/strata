#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ui/motion.hpp"
#include "ui/motion/catalog.hpp"
#include "ui/motion/players.hpp"

namespace strata::ui {

/** Private retained state shared by the motion runtime's lifecycle, move, and frame stages. */
struct MotionRuntime::Impl final {
    struct NodeState final {
        MotionComputedValues computed;
        std::map<MotionTrigger, motion_detail::TimelinePlayer> trigger_players;
        std::map<std::string, motion_detail::TimelinePlayer, std::less<>> timeline_players;
        std::map<std::string, motion_detail::MotionTimelinePlayer, std::less<>>
            motion_timeline_players;
        std::map<std::string, motion_detail::TargetPlayer, std::less<>> target_players;
        /** Immutable authored MOVE template cached when this node's description is discovered. */
        std::optional<CompiledMotion> move_template;
        std::optional<CompiledMotion> move_animation;
        bool move_requested = false;
        std::vector<MotionInspectionChannel> inspection;
        DirtyGenerationSnapshot observed_dirty;
    };

    MotionCatalog catalog;
    std::shared_ptr<const runtime::RuntimeUnit> unit;
    std::map<std::uint64_t, NodeState> nodes;
    std::set<std::uint64_t> active_nodes;
    std::map<std::uint64_t, motion_detail::TargetPlayer> disclosure_players;
    std::map<std::uint64_t, NormalizedMotionSample> disclosure_samples;
    std::optional<bool> reduced_motion;
};

} // namespace strata::ui
