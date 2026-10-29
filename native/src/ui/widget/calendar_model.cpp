#include "ui/widget/calendar_model.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "ui/text.hpp"
#include "ui/tree.hpp"
#include "ui/widget/presentation.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] const runtime::Value* property(const RetainedNode& node,
                                             const std::string_view name) noexcept {
    const auto found = node.description().properties.find(name);
    return found != node.description().properties.end() ? found->second.value() : nullptr;
}

[[nodiscard]] std::optional<CalendarDate> date_value(const runtime::Value* value) noexcept {
    return value != nullptr && value->string() != nullptr ? CalendarDate::parse(*value->string())
                                                          : std::nullopt;
}

/** A list of exactly `count` strings, else the English fallback. */
[[nodiscard]] std::vector<std::string> names(const runtime::Value* value, const std::size_t count,
                                             std::initializer_list<std::string_view> fallback) {
    std::vector<std::string> result;
    if (value != nullptr && value->list() != nullptr && value->list()->values.size() == count) {
        for (const runtime::Value& entry : value->list()->values) {
            if (entry.string() == nullptr)
                break;
            result.push_back(*entry.string());
        }
    }
    if (result.size() != count)
        result.assign(fallback.begin(), fallback.end());
    return result;
}

/** A date property: the controlled value if the property is present, else retained state. */
[[nodiscard]] std::optional<CalendarDate> controlled_date(const RetainedNode& node,
                                                         const std::string_view controlled,
                                                         const std::string_view retained) {
    if (const runtime::Value* value = property(node, controlled); value != nullptr)
        return date_value(value);
    return date_value(node.retained_value(retained));
}

[[nodiscard]] bool leap(const std::int32_t year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

} // namespace

std::uint32_t calendar_days_in_month(const std::int32_t year, const std::uint32_t month) noexcept {
    static constexpr std::array<std::uint32_t, 12U> lengths{31U, 28U, 31U, 30U, 31U, 30U,
                                                           31U, 31U, 30U, 31U, 30U, 31U};
    if (month < 1U || month > 12U)
        return 30U;
    return month == 2U && leap(year) ? 29U : lengths[month - 1U];
}


std::optional<CalendarDate> CalendarDate::parse(const std::string_view text) noexcept {
    // Exactly YYYY-MM-DD: the one form Calendar reads and writes.
    if (text.size() != 10U || text[4] != '-' || text[7] != '-')
        return std::nullopt;
    const auto number = [text](const std::size_t start, const std::size_t length,
                               auto& out) {
        const char* const begin = text.data() + start;
        const auto parsed = std::from_chars(begin, begin + length, out);
        return parsed.ec == std::errc{} && parsed.ptr == begin + length;
    };
    CalendarDate result;
    if (!number(0U, 4U, result.year) || !number(5U, 2U, result.month) ||
        !number(8U, 2U, result.day)) {
        return std::nullopt;
    }
    if (result.month < 1U || result.month > 12U || result.day < 1U ||
        result.day > calendar_days_in_month(result.year, result.month)) {
        return std::nullopt;
    }
    return result;
}

