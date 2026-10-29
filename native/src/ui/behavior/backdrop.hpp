#pragma once

#include <string_view>

#include "ui/tree.hpp"

namespace strata::ui {

/**
 * `strata.backdrop` lets a node follow what is behind it. The node's BACKDROP effect carries a
 * probe; a host that can measure reports the mean luma of the pixels that effect captured, and the
 * router resolves it into a bistable light/dark reading: light at or above `options.light`, dark at
 * or below `options.dark`, unchanged in between, and believed only once it has held for
 * `options.dwell` consecutive reports. The attachment's action fires with the reading as a boolean
 * event whenever it changes, so `state.setFromEvent` adapts it into authored state.
 */
inline constexpr std::string_view backdrop_behavior_id = "strata.backdrop";
/**
 * `strata.pointer` lets a node follow the pointer over it: its action receives an object event
 * as the pointer moves (`inside` true, `x`/`y` in the node's logical coordinates, `u`/`v` as
 * fractions of its box, `width`/`height`) and `{inside: false}` once it leaves.
 */
inline constexpr std::string_view pointer_behavior_id = "strata.pointer";
/**
 * `strata.scroll` lets a scrolling node report where it is scrolled: after every layout in which
 * its offset, content or viewport changed (wheel, drag, inertia, animated or programmatic scrolls
 * and reveals alike), its action receives `x`, `y`, `progressX`, `progressY` (0..1 of the scroll
 * range), `velocityX`, `velocityY` (logical px/s), `contentWidth`, `contentHeight`,
 * `viewportWidth` and `viewportHeight`; once the scroll stops, it is told once more with zero
 * velocity.
 */
inline constexpr std::string_view scroll_behavior_id = "strata.scroll";
/**
 * `strata.size` lets a node lay itself out by its own size: after every layout that changed its
 * laid-out logical size, its action receives `width` and `height`. A size that answers the event
 * by flipping straight back to the one before is not reported again (no feedback loop).
 */
inline constexpr std::string_view size_behavior_id = "strata.size";
/** Retained reading: true while the node's backdrop is light. Absent until the first report. */
inline constexpr std::string_view backdrop_light_value = "strata.backdrop.light";
/** The luma that produced the retained reading. */
inline constexpr std::string_view backdrop_luminance_value = "strata.backdrop.luminance";
/** Consecutive reports that disagree with the reading so far: positive light, negative dark. */
inline constexpr std::string_view backdrop_pending_value = "strata.backdrop.pending";
inline constexpr double default_backdrop_light_threshold = 0.60;
inline constexpr double default_backdrop_dark_threshold = 0.46;
/** Reports a changed reading must persist for before the node flips (`options.dwell`). */
inline constexpr int default_backdrop_dwell = 3;

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
