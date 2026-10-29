#pragma once

#include <cstddef>

#include "ui/layout.hpp"

namespace strata::ui {

class RetainedNode;
class WidgetRegistry;

/** What a RangeSlider shows and allows, read from its properties and retained state. */
struct RangeSliderState final {
    double minimum = 0.0;
    double maximum = 1.0;
    double step = 0.0;
    /** The least distance the thumbs keep between them. */
    double gap = 0.0;
    double start = 0.0;
    double end = 1.0;
    /** The thumb the keyboard moves: 0 for the start, 1 for the end. */
    std::size_t thumb = 0U;

    [[nodiscard]] double fraction(double value) const noexcept;
    /** `value` snapped to the step and clamped to the range. */
    [[nodiscard]] double snapped(double value) const noexcept;
};

[[nodiscard]] RangeSliderState range_slider_state(const RetainedNode& node);

/** The track a RangeSlider draws and maps the pointer along. */
[[nodiscard]] Rect range_slider_track(Rect bounds) noexcept;

/** Registers RangeSlider's description, input, semantics, subtarget and presentation phases. */
void register_range_slider_widget(WidgetRegistry& registry);

} // namespace strata::ui