std::int64_t CalendarDate::days() const noexcept {
    // Howard Hinnant's days_from_civil.
    const std::int64_t y = static_cast<std::int64_t>(year) - (month <= 2U ? 1 : 0);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto year_of_era = static_cast<std::uint32_t>(y - era * 400);
    const std::uint32_t day_of_year =
        (153U * (month > 2U ? month - 3U : month + 9U) + 2U) / 5U + day - 1U;
    const std::uint32_t day_of_era =
        year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

CalendarDate CalendarDate::from_days(std::int64_t days) noexcept {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto day_of_era = static_cast<std::uint32_t>(days - era * 146097);
    const std::uint32_t year_of_era =
        (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
    const std::uint32_t day_of_year =
        day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
    const std::uint32_t month_index = (5U * day_of_year + 2U) / 153U;
    CalendarDate result;
    result.day = day_of_year - (153U * month_index + 2U) / 5U + 1U;
    result.month = month_index < 10U ? month_index + 3U : month_index - 9U;
    result.year = static_cast<std::int32_t>(static_cast<std::int64_t>(year_of_era) + era * 400 +
                                            (result.month <= 2U ? 1 : 0));
    return result;
}

std::uint32_t CalendarDate::weekday() const noexcept {
    // 1970-01-01 was a Thursday.
    return static_cast<std::uint32_t>(((days() % 7) + 7 + 4) % 7);
}

std::string CalendarDate::text() const {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u", static_cast<int>(year),
                  static_cast<unsigned>(month), static_cast<unsigned>(day));
    return buffer;
}

CalendarDate CalendarDate::add_months(const std::int32_t delta) const noexcept {
    const std::int64_t index =
        static_cast<std::int64_t>(year) * 12 + static_cast<std::int64_t>(month) - 1 + delta;
    CalendarDate result;
    result.year = static_cast<std::int32_t>(index >= 0 ? index / 12 : (index - 11) / 12);
    result.month = static_cast<std::uint32_t>(index - static_cast<std::int64_t>(result.year) * 12) + 1U;
    result.day = std::min(day, calendar_days_in_month(result.year, result.month));
    return result;
}

bool CalendarState::allows(const CalendarDate& date) const noexcept {
    return (!minimum.has_value() || date >= *minimum) && (!maximum.has_value() || date <= *maximum);
}

const std::string& CalendarState::month_name(const std::uint32_t number) const noexcept {
    return month_names[(number + 11U) % 12U];
}

const std::string& CalendarState::weekday_name(const std::uint32_t weekday) const noexcept {
    return weekday_names[weekday % 7U];
}

bool CalendarState::in_range(const CalendarDate& date) const noexcept {
    if (!range_start.has_value())
        return false;
    if (!range_end.has_value())
        return date == *range_start;
    return date >= *range_start && date <= *range_end;
}

CalendarState calendar_state(const RetainedNode& node) {
    CalendarState state;
    // The selection: controlled value, then what the user picked, then the authored default.
    for (const runtime::Value* candidate : {property(node, "value"), node.retained_value("$value"),
                                            property(node, "defaultValue")}) {
        if (candidate == nullptr)
            continue;
        if (candidate->kind() == runtime::ValueKind::null_value) {
            if (candidate == property(node, "value"))
                break;
            continue;
        }
        if (const std::optional<CalendarDate> parsed = date_value(candidate); parsed.has_value()) {
            state.selected = parsed;
            break;
        }
    }
    state.month_names = names(property(node, "monthNames"), 12U,
                              {"January", "February", "March", "April", "May", "June", "July",
                               "August", "September", "October", "November", "December"});
    state.weekday_names = names(property(node, "weekdayNames"), 7U,
                                {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"});
    if (const runtime::Value* selection = property(node, "selection");
        selection != nullptr && selection->string() != nullptr && *selection->string() == "RANGE") {
        state.range = true;
        state.range_start = controlled_date(node, "rangeStart", "$rangeStart");
        state.range_end = controlled_date(node, "rangeEnd", "$rangeEnd");
    }
    state.minimum = date_value(property(node, "min"));
    state.maximum = date_value(property(node, "max"));
    state.today = date_value(property(node, "today"));
    if (const runtime::Value* first = property(node, "firstDayOfWeek");
        first != nullptr && first->number() != nullptr && std::isfinite(*first->number())) {
        state.first_weekday =
            static_cast<std::uint32_t>(std::clamp(std::floor(*first->number()), 0.0, 6.0));
    }
    const CalendarDate anchor = state.selected.value_or(state.range_start.value_or(
        state.today.value_or(state.minimum.value_or(CalendarDate{2000, 1U, 1U}))));
    state.cursor = date_value(node.retained_value("$cursor")).value_or(anchor);
    const CalendarDate shown = date_value(node.retained_value("$month")).value_or(state.cursor);
    state.month = CalendarDate{shown.year, shown.month, 1U};
    return state;
}

CalendarProjection calendar_projection(const CalendarState& state, const Rect bounds) {
    CalendarProjection result;
    result.month = state.month;
    // Header and weekday rows take fixed shares; six weeks share the rest.
    const double header_height = std::min(32.0, bounds.height / 8.0);
    const double weekday_height = std::min(22.0, bounds.height / 8.0);
    result.header = Rect{bounds.x, bounds.y, bounds.width, header_height};
    const double button = std::min(header_height, bounds.width / 7.0);
    result.previous = Rect{bounds.x, bounds.y, button, header_height};
    result.next = Rect{bounds.right() - button, bounds.y, button, header_height};
    const double column = bounds.width / 7.0;
    const double grid_top = bounds.y + header_height + weekday_height;
    const double row = std::max(0.0, bounds.bottom() - grid_top) / 6.0;
    for (std::uint32_t index = 0U; index < 7U; ++index) {
        result.weekdays.emplace_back(
            (state.first_weekday + index) % 7U,
            Rect{bounds.x + column * index, bounds.y + header_height, column, weekday_height});
    }
    const std::uint32_t lead = (state.month.weekday() + 7U - state.first_weekday) % 7U;
    const std::int64_t first_day = state.month.days() - lead;
    result.cells.reserve(42U);
    for (std::uint32_t index = 0U; index < 42U; ++index) {
        const CalendarDate date = CalendarDate::from_days(first_day + index);
        result.cells.push_back(CalendarCell{
            date,
            Rect{bounds.x + column * (index % 7U), grid_top + row * (index / 7U), column, row},
            date.year == state.month.year && date.month == state.month.month,
            state.allows(date),
        });
    }
    return result;
}

std::optional<CalendarDate> calendar_key_target(const CalendarState& state,
                                                const std::string_view key, const bool shift) {
    const std::int64_t day = state.cursor.days();
    const std::int64_t into_week =
        static_cast<std::int64_t>((state.cursor.weekday() + 7U - state.first_weekday) % 7U);
    if (key == "left") return CalendarDate::from_days(day - 1);
    if (key == "right") return CalendarDate::from_days(day + 1);
    if (key == "up") return CalendarDate::from_days(day - 7);
    if (key == "down") return CalendarDate::from_days(day + 7);
    if (key == "home") return CalendarDate::from_days(day - into_week);
    if (key == "end") return CalendarDate::from_days(day + 6 - into_week);
    if (key == "pageup") return state.cursor.add_months(shift ? -12 : -1);
    if (key == "pagedown") return state.cursor.add_months(shift ? 12 : 1);
    return std::nullopt;
}

void present_calendar_month(WidgetRenderScope& scope, const CalendarState& state,
                            const Rect area) {
    const CalendarProjection projection = calendar_projection(state, area);
    const RenderColor ink = scope.visual().foreground;
    const RenderColor muted = scope.palette("disabledForeground");
    const auto centred = [&scope](const std::string& value, const Rect bounds,
                                  const RenderColor color) {
        if (scope.text_engine() == nullptr)
            return;
        const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), value);
        scope.text(value, Point{bounds.x, bounds.y + (bounds.height - shaped.metrics.height) * 0.5},
                   color, bounds.width, WidgetTextAlignment::center);
    };
    centred(state.month_name(projection.month.month) + " " + std::to_string(projection.month.year),
            projection.header, ink);
    scope.interaction(projection.previous, "previous");
    scope.interaction(projection.next, "next");
    centred("<", projection.previous, ink);
    centred(">", projection.next, ink);
    for (const auto& [weekday, bounds] : projection.weekdays)
        centred(state.weekday_name(weekday), bounds, muted);
    for (const CalendarCell& cell : projection.cells) {
        const double side = std::max(0.0, std::min(cell.bounds.width, cell.bounds.height) - 4.0);
        const Rect mark{cell.bounds.x + (cell.bounds.width - side) * 0.5,
                        cell.bounds.y + (cell.bounds.height - side) * 0.5, side, side};
        const bool endpoint =
            state.range && ((state.range_start.has_value() && *state.range_start == cell.date) ||
                            (state.range_end.has_value() && *state.range_end == cell.date));
        const bool selected =
            endpoint || (!state.range && state.selected.has_value() && *state.selected == cell.date);
        if (state.range && state.in_range(cell.date) && state.range_end.has_value()) {
            // The band runs edge to edge through the range so consecutive days join up.
            const Paint band = scope.visual().fill;
            RenderColor tint = band.representative();
            tint.alpha = static_cast<std::uint8_t>(tint.alpha / 3U);
            scope.solid_rect(Rect{cell.bounds.x, mark.y, cell.bounds.width, mark.height}, tint);
        }
        if (selected)
            scope.rounded_rect(mark, scope.visual().fill, std::nullopt, side * 0.5);
        else if (state.today.has_value() && *state.today == cell.date)
            scope.border(mark, RenderBorder{1.0, scope.visual().fill.representative(), true},
                         side * 0.5);
        if (cell.enabled)
            scope.interaction(cell.bounds, "day:" + cell.date.text());
        if (scope.focus_visible() && cell.date == state.cursor) {
            scope.border(mark, scope.visual().focus_ring.value_or(RenderBorder{2.0, ink, true}),
                         side * 0.5);
        }
        const RenderColor color = selected ? scope.palette("tokenActive")
                                  : cell.in_month && cell.enabled ? ink
                                                                  : muted;
        centred(std::to_string(cell.date.day), cell.bounds, color);
    }
}

} // namespace strata::ui
