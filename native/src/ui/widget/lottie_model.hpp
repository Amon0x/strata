#pragma once

#include <cstdint>
#include <span>

#include "resource/lottie.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {

/**
 * Where a Lottie widget's playback stands, kept with the node as retained bytes: the playhead as
 * a fraction of its segment, and what it was measured against, so a change of source or segment
 * starts it over. Its frame hook moves it; its presenter draws the frame it names.
 */
struct LottiePlayhead final {
    double position = 0.0;
    double first = 0.0;
    double last = 0.0;
    std::uint64_t source = 0U;
    std::uint32_t flags = 0U;
    std::uint32_t reserved = 0U;
    /** The `progress` last sought: a new value seeks, an unchanged one leaves the playhead be. */
    double sought = 0.0;

    static constexpr std::uint32_t started = 1U;
    static constexpr std::uint32_t complete = 2U;
    static constexpr std::uint32_t playing = 4U;
    /** `sought` holds a `progress` that was applied. */
    static constexpr std::uint32_t has_sought = 8U;
};

/** The frames a Lottie widget plays: its `segment`, else the whole composition. */
struct LottieSegment final {
    double first = 0.0;
    double last = 0.0;
};

[[nodiscard]] LottieSegment lottie_segment(const resource::lottie::Animation& animation,
                                           const runtime::Value* segment) noexcept;

/**
 * The composition frame at `position` (0-1) through a segment. Segment ends are exclusive, as a
 * layer's out point is, so the end shows the last moment before it.
 */
[[nodiscard]] double lottie_frame(LottieSegment segment, double position) noexcept;

/**
 * The `Lottie` widget: an After Effects (Bodymovin) animation from a resource, played on the
 * widget's own clock (its frame hook) and drawn as vector paths each frame.
 */
void register_lottie_widget(WidgetRegistry& registry);

} // namespace strata::ui
