#include "ui/widget/chart_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <span>
#include <utility>

#include "ui/input.hpp"
#include "ui/motion/model.hpp"
#include "ui/path.hpp"
#include "ui/text.hpp"
#include "ui/tree.hpp"
#include "ui/widget/description.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/presentation.hpp"
#include "ui/widget/registry.hpp"
#include "ui/widget/semantics.hpp"
#include "ui/widget/subtarget.hpp"

namespace strata::ui {
namespace {

constexpr double chart_padding = 6.0;
constexpr double legend_row_height = 18.0;
constexpr double legend_swatch = 10.0;
constexpr double legend_gap = 22.0;
constexpr double legend_label_gap = 9.0;
constexpr double x_label_height = 18.0;
/** A numeric chart's spacing: plot to tick labels, tick labels to axis titles. */
constexpr double axis_label_gap = 12.0;
constexpr double x_title_gap = 15.0;
constexpr double y_title_gap = 20.0;
/** How far an axis' marks reach out from its line. */
constexpr double axis_mark_length = 5.0;
/** Degrees between the points of a slice outline. */
constexpr double outline_step = 6.0;

[[nodiscard]] const runtime::Value* property(const RetainedNode& node,
                                             const std::string_view name) noexcept {
    const auto found = node.description().properties.find(name);
    return found != node.description().properties.end() ? found->second.value() : nullptr;
}

[[nodiscard]] std::optional<double> finite(const runtime::Value* value) noexcept {
    return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
               ? std::optional<double>(*value->number())
               : std::nullopt;
}

[[nodiscard]] std::optional<RenderColor> color_of(const runtime::Value* value) noexcept {
    return value != nullptr && value->color() != nullptr ? std::optional(*value->color())
                                                         : std::nullopt;
}

[[nodiscard]] ChartKind kind_of(const runtime::Value* value) noexcept {
    const std::string* text = value != nullptr ? value->string() : nullptr;
    if (text == nullptr)
        return ChartKind::line;
    if (*text == "AREA") return ChartKind::area;
    if (*text == "BAR") return ChartKind::bar;
    if (*text == "SCATTER") return ChartKind::scatter;
    if (*text == "PIE") return ChartKind::pie;
    if (*text == "DONUT") return ChartKind::donut;
    return ChartKind::line;
}

/** A numeric axis as a chart's xAxis or yAxis property gives it. */
[[nodiscard]] ChartAxisSpec axis_of(const runtime::Value* value) {
    ChartAxisSpec spec;
    if (value == nullptr || value->object() == nullptr)
        return spec;
    if (const runtime::Value* scale = value->field("scale");
        scale != nullptr && scale->string() != nullptr && *scale->string() == "LOG")
        spec.scale = ChartAxisScale::log;
    spec.minimum = finite(value->field("min"));
    spec.maximum = finite(value->field("max"));
    if (const runtime::Value* ticks = value->field("ticks");
        ticks != nullptr && ticks->list() != nullptr) {
        for (const runtime::Value& entry : ticks->list()->values) {
            if (const std::optional<double> tick = finite(&entry); tick.has_value())
                spec.ticks.push_back(*tick);
        }
    }
    if (const runtime::Value* title = value->field("title");
        title != nullptr && title->string() != nullptr)
        spec.title = *title->string();
    if (const runtime::Value* labels = value->field("labels");
        labels != nullptr && labels->boolean() != nullptr)
        spec.labels = *labels->boolean();
    if (const runtime::Value* marks = value->field("marks");
        marks != nullptr && marks->string() != nullptr) {
        spec.marks = *marks->string() == "ALL"     ? ChartAxisMarks::all
                     : *marks->string() == "TICKS" ? ChartAxisMarks::ticks
                                                   : ChartAxisMarks::none;
    }
    if (const std::optional<double> gap = finite(value->field("labelGap"));
        gap.has_value() && *gap >= 0.0)
        spec.label_gap = *gap;
    if (const std::optional<double> gap = finite(value->field("titleGap"));
        gap.has_value() && *gap >= 0.0)
        spec.title_gap = *gap;
    return spec;
}

/** A style field (from `style`, merged into `$layout`) or the node's own property. */
[[nodiscard]] const runtime::Value* styled(const RetainedNode& node,
                                           const std::string_view name) noexcept {
    if (const runtime::Value* layout = property(node, "$layout");
        layout != nullptr && layout->object() != nullptr) {
        if (const runtime::Value* value = layout->field(name); value != nullptr)
            return value;
    }
    return property(node, name);
}

[[nodiscard]] std::vector<std::optional<double>> values_of(const runtime::Value* value) {
    std::vector<std::optional<double>> result;
    if (value == nullptr || value->list() == nullptr)
        return result;
    result.reserve(value->list()->values.size());
    for (const runtime::Value& entry : value->list()->values)
        result.push_back(finite(&entry));
    return result;
}

/** The series palette names; themes can restyle them like any palette token. */
constexpr std::string_view series_tokens[] = {"chart1", "chart2", "chart3", "chart4",
                                              "chart5", "chart6", "chart7", "chart8"};

/** Default series colours when no presentation scope can resolve the theme's palette. */
constexpr RenderColor fallback_series[] = {
    {96U, 165U, 250U, 255U},  {251U, 146U, 60U, 255U}, {74U, 222U, 128U, 255U},
    {244U, 114U, 182U, 255U}, {167U, 139U, 250U, 255U}, {250U, 204U, 21U, 255U},
    {45U, 212U, 191U, 255U},  {248U, 113U, 113U, 255U},
};

[[nodiscard]] std::string trimmed(const double value, const int decimals) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", std::clamp(decimals, 0, 9), value);
    std::string text = buffer;
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0')
            text.pop_back();
        if (!text.empty() && text.back() == '.')
            text.pop_back();
    }
    return text == "-0" ? "0" : text;
}

/** "12.5k", "3M": a large axis value short enough to sit in the gutter. */
[[nodiscard]] std::string compact(const double value) {
    const double magnitude = std::abs(value);
    if (magnitude >= 1.0e9) return trimmed(value / 1.0e9, 1) + "B";
    if (magnitude >= 1.0e6) return trimmed(value / 1.0e6, 1) + "M";
    if (magnitude >= 1.0e4) return trimmed(value / 1.0e3, 1) + "k";
    return trimmed(value, 2);
}

/** Groups thousands: "12,345.5". */
[[nodiscard]] std::string grouped(const double value, const int decimals) {
    std::string text = trimmed(value, decimals);
    const std::size_t sign = !text.empty() && text.front() == '-' ? 1U : 0U;
    std::size_t end = text.find('.');
    if (end == std::string::npos)
        end = text.size();
    for (std::size_t position = end; position > sign + 3U; position -= 3U)
        text.insert(position - 3U, 1U, ',');
    return text;
}

/** 1, 2, 2.5 or 5 times a power of ten: the step a person would choose. */
[[nodiscard]] double nice_step(const double raw) noexcept {
    if (!(raw > 0.0) || !std::isfinite(raw))
        return 1.0;
    const double power = std::pow(10.0, std::floor(std::log10(raw)));
    const double fraction = raw / power;
    const double nice = fraction <= 1.0 ? 1.0 : fraction <= 2.0 ? 2.0 : fraction <= 2.5 ? 2.5
                        : fraction <= 5.0 ? 5.0 : 10.0;
    return nice * power;
}

[[nodiscard]] Point on_circle(const Point center, const double radius,
                              const double degrees) noexcept {
    const double radians = degrees * std::numbers::pi / 180.0;
    return Point{center.x + radius * std::sin(radians), center.y - radius * std::cos(radians)};
}

[[nodiscard]] std::vector<Point> slice_outline(const Point center, const double radius,
                                               const double inner, const double start,
                                               const double sweep) {
    std::vector<Point> outline;
    const int steps = std::max(1, static_cast<int>(std::ceil(sweep / outline_step)));
    for (int step = 0; step <= steps; ++step)
        outline.push_back(on_circle(center, radius, start + sweep * step / steps));
    if (inner > 0.0) {
        for (int step = steps; step >= 0; --step)
            outline.push_back(on_circle(center, inner, start + sweep * step / steps));
    } else {
        outline.push_back(center);
    }
    return outline;
}

[[nodiscard]] Rect bounding(const std::vector<Point>& points) noexcept {
    if (points.empty())
        return Rect{};
    double left = points.front().x;
    double top = points.front().y;
    double right = left;
    double bottom = top;
    for (const Point point : points) {
        left = std::min(left, point.x);
        top = std::min(top, point.y);
        right = std::max(right, point.x);
        bottom = std::max(bottom, point.y);
    }
    return Rect{left, top, right - left, bottom - top};
}

[[nodiscard]] std::optional<std::size_t> index_after(const std::string_view id,
                                                     const std::string_view prefix) noexcept {
    if (!id.starts_with(prefix))
        return std::nullopt;
    std::size_t index = 0U;
    for (const char digit : id.substr(prefix.size())) {
        if (digit < '0' || digit > '9')
            return std::nullopt;
        index = index * 10U + static_cast<std::size_t>(digit - '0');
    }
    return index;
}

[[nodiscard]] double estimated_width(const std::string_view text) noexcept {
    return static_cast<double>(text.size()) * 7.0;
}


/**
 * The numbers a chart animates, led by a signature of their structure: for a pie, each slice's
 * weight (1 shown, 0 hidden) then the first series' values; otherwise each series' weight followed
 * by its values. Gaps are structure, not numbers, so a series gaining or losing a gap snaps.
 */
