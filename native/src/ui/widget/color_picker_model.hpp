#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "ui/layout.hpp"
#include "ui/render.hpp"

namespace strata::ui {

class RetainedNode;
class WidgetRegistry;

/** A colour as hue (degrees), saturation and value (0 to 1) with its alpha (0 to 1). */
struct ColorPickerHsv final {
    double hue = 0.0;
    double saturation = 0.0;
    double value = 0.0;
    double alpha = 1.0;

    [[nodiscard]] RenderColor rgb() const noexcept;
    [[nodiscard]] static ColorPickerHsv from(RenderColor color, double fallback_hue) noexcept;
};

/** What a ColorPicker shows and allows, read from its properties and retained state. */
struct ColorPickerState final {
    RenderColor color{59U, 130U, 246U, 255U};
    ColorPickerHsv hsv;
    bool alpha = false;
    std::vector<RenderColor> swatches;

    /** "#RRGGBB", or "#RRGGBBAA" when alpha is shown. */
    [[nodiscard]] std::string hex() const;
};

[[nodiscard]] ColorPickerState color_picker_state(const RetainedNode& node);

/** Where a ColorPicker draws and hits each of its parts at its current size. */
struct ColorPickerProjection final {
    Rect field;
    Rect hue;
    std::optional<Rect> alpha;
    Rect preview;
    Rect label;
    std::vector<Rect> swatches;
};

[[nodiscard]] ColorPickerProjection color_picker_projection(const ColorPickerState& state,
                                                            Rect bounds);

/** Registers ColorPicker's description, input, semantics, subtarget and presentation phases. */
void register_color_picker_widget(WidgetRegistry& registry);

} // namespace strata::ui
