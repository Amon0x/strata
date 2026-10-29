#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "geometry/primitives.hpp"
#include "ui/motion.hpp"
#include "ui/motion/catalog.hpp"
#include "ui/motion/config.hpp"
#include "ui/motion/players.hpp"
#include "ui/tree.hpp"

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
        /** The node's `sharedKey` and policy, as of its last discovered description. */
        std::optional<std::string> shared_key;
        std::string shared_timing;
        /** A shared-element flight from where the node's predecessor showed, while it runs. */
        std::optional<CompiledMotion> shared_animation;
        motion_detail::TimelinePlayer shared_player;
        std::vector<MotionInspectionChannel> inspection;
        DirtyGenerationSnapshot observed_dirty;
    };

    MotionCatalog catalog;
    std::shared_ptr<const runtime::RuntimeUnit> unit;
    std::map<std::uint64_t, NodeState> nodes;
    std::set<std::uint64_t> active_nodes;
    std::map<std::uint64_t, motion_detail::TargetPlayer> disclosure_players;
    std::map<std::uint64_t, NormalizedMotionSample> disclosure_samples;
    /** Where each shared element last showed on screen, and which node showed it. */
    struct SharedPlace final {
        std::uint64_t identity = 0U;
        geometry::Rect bounds;
    };
    std::map<std::string, SharedPlace, std::less<>> shared_places;
    std::optional<bool> reduced_motion;

    /**
     * A node's motion config is a pure function of its immutable description and this catalog, but
     * rebuilding it allocates its bindings, so it is kept until the node's description changes.
     * The description is held so a new one can never reuse a cached one's address; every catalog
     * change clears this along with the node state.
     */
    struct CachedConfig final {
        std::shared_ptr<const DescriptionNode> description;
        motion_detail::NodeMotionConfig config;
    };
    std::unordered_map<std::uint64_t, CachedConfig> configs;

    [[nodiscard]] const motion_detail::NodeMotionConfig& config(const RetainedNode& node) {
        CachedConfig& cached = configs[node.identity()];
        if (cached.description.get() != &node.description()) {
            cached.config = motion_detail::node_motion_config(node, catalog);
            cached.description = node.shared_description();
        }
        return cached.config;
    }
};

} // namespace strata::ui