[[nodiscard]] std::vector<double> chart_motion_numbers(const runtime::Value* kind,
                                                       const runtime::Value* series,
                                                       const runtime::Value* values,
                                                       const runtime::Value* hidden) {
    std::vector<std::vector<std::optional<double>>> rows;
    if (series != nullptr && series->list() != nullptr) {
        for (const runtime::Value& entry : series->list()->values) {
            if (entry.object() != nullptr)
                rows.push_back(values_of(entry.field("values")));
        }
    } else if (values != nullptr) {
        rows.push_back(values_of(values));
    }
    std::vector<bool> hidden_flags;
    if (hidden != nullptr && hidden->list() != nullptr) {
        for (const runtime::Value& entry : hidden->list()->values) {
            if (const std::optional<double> index = finite(&entry);
                index.has_value() && *index >= 0.0 && *index < 1.0e6) {
                const auto at = static_cast<std::size_t>(*index);
                if (hidden_flags.size() <= at)
                    hidden_flags.resize(at + 1U, false);
                hidden_flags[at] = true;
            }
        }
    }
    const auto shown = [&hidden_flags](const std::size_t index) {
        return index < hidden_flags.size() && hidden_flags[index] ? 0.0 : 1.0;
    };
    const ChartKind chart_kind = kind_of(kind);
    const bool circular = chart_kind == ChartKind::pie || chart_kind == ChartKind::donut;
    std::uint64_t signature = 1469598103934665603ULL;
    const auto mix = [&signature](const std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            signature ^= (value >> shift) & 0xFFU;
            signature *= 1099511628211ULL;
        }
    };
    std::vector<double> numbers{0.0};
    mix(circular ? 1U : 0U);
    mix(rows.size());
    const auto row_numbers = [&](const std::vector<std::optional<double>>& row) {
        mix(row.size());
        for (const std::optional<double>& value : row) {
            mix(value.has_value() ? 1U : 0U);
            if (value.has_value())
                numbers.push_back(*value);
        }
    };
    if (circular) {
        if (!rows.empty()) {
            for (std::size_t index = 0U; index < rows.front().size(); ++index)
                numbers.push_back(shown(index));
            row_numbers(rows.front());
        }
    } else {
        for (std::size_t index = 0U; index < rows.size(); ++index) {
            numbers.push_back(shown(index));
            row_numbers(rows[index]);
        }
    }
    numbers.front() = static_cast<double>(signature >> 12U);
    return numbers;
}

/** Puts animated numbers (chart_motion_numbers' layout, for this state's structure) in `state`. */
void apply_chart_motion(ChartState& state, const std::span<const double> numbers) {
    std::size_t next = 1U;
    const auto take = [&]() { return next < numbers.size() ? numbers[next++] : 0.0; };
    const auto row = [&](ChartSeries& series) {
        for (std::optional<double>& value : series.values) {
            if (value.has_value())
                *value = take();
        }
    };
    if (state.circular()) {
        if (state.series.empty())
            return;
        const std::size_t count = state.series.front().values.size();
        state.slice_weights.assign(count, 1.0);
        state.hidden_slices.resize(std::max(state.hidden_slices.size(), count), false);
        for (std::size_t index = 0U; index < count; ++index) {
            state.slice_weights[index] = std::clamp(take(), 0.0, 1.0);
            state.hidden_slices[index] = state.slice_weights[index] <= 0.0;
        }
        row(state.series.front());
        return;
    }
    for (ChartSeries& series : state.series) {
        series.weight = std::clamp(take(), 0.0, 1.0);
        series.hidden = series.weight <= 0.0;
        row(series);
    }
}

} // namespace

bool ChartState::circular() const noexcept {
    return kind == ChartKind::pie || kind == ChartKind::donut;
}

std::string ChartState::label(const std::size_t index) const {
    return index < labels.size() ? labels[index] : std::to_string(index + 1U);
}

std::optional<double> ChartState::slice_value(const std::size_t index) const {
    if (series.empty() || index >= series.front().values.size())
        return std::nullopt;
    if (index < hidden_slices.size() && hidden_slices[index])
        return std::nullopt;
    std::optional<double> value = series.front().values[index];
    if (value.has_value() && index < slice_weights.size())
        *value *= slice_weights[index];
    return value.has_value() && *value > 0.0 ? value : std::nullopt;
}

std::string ChartState::format(const double value) const {
    return grouped(value, decimals.value_or(2)) + suffix;
}

ChartState chart_state(const RetainedNode& node) {
    ChartState state;
    state.kind = kind_of(property(node, "kind"));
    if (const runtime::Value* labels = property(node, "labels");
        labels != nullptr && labels->list() != nullptr) {
        for (const runtime::Value& entry : labels->list()->values) {
            if (entry.string() != nullptr)
                state.labels.push_back(*entry.string());
            else if (const std::optional<double> number = finite(&entry); number.has_value())
                state.labels.push_back(trimmed(*number, 6));
            else
                state.labels.emplace_back();
        }
    }
    if (const runtime::Value* series = property(node, "series");
        series != nullptr && series->list() != nullptr) {
        for (const runtime::Value& entry : series->list()->values) {
            if (entry.object() == nullptr)
                continue;
            ChartSeries item;
            if (const runtime::Value* name = entry.field("name");
                name != nullptr && name->string() != nullptr)
                item.name = *name->string();
            item.values = values_of(entry.field("values"));
            if (const runtime::Value* points = entry.field("points");
                points != nullptr && points->list() != nullptr) {
                for (const runtime::Value& point : points->list()->values) {
                    const std::optional<double> x = finite(point.field("x"));
                    const std::optional<double> y = finite(point.field("y"));
                    if (!x.has_value() || !y.has_value())
                        continue;
                    const runtime::Value* label = point.field("label");
                    item.points.push_back(ChartXYPoint{
                        *x, *y,
                        label != nullptr && label->string() != nullptr ? *label->string() : "",
                        finite(point.field("low")), finite(point.field("high"))});
                }
                state.numeric = true;
            }
            item.color = color_of(entry.field("color"))
                             .value_or(fallback_series[state.series.size() %
                                                       std::size(fallback_series)]);
            state.series.push_back(std::move(item));
        }
    } else if (const runtime::Value* values = property(node, "values"); values != nullptr) {
        ChartSeries item;
        item.values = values_of(values);
        item.color = fallback_series[0];
        state.series.push_back(std::move(item));
    }
    state.count = state.labels.size();
    for (const ChartSeries& series : state.series)
        state.count = std::max({state.count, series.values.size(), series.points.size()});

    const auto flag = [&node](const std::string_view name, const bool fallback) {
        const runtime::Value* value = property(node, name);
        return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
    };
    state.stacked = flag("stacked", false);
    state.legend = flag("showLegend", state.series.size() > 1U || state.circular());
    state.grid = flag("showGrid", true);
    state.points = flag("showPoints", false);
    state.minimum = finite(property(node, "min"));
    state.maximum = finite(property(node, "max"));
    if (const runtime::Value* suffix = property(node, "valueSuffix");
        suffix != nullptr && suffix->string() != nullptr)
        state.suffix = *suffix->string();
    if (const std::optional<double> decimals = finite(property(node, "decimals"));
        decimals.has_value())
        state.decimals = static_cast<int>(std::clamp(std::floor(*decimals), 0.0, 9.0));
    state.x_axis = axis_of(property(node, "xAxis"));
    state.y_axis = axis_of(property(node, "yAxis"));
    if (!state.y_axis.minimum.has_value())
        state.y_axis.minimum = state.minimum;
    if (!state.y_axis.maximum.has_value())
        state.y_axis.maximum = state.maximum;
    if (const runtime::Value* place = property(node, "legendPosition");
        place != nullptr && place->string() != nullptr) {
        if (*place->string() == "TOP_END")
            state.legend_place = ChartLegendPlace::top_end;
        else if (*place->string() == "TOP_START")
            state.legend_place = ChartLegendPlace::top_start;
    }
    state.axes = flag("showAxes", false);
    if (const std::optional<double> width = finite(property(node, "lineWidth"));
        width.has_value() && *width > 0.0)
        state.line_width = *width;
    if (const std::optional<double> radius = finite(property(node, "markerRadius"));
        radius.has_value() && *radius >= 0.0)
        state.marker_radius = *radius;
    state.marker_outline = color_of(property(node, "markerOutline"));
    state.grid_color = color_of(property(node, "gridColor"));
    state.axis_color = color_of(property(node, "axisColor"));
    state.error_bar_color = color_of(property(node, "errorBarColor"));
    if (const std::optional<double> gap = finite(property(node, "legendGap"));
        gap.has_value() && *gap >= 0.0)
        state.legend_gap = *gap;
    const auto part_style = [&node](const std::string_view name) -> const runtime::Value* {
        const runtime::Value* style = property(node, name);
        return style != nullptr && style->object() != nullptr ? style : nullptr;
    };
    state.tick_style = part_style("tickStyle");
    state.title_style = part_style("titleStyle");
    state.label_style = part_style("labelStyle");
    state.legend_style = part_style("legendStyle");
    if (const std::optional<double> reveal = finite(styled(node, "reveal")); reveal.has_value())
        state.reveal = std::clamp(*reveal, 0.0, 1.0);

    std::vector<bool> hidden;
    if (const runtime::Value* value = node.retained_value("$hidden");
        value != nullptr && value->list() != nullptr) {
        for (const runtime::Value& entry : value->list()->values) {
            if (const std::optional<double> index = finite(&entry);
                index.has_value() && *index >= 0.0 && *index < 1.0e6) {
                const auto at = static_cast<std::size_t>(*index);
                if (hidden.size() <= at)
                    hidden.resize(at + 1U, false);
                hidden[at] = true;
            }
        }
    }
    if (state.circular()) {
        state.hidden_slices = std::move(hidden);
    } else {
        for (std::size_t index = 0U; index < state.series.size() && index < hidden.size(); ++index)
            state.series[index].hidden = hidden[index];
    }
    if (const std::optional<double> cursor = finite(node.retained_value("$cursor"));
        cursor.has_value() && *cursor >= 0.0 && *cursor < static_cast<double>(state.count))
        state.cursor = static_cast<std::size_t>(*cursor);
    return state;
}

