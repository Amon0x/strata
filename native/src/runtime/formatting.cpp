#include "runtime/formatting.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace strata::runtime {
namespace {

/** Fraction digits a number may ask for; beyond this a double carries no more digits. */
constexpr int maximum_decimals = 12;

/**
 * `value` (finite, not negative) in fixed notation with `decimals` digits after a '.', independent
 * of the process locale. It rounds the value's shortest exact decimal spelling half away from
 * zero, as people round and as Intl.NumberFormat does: 2.5 is 3, and 1.005 (stored a hair below
 * it) is 1.01 rather than the binary value's 1.00.
 */
[[nodiscard]] std::string fixed(const double value, const int decimals) {
    std::array<char, 400U> buffer{};
    const std::to_chars_result result = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                                      value, std::chars_format::fixed);
    if (result.ec != std::errc{})
        return "0";
    std::string text(buffer.data(), result.ptr);
    const auto digits = static_cast<std::size_t>(std::clamp(decimals, 0, maximum_decimals));
    std::size_t point = text.find('.');
    if (point == std::string::npos) {
        point = text.size();
        text.push_back('.');
    }
    const std::size_t fraction = text.size() - point - 1U;
    if (fraction <= digits) {
        text.append(digits - fraction, '0');
    } else {
        const bool up = text[point + 1U + digits] >= '5';
        text.resize(point + 1U + digits);
        // Carry the rounding through the kept digits, growing a digit in front if it runs out.
        for (std::size_t index = text.size(); up && index > 0U; --index) {
            char& digit = text[index - 1U];
            if (digit == '.')
                continue;
            if (digit != '9') {
                ++digit;
                break;
            }
            digit = '0';
            if (index == 1U)
                text.insert(text.begin(), '1');
        }
    }
    if (text.back() == '.')
        text.pop_back();
    return text;
}

/** Drops trailing zeros after a '.', and the '.' itself when nothing is left after it. */
[[nodiscard]] std::string trimmed(std::string text) {
    const std::size_t point = text.find('.');
    if (point == std::string::npos)
        return text;
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text;
}

/** Whether a fixed rendering shows only zeros: it rounded to zero. */
[[nodiscard]] bool shows_zero(const std::string_view text) noexcept {
    return std::ranges::all_of(
        text, [](const char character) { return character == '0' || character == '.'; });
}

/** A fixed rendering with its integer digits grouped in threes and its separators replaced. */
[[nodiscard]] std::string localized(const std::string_view text, const bool grouping,
                                    const std::string_view group_separator,
                                    const std::string_view decimal_separator) {
    const std::size_t point = text.find('.');
    const std::string_view whole = text.substr(0U, point);
    std::string result;
    result.reserve(text.size() + whole.size() / 3U * group_separator.size() + 4U);
    for (std::size_t index = 0U; index < whole.size(); ++index) {
        if (grouping && index > 0U && (whole.size() - index) % 3U == 0U)
            result.append(group_separator);
        result.push_back(whole[index]);
    }
    if (point != std::string_view::npos) {
        result.append(decimal_separator);
        result.append(text.substr(point + 1U));
    }
    return result;
}

struct Currency final {
    std::string_view code;
    std::string_view symbol;
    int digits = 2;
};

/** Symbols as English-language interfaces write them; codes absent here are written as codes. */
constexpr std::array currencies{
    Currency{"AUD", "A$", 2},
    Currency{"BRL", "R$", 2},
    Currency{"CAD", "CA$", 2},
    Currency{"CLP", "", 0},
    Currency{"CNY", "CN\xC2\xA5", 2},
    Currency{"EUR", "\xE2\x82\xAC", 2},
    Currency{"GBP", "\xC2\xA3", 2},
    Currency{"HKD", "HK$", 2},
    Currency{"ILS", "\xE2\x82\xAA", 2},
    Currency{"INR", "\xE2\x82\xB9", 2},
    Currency{"ISK", "", 0},
    Currency{"JPY", "\xC2\xA5", 0},
    Currency{"KRW", "\xE2\x82\xA9", 0},
    Currency{"MXN", "MX$", 2},
    Currency{"NGN", "\xE2\x82\xA6", 2},
    Currency{"NZD", "NZ$", 2},
    Currency{"PHP", "\xE2\x82\xB1", 2},
    Currency{"PYG", "", 0},
    Currency{"RUB", "\xE2\x82\xBD", 2},
    Currency{"THB", "\xE0\xB8\xBF", 2},
    Currency{"TRY", "\xE2\x82\xBA", 2},
    Currency{"TWD", "NT$", 2},
    Currency{"UAH", "\xE2\x82\xB4", 2},
    Currency{"UGX", "", 0},
    Currency{"USD", "$", 2},
    Currency{"VND", "\xE2\x82\xAB", 0},
    Currency{"XAF", "", 0},
    Currency{"XOF", "", 0},
};

