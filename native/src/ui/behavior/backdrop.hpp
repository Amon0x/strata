#pragma once

#include <string_view>

#include "ui/tree.hpp"

namespace strata::ui {

/**
 * `strata.backdrop` lets a node follow what is behind it. The node's BACKDROP effect carries a
 * probe; a host that can measure reports the mean luma of the pixels that effect captured, and the
 * router resolves it into a bistable light/dark reading: light at or above `options.light`, dark at
 * or below `options.dark`, unchanged in between. The attachment's action fires with the reading as
 * a boolean event whenever it changes, so `state.setFromEvent` adapts it into authored state.
 */
inline constexpr std::string_view backdrop_behavior_id = "strata.backdrop";
/** Retained reading: true while the node's backdrop is light. Absent until the first report. */
inline constexpr std::string_view backdrop_light_value = "strata.backdrop.light";
/** The luma that produced the retained reading. */
inline constexpr std::string_view backdrop_luminance_value = "strata.backdrop.luminance";
inline constexpr double default_backdrop_light_threshold = 0.60;
inline constexpr double default_backdrop_dark_threshold = 0.46;

[[nodiscard]] inline const DescriptionBehavior*
backdrop_observer(const RetainedNode& node) noexcept {
    for (const DescriptionBehavior& behavior : node.description().behaviors) {
        if (behavior.enabled && behavior.id == backdrop_behavior_id)
            return &behavior;
    }
    return nullptr;
}

[[nodiscard]] inline bool observes_backdrop(const RetainedNode& node) noexcept {
    return backdrop_observer(node) != nullptr;
}

} // namespace strata::ui