double ChartScale::y(const double value, const Rect plot) const noexcept {
    const double span = maximum - minimum;
    const double fraction = span > 0.0 ? (value - minimum) / span : 0.0;
    return plot.bottom() - fraction * plot.height;
}

std::string ChartScale::tick_label(const double value, const ChartState& state) const {
    if (state.decimals.has_value())
        return trimmed(value, *state.decimals) + state.suffix;
    if (std::max(std::abs(minimum), std::abs(maximum)) >= 1.0e4)
        return compact(value) + state.suffix;
    const int decimals = step < 1.0 ? static_cast<int>(std::ceil(-std::log10(step) - 1.0e-9)) : 0;
    return trimmed(value, std::clamp(decimals, 0, 6)) + state.suffix;
}

double ChartNumericAxis::fraction(const double value) const noexcept {
    if (scale == ChartAxisScale::log) {
        if (!(value > 0.0) || !(minimum > 0.0) || !(maximum > minimum))
            return 0.0;
        return (std::log(value) - std::log(minimum)) / (std::log(maximum) - std::log(minimum));
    }
    return maximum > minimum ? (value - minimum) / (maximum - minimum) : 0.0;
}

double ChartProjection::x(const std::size_t index) const noexcept {
    return index < bands.size() ? bands[index].x + bands[index].width * 0.5 : plot.x;
}

namespace {

/** A numeric axis' range and ticks: the authored ones, else the data's, padded, with even ticks. */
[[nodiscard]] ChartNumericAxis numeric_ticks(const ChartAxisSpec& spec, double low, double high,
                                             const double length, const bool zero_based) {
    ChartNumericAxis axis;
    axis.scale = spec.scale;
    const bool log = spec.scale == ChartAxisScale::log;
    for (const double tick : spec.ticks) {
        if (!log || tick > 0.0) {
            low = std::min(low, tick);
            high = std::max(high, tick);
        }
    }
    if (!std::isfinite(low) || !std::isfinite(high)) {
        low = log ? 1.0 : 0.0;
        high = log ? 10.0 : 1.0;
    }
    if (log) {
        low = std::max(low, 1.0e-12);
        high = std::max(high, low * 1.0001);
        if (!spec.minimum.has_value())
            low /= 1.15;
        if (!spec.maximum.has_value())
            high *= 1.15;
    } else if (zero_based) {
        low = std::min(low, 0.0);
    }
    if (spec.minimum.has_value() && (!log || *spec.minimum > 0.0))
        low = *spec.minimum;
    if (spec.maximum.has_value() && (!log || *spec.maximum > low))
        high = *spec.maximum;
    if (high <= low)
        high = log ? low * 10.0 : low + 1.0;
    axis.minimum = low;
    axis.maximum = high;
    if (!spec.ticks.empty()) {
        for (const double tick : spec.ticks) {
            if (tick >= low - 1.0e-9 && tick <= high + 1.0e-9 && (!log || tick > 0.0))
                axis.ticks.push_back(tick);
        }
        return axis;
    }
    if (log) {
        // 1, 2 and 5 times each power of ten in range.
        for (int power = static_cast<int>(std::floor(std::log10(low)));
             power <= static_cast<int>(std::ceil(std::log10(high))) && axis.ticks.size() < 60U;
             ++power) {
            for (const double step : {1.0, 2.0, 5.0}) {
                const double tick = step * std::pow(10.0, power);
                if (tick >= low * (1.0 - 1.0e-9) && tick <= high * (1.0 + 1.0e-9))
                    axis.ticks.push_back(tick);
            }
        }
        return axis;
    }
    const double target = std::clamp(std::floor(length / 64.0), 2.0, 10.0);
    const double step = nice_step((high - low) / target);
    if (!spec.minimum.has_value())
        axis.minimum = std::floor(low / step + 1.0e-9) * step;
    if (!spec.maximum.has_value())
        axis.maximum = std::ceil(high / step - 1.0e-9) * step;
    for (double tick = std::ceil(axis.minimum / step - 1.0e-9) * step;
         tick <= axis.maximum + step * 1.0e-6 && axis.ticks.size() < 50U; tick += step)
        axis.ticks.push_back(std::abs(tick) < step * 1.0e-9 ? 0.0 : tick);
    return axis;
}

/**
 * A numeric axis laid out: numeric_ticks' range and ticks, its marks, and its gaps (the authored
 * ones, else `title_gap` for its title).
 */
[[nodiscard]] ChartNumericAxis numeric_axis(const ChartAxisSpec& spec, const double low,
                                            const double high, const double length,
                                            const bool zero_based, const double title_gap) {
    ChartNumericAxis axis = numeric_ticks(spec, low, high, length, zero_based);
    axis.labels = spec.labels;
    axis.label_gap = spec.label_gap.value_or(axis_label_gap);
    axis.title_gap = spec.title_gap.value_or(title_gap);
    if (spec.marks == ChartAxisMarks::ticks) {
        axis.marks = axis.ticks;
    } else if (spec.marks == ChartAxisMarks::all && axis.scale == ChartAxisScale::log) {
        for (int power = static_cast<int>(std::floor(std::log10(axis.minimum)));
             power <= static_cast<int>(std::ceil(std::log10(axis.maximum))) &&
             axis.marks.size() < 400U;
             ++power) {
            for (int multiple = 1; multiple <= 9; ++multiple) {
                const double mark = multiple * std::pow(10.0, power);
                if (mark >= axis.minimum * (1.0 - 1.0e-9) && mark <= axis.maximum * (1.0 + 1.0e-9))
                    axis.marks.push_back(mark);
            }
        }
    } else if (spec.marks == ChartAxisMarks::all && axis.ticks.size() >= 2U) {
        const double step = (axis.ticks[1] - axis.ticks[0]) / 5.0;
        const double first = axis.ticks.front() - step * std::floor((axis.ticks.front() - axis.minimum) / step + 1.0e-9);
        for (double mark = first; mark <= axis.maximum + step * 1.0e-6 && axis.marks.size() < 400U;
             mark += step)
            axis.marks.push_back(mark);
    }
    return axis;
}

/** A tick's label: on the y axis with the chart's decimals and suffix. */
[[nodiscard]] std::string chart_axis_label(const double value, const ChartState& state,
                                           const bool vertical) {
    if (vertical && state.decimals.has_value())
        return trimmed(value, *state.decimals) + state.suffix;
    return trimmed(value, std::abs(value) < 10.0 ? 2 : 1) + (vertical ? state.suffix : "");
}

void numeric_projection(const ChartState& state, const Rect area, const ChartTextMeasure& measure,
                        ChartProjection& projection) {
    const double tick_height = measure("0", state.tick_style).height;
    const double title_height = measure("0", state.title_style).height;
    double x_low = std::numeric_limits<double>::infinity();
    double x_high = -std::numeric_limits<double>::infinity();
    double y_low = std::numeric_limits<double>::infinity();
    double y_high = -std::numeric_limits<double>::infinity();
    for (const ChartSeries& series : state.series) {
        if (series.hidden)
            continue;
        for (const ChartXYPoint& point : series.points) {
            if (state.x_axis.scale != ChartAxisScale::log || point.x > 0.0) {
                x_low = std::min(x_low, point.x);
                x_high = std::max(x_high, point.x);
            }
            y_low = std::min({y_low, point.y, point.low.value_or(point.y)});
            y_high = std::max({y_high, point.y, point.high.value_or(point.y)});
        }
    }
    // Room for the axis titles, the x labels (or marks) and the y labels' gutter, then the plot.
    const ChartAxisSpec& x_spec = state.x_axis;
    const ChartAxisSpec& y_spec = state.y_axis;
    const double x_marks = x_spec.marks != ChartAxisMarks::none ? axis_mark_length : 0.0;
    const double y_marks = y_spec.marks != ChartAxisMarks::none ? axis_mark_length : 0.0;
    double left = area.x;
    double bottom = area.bottom();
    if (!y_spec.title.empty())
        left += title_height + y_spec.title_gap.value_or(y_title_gap);
    if (!x_spec.title.empty())
        bottom -= title_height + x_spec.title_gap.value_or(x_title_gap);
    bottom -= x_spec.labels ? tick_height + x_spec.label_gap.value_or(axis_label_gap) : x_marks;
    const double top = area.y;
    projection.y_axis = numeric_axis(y_spec, y_low, y_high, std::max(1.0, bottom - top), true,
                                     y_title_gap);
    double gutter = 0.0;
    for (const double tick : projection.y_axis.ticks)
        gutter = std::max(gutter, measure(chart_axis_label(tick, state, true), state.tick_style).width);
    projection.gutter = y_spec.labels ? gutter + projection.y_axis.label_gap : y_marks;
    left += projection.gutter;
    // The right keeps room for half a label centred under a tick at the plot's end.
    projection.plot = Rect{left, top, std::max(1.0, area.right() - 14.0 - left),
                           std::max(1.0, bottom - top)};
    projection.x_axis = numeric_axis(x_spec, x_low, x_high, projection.plot.width, false,
                                     x_title_gap);
}

} // namespace

