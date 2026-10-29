#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace strata::runtime {

/**
 * How `formatNumber` writes a number. Every field is optional in the authored call; the defaults
 * keep a bare `formatNumber(x)` what it always was (two decimals, no grouping) while a currency
 * groups and takes its own minor-unit digits, a percentage takes none, and a compact number keeps
 * two significant digits.
 */
struct NumberFormat final {
    /** Digits after the decimal separator, 0 to 12; the style's default when absent. */
    std::optional<int> decimals;
    /** Thousands grouping; on by default only for currencies and compact numbers. */
    std::optional<bool> grouping;
    /** Scales by thousands with a suffix: 1.2K, 3.4M, 5B, 6T. */
    bool compact = false;
    /**
     * An ISO 4217 code ("USD" writes "$1,234.50"; one without a symbol of its own is written before
     * the number, "CHF 1,234.50"), or any other text used as the symbol itself ("€").
     */
    std::string currency;
    /** Multiplies by 100 and appends "%". */
    bool percent = false;
    /** "auto" (a minus for negatives), "always", "exceptZero", "negative" or "never". */
    std::string sign_display = "auto";
    std::string group_separator = ",";
    std::string decimal_separator = ".";
};

/** A number written as `format` asks; non-finite values are "NaN", "∞" and "-∞". */
[[nodiscard]] std::string format_number(double value, const NumberFormat& format);

/**
 * A byte count in the largest unit that keeps it at least one: "512 B", "1.5 KB", "3.2 GB", with
 * `decimals` (1 by default) trailing zeros trimmed. Units are powers of 1000 (KB, MB, GB, TB, PB,
 * EB), or of 1024 (KiB, MiB, GiB, TiB, PiB, EiB) when `binary`.
 */
[[nodiscard]] std::string format_bytes(double bytes, std::optional<int> decimals, bool binary,
                                       std::string_view decimal_separator = ".");

/**
 * How far `milliseconds` lies from `now` (both UTC millisecond timestamps), in words: "just now"
 * within ten seconds, then "3 minutes ago" or "in 2 days" in the largest whole unit (seconds,
 * minutes, hours, days, weeks, months, years). `short_style` writes "3m ago" and "in 2d" instead.
 */
[[nodiscard]] std::string time_ago(double milliseconds, double now, bool short_style);

/**
 * A span of `milliseconds`: style "clock" (the default) writes "1:05:09" (or "5:09" under an
 * hour), "short" the two largest units that are not zero ("1h 5m", "5m 9s", "350ms"), and "long"
 * spells them out ("1 hour 5 minutes").
 */
[[nodiscard]] std::string format_duration(double milliseconds, std::string_view style);

} // namespace strata::runtime