[[nodiscard]] bool currency_code(const std::string_view text) noexcept {
    return text.size() == 3U && std::ranges::all_of(text, [](const char character) {
               return character >= 'A' && character <= 'Z';
           });
}

/** What a currency writes before the number, and its default fraction digits. */
[[nodiscard]] std::pair<std::string, int> currency_prefix(const std::string_view currency) {
    if (!currency_code(currency))
        return {std::string(currency), 2};
    const auto found = std::ranges::find(currencies, currency, &Currency::code);
    if (found == currencies.end())
        return {std::string(currency) + " ", 2};
    if (found->symbol.empty())
        return {std::string(currency) + " ", found->digits};
    return {std::string(found->symbol), found->digits};
}

/** The sign `sign_display` puts before a number of this sign that shows as `zero` or not. */
[[nodiscard]] std::string_view sign(const std::string_view display, const bool negative,
                                    const bool zero) noexcept {
    if (display == "never")
        return "";
    if (display == "always")
        return negative && !zero ? "-" : "+";
    if (display == "exceptZero")
        return zero ? "" : (negative ? "-" : "+");
    return negative && !zero ? "-" : "";
}

/** A magnitude scaled to its compact unit with the digits to show, promoted when it rounds up. */
struct Compact final {
    std::string digits;
    std::string_view suffix;
};

[[nodiscard]] Compact compact(const double magnitude, const std::optional<int> decimals) {
    static constexpr std::array<std::string_view, 5U> suffixes{"", "K", "M", "B", "T"};
    double scaled = magnitude;
    std::size_t unit = 0U;
    while (unit + 1U < suffixes.size() && scaled >= 1000.0) {
        scaled /= 1000.0;
        ++unit;
    }
    // Two significant digits unless the author fixes the decimals: 1.2K, 12K, 123K.
    const auto digits_for = [decimals](const double value) {
        return decimals.has_value() ? *decimals : (value < 10.0 ? 1 : 0);
    };
    std::string text = fixed(scaled, digits_for(scaled));
    double rounded = 0.0;
    std::from_chars(text.data(), text.data() + text.size(), rounded);
    if (rounded >= 1000.0 && unit + 1U < suffixes.size()) {
        scaled = rounded / 1000.0;
        ++unit;
        text = fixed(scaled, digits_for(scaled));
    }
    if (!decimals.has_value())
        text = trimmed(std::move(text));
    return Compact{std::move(text), suffixes[unit]};
}

} // namespace

std::string format_number(double value, const NumberFormat& format) {
    const bool negative = std::signbit(value);
    if (std::isnan(value))
        return "NaN";
    if (format.percent)
        value *= 100.0;
    const double magnitude = std::abs(value);
    const auto [prefix, currency_digits] = format.currency.empty()
                                               ? std::pair<std::string, int>{std::string(), 2}
                                               : currency_prefix(format.currency);
    const std::string_view percent_suffix = format.percent ? "%" : "";
    if (std::isinf(magnitude)) {
        return std::string(sign(format.sign_display, negative, false)) + prefix + "\xE2\x88\x9E" +
               std::string(percent_suffix);
    }
    const std::optional<int> decimals =
        format.decimals.has_value()
            ? std::optional<int>(std::clamp(*format.decimals, 0, maximum_decimals))
            : std::nullopt;
    const bool grouping = format.grouping.value_or(!format.currency.empty() || format.compact);
    std::string digits;
    std::string_view suffix;
    if (format.compact) {
        Compact scaled = compact(magnitude, decimals);
        digits = std::move(scaled.digits);
        suffix = scaled.suffix;
    } else {
        const int fraction = decimals.value_or(!format.currency.empty() ? currency_digits
                                               : format.percent         ? 0
                                                                        : 2);
        digits = fixed(magnitude, fraction);
    }
    const bool zero = shows_zero(digits);
    std::string result(sign(format.sign_display, negative, zero));
    result += prefix;
    result += localized(digits, grouping, format.group_separator, format.decimal_separator);
    result += suffix;
    result += percent_suffix;
    return result;
}