ChartProjection chart_projection(const ChartState& state, const Rect bounds,
                                 const ChartTextMeasure& text) {
    ChartProjection projection;
    const ChartTextMeasure measure = [&text](const std::string_view value,
                                             const runtime::Value* style) {
        return text ? text(value, style) : Size{estimated_width(value), 14.0};
    };
    Rect area{bounds.x + chart_padding, bounds.y + chart_padding,
              std::max(0.0, bounds.width - 2.0 * chart_padding),
              std::max(0.0, bounds.height - 2.0 * chart_padding)};

    // The legend wraps its entries into rows: centred along the bottom, or above the plot at its
    // start or end.
    projection.round_swatches =
        state.numeric && (state.marker_radius.has_value() || state.points ||
                          state.kind == ChartKind::scatter);
    // Round swatches are the legend text's size, and never smaller than the points.
    const double swatch =
        projection.round_swatches
            ? std::max({legend_swatch, 2.0 * state.marker_radius.value_or(5.0),
                        0.8 * measure("0", state.legend_style).height})
            : legend_swatch;
    if (state.legend) {
        std::vector<ChartLegendEntry> entries;
        if (state.circular()) {
            for (std::size_t index = 0U; index < state.count; ++index) {
                const bool hidden =
                    index < state.hidden_slices.size() && state.hidden_slices[index];
                entries.push_back(ChartLegendEntry{
                    index, state.label(index),
                    fallback_series[index % std::size(fallback_series)], hidden, {}, {}});
            }
        } else {
            for (std::size_t index = 0U; index < state.series.size(); ++index) {
                const ChartSeries& series = state.series[index];
                entries.push_back(ChartLegendEntry{
                    index, series.name.empty() ? "Series " + std::to_string(index + 1U)
                                               : series.name,
                    series.color, series.hidden, {}, {}});
            }
        }
        std::vector<std::vector<ChartLegendEntry>> rows(1U);
        double row_width = 0.0;
        for (ChartLegendEntry& entry : entries) {
            const double entry_width =
                swatch + legend_label_gap + measure(entry.label, state.legend_style).width;
            const double needed = row_width == 0.0 ? entry_width : row_width + legend_gap + entry_width;
            if (needed > area.width && !rows.back().empty()) {
                rows.emplace_back();
                row_width = entry_width;
            } else {
                row_width = needed;
            }
            entry.bounds.width = entry_width;
            rows.back().push_back(std::move(entry));
        }
        const double row_height =
            std::max({legend_row_height, swatch + 4.0, measure("0", state.legend_style).height});
        const double legend_height = static_cast<double>(rows.size()) * row_height;
        const bool top = state.legend_place != ChartLegendPlace::bottom;
        double y = top ? area.y : area.bottom() - legend_height;
        for (std::vector<ChartLegendEntry>& row : rows) {
            double total = 0.0;
            for (const ChartLegendEntry& entry : row)
                total += entry.bounds.width + (total > 0.0 ? legend_gap : 0.0);
            double x = state.legend_place == ChartLegendPlace::top_start ? area.x
                       : state.legend_place == ChartLegendPlace::top_end
                           ? area.right() - total
                           : area.x + std::max(0.0, (area.width - total) * 0.5);
            for (ChartLegendEntry& entry : row) {
                entry.bounds = Rect{x, y, entry.bounds.width, row_height};
                entry.swatch = Rect{x, y + (row_height - swatch) * 0.5, swatch, swatch};
                x += entry.bounds.width + legend_gap;
                projection.legend.push_back(std::move(entry));
            }
            y += row_height;
        }
        const double gap = state.legend_gap.value_or(top ? 12.0 : 6.0);
        if (top)
            area.y += legend_height + gap;
        area.height = std::max(0.0, area.height - legend_height - gap);
    }

    if (state.circular()) {
        const double side = std::min(area.width, area.height);
        projection.center = Point{area.x + area.width * 0.5, area.y + area.height * 0.5};
        projection.radius = std::max(0.0, side * 0.5 - 2.0);
        projection.inner_radius = state.kind == ChartKind::donut ? projection.radius * 0.6 : 0.0;
        projection.plot = Rect{projection.center.x - projection.radius,
                               projection.center.y - projection.radius, projection.radius * 2.0,
                               projection.radius * 2.0};
        double total = 0.0;
        for (std::size_t index = 0U; index < state.count; ++index)
            total += state.slice_value(index).value_or(0.0);
        double start = 0.0;
        for (std::size_t index = 0U; index < state.count && total > 0.0; ++index) {
            const std::optional<double> value = state.slice_value(index);
            if (!value.has_value())
                continue;
            ChartSlice slice;
            slice.index = index;
            slice.value = *value;
            slice.fraction = *value / total;
            slice.start = start;
            slice.sweep = 360.0 * slice.fraction;
            slice.color = fallback_series[index % std::size(fallback_series)];
            slice.outline = slice_outline(projection.center, projection.radius,
                                          projection.inner_radius, slice.start, slice.sweep);
            slice.bounds = bounding(slice.outline);
            start += slice.sweep;
            projection.slices.push_back(std::move(slice));
        }
        return projection;
    }

    if (state.numeric) {
        numeric_projection(state, area, measure, projection);
        return projection;
    }

    // The value range covers what is shown: each value, or each category's stacked totals.
    double low = std::numeric_limits<double>::infinity();
    double high = -std::numeric_limits<double>::infinity();
    projection.points.resize(state.count);
    for (std::size_t category = 0U; category < state.count; ++category) {
        double positive = 0.0;
        double negative = 0.0;
        for (std::size_t index = 0U; index < state.series.size(); ++index) {
            const ChartSeries& series = state.series[index];
            if (series.hidden || category >= series.values.size() ||
                !series.values[category].has_value())
                continue;
            const double value = *series.values[category];
            ChartPoint point{index, value, 0.0, value};
            if (state.stacked) {
                double& accumulated = value >= 0.0 ? positive : negative;
                point.base = accumulated;
                point.top = accumulated + value;
                accumulated = point.top;
            }
            low = std::min({low, point.top, state.stacked ? point.base : point.top});
            high = std::max({high, point.top, state.stacked ? point.base : point.top});
            projection.points[category].push_back(point);
        }
    }
    if (!std::isfinite(low)) {
        low = 0.0;
        high = 1.0;
    }
    if (state.kind == ChartKind::bar || state.kind == ChartKind::area) {
        low = std::min(low, 0.0);
        high = std::max(high, 0.0);
    }
    if (state.minimum.has_value()) low = *state.minimum;
    if (state.maximum.has_value()) high = *state.maximum;
    if (high <= low) {
        const double spread = std::max(1.0, std::abs(low) * 0.1);
        if (!state.minimum.has_value()) low -= spread;
        if (!state.maximum.has_value() || high <= low) high = low + 2.0 * spread;
    }

    projection.plot.y = area.y + 6.0;
    projection.plot.height = std::max(
        1.0, area.bottom() - std::max(x_label_height, measure("0", state.tick_style).height) -
                 projection.plot.y);
    const double target = std::clamp(std::floor(projection.plot.height / 44.0), 2.0, 8.0);
    ChartScale& scale = projection.scale;
    scale.step = nice_step((high - low) / target);
    scale.minimum = state.minimum.value_or(std::floor(low / scale.step + 1.0e-9) * scale.step);
    scale.maximum = state.maximum.value_or(std::ceil(high / scale.step - 1.0e-9) * scale.step);
    if (scale.maximum <= scale.minimum)
        scale.maximum = scale.minimum + scale.step;
    for (double tick = std::ceil(scale.minimum / scale.step - 1.0e-9) * scale.step;
         tick <= scale.maximum + scale.step * 1.0e-6 && scale.ticks.size() < 50U;
         tick += scale.step) {
        scale.ticks.push_back(std::abs(tick) < scale.step * 1.0e-9 ? 0.0 : tick);
    }

    double gutter = 0.0;
    for (const double tick : scale.ticks)
        gutter = std::max(gutter, measure(scale.tick_label(tick, state), state.tick_style).width);
    projection.gutter = gutter + 8.0;
    projection.plot.x = area.x + projection.gutter;
    projection.plot.width = std::max(1.0, area.right() - 4.0 - projection.plot.x);
    const double band = state.count > 0U ? projection.plot.width / static_cast<double>(state.count)
                                         : projection.plot.width;
    for (std::size_t index = 0U; index < state.count; ++index) {
        projection.bands.push_back(Rect{projection.plot.x + band * static_cast<double>(index),
                                        projection.plot.y, band, projection.plot.height});
    }
    double widest = 0.0;
    for (std::size_t index = 0U; index < state.count; ++index)
        widest = std::max(widest, measure(state.label(index), state.tick_style).width);
    projection.x_label_step =
        band > 0.0 ? std::max<std::size_t>(1U, static_cast<std::size_t>(
                                                   std::ceil((widest + 8.0) / band)))
                   : 1U;
    return projection;
}

