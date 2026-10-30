#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/value.hpp"
#include "ui/layout.hpp"
#include "ui/render.hpp"

namespace strata::ui {

class RetainedNode;
class WidgetRegistry;

enum class ChartKind { line, area, bar, scatter, pie, donut };

/** How a numeric axis places values: evenly, or by their logarithm (equal ratios, equal steps). */
enum class ChartAxisScale { linear, log };

/** Where an axis marks its values along its line: nowhere, at its ticks, or at every multiple
 * of each power of ten a log axis passes (a linear axis: fifths of its tick step). */
enum class ChartAxisMarks { none, ticks, all };

/** Where a chart's legend sits: centred below, or in a row above at its start or end. */
enum class ChartLegendPlace { bottom, top_start, top_end };

/** A numeric axis as authored (xAxis, yAxis): its scale, range, ticks and title. */
struct ChartAxisSpec final {
    ChartAxisScale scale = ChartAxisScale::linear;
    std::optional<double> minimum;
    std::optional<double> maximum;
    /** Where ticks and gridlines go; chosen evenly when empty. */
    std::vector<double> ticks;
    std::string title;
    /** Its ticks' labels are drawn. */
    bool labels = true;
    ChartAxisMarks marks = ChartAxisMarks::none;
    /** Space from the axis line to its labels, and from them (or its marks) to its title. */
    std::optional<double> label_gap;
    std::optional<double> title_gap;
};

/** A point at a numeric x, the label drawn above it, and the range around it (an error bar). */
struct ChartXYPoint final {
    double x = 0.0;
    double y = 0.0;
    std::string label;
    std::optional<double> low;
    std::optional<double> high;
};

/**
 * One named row of values, one per category (a missing value leaves a gap), or of points at
 * numeric x.
 */
struct ChartSeries final {
    std::string name;
    std::vector<std::optional<double>> values;
    std::vector<ChartXYPoint> points;
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
    /** Its series are points at numeric x (series `points`), plotted on two numeric axes. */
    bool numeric = false;
    ChartAxisSpec x_axis;
    ChartAxisSpec y_axis;
    ChartLegendPlace legend_place = ChartLegendPlace::bottom;
    /** Lines along the plot's left and bottom edges. */
    bool axes = false;
    double line_width = 2.0;
    /** The points' radius; markers show when set, or with showPoints. */
    std::optional<double> marker_radius;
    std::optional<RenderColor> marker_outline;
    std::optional<RenderColor> grid_color;
    std::optional<RenderColor> axis_color;
    std::optional<RenderColor> error_bar_color;
    /** Space between the legend and the plot; the legend's own default when unset. */
    std::optional<double> legend_gap;
    /**
     * The styles its parts' text is drawn in (resolved style objects, from the node's
     * properties), each over the chart's own: tick labels, axis titles, point labels, legend.
     */
    const runtime::Value* tick_style = nullptr;
    const runtime::Value* title_style = nullptr;
    const runtime::Value* label_style = nullptr;
    const runtime::Value* legend_style = nullptr;
    /**
     * How much has appeared, 0 to 1 (the `reveal` style, animated by an enter): each line drawn
     * along itself that far, its points and their labels appearing as it reaches them.
     */
    double reveal = 1.0;

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

/** A numeric axis laid out: its range and the values its ticks sit at. */
struct ChartNumericAxis final {
    ChartAxisScale scale = ChartAxisScale::linear;
    double minimum = 0.0;
    double maximum = 1.0;
    std::vector<double> ticks;
    /** Where marks go along the axis line. */
    std::vector<double> marks;
    bool labels = true;
    double label_gap = 0.0;
    double title_gap = 0.0;

    /** How far along the axis a value is, 0 at its minimum and 1 at its maximum. */
    [[nodiscard]] double fraction(double value) const noexcept;
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
    /** A numeric chart's axes. */
    ChartNumericAxis x_axis;
    ChartNumericAxis y_axis;
    /** Legend swatches are dots the size of the points (a numeric chart with markers). */
    bool round_swatches = false;

    /** The x of a category's centre. */
    [[nodiscard]] double x(std::size_t index) const noexcept;
};

/** A text's size as the chart draws it: in one of its parts' styles, or its own when null. */
using ChartTextMeasure = std::function<Size(std::string_view text, const runtime::Value* style)>;

[[nodiscard]] ChartProjection chart_projection(const ChartState& state, Rect bounds,
                                               const ChartTextMeasure& text);

/** Registers Chart's description, input, semantics and subtarget phases. */
void register_chart_widget(WidgetRegistry& registry);
/** Registers Chart's presenter with the other built-in presenters. */
void register_chart_widget_presenter(WidgetRegistry& registry);

} // namespace strata::ui