std::string format_bytes(const double bytes, const std::optional<int> decimals, const bool binary,
                         const std::string_view decimal_separator) {
    if (std::isnan(bytes))
        return "NaN";
    static constexpr std::array<std::string_view, 7U> decimal_units{"B",  "KB", "MB", "GB",
                                                                    "TB", "PB", "EB"};
    static constexpr std::array<std::string_view, 7U> binary_units{"B",   "KiB", "MiB", "GiB",
                                                                   "TiB", "PiB", "EiB"};
    const std::array<std::string_view, 7U>& units = binary ? binary_units : decimal_units;
    const double base = binary ? 1024.0 : 1000.0;
    const bool negative = bytes < 0.0;
    double scaled = std::abs(bytes);
    if (std::isinf(scaled))
        return std::string(negative ? "-" : "") + "\xE2\x88\x9E B";
    std::size_t unit = 0U;
    while (unit + 1U < units.size() && scaled >= base) {
        scaled /= base;
        ++unit;
    }
    const int fraction = unit == 0U ? 0 : std::clamp(decimals.value_or(1), 0, maximum_decimals);
    std::string text = fixed(scaled, fraction);
    double rounded = 0.0;
    std::from_chars(text.data(), text.data() + text.size(), rounded);
    if (rounded >= base && unit + 1U < units.size()) {
        ++unit;
        text = fixed(rounded / base, std::clamp(decimals.value_or(1), 0, maximum_decimals));
    }
    text = trimmed(std::move(text));
    const bool zero = shows_zero(text);
    return std::string(negative && !zero ? "-" : "") +
           localized(text, false, "", decimal_separator) + " " + std::string(units[unit]);
}

std::string time_ago(const double milliseconds, const double now, const bool short_style) {
    const double difference = milliseconds - now;
    if (!std::isfinite(difference))
        return std::string();
    const bool future = difference > 0.0;
    const double seconds = std::abs(difference) / 1000.0;
    if (seconds < 10.0)
        return short_style ? "now" : "just now";
    struct Unit final {
        double seconds;
        std::string_view name;
        std::string_view short_name;
    };
    // A month is a twelfth of a Julian year, so twelve of them make a year.
    static constexpr std::array<Unit, 7U> units{
        Unit{1.0, "second", "s"},      Unit{60.0, "minute", "m"},   Unit{3600.0, "hour", "h"},
        Unit{86400.0, "day", "d"},     Unit{604800.0, "week", "w"}, Unit{2629800.0, "month", "mo"},
        Unit{31557600.0, "year", "y"},
    };
    std::size_t index = 0U;
    while (index + 1U < units.size() && seconds >= units[index + 1U].seconds)
        ++index;
    const auto count = static_cast<std::int64_t>(std::floor(seconds / units[index].seconds));
    std::string amount = std::to_string(count);
    if (short_style) {
        amount += units[index].short_name;
    } else {
        amount += " ";
        amount += units[index].name;
        if (count != 1)
            amount += "s";
    }
    return future ? "in " + amount : amount + " ago";
}

std::string format_duration(const double milliseconds, const std::string_view style) {
    if (!std::isfinite(milliseconds))
        return std::string();
    const std::string sign = milliseconds < 0.0 ? "-" : "";
    const double magnitude = std::abs(milliseconds);
    if (style != "short" && style != "long") {
        const auto total = static_cast<std::int64_t>(std::floor(magnitude / 1000.0));
        const std::int64_t hours = total / 3600;
        const std::int64_t minutes = total / 60 % 60;
        const std::int64_t seconds = total % 60;
        const auto two = [](const std::int64_t value) {
            return (value < 10 ? "0" : "") + std::to_string(value);
        };
        if (hours > 0)
            return sign + std::to_string(hours) + ":" + two(minutes) + ":" + two(seconds);
        return sign + std::to_string(minutes) + ":" + two(seconds);
    }
    const bool spelled = style == "long";
    const auto unit_text = [spelled](const std::int64_t count, const std::string_view name,
                                     const std::string_view short_name) {
        if (!spelled)
            return std::to_string(count) + std::string(short_name);
        return std::to_string(count) + " " + std::string(name) + (count == 1 ? "" : "s");
    };
    if (magnitude < 1000.0) {
        return sign +
               unit_text(static_cast<std::int64_t>(std::round(magnitude)), "millisecond", "ms");
    }
    const auto total = static_cast<std::int64_t>(std::floor(magnitude / 1000.0));
    const std::array<std::pair<std::int64_t, std::pair<std::string_view, std::string_view>>, 4U>
        parts{{
            {total / 86400, {"day", "d"}},
            {total / 3600 % 24, {"hour", "h"}},
            {total / 60 % 60, {"minute", "m"}},
            {total % 60, {"second", "s"}},
        }};
    std::vector<std::string> shown;
    for (const auto& [count, names] : parts) {
        if (count == 0)
            continue;
        shown.push_back(unit_text(count, names.first, names.second));
        if (shown.size() == 2U)
            break;
    }
    std::string result = sign;
    for (std::size_t index = 0U; index < shown.size(); ++index) {
        if (index > 0U)
            result += " ";
        result += shown[index];
    }
    return result;
}

} // namespace strata::runtime