namespace {

/** The category or slice to inspect: the one under the pointer, else the keyboard's. */
[[nodiscard]] std::optional<std::size_t> inspected(const WidgetRenderScope& scope,
                                                   const ChartState& state) {
    if (const std::optional<std::string_view> hovered =
            scope.input().hovered_subtarget(scope.node().identity());
        hovered.has_value()) {
        if (std::optional<std::size_t> index = index_after(*hovered, "point:"); index.has_value())
            return index;
        if (std::optional<std::size_t> index = index_after(*hovered, "slice:"); index.has_value())
            return index;
    }
    return scope.focused() && scope.focus_visible() ? state.cursor : std::nullopt;
}

[[nodiscard]] RenderColor with_alpha(RenderColor color, const double alpha) noexcept {
    color.alpha = static_cast<std::uint8_t>(
        std::clamp(std::lround(static_cast<double>(color.alpha) * alpha), 0L, 255L));
    return color;
}

/** Resolves each series' and slice's colour through the theme's chart palette. */
void themed_colors(const WidgetRenderScope& scope, ChartState& state, const RetainedNode& node) {
    const runtime::Value* series = property(node, "series");
    for (std::size_t index = 0U; index < state.series.size(); ++index) {
        const runtime::Value* entry =
            series != nullptr && series->list() != nullptr && index < series->list()->values.size()
                ? &series->list()->values[index]
                : nullptr;
        if (entry == nullptr || color_of(entry->field("color")) == std::nullopt)
            state.series[index].color =
                scope.palette(series_tokens[index % std::size(series_tokens)]);
    }
}

[[nodiscard]] RenderColor slice_color(const WidgetRenderScope& scope, const std::size_t index) {
    return scope.palette(series_tokens[index % std::size(series_tokens)]);
}

/** A run of points drawn as one line: a missing value starts a new run. */
[[nodiscard]] std::vector<std::vector<Point>> runs(const ChartState& state,
                                                   const ChartProjection& projection,
                                                   const std::size_t series) {
    std::vector<std::vector<Point>> result(1U);
    for (std::size_t category = 0U; category < state.count; ++category) {
        const auto& points = projection.points[category];
        const auto found = std::ranges::find(points, series, &ChartPoint::series);
        if (found == points.end()) {
            if (!result.back().empty())
                result.emplace_back();
            continue;
        }
        result.back().push_back(
            Point{projection.x(category), projection.scale.y(found->top, projection.plot)});
    }
    if (result.back().empty())
        result.pop_back();
    return result;
}

void stroke_polyline(WidgetRenderScope& scope, const std::vector<Point>& points,
                     const RenderColor color, const double width) {
    StrokeStyle style;
    style.width = width;
    style.cap = PathCap::round;
    style.join = PathJoin::round;
    scope.polyline(points, Paint(color), style);
}

void dot(WidgetRenderScope& scope, const Point center, const double radius,
         const RenderColor fill, const std::optional<RenderColor> ring = std::nullopt) {
    StrokeStyle style;
    style.width = 2.0;
    scope.circle(center, radius, Paint(fill),
                 ring.has_value() ? std::optional<Paint>(Paint(*ring)) : std::nullopt, style);
}

[[nodiscard]] double text_height(const WidgetRenderScope& scope) {
    if (scope.text_engine() == nullptr)
        return 14.0;
    return scope.text_engine()->shape(scope.node(), "0").metrics.height;
}

/** A text's size in one of the chart's parts' styles, or its own when `style` is null. */
[[nodiscard]] Size chart_text_size(const WidgetRenderScope& scope, const std::string_view text,
                                   const runtime::Value* style) {
    if (scope.text_engine() == nullptr)
        return Size{estimated_width(text), 14.0};
    if (style == nullptr) {
        const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), text);
        return Size{shaped.metrics.width, shaped.metrics.height};
    }
    const TextLayout layout =
        scope.text_engine()->layout(scope.node(), text, TextLayoutOptions{.style = style});
    return Size{layout.shaped.metrics.width, layout.shaped.metrics.height};
}

/** A part style's own colour, if it sets one. */
[[nodiscard]] std::optional<RenderColor> style_color(const runtime::Value* style) {
    return style != nullptr ? color_of(style->field("color")) : std::nullopt;
}

/** Text in one of the chart's parts' styles (its own when `style` is null), aligned in `width`. */
void chart_text(WidgetRenderScope& scope, const std::string_view text, const runtime::Value* style,
                const Point origin, const RenderColor color, const double width = 0.0,
                const WidgetTextAlignment alignment = WidgetTextAlignment::start) {
    if (style == nullptr || scope.text_engine() == nullptr) {
        scope.text(text, origin, color, width, alignment);
        return;
    }
    if (text.empty())
        return;
    const TextLayout layout =
        scope.text_engine()->layout(scope.node(), text, TextLayoutOptions{.style = style});
    const double extent = layout.shaped.metrics.width;
    const double offset = alignment == WidgetTextAlignment::center ? std::max(0.0, (width - extent) * 0.5)
                          : alignment == WidgetTextAlignment::end ? std::max(0.0, width - extent)
                                                                  : 0.0;
    scope.text(layout, Point{origin.x + offset, origin.y}, color);
}

/** A floating box listing `lines`, beside `anchor` and kept inside `limits`. */
void tooltip(WidgetRenderScope& scope, const ChartTextMeasure& measure, const Point anchor,
             const Rect limits, const std::string& title,
             const std::vector<std::pair<RenderColor, std::string>>& lines) {
    const double line_height = text_height(scope) + 4.0;
    double width = measure(title, nullptr).width;
    for (const auto& [color, text] : lines)
        width = std::max(width, measure(text, nullptr).width + 14.0);
    width += 16.0;
    const double height = line_height * static_cast<double>(lines.size() + 1U) + 10.0;
    double x = anchor.x + 12.0;
    if (x + width > limits.right())
        x = anchor.x - 12.0 - width;
    x = std::clamp(x, limits.x, std::max(limits.x, limits.right() - width));
    const double y = std::clamp(anchor.y - height * 0.5, limits.y,
                                std::max(limits.y, limits.bottom() - height));
    const Rect box{x, y, width, height};
    scope.shadow(box, CornerRadii::all(6.0), RenderColor{0U, 0U, 0U, 70U}, 10.0);
    scope.rounded_rect(box, Paint(scope.palette("popupSurface")),
                       RenderBorder{1.0, with_alpha(scope.visual().foreground, 0.12), true}, 6.0);
    const RenderColor ink = scope.visual().foreground;
    scope.text(title, Point{box.x + 8.0, box.y + 5.0}, ink);
    double line_y = box.y + 5.0 + line_height;
    for (const auto& [color, text] : lines) {
        scope.rounded_rect(Rect{box.x + 8.0, line_y + (line_height - 4.0) * 0.5 - 4.0, 8.0, 8.0},
                           Paint(color), std::nullopt, 2.0);
        scope.text(text, Point{box.x + 22.0, line_y}, ink);
        line_y += line_height;
    }
}

void legend(WidgetRenderScope& scope, const ChartProjection& projection, const ChartState& state,
            const std::optional<RenderColor> outline = std::nullopt) {
    const RenderColor ink = style_color(state.legend_style).value_or(with_alpha(scope.visual().foreground, 0.85));
    const double height = chart_text_size(scope, "0", state.legend_style).height;
    for (const ChartLegendEntry& entry : projection.legend) {
        const std::string id = "legend:" + std::to_string(entry.index);
        scope.interaction(entry.bounds, id);
        const double alpha = entry.hidden ? 0.35 : 1.0;
        if (projection.round_swatches) {
            StrokeStyle ring;
            ring.width = 1.0;
            scope.circle(Point{entry.swatch.x + entry.swatch.width * 0.5,
                               entry.swatch.y + entry.swatch.height * 0.5},
                         entry.swatch.width * 0.5 - (outline.has_value() ? 0.5 : 0.0),
                         Paint(with_alpha(entry.color, alpha)),
                         outline.has_value()
                             ? std::optional<Paint>(Paint(with_alpha(*outline, alpha)))
                             : std::nullopt,
                         ring);
        } else {
            scope.rounded_rect(entry.swatch, Paint(with_alpha(entry.color, alpha)), std::nullopt,
                               2.0);
        }
        chart_text(scope, entry.label, state.legend_style,
                   Point{entry.swatch.right() + legend_label_gap,
                         entry.bounds.y + (entry.bounds.height - height) * 0.5},
                   with_alpha(ink, entry.hidden ? 0.5 : 1.0));
    }
}

void circular_content(WidgetRenderScope& scope, ChartState& state, ChartProjection& projection,
                      const ChartTextMeasure& measure) {
    for (ChartLegendEntry& entry : projection.legend)
        entry.color = slice_color(scope, entry.index);
    const std::optional<std::size_t> focus = inspected(scope, state);
    const RenderColor ink = scope.visual().foreground;
    // Slices part with a hairline of empty angle and all keep one radius; inspecting one dims
    // the others rather than resizing anything.
    const double inner = projection.inner_radius;
    const double gap_degrees =
        projection.slices.size() > 1U
            ? 1.5 / std::max(1.0, (projection.radius + inner) * 0.5) * 180.0 / std::numbers::pi
            : 0.0;
    const double outer = projection.radius;
    for (ChartSlice& slice : projection.slices) {
        slice.color = slice_color(scope, slice.index);
        const double pad = std::min(gap_degrees, slice.sweep * 0.25);
        const double sweep = std::max(0.0, slice.sweep - pad);
        if (sweep <= 0.0 || outer <= 0.0)
            continue;
        const RenderColor paint =
            focus.has_value() && *focus != slice.index ? with_alpha(slice.color, 0.4) : slice.color;
        scope.sector(projection.center, outer, inner, slice.start + pad * 0.5 - 90.0, sweep,
                     Paint(paint));
    }
    if (projection.inner_radius > 0.0) {
        // The ring's middle names the inspected slice, or the total.
        double total = 0.0;
        for (const ChartSlice& slice : projection.slices)
            total += slice.value;
        const auto found = focus.has_value()
                               ? std::ranges::find(projection.slices, *focus, &ChartSlice::index)
                               : projection.slices.end();
        const std::string headline =
            found != projection.slices.end() ? state.format(found->value) : state.format(total);
        const std::string caption =
            found != projection.slices.end() ? state.label(found->index) : std::string("Total");
        const double height = text_height(scope);
        const double width = projection.inner_radius * 2.0;
        scope.text(headline,
                   Point{projection.center.x - projection.inner_radius,
                         projection.center.y - height},
                   ink, width, WidgetTextAlignment::center);
        scope.text(caption,
                   Point{projection.center.x - projection.inner_radius, projection.center.y + 2.0},
                   with_alpha(ink, 0.6), width, WidgetTextAlignment::center);
    } else if (focus.has_value()) {
        const auto found = std::ranges::find(projection.slices, *focus, &ChartSlice::index);
        if (found != projection.slices.end()) {
            char percent[32];
            std::snprintf(percent, sizeof(percent), "%.1f%%", found->fraction * 100.0);
            tooltip(scope, measure,
                    on_circle(projection.center, projection.radius * 0.6,
                              found->start + found->sweep * 0.5),
                    scope.layout().bounds, state.label(found->index),
                    {{found->color, state.format(found->value) + "  (" + percent + ")"}});
        }
    }
    legend(scope, projection, state);
}

