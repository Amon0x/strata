#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui/layout.hpp"

namespace strata::ui {

/** A proleptic Gregorian day, as Calendar exchanges it: "YYYY-MM-DD". */
struct CalendarDate final {
    std::int32_t year = 1970;
    std::uint32_t month = 1U;
    std::uint32_t day = 1U;

    [[nodiscard]] static std::optional<CalendarDate> parse(std::string_view text) noexcept;
    [[nodiscard]] static CalendarDate from_days(std::int64_t days) noexcept;
    /** Days since 1970-01-01. */
    [[nodiscard]] std::int64_t days() const noexcept;
    /** 0 for Sunday through 6 for Saturday. */
    [[nodiscard]] std::uint32_t weekday() const noexcept;
    [[nodiscard]] std::string text() const;
    /** The same day in a month `delta` months away, clamped to that month's length. */
    [[nodiscard]] CalendarDate add_months(std::int32_t delta) const noexcept;
    [[nodiscard]] friend auto operator<=>(const CalendarDate&, const CalendarDate&) = default;
};

[[nodiscard]] std::uint32_t calendar_days_in_month(std::int32_t year, std::uint32_t month) noexcept;
[[nodiscard]] std::string_view calendar_month_name(std::uint32_t month) noexcept;
/** Two-letter weekday names from Sunday. */
[[nodiscard]] std::string_view calendar_weekday_short(std::uint32_t weekday) noexcept;

/** One day cell of a month view. */
struct CalendarCell final {
    CalendarDate date;
    Rect bounds;
    bool in_month = true;
    bool enabled = true;
};

/**
 * Where a Calendar draws and hits everything for the month it shows: a header with previous and
 * next buttons, a weekday row, and six weeks of seven days starting on `first_weekday`.
 */
struct CalendarProjection final {
    CalendarDate month;
    Rect header;
    Rect previous;
    Rect next;
    std::vector<std::pair<std::uint32_t, Rect>> weekdays;
    std::vector<CalendarCell> cells;
};

/** What a Calendar node shows and allows, read from its properties and retained state. */
struct CalendarState final {
    std::optional<CalendarDate> selected;
    /** The first day of the month on screen. */
    CalendarDate month;
    /** The day the keyboard is on. */
    CalendarDate cursor;
    std::optional<CalendarDate> minimum;
    std::optional<CalendarDate> maximum;
    std::optional<CalendarDate> today;
    std::uint32_t first_weekday = 1U;

    [[nodiscard]] bool allows(const CalendarDate& date) const noexcept;
};

[[nodiscard]] CalendarState calendar_state(const RetainedNode& node);
[[nodiscard]] CalendarProjection calendar_projection(const CalendarState& state, Rect bounds);

} // namespace strata::ui
