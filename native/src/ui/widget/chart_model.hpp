#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui/layout.hpp"
#include "ui/render.hpp"

namespace strata::ui {

class RetainedNode;
class WidgetRegistry;

enum class ChartKind { line, area, bar, scatter, pie, donut };

/** One named row of values, one per category; a missing value leaves a gap. */
struct ChartSeries final {
    std::string name;
    std::vector<std::optional<double>> values;
    RenderColor color;
    /** Hidden from its legend entry: not drawn and not scaled. */
    bool hidden = false;
    /** How shown it is while it fades in or out: 1 shown, 0 hidden. */
    double weight = 1.0;
};

/** What a Chart node plots, read from its properties and retained state. */
struct ChartState final {
    ChartKind kind = ChartKind::line;
    std::vector<std::string> labels;
    std::vector<ChartSeries> series;
    /** Categories along the x axis, or slices of a pie. */
    std::size_t count = 0U;
    bool stacked = false;
    bool legend = true;
    bool grid = true;
    bool points = false;
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::string suffix;
    /** Fixed decimals for values, or automatic when absent. */
    std::optional<int> decimals;
    /** The category or slice the keyboard is on. */
    std::optional<std::size_t> cursor;
    /** A pie's slices hidden from the legend (a cartesian chart hides whole series instead). */
    std::vector<bool> hidden_slices;
    /** How shown each slice is while it sweeps open or closed: 1 shown, 0 hidden. */
    std::vector<double> slice_weights;

    [[nodiscard]] bool circular() const noexcept;
    /** The category's label, or its 1-based position when it has none. */
    [[nodiscard]] std::string label(std::size_t index) const;
    /** A pie's slice value: the first series' value at `index`, when positive and shown. */
    [[nodiscard]] std::optional<double> slice_value(std::size_t index) const;
    /** A value as the chart prints it, with its suffix. */
    [[nodiscard]] std::string format(double value) const;
};

[[nodiscard]] ChartState chart_state(const RetainedNode& node);

/** The value axis: its range and the values gridlines and labels sit at. */
struct ChartScale final {
    double minimum = 0.0;
    double maximum = 1.0;
    double step = 0.25;
    std::vector<double> ticks;

    [[nodiscard]] double y(double value, Rect plot) const noexcept;
    [[nodiscard]] std::string tick_label(double value, const ChartState& state) const;
};

struct ChartLegendEntry final {
    /** Series index, or slice index for a pie. */
    std::size_t index = 0U;
    std::string label;
    RenderColor color;
    bool hidden = false;
    Rect bounds;
    Rect swatch;
};

struct ChartSlice final {
    std::size_t index = 0U;
    double value = 0.0;
    double fraction = 0.0;
    /** Degrees clockwise from the top. */
    double start = 0.0;
    double sweep = 0.0;
    RenderColor color;
    /** The slice's outline, for hit testing. */
    std::vector<Point> outline;
    Rect bounds;
};

/** A category's stacked or plain extent: where its bars and points sit for each series. */
struct ChartPoint final {
    std::size_t series = 0U;
    double value = 0.0;
    /** The value's span on the axis: from `base` to `top` (equal to value unless stacked). */
    double base = 0.0;
    double top = 0.0;
};

/** Where a chart draws and hits everything at its current size. */
struct ChartProjection final {
    Rect plot;
    ChartScale scale;
    /** One full-height band per category. */
    std::vector<Rect> bands;
    /** Each category's visible series values, in series order. */
    std::vector<std::vector<ChartPoint>> points;
    /** Every `x_label_step`th category label is drawn, so labels do not collide. */
    std::size_t x_label_step = 1U;
    double gutter = 0.0;
    std::vector<ChartLegendEntry> legend;
    Point center;
    double radius = 0.0;
    double inner_radius = 0.0;
    std::vector<ChartSlice> slices;

    /** The x of a category's centre. */
    [[nodiscard]] double x(std::size_t index) const noexcept;
};

using ChartTextWidth = std::function<double(std::string_view)>;

[[nodiscard]] ChartProjection chart_projection(const ChartState& state, Rect bounds,
                                               const ChartTextWidth& width);

/** Registers Chart's description, input, semantics and subtarget phases. */
void register_chart_widget(WidgetRegistry& registry);
/** Registers Chart's presenter with the other built-in presenters. */
void register_chart_widget_presenter(WidgetRegistry& registry);

} // namespace strata::ui