/**
 * A chart of points at numeric x: gridlines and axis lines, tick labels and axis titles, then
 * each series' line drawn along itself as far as `reveal` has it, and its points (with their
 * labels) appearing as the line reaches them.
 */
void numeric_content(WidgetRenderScope& scope, ChartState& state, ChartProjection& projection,
                     const ChartTextMeasure& measure) {
    themed_colors(scope, state, scope.node());
    for (ChartLegendEntry& entry : projection.legend)
        entry.color = state.series[entry.index].color;
    const Rect plot = projection.plot;
    const RenderColor ink = scope.visual().foreground;
    const RenderColor ticks = style_color(state.tick_style).value_or(with_alpha(ink, 0.55));
    const RenderColor titles = style_color(state.title_style).value_or(ink);
    const RenderColor labels = style_color(state.label_style).value_or(ink);
    const RenderColor grid = state.grid_color.value_or(scope.palette("chartGrid"));
    const RenderColor axis = state.axis_color.value_or(with_alpha(ink, 0.6));
    const double tick_height = measure("0", state.tick_style).height;
    const double title_height = measure("0", state.title_style).height;
    const double label_height = measure("0", state.label_style).height;
    const ChartNumericAxis& x_axis = projection.x_axis;
    const ChartNumericAxis& y_axis = projection.y_axis;
    const auto x_of = [&](const double value) {
        return plot.x + x_axis.fraction(value) * plot.width;
    };
    const auto y_of = [&](const double value) {
        return plot.bottom() - y_axis.fraction(value) * plot.height;
    };

    for (const double tick : y_axis.ticks) {
        // A line at y lights the pixel row it falls in.
        const double y = std::floor(y_of(tick));
        if (state.grid && !(state.axes && std::abs(y - std::floor(plot.bottom())) < 0.5))
            scope.solid_rect(Rect{plot.x, y, plot.width, 1.0}, Paint(grid));
        if (y_axis.labels) {
            chart_text(scope, chart_axis_label(tick, state, true), state.tick_style,
                       Point{plot.x - projection.gutter, y + 0.5 - tick_height * 0.5}, ticks,
                       projection.gutter - y_axis.label_gap, WidgetTextAlignment::end);
        }
    }
    if (state.axes) {
        scope.solid_rect(Rect{std::floor(plot.x), plot.y, 1.0, plot.height + 1.0}, Paint(axis));
        scope.solid_rect(Rect{plot.x, std::floor(plot.bottom()), plot.width, 1.0}, Paint(axis));
    }
    // Marks reach out from each axis line: down from the x axis, left from the y axis.
    for (const double mark : x_axis.marks) {
        scope.solid_rect(Rect{std::floor(x_of(mark)), std::floor(plot.bottom()) + 1.0, 1.0,
                              axis_mark_length},
                         Paint(axis));
    }
    for (const double mark : y_axis.marks) {
        scope.solid_rect(Rect{std::floor(plot.x) - axis_mark_length, std::floor(y_of(mark)),
                              axis_mark_length, 1.0},
                         Paint(axis));
    }
    if (x_axis.labels) {
        for (const double tick : x_axis.ticks) {
            const std::string text = chart_axis_label(tick, state, false);
            const double width = measure(text, state.tick_style).width + 8.0;
            chart_text(scope, text, state.tick_style,
                       Point{x_of(tick) - width * 0.5, plot.bottom() + x_axis.label_gap}, ticks,
                       width, WidgetTextAlignment::center);
        }
    }
    if (!state.x_axis.title.empty()) {
        const double under = x_axis.labels ? x_axis.label_gap + tick_height
                             : x_axis.marks.empty() ? 0.0
                                                    : axis_mark_length;
        chart_text(scope, state.x_axis.title, state.title_style,
                   Point{plot.x, plot.bottom() + under + x_axis.title_gap}, titles, plot.width,
                   WidgetTextAlignment::center);
    }
    if (!state.y_axis.title.empty()) {
        // Turned a quarter to read upward, centred on the plot's height left of the labels.
        const Point centre{plot.x - projection.gutter - y_axis.title_gap - title_height * 0.5,
                           plot.y + plot.height * 0.5};
        scope.push_transform(TransformPushRenderCommand{0.0, 1.0, centre.x - centre.y, -1.0, 0.0,
                                                        centre.y + centre.x});
        chart_text(scope, state.y_axis.title, state.title_style,
                   Point{centre.x - plot.height * 0.5, centre.y - title_height * 0.5}, titles,
                   plot.height, WidgetTextAlignment::center);
        scope.pop_transform();
    }

    // Each line as far along itself as the reveal has drawn it.
    const double reveal = std::clamp(state.reveal, 0.0, 1.0);
    const bool markers = state.marker_radius.has_value() || state.points ||
                         state.kind == ChartKind::scatter;
    const double radius = state.marker_radius.value_or(state.kind == ChartKind::scatter ? 4.0 : 3.5);
    struct Drawn final {
        std::vector<Point> points;
        std::vector<double> arrivals;
        double drawn = 0.0;
    };
    std::vector<Drawn> drawn(state.series.size());
    for (std::size_t index = 0U; index < state.series.size(); ++index) {
        const ChartSeries& series = state.series[index];
        if (series.hidden)
            continue;
        Drawn& line = drawn[index];
        double length = 0.0;
        for (const ChartXYPoint& point : series.points) {
            const Point at{x_of(point.x), y_of(point.y)};
            if (!line.points.empty()) {
                length += std::hypot(at.x - line.points.back().x, at.y - line.points.back().y);
            }
            line.points.push_back(at);
            line.arrivals.push_back(length);
        }
        line.drawn = length * reveal;
    }
    // How far a point has appeared, 0 to 1, as its line comes within a few radii of it.
    const double ramp = std::max(1.0, radius * 3.0);
    const auto appeared = [&](const Drawn& line, const std::size_t point) {
        const double appear =
            reveal >= 1.0 ? 1.0
                          : std::clamp((line.drawn - line.arrivals[point]) / ramp + 1.0, 0.0,
                                       1.0) * (reveal > 0.0 ? 1.0 : 0.0);
        return appear * appear * (3.0 - 2.0 * appear);
    };
    // Error bars under everything, each growing out of its point as the point appears.
    for (std::size_t index = 0U; index < state.series.size(); ++index) {
        const ChartSeries& series = state.series[index];
        if (series.hidden)
            continue;
        const RenderColor bar =
            with_alpha(state.error_bar_color.value_or(with_alpha(series.color, 0.35)),
                       series.weight);
        for (std::size_t point = 0U; point < series.points.size(); ++point) {
            const ChartXYPoint& value = series.points[point];
            if (!value.low.has_value() && !value.high.has_value())
                continue;
            const double eased = appeared(drawn[index], point);
            if (eased <= 0.0)
                continue;
            const Point at = drawn[index].points[point];
            const double top = at.y + (y_of(value.high.value_or(value.y)) - at.y) * eased;
            const double bottom = at.y + (y_of(value.low.value_or(value.y)) - at.y) * eased;
            scope.solid_rect(Rect{std::floor(at.x), top, 1.0, std::max(0.0, bottom - top)},
                             Paint(bar));
        }
    }
    // The first series is the one to read: it draws last, over the others.
    for (std::size_t index = state.series.size(); index-- > 0U;) {
        const ChartSeries& series = state.series[index];
        if (series.hidden)
            continue;
        Drawn& line = drawn[index];
        if (state.kind == ChartKind::scatter || line.points.size() < 2U || line.drawn <= 0.0)
            continue;
        std::vector<Point> partial{line.points.front()};
        for (std::size_t point = 1U; point < line.points.size(); ++point) {
            if (line.arrivals[point] <= line.drawn) {
                partial.push_back(line.points[point]);
                continue;
            }
            const double span = line.arrivals[point] - line.arrivals[point - 1U];
            const double t = span > 0.0 ? (line.drawn - line.arrivals[point - 1U]) / span : 0.0;
            const Point from = line.points[point - 1U];
            const Point to = line.points[point];
            partial.push_back(Point{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t});
            break;
        }
        stroke_polyline(scope, partial, with_alpha(series.color, series.weight), state.line_width);
    }
    // Points over every line, each growing in as its line reaches it.
    for (std::size_t index = state.series.size(); index-- > 0U;) {
        const ChartSeries& series = state.series[index];
        const Drawn& line = drawn[index];
        if (series.hidden)
            continue;
        for (std::size_t point = 0U; point < line.points.size(); ++point) {
            const double eased = appeared(line, point);
            if (eased <= 0.0)
                continue;
            if (markers && radius > 0.0) {
                StrokeStyle ring;
                ring.width = 1.0;
                const double shown = radius * eased;
                scope.circle(line.points[point],
                             std::max(0.0, shown - (state.marker_outline.has_value() ? 0.5 : 0.0)),
                             Paint(with_alpha(series.color, series.weight)),
                             state.marker_outline.has_value()
                                 ? std::optional<Paint>(Paint(
                                       with_alpha(*state.marker_outline, series.weight * eased)))
                                 : std::nullopt,
                             ring);
            }
            const std::string& label = series.points[point].label;
            if (!label.empty()) {
                const double width = measure(label, state.label_style).width + 8.0;
                chart_text(scope, label, state.label_style,
                           Point{line.points[point].x - width * 0.5,
                                 line.points[point].y - (markers ? radius : 2.0) - 3.0 -
                                     label_height},
                           with_alpha(labels, eased * series.weight), width,
                           WidgetTextAlignment::center);
            }
        }
    }
    legend(scope, projection, state, state.marker_outline);
}

void cartesian_content(WidgetRenderScope& scope, ChartState& state, ChartProjection& projection,
                       const ChartTextMeasure& measure) {
    themed_colors(scope, state, scope.node());
    for (ChartLegendEntry& entry : projection.legend)
        entry.color = state.series[entry.index].color;
    for (ChartSeries& series : state.series) {
        if (series.weight < 1.0)
            series.color = with_alpha(series.color, series.weight);
    }
    const Rect plot = projection.plot;
    const RenderColor ink = scope.visual().foreground;
    const RenderColor muted = style_color(state.tick_style).value_or(with_alpha(ink, 0.55));
    const double label_height = chart_text_size(scope, "0", state.tick_style).height;
    const ChartScale& scale = projection.scale;

    for (const double tick : scale.ticks) {
        const double y = scale.y(tick, plot);
        if (state.grid || tick == 0.0) {
            scope.solid_rect(Rect{plot.x, std::round(y), plot.width, 1.0},
                             Paint(tick == 0.0 ? with_alpha(ink, 0.3) : scope.palette("chartGrid")));
        }
        chart_text(scope, scale.tick_label(tick, state), state.tick_style,
                   Point{plot.x - projection.gutter, y - label_height * 0.5}, muted,
                   projection.gutter - 8.0, WidgetTextAlignment::end);
    }
    for (std::size_t index = 0U; index < state.count; index += projection.x_label_step) {
        const Rect band = projection.bands[index];
        const double span = band.width * static_cast<double>(projection.x_label_step);
        chart_text(scope, state.label(index), state.tick_style,
                   Point{band.x + band.width * 0.5 - span * 0.5, plot.bottom() + 4.0}, muted, span,
                   WidgetTextAlignment::center);
    }

    const std::optional<std::size_t> focus = inspected(scope, state);
    if (focus.has_value() && *focus < projection.bands.size()) {
        const Rect band = projection.bands[*focus];
        if (state.kind == ChartKind::bar)
            scope.solid_rect(band, Paint(with_alpha(ink, 0.06)));
        else
            scope.solid_rect(Rect{std::round(projection.x(*focus)), plot.y, 1.0, plot.height},
                             Paint(with_alpha(ink, 0.35)));
    }
    for (std::size_t index = 0U; index < projection.bands.size(); ++index)
        scope.interaction(Rect{}, "point:" + std::to_string(index));

    const double zero = scale.y(std::clamp(0.0, scale.minimum, scale.maximum), plot);
    scope.push_clip(Rect{plot.x, plot.y - 4.0, plot.width, plot.height + 8.0});
    if (state.kind == ChartKind::bar) {
        std::size_t visible = 0U;
        for (const ChartSeries& series : state.series)
            visible += series.hidden ? 0U : 1U;
        for (std::size_t category = 0U; category < state.count; ++category) {
            const Rect band = projection.bands[category];
            const double group = band.width * 0.72;
            const std::size_t slots = state.stacked ? 1U : std::max<std::size_t>(1U, visible);
            const double width = std::max(1.0, group / static_cast<double>(slots));
            std::size_t slot = 0U;
            for (const ChartPoint& point : projection.points[category]) {
                const double x = band.x + (band.width - group) * 0.5 +
                                 (state.stacked ? 0.0 : width * static_cast<double>(slot));
                const double from = state.stacked ? scale.y(point.base, plot) : zero;
                const double to = scale.y(point.top, plot);
                const Rect bar{x + (slots > 1U ? 1.0 : 0.0), std::min(from, to),
                               std::max(1.0, width - (slots > 1U ? 2.0 : 0.0)),
                               std::max(1.0, std::abs(to - from))};
                RenderColor color = state.series[point.series].color;
                if (focus.has_value() && *focus != category)
                    color = with_alpha(color, 0.75);
                scope.rounded_rect(bar, Paint(color), std::nullopt,
                                   std::min(3.0, bar.width * 0.5));
                ++slot;
            }
        }
    } else {
        // Areas draw first and together: stacked ones meet along the series between them.
        std::vector<PathShape> areas;
        for (std::size_t index = 0U; index < state.series.size(); ++index) {
            const ChartSeries& series = state.series[index];
            if (series.hidden || state.kind != ChartKind::area || plot.width <= 0.0 ||
                plot.height <= 0.0) {
                continue;
            }
            for (const std::vector<Point>& run : runs(state, projection, index)) {
                if (run.size() < 2U)
                    continue;
                // Filled down to the series below it when stacked, else to zero.
                std::vector<Point> outline = run;
                for (auto point = run.rbegin(); point != run.rend(); ++point) {
                    double base = zero;
                    if (state.stacked) {
                        const auto category = static_cast<std::size_t>(std::lround(
                            (point->x - plot.x) / projection.bands.front().width - 0.5));
                        const auto& points = projection.points[category];
                        const auto found = std::ranges::find(points, index, &ChartPoint::series);
                        if (found != points.end())
                            base = scale.y(found->base, plot);
                    }
                    outline.push_back(Point{point->x, base});
                }
                for (Point& point : outline)
                    point = Point{(point.x - plot.x) / plot.width, (point.y - plot.y) / plot.height};
                areas.push_back(PathShape{Path::polyline(outline, true),
                                          Paint(with_alpha(series.color, 0.22))});
            }
        }
        scope.shapes(plot, std::move(areas));
        for (std::size_t index = 0U; index < state.series.size(); ++index) {
            const ChartSeries& series = state.series[index];
            if (series.hidden)
                continue;
            for (const std::vector<Point>& run : runs(state, projection, index)) {
                if (state.kind != ChartKind::scatter)
                    stroke_polyline(scope, run, series.color, 2.0);
                if (state.kind == ChartKind::scatter || state.points || run.size() == 1U) {
                    for (const Point point : run)
                        dot(scope, point, state.kind == ChartKind::scatter ? 4.0 : 3.0,
                            series.color);
                }
            }
        }
    }
    scope.pop_clip();

    if (focus.has_value() && *focus < projection.points.size()) {
        std::vector<std::pair<RenderColor, std::string>> lines;
        double anchor_y = plot.y + plot.height * 0.5;
        bool anchored = false;
        for (const ChartPoint& point : projection.points[*focus]) {
            const ChartSeries& series = state.series[point.series];
            const double y = scale.y(point.top, plot);
            if (state.kind != ChartKind::bar)
                dot(scope, Point{projection.x(*focus), y}, 4.0, series.color,
                    scope.palette("popupSurface"));
            if (!anchored) {
                anchor_y = y;
                anchored = true;
            }
            lines.emplace_back(series.color,
                               (series.name.empty() ? std::string() : series.name + ": ") +
                                   state.format(point.value));
        }
        if (!lines.empty()) {
            tooltip(scope, measure, Point{projection.x(*focus), anchor_y}, scope.layout().bounds,
                    state.label(*focus), lines);
        }
    }
    legend(scope, projection, state);
}

void chart_content(WidgetRenderScope& scope) {
    ChartState state = chart_state(scope.node());
    // Animated data: values morph, and series and slices fade or sweep as the legend toggles them.
    if (const MotionComputedValues* computed = scope.motion_values(); computed != nullptr) {
        if (const std::optional<double> reveal = computed->number(MotionProperty::reveal);
            reveal.has_value())
            state.reveal = std::clamp(*reveal, 0.0, 1.0);
        if (const MotionNumbers* animated = computed->numbers(MotionProperty::data);
            animated != nullptr && animated->values != nullptr) {
            const RetainedNode& node = scope.node();
            const std::vector<double> own =
                chart_motion_numbers(property(node, "kind"), property(node, "series"),
                                     property(node, "values"), node.retained_value("$hidden"));
            if (own.size() == animated->values->size() && own.front() == animated->values->front())
                apply_chart_motion(state, *animated->values);
        }
    }
    const ChartTextMeasure measure = [&scope](const std::string_view text,
                                              const runtime::Value* style) {
        return chart_text_size(scope, text, style);
    };
    ChartProjection projection = chart_projection(state, scope.layout().bounds, measure);
    if (scope.visual().background.has_value()) {
        scope.rounded_rect(scope.layout().bounds, *scope.visual().background,
                           scope.visual().border);
    }
    if (state.circular())
        circular_content(scope, state, projection, measure);
    else if (state.numeric)
        numeric_content(scope, state, projection, measure);
    else
        cartesian_content(scope, state, projection, measure);
    scope.focus(scope.layout().bounds);
}

/**
 * A chart animates its data by default: its numbers go out as `$motionData`, which
 * `animateChanges` follows (the theme's standard policy; reduced motion snaps).
 */
void chart_expand(WidgetDescriptionScope& scope) {
    const std::vector<double> numbers =
        chart_motion_numbers(scope.property("kind"), scope.property("series"),
                             scope.property("values"), scope.retained("$hidden"));
    std::vector<runtime::Value> values;
    values.reserve(numbers.size());
    for (const double number : numbers)
        values.emplace_back(number);
    scope.description().properties.insert_or_assign(
        "$motionData", runtime::ExpressionValue(runtime::Value(std::move(values))));
    if (scope.property("animateChanges") == nullptr) {
        scope.description().properties.insert_or_assign(
            "animateChanges",
            runtime::ExpressionValue(widget_object({
                {"policy", runtime::Value("standard")},
                {"properties", runtime::Value(std::vector<runtime::Value>{runtime::Value("data")})},
            })));
    }
}

void chart_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("width", widget_fill());
    scope.set("height", runtime::Value(240.0));
    scope.intrinsic(320.0, 240.0);
}

/** What `onSelect` carries: the category's index and label, and each shown series' value. */
[[nodiscard]] runtime::Value selection_payload(const ChartState& state, const std::size_t index) {
    std::vector<runtime::Value> values;
    std::optional<double> first;
    if (state.circular()) {
        first = state.slice_value(index);
        values.emplace_back(first.has_value() ? runtime::Value(*first) : runtime::Value{});
    } else {
        for (const ChartSeries& series : state.series) {
            if (series.hidden)
                continue;
            const std::optional<double> value =
                index < series.values.size() ? series.values[index] : std::nullopt;
            if (!first.has_value())
                first = value;
            values.emplace_back(value.has_value() ? runtime::Value(*value) : runtime::Value{});
        }
    }
    return runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
        {"index", runtime::Value(static_cast<double>(index))},
        {"label", runtime::Value(state.label(index))},
        {"value", first.has_value() ? runtime::Value(*first) : runtime::Value{}},
        {"values", runtime::Value(std::move(values))},
    });
}

void chart_select(WidgetInputScope& scope, const ChartState& state, const std::size_t index) {
    scope.set_retained("$cursor", runtime::Value(static_cast<double>(index)), DirtyReason::paint);
    scope.value_changed("onSelect", "point-selected", selection_payload(state, index));
}

/** Shows or hides a legend entry's series, or a pie's slice. */
void chart_toggle(WidgetInputScope& scope, const std::size_t index) {
    std::vector<runtime::Value> hidden;
    bool removed = false;
    if (const runtime::Value* value = scope.retained("$hidden");
        value != nullptr && value->list() != nullptr) {
        for (const runtime::Value& entry : value->list()->values) {
            if (entry.number() != nullptr && *entry.number() == static_cast<double>(index)) {
                removed = true;
                continue;
            }
            hidden.push_back(entry);
        }
    }
    if (!removed)
        hidden.emplace_back(static_cast<double>(index));
    // Properties, not paint: the description publishes the shown series for them to fade.
    scope.set_retained("$hidden", runtime::Value(std::move(hidden)), DirtyReason::properties);
}

bool chart_click(WidgetInputScope& scope) {
    const WidgetSubtarget* target = scope.subtarget();
    if (target == nullptr || !target->enabled)
        return false;
    const ChartState state = chart_state(scope.node());
    if (const std::optional<std::size_t> index = index_after(target->id, "legend:");
        index.has_value()) {
        chart_toggle(scope, *index);
        return true;
    }
    std::optional<std::size_t> index = index_after(target->id, "point:");
    if (!index.has_value())
        index = index_after(target->id, "slice:");
    if (!index.has_value() || *index >= state.count)
        return false;
    chart_select(scope, state, *index);
    return true;
}

/** Whether the keyboard can stop on a category or slice: a pie skips hidden slices. */
[[nodiscard]] bool stoppable(const ChartState& state, const std::size_t index) {
    return !state.circular() || state.slice_value(index).has_value();
}

/** Left and Right (or Up and Down) step through categories, Home and End jump; Enter selects. */
bool chart_key(WidgetInputScope& scope) {
    const ChartState state = chart_state(scope.node());
    if (state.count == 0U)
        return false;
    const std::string_view key = scope.key();
    if ((key == "enter" || key == "space") && state.cursor.has_value()) {
        chart_select(scope, state, *state.cursor);
        return true;
    }
    int direction = 0;
    std::size_t from = state.cursor.value_or(0U);
    if (key == "right" || key == "down") direction = 1;
    else if (key == "left" || key == "up") direction = -1;
    else if (key == "home") {
        direction = 1;
        from = state.count - 1U;
    } else if (key == "end") {
        direction = -1;
        from = 0U;
    } else {
        return false;
    }
    std::optional<std::size_t> next;
    if (!state.cursor.has_value() && key != "home" && key != "end") {
        // The first step lands on the first stop.
        for (std::size_t index = 0U; index < state.count && !next.has_value(); ++index) {
            const std::size_t candidate = direction > 0 ? index : state.count - 1U - index;
            if (stoppable(state, candidate))
                next = candidate;
        }
    } else {
        for (std::size_t step = 1U; step <= state.count && !next.has_value(); ++step) {
            const std::size_t candidate =
                (from + state.count + (direction > 0 ? step : state.count - step)) % state.count;
            if (key != "home" && key != "end" &&
                ((direction > 0 && candidate < from) || (direction < 0 && candidate > from)))
                break;
            if (stoppable(state, candidate))
                next = candidate;
        }
    }
    if (next.has_value())
        scope.set_retained("$cursor", runtime::Value(static_cast<double>(*next)),
                           DirtyReason::paint);
    return true;
}

void chart_semantics(WidgetSemanticsScope& scope) {
    label_or_key(scope);
    const ChartState state = chart_state(scope.node());
    std::string summary;
    if (state.circular()) {
        summary = state.kind == ChartKind::pie ? "Pie chart" : "Donut chart";
    } else {
        static constexpr std::string_view names[] = {"Line chart", "Area chart", "Bar chart",
                                                     "Scatter chart"};
        summary = std::string(names[static_cast<std::size_t>(state.kind)]);
        summary += ", " + std::to_string(state.series.size()) +
                   (state.series.size() == 1U ? " series" : " series");
    }
    summary += ", " + std::to_string(state.count) +
               (state.circular() ? (state.count == 1U ? " slice" : " slices")
                                 : (state.count == 1U ? " point" : " points"));
    scope.default_value_text(std::move(summary));
}

/** Each category's band, or each slice's outline, plus the legend entries. */
std::vector<WidgetSubtarget> chart_subtargets(const WidgetSubtargetContext& context) {
    std::vector<WidgetSubtarget> result;
    if (context.record.bounds.empty())
        return result;
    const RetainedNode& node = context.node;
    const ChartState state = chart_state(node);
    const ChartTextMeasure measure = [&context](const std::string_view text,
                                                const runtime::Value* style) {
        Size size{estimated_width(text), 14.0};
        if (context.text_layout != nullptr && *context.text_layout) {
            const TextLayout layout =
                (*context.text_layout)(context.node, text, TextLayoutOptions{.style = style});
            size = Size{layout.shaped.metrics.width, layout.shaped.metrics.height};
        }
        if (style == nullptr && context.text_width != nullptr && *context.text_width)
            size.width = (*context.text_width)(context.node, text);
        return size;
    };
    const ChartProjection projection = chart_projection(state, context.record.bounds, measure);
    const int z = context.record.z_index;
    std::size_t order = 0U;
    if (state.circular()) {
        for (const ChartSlice& slice : projection.slices) {
            WidgetSubtarget target{node.identity(), "slice:" + std::to_string(slice.index),
                                   WidgetSubtargetKind::choice, order++, slice.bounds,
                                   runtime::Value(static_cast<double>(slice.index)),
                                   state.label(slice.index) + ": " + state.format(slice.value),
                                   {}, true, false, z};
            target.shape = slice.outline;
            result.push_back(std::move(target));
        }
    } else {
        for (std::size_t index = 0U; index < projection.bands.size(); ++index) {
            result.push_back(WidgetSubtarget{node.identity(), "point:" + std::to_string(index),
                                             WidgetSubtargetKind::choice, order++,
                                             projection.bands[index],
                                             runtime::Value(static_cast<double>(index)),
                                             state.label(index), {}, true, false, z});
        }
    }
    for (const ChartLegendEntry& entry : projection.legend) {
        result.push_back(WidgetSubtarget{node.identity(), "legend:" + std::to_string(entry.index),
                                         WidgetSubtargetKind::control, order++, entry.bounds,
                                         runtime::Value(static_cast<double>(entry.index)),
                                         (entry.hidden ? "Show " : "Hide ") + entry.label, {},
                                         true, false, z});
    }
    return result;
}

} // namespace

void register_chart_widget(WidgetRegistry& registry) {
    WidgetDescribePhase describe{&chart_defaults, &chart_expand, {}, "Chart", true};
    registry.register_describe_phase("Chart", std::move(describe));
    WidgetInputPhase input;
    input.click = &chart_click;
    input.key = &chart_key;
    input.action_property = "onSelect";
    input.fallback_action = "Chart";
    input.focusable = true;
    registry.register_input_phase("Chart", std::move(input));
    registry.register_semantics_phase(
        "Chart", WidgetSemanticsPhase{"image", {}, &chart_semantics, false, false});
    registry.register_subtargets("Chart", &chart_subtargets);
}

void register_chart_widget_presenter(WidgetRegistry& registry) {
    WidgetPresentPhase phase{&chart_content, nullptr, nullptr, nullptr, false};
    phase.visual = WidgetVisualProfile{true, false, false};
    registry.register_present_phase("Chart", std::move(phase));
    // Charts sit on whatever surface holds them; their text is body copy.
    registry.register_style_schema("Chart", ThemeStyleSchema{.chromeless = true, .body_text = true});
}

} // namespace strata::ui
