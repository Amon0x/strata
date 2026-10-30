#include "runtime/expression.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "runtime/formatting.hpp"

namespace strata::runtime {
namespace {

constexpr std::size_t maximum_derived_items = 100'000U;

[[nodiscard]] std::string operator+(const char* const left, const std::string_view right) {
    std::string result(left);
    result.append(right);
    return result;
}

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                    : static_cast<char>(character);
    });
    return value;
}

[[nodiscard]] std::string upper_ascii(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return character >= 'a' && character <= 'z' ? static_cast<char>(character - 'a' + 'A')
                                                    : static_cast<char>(character);
    });
    return value;
}

[[nodiscard]] std::size_t utf16_length(const std::string_view value) noexcept {
    std::size_t units = 0U;
    for (std::size_t index = 0U; index < value.size();) {
        const auto lead = static_cast<unsigned char>(value[index]);
        if (lead < 0x80U) {
            ++index;
            ++units;
        } else if ((lead & 0xE0U) == 0xC0U) {
            index += 2U;
            ++units;
        } else if ((lead & 0xF0U) == 0xE0U) {
            index += 3U;
            ++units;
        } else {
            index += 4U;
            units += 2U;
        }
    }
    return units;
}

/** The byte where a UTF-16 unit offset falls, clamped to the string; never inside a sequence. */
[[nodiscard]] std::size_t utf8_offset(const std::string_view value, const std::size_t units) noexcept {
    std::size_t counted = 0U;
    std::size_t index = 0U;
    while (index < value.size() && counted < units) {
        const auto lead = static_cast<unsigned char>(value[index]);
        const std::size_t width = lead < 0x80U                ? 1U
                                  : (lead & 0xE0U) == 0xC0U ? 2U
                                  : (lead & 0xF0U) == 0xE0U ? 3U
                                                            : 4U;
        index = std::min(value.size(), index + width);
        counted += width == 4U ? 2U : 1U;
    }
    return index;
}

/** A proleptic Gregorian civil date from days since 1970-01-01 (Howard Hinnant's algorithm). */
struct CivilDate final {
    std::int64_t year = 1970;
    unsigned month = 1U;
    unsigned day = 1U;
};

[[nodiscard]] CivilDate civil_from_days(std::int64_t days) noexcept {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto day_of_era = static_cast<unsigned>(days - era * 146097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
    const unsigned day_of_year = day_of_era - (365U * year_of_era + year_of_era / 4U -
                                               year_of_era / 100U);
    const unsigned month_index = (5U * day_of_year + 2U) / 153U;
    const unsigned day = day_of_year - (153U * month_index + 2U) / 5U + 1U;
    const unsigned month = month_index < 10U ? month_index + 3U : month_index - 9U;
    return CivilDate{static_cast<std::int64_t>(year_of_era) + era * 400 + (month <= 2U ? 1 : 0),
                     month, day};
}

/**
 * Formats a UTC millisecond timestamp, shifted by an offset, with a pattern: yyyy yy, MMMM MMM MM M,
 * dd d, EEEE EEE, HH H, hh h, mm m, ss s, SSS and a. Text in single quotes is literal ('' is a
 * quote); any other character is copied. Names are English.
 */
[[nodiscard]] std::string format_date(const double milliseconds, const std::string_view pattern,
                                      const double offset_minutes,
                                      const std::vector<std::string>& month_names,
                                      const std::vector<std::string>& weekday_names) {
    static const std::vector<std::string> english_months{
        "January", "February", "March",     "April",   "May",      "June",
        "July",    "August",   "September", "October", "November", "December"};
    static const std::vector<std::string> english_weekdays{
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    // Authored names replace English only when complete: twelve months, seven days from Sunday.
    const std::vector<std::string>& months =
        month_names.size() == 12U ? month_names : english_months;
    const std::vector<std::string>& weekdays =
        weekday_names.size() == 7U ? weekday_names : english_weekdays;
    // Short names are a name's first three characters.
    const auto short_name = [](const std::string& name) {
        return name.substr(0U, utf8_offset(name, 3U));
    };
    // Bounded to years -9999..9999 so every field stays a sensible width.
    constexpr double limit = 253402300799999.0;
    const double clamped = std::clamp(
        std::floor(milliseconds + (std::isfinite(offset_minutes) ? offset_minutes : 0.0) * 60000.0),
        -limit, limit);
    const auto total = static_cast<std::int64_t>(clamped);
    const std::int64_t day_milliseconds = 86'400'000;
    std::int64_t days = total / day_milliseconds;
    std::int64_t within = total % day_milliseconds;
    if (within < 0) {
        within += day_milliseconds;
        --days;
    }
    const CivilDate date = civil_from_days(days);
    const auto hour = static_cast<int>(within / 3'600'000);
    const auto minute = static_cast<int>(within / 60'000 % 60);
    const auto second = static_cast<int>(within / 1000 % 60);
    const auto millisecond = static_cast<int>(within % 1000);
    // 1970-01-01 was a Thursday; names are indexed from Sunday.
    const auto weekday = static_cast<std::size_t>(((days % 7) + 7 + 4) % 7);
    const auto padded = [](const std::int64_t value, const std::size_t width) {
        std::string digits = std::to_string(value < 0 ? -value : value);
        if (digits.size() < width)
            digits.insert(0U, width - digits.size(), '0');
        return value < 0 ? "-" + digits : digits;
    };
    std::string result;
    for (std::size_t index = 0U; index < pattern.size();) {
        const char token = pattern[index];
        if (token == '\'') {
            const std::size_t close = pattern.find('\'', index + 1U);
            if (close == index + 1U) {
                result.push_back('\'');
                index += 2U;
                continue;
            }
            const std::size_t end = close == std::string_view::npos ? pattern.size() : close;
            result.append(pattern.substr(index + 1U, end - index - 1U));
            index = end == pattern.size() ? end : end + 1U;
            continue;
        }
        std::size_t run = 1U;
        while (index + run < pattern.size() && pattern[index + run] == token)
            ++run;
        index += run;
        switch (token) {
        case 'y':
            result += run == 2U ? padded(((date.year % 100) + 100) % 100, 2U)
                                : padded(date.year, run);
            break;
        case 'M':
            result += run >= 4U   ? months[date.month - 1U]
                      : run == 3U ? short_name(months[date.month - 1U])
                                  : padded(date.month, run);
            break;
        case 'd': result += padded(date.day, run); break;
        case 'E':
            result += run >= 4U ? weekdays[weekday] : short_name(weekdays[weekday]);
            break;
        case 'H': result += padded(hour, run); break;
        case 'h': result += padded(hour % 12 == 0 ? 12 : hour % 12, run); break;
        case 'm': result += padded(minute, run); break;
        case 's': result += padded(second, run); break;
        case 'S': result += padded(millisecond, 3U).substr(0U, std::min<std::size_t>(run, 3U)); break;
        case 'a': result += hour < 12 ? "AM" : "PM"; break;
        default: result.append(run, token); break;
        }
    }
    return result;
}

/** A `slice` bound: a negative value counts back from `size`; the result is clamped to it. */
[[nodiscard]] std::size_t slice_bound(const double bound, const std::size_t size) noexcept {
    if (!std::isfinite(bound))
        return bound > 0.0 ? size : 0U;
    const double resolved =
        bound < 0.0 ? static_cast<double>(size) + std::trunc(bound) : std::trunc(bound);
    return static_cast<std::size_t>(std::clamp(resolved, 0.0, static_cast<double>(size)));
}

[[nodiscard]] std::uint8_t color_channel(const Value& value) noexcept {
    const double numeric = value.number() != nullptr ? *value.number() : 0.0;
    const double scaled = numeric <= 1.0 ? numeric * 255.0 : numeric;
    return static_cast<std::uint8_t>(std::clamp(std::trunc(scaled), 0.0, 255.0));
}

[[nodiscard]] int compare_keys(const Value& left, const Value& right) {
    if (left.number() != nullptr && right.number() != nullptr) {
        return *left.number() < *right.number() ? -1 : *left.number() > *right.number() ? 1 : 0;
    }
    if (left.duration() != nullptr && right.duration() != nullptr) {
        return left.duration()->nanoseconds < right.duration()->nanoseconds   ? -1
               : left.duration()->nanoseconds > right.duration()->nanoseconds ? 1
                                                                              : 0;
    }
    if (left.boolean() != nullptr && right.boolean() != nullptr) {
        return *left.boolean() == *right.boolean() ? 0 : *left.boolean() ? 1 : -1;
    }
    const std::string left_text = lower_ascii(display_string(left));
    const std::string right_text = lower_ascii(display_string(right));
    return left_text < right_text ? -1 : left_text > right_text ? 1 : 0;
}

[[nodiscard]] const ValueList* collection_items(const ExpressionValue& value) {
    if (const Value* scalar = value.value())
        return scalar->list();
    if (const auto* collection = value.collection())
        return (*collection)->items.list();
    return nullptr;
}

/** Scalar projection used only when executable composites cross into data-only consumers. */
[[nodiscard]] Value materialized_value(const ExpressionValue& value) {
    if (const Value* scalar = value.value())
        return *scalar;
    if (const auto* collection = value.collection())
        return (*collection)->items;
    return Value{};
}

/** A value a literal can hold as plain data: not executable, and no state binding to keep. */
[[nodiscard]] bool plain_data(const ExpressionValue& value) noexcept {
    return !value.executable() && value.lexical_state_binding() == nullptr;
}

[[nodiscard]] std::pair<std::size_t, std::size_t> collection_counts(const ExpressionValue& value) {
    if (const auto* collection = value.collection())
        return {(*collection)->total, (*collection)->matched};
    const ValueList* list = value.value() != nullptr ? value.value()->list() : nullptr;
    return list != nullptr
               ? std::pair<std::size_t, std::size_t>{list->values.size(), list->values.size()}
               : std::pair<std::size_t, std::size_t>{0U, 0U};
}

/** Names the evaluator looks arguments and properties up by, interned once. */
struct Names final {
    Symbol source = Symbol::intern("source");
    Symbol predicate = Symbol::intern("predicate");
    Symbol transform = Symbol::intern("transform");
    Symbol selector = Symbol::intern("selector");
    Symbol initial = Symbol::intern("initial");
    Symbol condition = Symbol::intern("condition");
    Symbol when_true = Symbol::intern("whenTrue");
    Symbol when_false = Symbol::intern("whenFalse");
    Symbol value = Symbol::intern("value");
    Symbol min = Symbol::intern("min");
    Symbol max = Symbol::intern("max");
    Symbol precision = Symbol::intern("precision");
    Symbol separator = Symbol::intern("separator");
    Symbol needle = Symbol::intern("needle");
    Symbol search = Symbol::intern("search");
    Symbol replacement = Symbol::intern("replacement");
    Symbol start = Symbol::intern("start");
    Symbol end = Symbol::intern("end");
    Symbol step = Symbol::intern("step");
    Symbol fill = Symbol::intern("fill");
    Symbol base = Symbol::intern("base");
    Symbol exponent = Symbol::intern("exponent");
    Symbol after = Symbol::intern("after");
    Symbol next = Symbol::intern("next");
    Symbol pattern = Symbol::intern("pattern");
    Symbol utc_offset_minutes = Symbol::intern("utcOffsetMinutes");
    Symbol month_names = Symbol::intern("monthNames");
    Symbol weekday_names = Symbol::intern("weekdayNames");
    Symbol decimals = Symbol::intern("decimals");
    Symbol grouping = Symbol::intern("grouping");
    Symbol compact = Symbol::intern("compact");
    Symbol currency = Symbol::intern("currency");
    Symbol percent = Symbol::intern("percent");
    Symbol sign_display = Symbol::intern("signDisplay");
    Symbol group_separator = Symbol::intern("groupSeparator");
    Symbol decimal_separator = Symbol::intern("decimalSeparator");
    Symbol binary = Symbol::intern("binary");
    Symbol now = Symbol::intern("now");
    Symbol format_style = Symbol::intern("style");
    Symbol action = Symbol::intern("action");
    Symbol prefix = Symbol::intern("prefix");
    Symbol suffix = Symbol::intern("suffix");
    Symbol red = Symbol::intern("red");
    Symbol green = Symbol::intern("green");
    Symbol blue = Symbol::intern("blue");
    Symbol alpha = Symbol::intern("alpha");
    Symbol name = Symbol::intern("name");
    Symbol active = Symbol::intern("active");
    Symbol backdrop_source = Symbol::intern("backdropSource");
    Symbol refresh_rate = Symbol::intern("refreshRate");
    Symbol size = Symbol::intern("size");
    Symbol length = Symbol::intern("length");
    Symbol is_empty = Symbol::intern("isEmpty");
    Symbol items = Symbol::intern("items");
    Symbol matched = Symbol::intern("matched");
    Symbol total = Symbol::intern("total");
    Symbol range_start = Symbol::intern("rangeStart");
    Symbol range_end = Symbol::intern("rangeEnd");
    Symbol cache_hits = Symbol::intern("cacheHits");
    Symbol rebuilds = Symbol::intern("rebuilds");
    Symbol target = Symbol::intern("target");
    Symbol duration = Symbol::intern("duration");
    Symbol easing = Symbol::intern("easing");
    Symbol delay = Symbol::intern("delay");
    Symbol from = Symbol::intern("from");
};

[[nodiscard]] const Names& names() {
    static const Names instance;
    return instance;
}

[[nodiscard]] bool same_state_binding(const LexicalStateBinding* const left,
                                      const LexicalStateBinding* const right) {
    return left == right || (left != nullptr && right != nullptr && *left == *right);
}

[[nodiscard]] bool same_lambda(const LambdaValue& left, const LambdaValue& right) {
    if (&left == &right)
        return true;
    if (left.program != right.program || left.expression != right.expression ||
        left.function != right.function || (left.group == nullptr) != (right.group == nullptr)) {
        return false;
    }
    // A block's function closes over its group's scope, which a rebuild replaces.
    const ExpressionScope& first = left.group != nullptr ? left.group->scope : left.captured;
    const ExpressionScope& second = right.group != nullptr ? right.group->scope : right.captured;
    if (first.shared_contextual_host_roots() != second.shared_contextual_host_roots() &&
        first.contextual_host_roots() != second.contextual_host_roots()) {
        return false;
    }
    const FrozenHostReads* first_frozen = first.host_dependency_overrides();
    const FrozenHostReads* second_frozen = second.host_dependency_overrides();
    if (first_frozen != second_frozen &&
        (first_frozen == nullptr || second_frozen == nullptr || *first_frozen != *second_frozen)) {
        return false;
    }
    if (first.frame() == second.frame())
        return true;
    // Only what the body reads can differ in effect: of a name it reads only fields of, those.
    for (const LexicalRead& read : left.program->captures(left.declaration())) {
        const ExpressionValue* first_value = first.find(read.name);
        const ExpressionValue* second_value = second.find(read.name);
        if ((first_value == nullptr) != (second_value == nullptr))
            return false;
        if (first_value == nullptr)
            continue;
        if (read.path.empty()) {
            if (!same_expression_value(*first_value, *second_value))
                return false;
            continue;
        }
        const std::optional<ExpressionValue> first_field =
            project_lexical_read(*first_value, read.path);
        const std::optional<ExpressionValue> second_field =
            project_lexical_read(*second_value, read.path);
        if (first_field.has_value() != second_field.has_value() ||
            (first_field.has_value() && !same_expression_value(*first_field, *second_field))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<DiagnosticRange> portable_expression_range(const data::JsonView expression) {
    const data::JsonView span = expression.find("span");
    const std::optional<std::string_view> source_id = span.find("sourceId").string();
    const data::JsonView start = span.find("start");
    const data::JsonView end = span.find("end");
    if (!source_id.has_value() || !start || !end)
        return std::nullopt;
    const auto position = [](const data::JsonView value) -> std::optional<DiagnosticPosition> {
        const std::optional<std::int64_t> line = value.find("line").integer();
        const std::optional<std::int64_t> column = value.find("column").integer();
        if (!line.has_value() || !column.has_value() || *line <= 0 || *column <= 0 ||
            *line > static_cast<std::int64_t>(UINT32_MAX) ||
            *column > static_cast<std::int64_t>(UINT32_MAX)) {
            return std::nullopt;
        }
        std::optional<std::uint64_t> offset;
        if (const std::optional<std::int64_t> encoded = value.find("offset").integer();
            encoded.has_value() && *encoded >= 0) {
            offset = static_cast<std::uint64_t>(*encoded);
        }
        return DiagnosticPosition{
            static_cast<std::uint32_t>(*line),
            static_cast<std::uint32_t>(*column),
            offset,
        };
    };
    const std::optional<DiagnosticPosition> start_position = position(start);
    const std::optional<DiagnosticPosition> end_position = position(end);
    return start_position.has_value() && end_position.has_value()
               ? std::optional<DiagnosticRange>(DiagnosticRange{
                     std::string(*source_id),
                     *start_position,
                     *end_position,
                 })
               : std::nullopt;
}

ExpressionValue::ExpressionValue() : storage_(Value{}) {}
ExpressionValue::ExpressionValue(Value value) : storage_(std::move(value)) {}
ExpressionValue::ExpressionValue(Value value, LexicalStateBinding state_binding)
    : storage_(std::move(value)),
      lexical_state_binding_(
          std::make_shared<const LexicalStateBinding>(std::move(state_binding))) {}
ExpressionValue::ExpressionValue(Value value,
                                 std::shared_ptr<const LexicalStateBinding> state_binding)
    : storage_(std::move(value)), lexical_state_binding_(std::move(state_binding)) {}
ExpressionValue::ExpressionValue(std::shared_ptr<const CollectionViewValue> value)
    : storage_(std::move(value)) {}
ExpressionValue::ExpressionValue(std::shared_ptr<const LambdaValue> value)
    : storage_(std::move(value)) {}
ExpressionValue::ExpressionValue(std::shared_ptr<const ActionValue> value)
    : storage_(std::move(value)) {}
ExpressionValue::ExpressionValue(std::shared_ptr<const ExpressionListValue> value)
    : storage_(std::move(value)) {}
ExpressionValue::ExpressionValue(std::shared_ptr<const ExpressionObjectValue> value)
    : storage_(std::move(value)) {}
ExpressionValue::ExpressionValue(std::shared_ptr<const ComponentTemplateValue> value)
    : storage_(std::move(value)) {}
const Value* ExpressionValue::value() const {
    if (const Value* scalar = std::get_if<Value>(&storage_))
        return scalar;
    if (const auto* list_value = list())
        return &(**list_value).materialized();
    if (const auto* object_value = object())
        return &(**object_value).materialized();
    if (const auto* component_value = component_template())
        return &(**component_value).materialized;
    return nullptr;
}
Value ExpressionValue::take_value() && {
    return std::move(std::get<Value>(storage_));
}
const Value* ExpressionValue::data_value() const {
    if (const Value* materialized = value())
        return materialized;
    if (const auto* collection_value = collection();
        collection_value != nullptr && *collection_value != nullptr) {
        return &(**collection_value).items;
    }
    return nullptr;
}
const std::shared_ptr<const CollectionViewValue>* ExpressionValue::collection() const noexcept {
    return std::get_if<std::shared_ptr<const CollectionViewValue>>(&storage_);
}
const std::shared_ptr<const LambdaValue>* ExpressionValue::lambda() const noexcept {
    return std::get_if<std::shared_ptr<const LambdaValue>>(&storage_);
}
const std::shared_ptr<const ActionValue>* ExpressionValue::action() const noexcept {
    return std::get_if<std::shared_ptr<const ActionValue>>(&storage_);
}
const std::shared_ptr<const ExpressionListValue>* ExpressionValue::list() const noexcept {
    return std::get_if<std::shared_ptr<const ExpressionListValue>>(&storage_);
}
const std::shared_ptr<const ExpressionObjectValue>* ExpressionValue::object() const noexcept {
    return std::get_if<std::shared_ptr<const ExpressionObjectValue>>(&storage_);
}
const std::shared_ptr<const ComponentTemplateValue>*
ExpressionValue::component_template() const noexcept {
    return std::get_if<std::shared_ptr<const ComponentTemplateValue>>(&storage_);
}
bool ExpressionValue::executable() const noexcept {
    return !std::holds_alternative<Value>(storage_);
}
ExpressionValue ExpressionValue::without_state_binding() const {
    ExpressionValue result = *this;
    result.lexical_state_binding_.reset();
    return result;
}

const Value& ExpressionListValue::materialized() const {
    std::call_once(materialize_once_, [this] {
        std::vector<Value> elements;
        elements.reserve(values.size());
        for (const ExpressionValue& value : values)
            elements.push_back(materialized_value(value));
        materialized_ = Value(std::move(elements));
    });
    return materialized_;
}

const ExpressionValue* ExpressionObjectValue::field(const std::string_view name) const noexcept {
    const auto found = std::ranges::find(fields, name, &decltype(fields)::value_type::first);
    return found != fields.end() ? &found->second : nullptr;
}

const Value& ExpressionObjectValue::materialized() const {
    std::call_once(materialize_once_, [this] {
        std::vector<std::pair<std::string, Value>> entries;
        entries.reserve(fields.size());
        for (const auto& [name, value] : fields)
            entries.emplace_back(name, materialized_value(value));
        materialized_ = Value(std::move(entries));
    });
    return materialized_;
}

const ExpressionValue* ExpressionScope::find(const Symbol name) const noexcept {
    return lookup(name).value;
}

ScopeLookup ExpressionScope::lookup(const Symbol name) const noexcept {
    for (const ScopeFrame* frame = frame_.get(); frame != nullptr; frame = frame->parent.get()) {
        for (auto binding = frame->bindings.rbegin(); binding != frame->bindings.rend();
             ++binding) {
            if (binding->name == name && binding->has_value) {
                return ScopeLookup{binding->cell != nullptr ? binding->cell.get() : &binding->value,
                                   &*binding};
            }
        }
        if (frame->functions != nullptr) {
            for (const auto& [function_name, index] : frame->functions->functions) {
                if (function_name == name)
                    return ScopeLookup{nullptr, nullptr, frame->functions, index};
            }
        }
        if (frame->hides_parent_values)
            return {};
    }
    return {};
}

const std::shared_ptr<const LexicalStateBinding>*
ExpressionScope::script_state(const Symbol name) const noexcept {
    for (const ScopeFrame* frame = frame_.get(); frame != nullptr; frame = frame->parent.get()) {
        for (auto binding = frame->bindings.rbegin(); binding != frame->bindings.rend();
             ++binding) {
            if (binding->name != name)
                continue;
            // A local of the same name shadows the state.
            if (binding->state == ScopeStateBinding::bound && binding->binding != nullptr)
                return &binding->binding;
            if (binding->state == ScopeStateBinding::cleared || binding->has_value)
                return nullptr;
        }
        if (frame->functions != nullptr &&
            std::ranges::any_of(frame->functions->functions,
                                [name](const auto& function) { return function.first == name; })) {
            return nullptr;
        }
    }
    return nullptr;
}

const ExpressionValue* ExpressionScope::find(const std::string_view name) const {
    const std::optional<Symbol> symbol = Symbol::find(name);
    return symbol.has_value() ? find(*symbol) : nullptr;
}

const std::shared_ptr<const LexicalStateBinding>*
ExpressionScope::state_binding(const Symbol name) const noexcept {
    for (const ScopeFrame* frame = frame_.get(); frame != nullptr; frame = frame->parent.get()) {
        for (auto binding = frame->bindings.rbegin(); binding != frame->bindings.rend();
             ++binding) {
            if (binding->name != name || binding->state == ScopeStateBinding::inherit)
                continue;
            return binding->state == ScopeStateBinding::bound && binding->binding != nullptr
                       ? &binding->binding
                       : nullptr;
        }
        if (frame->hides_parent_states)
            return nullptr;
    }
    return nullptr;
}

const HostRoots& ExpressionScope::contextual_host_roots() const noexcept {
    static const HostRoots none;
    return contextual_host_roots_ != nullptr ? *contextual_host_roots_ : none;
}

const std::string& ExpressionScope::component_path() const noexcept {
    static const std::string none;
    return component_path_ != nullptr ? *component_path_ : none;
}

void ExpressionScope::push(ScopeFrame frame) {
    frame.parent = std::move(frame_);
    frame_ = std::make_shared<const ScopeFrame>(std::move(frame));
}

void ExpressionScope::bind(const Symbol name, ExpressionValue value) {
    ScopeFrame frame;
    frame.bindings.push_back(
        ScopeBinding{name, true, ScopeStateBinding::inherit, std::move(value), nullptr});
    push(std::move(frame));
}

void ExpressionScope::bind(const std::string_view name, ExpressionValue value) {
    bind(Symbol::intern(name), std::move(value));
}

void ExpressionScope::declare(const Symbol name, ExpressionValue value,
                              std::shared_ptr<const LexicalStateBinding> binding) {
    ScopeFrame frame;
    const ScopeStateBinding state =
        binding != nullptr ? ScopeStateBinding::bound : ScopeStateBinding::cleared;
    frame.bindings.push_back(ScopeBinding{name, true, state, std::move(value), std::move(binding)});
    push(std::move(frame));
}

std::shared_ptr<ExpressionValue> ExpressionScope::declare_variable(const Symbol name,
                                                                  ExpressionValue value) {
    auto cell = std::make_shared<ExpressionValue>(std::move(value));
    ScopeFrame frame;
    frame.bindings.push_back(
        ScopeBinding{name, true, ScopeStateBinding::cleared, {}, nullptr, cell});
    push(std::move(frame));
    return cell;
}

void ExpressionScope::bind_functions(const std::shared_ptr<const FunctionGroup>& group) {
    ScopeFrame frame;
    frame.functions = group.get();
    push(std::move(frame));
    keep(group);
}

void ExpressionScope::keep(std::shared_ptr<const FunctionGroup> group) {
    kept_ = std::make_shared<const KeptFunctionGroups>(
        KeptFunctionGroups{std::move(group), std::move(kept_)});
}

ExpressionScope ExpressionScope::without_newest(const FunctionGroup* const group) const {
    ExpressionScope result = *this;
    if (result.kept_ != nullptr && result.kept_->group.get() == group)
        result.kept_ = result.kept_->next;
    return result;
}

void ExpressionScope::set_component_path(std::string path) {
    component_path_ = std::make_shared<const std::string>(std::move(path));
}

void ExpressionScope::set_contextual_host_root(std::string name, Value value) {
    auto roots = contextual_host_roots_ != nullptr
                     ? std::make_shared<HostRoots>(*contextual_host_roots_)
                     : std::make_shared<HostRoots>();
    roots->insert_or_assign(std::move(name), std::move(value));
    contextual_host_roots_ = std::move(roots);
}

bool same_action(const std::shared_ptr<const ActionValue>& left,
                 const std::shared_ptr<const ActionValue>& right) {
    if (left == right)
        return true;
    if (left == nullptr || right == nullptr || left->composition != right->composition ||
        left->lexical_state_binding != right->lexical_state_binding ||
        left->children.size() != right->children.size() ||
        (left->action == nullptr) != (right->action == nullptr)) {
        return false;
    }
    if (left->action != nullptr && left->action != right->action) {
        const Action& first = *left->action;
        const Action& second = *right->action;
        // Registered contracts are shared; a dynamic action builds its contract from the id.
        const bool same_contract = first.contract == second.contract ||
                                   (first.dynamic && second.dynamic && first.id() == second.id());
        if (!same_contract || first.dynamic != second.dynamic || first.payload != second.payload ||
            first.origin != second.origin) {
            return false;
        }
    }
    for (std::size_t index = 0U; index < left->children.size(); ++index) {
        if (!same_action(left->children[index], right->children[index]))
            return false;
    }
    if ((left->script == nullptr) != (right->script == nullptr))
        return false;
    return left->script == nullptr || left->script == right->script ||
           same_lambda(*left->script, *right->script);
}

const std::shared_ptr<const ActionContract>& script_action_contract() {
    static const std::shared_ptr<const ActionContract> contract =
        std::make_shared<const ActionContract>(ActionContract{
            "script.run",
            ValueSchema::any(),
            ActionDispatchPolicy::framework,
            "the event",
            "Runs a script handler with the event.",
        });
    return contract;
}

const Value* ScriptTransaction::written(const StateAddress& address) const noexcept {
    const auto found = std::ranges::find_if(writes, [&address](const ScriptWrite& write) {
        return write.binding->address == address;
    });
    return found != writes.end() ? &found->value : nullptr;
}

void ScriptTransaction::write(std::shared_ptr<const LexicalStateBinding> binding, Value value) {
    const auto found = std::ranges::find_if(writes, [&binding](const ScriptWrite& write) {
        return write.binding->address == binding->address;
    });
    if (found != writes.end()) {
        found->value = std::move(value);
        return;
    }
    writes.push_back(ScriptWrite{std::move(binding), std::move(value)});
}

bool same_expression_value(const ExpressionValue& left, const ExpressionValue& right) {
    if (!same_state_binding(left.lexical_state_binding(), right.lexical_state_binding()))
        return false;
    if (left.list() != nullptr || right.list() != nullptr) {
        if (left.list() == nullptr || right.list() == nullptr)
            return false;
        const ExpressionListValue& first = **left.list();
        const ExpressionListValue& second = **right.list();
        if (&first == &second)
            return true;
        return std::ranges::equal(first.values, second.values, same_expression_value);
    }
    if (left.object() != nullptr || right.object() != nullptr) {
        if (left.object() == nullptr || right.object() == nullptr)
            return false;
        const ExpressionObjectValue& first = **left.object();
        const ExpressionObjectValue& second = **right.object();
        if (&first == &second)
            return true;
        return std::ranges::equal(first.fields, second.fields, [](const auto& a, const auto& b) {
            return a.first == b.first && same_expression_value(a.second, b.second);
        });
    }
    if (left.component_template() != nullptr || right.component_template() != nullptr) {
        if (left.component_template() == nullptr || right.component_template() == nullptr)
            return false;
        const ComponentTemplateValue& first = **left.component_template();
        const ComponentTemplateValue& second = **right.component_template();
        if (&first == &second)
            return true;
        return first.component == second.component &&
               std::ranges::equal(
                   first.arguments, second.arguments, [](const auto& a, const auto& b) {
                       return a.first == b.first && same_expression_value(a.second, b.second);
                   });
    }
    if (left.collection() != nullptr || right.collection() != nullptr) {
        if (left.collection() == nullptr || right.collection() == nullptr)
            return false;
        const auto& first = *left.collection();
        const auto& second = *right.collection();
        return first == second || (first != nullptr && second != nullptr &&
                                   collection_view_immutable_identity(*first) ==
                                       collection_view_immutable_identity(*second));
    }
    if (left.action() != nullptr || right.action() != nullptr) {
        return left.action() != nullptr && right.action() != nullptr &&
               same_action(*left.action(), *right.action());
    }
    if (left.lambda() != nullptr || right.lambda() != nullptr) {
        if (left.lambda() == nullptr || right.lambda() == nullptr)
            return false;
        const auto& first = *left.lambda();
        const auto& second = *right.lambda();
        return first == second ||
               (first != nullptr && second != nullptr && same_lambda(*first, *second));
    }
    return *left.value() == *right.value();
}

ExpressionDependencyValueKind ExpressionDependencyValue::kind() const noexcept {
    if (value.collection() != nullptr)
        return ExpressionDependencyValueKind::collection;
    if (value.list() != nullptr)
        return ExpressionDependencyValueKind::executable_list;
    if (value.object() != nullptr)
        return ExpressionDependencyValueKind::executable_object;
    if (value.component_template() != nullptr)
        return ExpressionDependencyValueKind::component_template;
    if (value.action() != nullptr)
        return ExpressionDependencyValueKind::action;
    if (value.lambda() != nullptr)
        return ExpressionDependencyValueKind::lambda;
    return ExpressionDependencyValueKind::scalar;
}

ExpressionDependencyValue capture_expression_dependency(const ExpressionValue& value) {
    return ExpressionDependencyValue{value};
}

ExpressionValue restore_expression_dependency(const ExpressionDependencyValue& value) {
    return value.value;
}

const std::shared_ptr<const HostPathKey>& frame_time_path() {
    static const std::shared_ptr<const HostPathKey> path = HostPathKey::make(
        {HostPathSegment::named("strataFrame"), HostPathSegment::named("time")});
    return path;
}

Value frame_time_value(const std::int64_t nanos) {
    return Value(static_cast<double>(nanos) / 1.0e6);
}

std::string canonical_host_dependency_path(const std::span<const HostPathSegment> path) {
    return canonical_host_path(path);
}

ExpressionRuntime::ExpressionRuntime(const HostStore& host, const RuntimeActionRegistry& actions)
    : host_(host), actions_(actions) {}

void DependencyRecorder::record_lexical(const LexicalRead& read, const ExpressionValue& value) {
    if (!hidden_.empty() && std::ranges::find(hidden_, read.name) != hidden_.end())
        return;
    if (upstream_ != nullptr)
        upstream_->lexical(read, value);
    if (lexical_ == Keep::none)
        return;
    if (outer_ != nullptr && !lexical_read_current(*outer_, read, value))
        return;
    if (lexical_ == Keep::latest) {
        kept().lexical.insert_or_assign(read, capture_expression_dependency(value));
    } else if (const auto [kept_read, inserted] =
                   kept().lexical.try_emplace(read, capture_expression_dependency(value));
               inserted && ordered_) {
        kept_->order.push_back(Read{&kept_read->first, nullptr});
    }
}

std::optional<ExpressionValue> project_lexical_read(const ExpressionValue& value,
                                                    const std::string_view path) {
    if (path.empty())
        return value;
    std::optional<ExpressionValue> projected;
    const ExpressionValue* at = &value;
    std::size_t begin = 0U;
    while (begin < path.size()) {
        const std::size_t end = path.find(lexical_path_separator, begin);
        const std::string_view field = path.substr(begin, end - begin);
        begin = end + 1U;
        if (const auto* object = at->object()) {
            const ExpressionValue* found = (**object).field(field);
            if (found == nullptr)
                return std::nullopt;
            projected = *found;
        } else if (const Value* data = at->value()) {
            const Value* found = data->field(field);
            if (found == nullptr)
                return std::nullopt;
            projected = ExpressionValue(*found);
        } else {
            return std::nullopt;
        }
        at = &*projected;
    }
    return projected;
}

bool lexical_read_current(const ExpressionScope& scope, const LexicalRead& read,
                          const ExpressionValue& seen) {
    const ExpressionValue* const bound = scope.find(read.name);
    if (bound == nullptr)
        return false;
    if (read.path.empty())
        return same_expression_value(*bound, seen);
    const std::optional<ExpressionValue> field = project_lexical_read(*bound, read.path);
    return field.has_value() && same_expression_value(*field, seen);
}

void DependencyRecorder::record_host(const ExpressionHostDependency& dependency) {
    if (upstream_ != nullptr)
        upstream_->host(dependency);
    if (host_ == Keep::none)
        return;
    if (host_ == Keep::latest) {
        kept().host.insert_or_assign(dependency.canonical(), dependency);
        return;
    }
    const auto [read, inserted] = kept().host.try_emplace(dependency.canonical(), dependency);
    if (inserted && ordered_)
        kept_->order.push_back(Read{{}, &read->first});
}

ExpressionValue ExpressionRuntime::evaluate(const Program& program, const ExpressionId expression,
                                            const ExpressionScope& scope,
                                            DependencyRecorder& reads) {
    // A function called here that throws, outside any handler, makes the expression null.
    if (active_run_ != nullptr)
        return evaluate_node(program, expression, scope, reads);
    try {
        return evaluate_node(program, expression, scope, reads);
    } catch (const ScriptThrow& thrown) {
        const Value* message = thrown.value.field("message");
        report(program.expression(expression), scope, "STRATA.SCRIPT.UNCAUGHT",
               "A function threw: " +
                   (message != nullptr ? display_string(*message) : display_string(thrown.value)));
        return ExpressionValue{};
    }
}

ExpressionValue ExpressionRuntime::evaluate(const Program& program, const ExpressionId expression,
                                            const ExpressionScope& scope) {
    DependencyRecorder ignored;
    return evaluate(program, expression, scope, ignored);
}

ExpressionValue ExpressionRuntime::evaluate(const data::JsonView expression,
                                            const ExpressionScope& scope) {
    DependencyRecorder ignored;
    return evaluate(expression, scope, ignored);
}

ExpressionValue ExpressionRuntime::evaluate(const data::JsonView expression,
                                            const ExpressionScope& scope,
                                            DependencyRecorder& reads) {
    std::shared_ptr<const Program>& program =
        standalone_programs_[data::encode_canonical_json(expression)];
    if (program == nullptr)
        program = Program::lower_expression(expression);
    return evaluate(*program, program->root(), scope, reads);
}

const std::vector<RuntimeDiagnostic>& ExpressionRuntime::diagnostics() const noexcept {
    return diagnostics_;
}

void ExpressionRuntime::clear_diagnostics() {
    diagnostics_.clear();
}

void ExpressionRuntime::clear_caches() noexcept {
    collection_cache_.clear();
    collection_cache_entries_ = 0U;
    dynamic_host_paths_.clear();
    constant_functions_.clear();
    constant_values_.clear();
    memoized_calls_.clear();
}

ExpressionHostDependency
ExpressionRuntime::read_host_dependency(const std::shared_ptr<const HostPathKey>& key,
                                        const ExpressionScope& scope) const {
    if (key == nullptr || key->segments.empty() ||
        key->segments.front().kind != HostPathSegmentKind::field ||
        key->segments.front().field.empty()) {
        throw std::invalid_argument("host dependency path requires a named root");
    }
    const std::vector<HostPathSegment>& path = key->segments;
    // Only retained lazy identity evaluators freeze reads.
    if (const FrozenHostReads* frozen = scope.host_dependency_overrides();
        frozen != nullptr && !frozen->empty()) {
        if (const auto read = frozen->find(key->canonical); read != frozen->end())
            return read->second;
    }

    const HostRoots& roots = scope.contextual_host_roots();
    const auto contextual = roots.find(path.front().field);
    if (contextual == roots.end()) {
        std::optional<HostResolution> resolution = host_.resolve_with_origin(path);
        return ExpressionHostDependency{
            key,
            false,
            resolution.has_value() ? std::optional<Value>(std::move(resolution->value))
                                   : std::nullopt,
            resolution.has_value() ? std::optional<std::string>(std::move(resolution->snapshot_id))
                                   : std::nullopt,
            resolution.has_value() ? resolution->snapshot_generation : 0U,
        };
    }

    const Value* current = &contextual->second;
    for (std::size_t index = 1U; index < path.size() && current != nullptr; ++index) {
        const HostPathSegment& segment = path[index];
        if (segment.kind == HostPathSegmentKind::field) {
            current = current->field(segment.field);
        } else if (current->list() != nullptr) {
            current = segment.index.has_value() && *segment.index < current->list()->values.size()
                          ? &current->list()->values[*segment.index]
                          : nullptr;
        } else {
            current = current->field(segment.field);
        }
    }
    return ExpressionHostDependency{
        key,          true, current != nullptr ? std::optional<Value>(*current) : std::nullopt,
        std::nullopt, 0U,
    };
}

std::optional<ExpressionHostDependency> ExpressionRuntime::host_read(const Program& program,
                                                                     const ProgramExpression& node,
                                                                     const ExpressionScope& scope,
                                                                     DependencyRecorder& reads) {
    const ProgramHostPath& path = program.host_path(node.host);
    // A local of the root's name shadows the host root.
    if (scope.find(path.root) != nullptr)
        return std::nullopt;
    if (path.fixed != nullptr)
        return read_host_dependency(path.fixed, scope);
    // Every computed step is evaluated, in order; while they look the same things up as the
    // last time, the path read is the one resolved then.
    std::shared_ptr<const HostPathKey>& cached =
        dynamic_host_paths_[ProgramNodeKey{program.serial(), node.host}];
    std::vector<HostPathSegment> segments;
    bool rebuilt = cached == nullptr;
    if (rebuilt) {
        segments.reserve(path.steps.size() + 1U);
        segments.push_back(HostPathSegment::named(std::string(path.root.name())));
    }
    for (std::size_t position = 0U; position < path.steps.size(); ++position) {
        const ProgramHostStep& step = path.steps[position];
        if (!step.dynamic) {
            if (rebuilt)
                segments.push_back(step.segment);
            continue;
        }
        const ProgramExpression& index = program.expression(step.index);
        const Value lookup =
            require_value(evaluate_node(program, step.index, scope, reads), index, scope);
        const std::string* text = lookup.string();
        const std::optional<std::size_t> bounded = bounded_index(lookup);
        if (!rebuilt) {
            const HostPathSegment& previous = cached->segments[position + 1U];
            if (previous.index == bounded &&
                (text != nullptr ? previous.field == *text
                                 : previous.field == display_string(lookup))) {
                continue;
            }
            rebuilt = true;
            segments.assign(cached->segments.begin(),
                            cached->segments.begin() + static_cast<std::ptrdiff_t>(position + 1U));
        }
        segments.push_back(
            HostPathSegment::lookup(text != nullptr ? *text : display_string(lookup), bounded));
    }
    if (rebuilt)
        cached = HostPathKey::make(std::move(segments));
    return read_host_dependency(cached, scope);
}

ExpressionValue ExpressionRuntime::evaluate_node(const Program& program,
                                                 const ExpressionId expression,
                                                 const ExpressionScope& scope,
                                                 DependencyRecorder& reads) {
    const ProgramExpression& node = program.expression(expression);
    switch (node.kind) {
    case ExpressionKind::literal:
        return ExpressionValue(node.value);
    case ExpressionKind::variable:
        return evaluate_variable(program, node, scope, reads);
    case ExpressionKind::property:
        return evaluate_property(program, node, scope, reads);
    case ExpressionKind::index:
        return evaluate_index(program, node, scope, reads);
    case ExpressionKind::list: {
        // Plain data, the common case, is a list Value; the first element that is more than
        // data makes the literal an executable composite of its elements.
        const std::span<const ProgramArgument> elements = program.arguments(node);
        std::vector<Value> data;
        data.reserve(elements.size());
        for (std::size_t index = 0U; index < elements.size(); ++index) {
            // A spread puts its list's elements in place.
            if (const ProgramExpression& element = program.expression(elements[index].value);
                element.kind == ExpressionKind::spread) {
                const ExpressionValue spread = evaluate_node(program, element.first, scope, reads);
                const Value* items = spread.data_value();
                if (items != nullptr && items->list() != nullptr) {
                    for (const Value& item : items->list()->values)
                        data.push_back(item);
                } else if (items == nullptr || items->kind() != ValueKind::null_value) {
                    report(element, scope, "STRATA.DSL.RUNTIME_TYPE_MISMATCH",
                           "Only a list can be spread into a list.");
                }
                continue;
            }
            ExpressionValue value = evaluate_node(program, elements[index].value, scope, reads);
            if (plain_data(value)) {
                data.push_back(std::move(value).take_value());
                continue;
            }
            std::vector<ExpressionValue> executable;
            executable.reserve(elements.size());
            for (Value& element : data)
                executable.emplace_back(std::move(element));
            executable.push_back(std::move(value));
            for (++index; index < elements.size(); ++index)
                executable.push_back(evaluate_node(program, elements[index].value, scope, reads));
            return ExpressionValue(
                std::make_shared<const ExpressionListValue>(std::move(executable)));
        }
        return ExpressionValue(Value(std::move(data)));
    }
    case ExpressionKind::map: {
        const std::span<const ProgramArgument> entries = program.arguments(node);
        std::vector<std::pair<std::string, Value>> data;
        data.reserve(entries.size());
        for (std::size_t index = 0U; index < entries.size(); ++index) {
            ExpressionValue value = evaluate_node(program, entries[index].value, scope, reads);
            if (plain_data(value)) {
                data.emplace_back(std::string(entries[index].name.name()),
                                  std::move(value).take_value());
                continue;
            }
            std::vector<std::pair<std::string, ExpressionValue>> executable;
            executable.reserve(entries.size());
            for (auto& [name, element] : data)
                executable.emplace_back(std::move(name), ExpressionValue(std::move(element)));
            executable.emplace_back(std::string(entries[index].name.name()), std::move(value));
            for (++index; index < entries.size(); ++index) {
                executable.emplace_back(std::string(entries[index].name.name()),
                                        evaluate_node(program, entries[index].value, scope, reads));
            }
            return ExpressionValue(
                std::make_shared<const ExpressionObjectValue>(std::move(executable)));
        }
        return ExpressionValue(Value(std::move(data)));
    }
    case ExpressionKind::component_template: {
        std::map<std::string, ExpressionValue, std::less<>> arguments;
        for (const ProgramArgument& argument : program.arguments(node)) {
            arguments.insert_or_assign(std::string(argument.name.name()),
                                       evaluate_node(program, argument.value, scope, reads));
        }
        return ExpressionValue(std::make_shared<const ComponentTemplateValue>(
            ComponentTemplateValue{node.text, std::move(arguments), Value(node.text)}));
    }
    case ExpressionKind::lambda:
        // A lambda is as current as the values it captures: they are what it read.
        if (node.function != no_program_id) {
            for (const LexicalRead& read : program.captures(program.function(node.function))) {
                const ExpressionValue* captured = scope.find(read.name);
                if (captured == nullptr)
                    continue;
                if (read.path.empty()) {
                    reads.lexical(read, *captured);
                } else if (const std::optional<ExpressionValue> field =
                               project_lexical_read(*captured, read.path)) {
                    reads.lexical(read, *field);
                } else {
                    reads.lexical(read.name, *captured);
                }
            }
        }
        return ExpressionValue(std::make_shared<const LambdaValue>(LambdaValue{
            program.shared_from_this(),
            expression,
            node.function,
            scope,
            nullptr,
        }));
    case ExpressionKind::call:
        return evaluate_call(program, node, scope, reads);
    case ExpressionKind::spread:
        report(node, scope, "STRATA.DSL.RUNTIME_TYPE_MISMATCH",
               "A spread belongs in a list or record literal.");
        return ExpressionValue{};
    case ExpressionKind::await_value:
        // Only a script statement waits; anywhere else the operand is its own value.
        return evaluate_node(program, node.first, scope, reads);
    case ExpressionKind::record: {
        // Entries and spreads apply in order, a later field replacing an earlier one.
        std::vector<std::pair<std::string, Value>> fields;
        const auto set = [&fields](std::string name, Value value) {
            const auto found = std::ranges::find(fields, name, &std::pair<std::string, Value>::first);
            if (found != fields.end())
                found->second = std::move(value);
            else
                fields.emplace_back(std::move(name), std::move(value));
        };
        for (const ProgramArgument& part : program.arguments(node)) {
            const ExpressionValue value = evaluate_node(program, part.value, scope, reads);
            const Value* data = value.data_value();
            if (part.named) {
                set(std::string(part.name.name()), data != nullptr ? *data : Value{});
                continue;
            }
            if (data != nullptr && data->object() != nullptr) {
                for (const auto& [name, field] : data->object()->fields)
                    set(name, field);
            } else if (data == nullptr || data->kind() != ValueKind::null_value) {
                report(program.expression(part.value), scope, "STRATA.DSL.RUNTIME_TYPE_MISMATCH",
                       "Only a record can be spread into a record.");
            }
        }
        return ExpressionValue(Value(std::move(fields)));
    }
    case ExpressionKind::handler: {
        ExpressionValue function = evaluate_node(program, node.first, scope, reads);
        if (function.action() != nullptr)
            return function;
        if (function.lambda() == nullptr || *function.lambda() == nullptr)
            return ExpressionValue{};
        auto handler = std::make_shared<ActionValue>(
            std::make_shared<const Action>(script_action_contract(), Value{}), std::nullopt,
            std::vector<std::shared_ptr<const ActionValue>>{});
        handler->script = *function.lambda();
        return ExpressionValue(std::shared_ptr<const ActionValue>(std::move(handler)));
    }
    case ExpressionKind::action:
        return evaluate_action(program, node, scope, reads);
    case ExpressionKind::helper:
        return evaluate_helper(program, expression, scope, reads);
    case ExpressionKind::conditional: {
        const Value condition =
            require_value(evaluate_node(program, node.first, scope, reads), node, scope);
        return evaluate_node(program, truthy(condition) ? node.second : node.third, scope, reads);
    }
    case ExpressionKind::unary: {
        const Value operand =
            require_value(evaluate_node(program, node.first, scope, reads), node, scope);
        if (node.unary == UnaryOperator::logical_not)
            return ExpressionValue(Value(!truthy(operand)));
        if (operand.number() != nullptr)
            return ExpressionValue(Value(-*operand.number()));
        if (operand.duration() != nullptr)
            return ExpressionValue(Value(DurationValue{-operand.duration()->nanoseconds}));
        report(node, scope, "STRATA.DSL.RUNTIME_TYPE_MISMATCH",
               "Unary operator received an incompatible value.");
        return ExpressionValue(Value(0.0));
    }
    case ExpressionKind::binary:
        return evaluate_binary(program, node, scope, reads);
    case ExpressionKind::material: {
        std::vector<std::pair<std::string, Value>> material{
            {"id", Value(node.text)},
        };
        if (node.material_call) {
            std::vector<std::pair<std::string, Value>> parameters;
            for (const ProgramArgument& parameter : program.arguments(node)) {
                parameters.emplace_back(
                    std::string(parameter.name.name()),
                    require_value(evaluate_node(program, parameter.value, scope, reads),
                                  program.expression(parameter.value), scope));
            }
            material.emplace_back("parameters", Value(std::move(parameters)));
        }
        return ExpressionValue(Value(std::move(material)));
    }
    case ExpressionKind::unknown:
        if (node.text.starts_with("literal ")) {
            report(node, scope, "STRATA.DSL.RUNTIME_UNKNOWN_LITERAL",
                   "Portable literal kind '" + node.text.substr(8U) + "' is not supported.");
        } else {
            report(node, scope, "STRATA.DSL.RUNTIME_UNKNOWN_EXPRESSION",
                   "Portable expression kind '" + node.text + "' is not supported.");
        }
        return ExpressionValue{};
    }
    return ExpressionValue{};
}

ExpressionValue ExpressionRuntime::evaluate_variable(const Program& program,
                                                     const ProgramExpression& node,
                                                     const ExpressionScope& scope,
                                                     DependencyRecorder& reads) {
    if (node.host != no_program_id) {
        if (std::optional<ExpressionHostDependency> read = host_read(program, node, scope, reads)) {
            reads.host(*read);
            if (read->value.has_value())
                return ExpressionValue(std::move(*read->value));
            report(node, scope, "STRATA.DSL.RUNTIME_MISSING_HOST_ROOT",
                   "Host binding '" + node.name.name() + "' is not available.",
                   "host adapter root");
            return ExpressionValue{};
        }
    }
    const ScopeLookup found = scope.lookup(node.name);
    if (const ExpressionValue* bound = found.value) {
        if (bound->executable()) {
            reads.lexical(node.name, *bound);
            return *bound;
        }
        // In a handler a state reads what the handler wrote, else what the store holds now.
        if (active_run_ != nullptr) {
            if (const auto* state = scope.script_state(node.name)) {
                if (const Value* current = script_state_value(**state))
                    return ExpressionValue(*current);
            }
        }
        const std::shared_ptr<const LexicalStateBinding>* binding =
            node.state_binding ? scope.state_binding(node.name) : nullptr;
        ExpressionValue value = binding != nullptr ? ExpressionValue(*bound->value(), *binding)
                                                   : ExpressionValue(*bound->value());
        reads.lexical(node.name, value);
        return value;
    }
    if (found.group != nullptr) {
        return ExpressionValue(std::make_shared<const LambdaValue>(LambdaValue{
            found.group->program,
            no_program_id,
            found.function,
            {},
            found.group->shared_from_this(),
        }));
    }
    switch (node.fallback) {
    case VariableFallback::host:
        throw std::logic_error("host variable bypassed structural host resolution");
    case VariableFallback::name:
        return ExpressionValue(node.value);
    case VariableFallback::function:
        if (node.function != no_program_id) {
            ExpressionScope top;
            top.set_component_path("function " + std::string(node.name.name()));
            return ExpressionValue(std::make_shared<const LambdaValue>(LambdaValue{
                program.shared_from_this(), no_program_id, node.function, std::move(top), nullptr}));
        }
        break;
    case VariableFallback::missing:
        break;
    }
    report(node, scope, "STRATA.DSL.RUNTIME_MISSING_BINDING",
           "Binding '" + node.name.name() + "' is not available.",
           "local state, component parameter, or host root");
    return ExpressionValue{};
}

ExpressionValue ExpressionRuntime::evaluate_property(const Program& program,
                                                     const ProgramExpression& node,
                                                     const ExpressionScope& scope,
                                                     DependencyRecorder& reads) {
    const Names& known = names();
    const Symbol name = node.name;
    if (node.host != no_program_id) {
        if (std::optional<ExpressionHostDependency> read = host_read(program, node, scope, reads)) {
            // A size of a host list or string is computed, not a host field.
            bool computed = false;
            if (!read->value.has_value() &&
                (name == known.size || name == known.length || name == known.is_empty)) {
                const ProgramExpression& receiver = program.expression(node.first);
                if (receiver.host != no_program_id) {
                    if (const std::optional<ExpressionHostDependency> receiver_read =
                            host_read(program, receiver, scope, reads);
                        receiver_read.has_value() && receiver_read->value.has_value()) {
                        const Value& value = *receiver_read->value;
                        computed = value.list() != nullptr ||
                                   (value.string() != nullptr && name != known.size);
                    }
                }
            }
            if (!computed) {
                reads.host(*read);
                if (read->value.has_value())
                    return ExpressionValue(std::move(*read->value));
                if (probing_ == 0U)
                    report(node, scope, "STRATA.DSL.RUNTIME_MISSING_PROPERTY",
                           "Host path selected by the expression is not available.");
                return ExpressionValue{};
            }
        }
    }
    // A field of a variable's record is read on its own: whatever reads `node.kind` depends on
    // the kind, not on the rest of the node.
    if (const LexicalRead* read = program.field_read(node)) {
        const ProgramExpression* base = &node;
        while (base->kind == ExpressionKind::property)
            base = &program.expression(base->first);
        // Only a bound value has fields; anything else is read and reported as before.
        if (scope.find(base->name) != nullptr) {
            DependencyRecorder unrecorded;
            const ExpressionValue whole = evaluate_variable(program, *base, scope, unrecorded);
            if (std::optional<ExpressionValue> field = project_lexical_read(whole, read->path)) {
                reads.lexical(*read, *field);
                return std::move(*field);
            }
        }
    }
    const ExpressionValue receiver = evaluate_node(program, node.first, scope, reads);
    if (const auto* object_value = receiver.object()) {
        if (const ExpressionValue* field = (**object_value).field(node.text); field != nullptr)
            return *field;
    }
    if (const auto* list_value = receiver.list()) {
        if (name == known.size || name == known.length)
            return ExpressionValue(Value(static_cast<double>((**list_value).values.size())));
        if (name == known.is_empty)
            return ExpressionValue(Value((**list_value).values.empty()));
    }
    if (const Value* value = receiver.value()) {
        if (const Value* field = value->field(node.text))
            return ExpressionValue(*field);
        if (const ValueList* list = value->list()) {
            if (name == known.size || name == known.length)
                return ExpressionValue(Value(static_cast<double>(list->values.size())));
            if (name == known.is_empty)
                return ExpressionValue(Value(list->values.empty()));
        }
        if (const std::string* text = value->string()) {
            if (name == known.length)
                return ExpressionValue(Value(static_cast<double>(utf16_length(*text))));
            if (name == known.is_empty)
                return ExpressionValue(Value(text->empty()));
        }
    } else if (const auto* view_pointer = receiver.collection()) {
        const CollectionViewValue& view = **view_pointer;
        if (name == known.items)
            return ExpressionValue(view.items);
        if (name == known.size || name == known.length || name == known.matched)
            return ExpressionValue(Value(static_cast<double>(view.matched)));
        if (name == known.total)
            return ExpressionValue(Value(static_cast<double>(view.total)));
        if (name == known.range_start)
            return ExpressionValue(Value(static_cast<double>(view.range_start)));
        if (name == known.range_end)
            return ExpressionValue(Value(static_cast<double>(view.range_end_exclusive)));
        if (name == known.is_empty)
            return ExpressionValue(Value(view.matched == 0U));
        if (name == known.cache_hits) {
            return ExpressionValue(
                Value(static_cast<double>(view.cache_hits.load(std::memory_order_relaxed))));
        }
        if (name == known.rebuilds)
            return ExpressionValue(Value(static_cast<double>(view.rebuilds)));
    }
    if (probing_ == 0U)
        report(node, scope, "STRATA.DSL.RUNTIME_MISSING_PROPERTY",
               "Property '" + node.text + "' is not available on this value.");
    return ExpressionValue{};
}

ExpressionValue ExpressionRuntime::evaluate_index(const Program& program,
                                                  const ProgramExpression& node,
                                                  const ExpressionScope& scope,
                                                  DependencyRecorder& reads) {
    if (node.host != no_program_id) {
        if (std::optional<ExpressionHostDependency> read = host_read(program, node, scope, reads)) {
            reads.host(*read);
            if (read->value.has_value())
                return ExpressionValue(std::move(*read->value));
            report(node, scope, "STRATA.DSL.RUNTIME_MISSING_PROPERTY",
                   "Host path selected by the expression is not available.");
            return ExpressionValue{};
        }
    }
    const ExpressionValue receiver = evaluate_node(program, node.first, scope, reads);
    const Value index =
        require_value(evaluate_node(program, node.second, scope, reads), node, scope);
    if (const auto* list_value = receiver.list()) {
        const auto position = bounded_index(index);
        return position.has_value() && *position < (**list_value).values.size()
                   ? (**list_value).values[*position]
                   : ExpressionValue{};
    }
    if (const auto* object_value = receiver.object()) {
        const std::string name =
            index.string() != nullptr ? *index.string() : display_string(index);
        const ExpressionValue* field = (**object_value).field(name);
        return field != nullptr ? *field : ExpressionValue{};
    }
    const Value scalar_receiver = require_value(receiver, node, scope);
    if (const ValueList* list = scalar_receiver.list()) {
        const auto position = bounded_index(index);
        return position.has_value() && *position < list->values.size()
                   ? ExpressionValue(list->values[*position])
                   : ExpressionValue{};
    }
    if (scalar_receiver.object() != nullptr) {
        const std::string name =
            index.string() != nullptr ? *index.string() : display_string(index);
        const Value* field = scalar_receiver.field(name);
        return field != nullptr ? ExpressionValue(*field) : ExpressionValue{};
    }
    return ExpressionValue{};
}

ExpressionValue ExpressionRuntime::evaluate_binary(const Program& program,
                                                   const ProgramExpression& node,
                                                   const ExpressionScope& scope,
                                                   DependencyRecorder& reads) {
    const BinaryOperator operation = node.binary;
    const bool known = node.text.empty();
    // `value.field ?? fallback` asks whether the field is there: its absence is the answer.
    const bool probing = known && operation == BinaryOperator::coalesce;
    if (probing)
        ++probing_;
    Value left;
    try {
        left = require_value(evaluate_node(program, node.first, scope, reads), node, scope);
    } catch (...) {
        if (probing)
            --probing_;
        throw;
    }
    if (probing)
        --probing_;
    if (known) {
        if (operation == BinaryOperator::logical_and && !truthy(left))
            return ExpressionValue(Value(false));
        if (operation == BinaryOperator::logical_or && truthy(left))
            return ExpressionValue(Value(true));
        if (operation == BinaryOperator::coalesce && left.kind() != ValueKind::null_value)
            return ExpressionValue(left);
    }
    const Value right =
        require_value(evaluate_node(program, node.second, scope, reads), node, scope);
    if (known) {
        switch (operation) {
        case BinaryOperator::logical_and:
        case BinaryOperator::logical_or:
            return ExpressionValue(Value(truthy(right)));
        case BinaryOperator::coalesce:
            return ExpressionValue(right);
        case BinaryOperator::equal:
            return ExpressionValue(Value(left == right));
        case BinaryOperator::not_equal:
            return ExpressionValue(Value(!(left == right)));
        case BinaryOperator::add:
            if (left.string() != nullptr || right.string() != nullptr)
                return ExpressionValue(Value(display_string(left) + display_string(right)));
            break;
        default:
            break;
        }
    }
    const double* left_number = left.number();
    const double* right_number = right.number();
    const double left_numeric = left_number != nullptr ? *left_number
                                : left.duration() != nullptr
                                    ? static_cast<double>(left.duration()->nanoseconds)
                                    : 0.0;
    const double right_numeric = right_number != nullptr ? *right_number
                                 : right.duration() != nullptr
                                     ? static_cast<double>(right.duration()->nanoseconds)
                                     : 0.0;
    const bool numeric = (left_number != nullptr || left.duration() != nullptr) &&
                         (right_number != nullptr || right.duration() != nullptr);
    if (known && numeric) {
        switch (operation) {
        case BinaryOperator::less:
            return ExpressionValue(Value(left_numeric < right_numeric));
        case BinaryOperator::less_equal:
            return ExpressionValue(Value(left_numeric <= right_numeric));
        case BinaryOperator::greater:
            return ExpressionValue(Value(left_numeric > right_numeric));
        case BinaryOperator::greater_equal:
            return ExpressionValue(Value(left_numeric >= right_numeric));
        default:
            break;
        }
    }
    if (numeric) {
        double result = 0.0;
        if (!known) {
            report(node, scope, "STRATA.DSL.RUNTIME_UNKNOWN_OPERATOR",
                   "Binary operator '" + node.text + "' is not supported.");
            return ExpressionValue{};
        }
        switch (operation) {
        case BinaryOperator::add:
            result = left_numeric + right_numeric;
            break;
        case BinaryOperator::subtract:
            result = left_numeric - right_numeric;
            break;
        case BinaryOperator::multiply:
            result = left_numeric * right_numeric;
            break;
        case BinaryOperator::divide:
            result = right_numeric == 0.0 ? 0.0 : left_numeric / right_numeric;
            break;
        case BinaryOperator::modulo:
            result = right_numeric == 0.0 ? 0.0 : std::fmod(left_numeric, right_numeric);
            break;
        default:
            report(node, scope, "STRATA.DSL.RUNTIME_TYPE_MISMATCH",
                   "Binary operator received incompatible values.");
            return ExpressionValue(Value(0.0));
        }
        if (!std::isfinite(result))
            result = 0.0;
        return left.duration() != nullptr || right.duration() != nullptr
                   ? ExpressionValue(Value(DurationValue{static_cast<std::int64_t>(result)}))
                   : ExpressionValue(Value(result));
    }
    report(node, scope, "STRATA.DSL.RUNTIME_TYPE_MISMATCH",
           "Binary operator received incompatible values.");
    return ExpressionValue(Value(0.0));
}

ExpressionValue ExpressionRuntime::evaluate_action(const Program& program,
                                                   const ProgramExpression& node,
                                                   const ExpressionScope& scope,
                                                   DependencyRecorder& reads) {
    const std::string& id = node.text;
    const auto contract = actions_.contract(id);
    if (contract == nullptr) {
        report(node, scope, "STRATA.DSL.RUNTIME_UNKNOWN_ACTION",
               "Action '" + id + "' is not registered.");
        return ExpressionValue{};
    }
    Value payload;
    if (contract->payload_contract != "no payload") {
        std::vector<std::pair<std::string, Value>> fields;
        fields.reserve(node.arguments_count);
        for (const ProgramArgument& argument : program.arguments(node)) {
            fields.emplace_back(std::string(argument.name.name()),
                                require_value(evaluate_node(program, argument.value, scope, reads),
                                              program.expression(argument.value), scope));
        }
        payload = Value(std::move(fields));
    }
    const std::optional<ActionOrigin> origin = action_origin(program, node, scope);
    try {
        payload = actions_.decode_payload(id, std::move(payload));
        auto action = std::make_shared<const Action>(contract, std::move(payload), origin);
        std::optional<LexicalStateBinding> state_binding;
        if (id.starts_with("state.")) {
            const Value* state_name_value = action->payload.field("name");
            const std::string* state_name =
                state_name_value != nullptr ? state_name_value->string() : nullptr;
            if (state_name != nullptr) {
                if (const std::optional<Symbol> name = Symbol::find(*state_name)) {
                    if (const auto* binding = scope.state_binding(*name))
                        state_binding = **binding;
                }
            }
        }
        return ExpressionValue(std::make_shared<const ActionValue>(ActionValue{
            std::move(action),
            std::nullopt,
            {},
            std::move(state_binding),
        }));
    } catch (const std::exception& error) {
        std::optional<DiagnosticRange> range;
        if (origin.has_value() && origin->line.has_value()) {
            const DiagnosticPosition start{
                *origin->line,
                origin->column.value_or(1U),
                std::nullopt,
            };
            const DiagnosticPosition end =
                origin->end_line.has_value()
                    ? DiagnosticPosition{
                          *origin->end_line,
                          origin->end_column.value_or(1U),
                          std::nullopt,
                      }
                    : start;
            range = DiagnosticRange{origin->source_id, start, end};
        }
        report(RuntimeDiagnostic{
            "STRATA.DSL.RUNTIME_ACTION_PAYLOAD_DECODE",
            "Action '" + id + "' payload could not be decoded: " + error.what(),
            origin.has_value() && origin->component_path.has_value() ? *origin->component_path
                                                                     : std::string{},
            contract->payload_contract,
            DiagnosticSeverity::error,
            std::move(range),
        });
        // Preserve that an action expression was authored. A null action variant prevents widget
        // defaults from silently replacing a rejected host payload with a different action.
        return ExpressionValue(std::shared_ptr<const ActionValue>{});
    }
}

std::optional<ActionOrigin> ExpressionRuntime::action_origin(const Program& program,
                                                             const ProgramExpression& node,
                                                             const ExpressionScope& scope) const {
    if (node.origin == no_program_id)
        return std::nullopt;
    const ProgramActionOrigin& authored = program.action_origin(node.origin);
    ActionOrigin origin = authored.origin;
    const std::string& path = scope.component_path();
    if (path.starts_with("/component/")) {
        origin.component_path = path;
    } else if (authored.fallback_component_path.has_value()) {
        origin.component_path = authored.fallback_component_path;
    } else if (!path.empty()) {
        origin.component_path = path;
    }
    constexpr std::string_view instance_root_suffix = "/root";
    if (origin.component_path.has_value() && origin.component_path->starts_with("/component/") &&
        origin.component_path->ends_with(instance_root_suffix)) {
        origin.component_path->erase(origin.component_path->size() - instance_root_suffix.size());
    }
    return origin;
}

const ProgramArgument* ExpressionRuntime::argument(const Program& program,
                                                   const ProgramExpression& node, const Symbol name,
                                                   const std::size_t position) const {
    const std::span<const ProgramArgument> arguments = program.arguments(node);
    for (const ProgramArgument& candidate : arguments) {
        if (candidate.named && candidate.name == name)
            return &candidate;
    }
    return position < arguments.size() && !arguments[position].named ? &arguments[position]
                                                                     : nullptr;
}

Value ExpressionRuntime::argument_value(const Program& program, const ProgramExpression& node,
                                        const ExpressionScope& scope, const Symbol name,
                                        const std::size_t position, DependencyRecorder& reads) {
    const ProgramArgument* found = argument(program, node, name, position);
    return found != nullptr ? require_value(evaluate_node(program, found->value, scope, reads),
                                            program.expression(found->value), scope)
                            : Value{};
}

ExpressionValue ExpressionRuntime::evaluate_helper(const Program& program,
                                                   const ExpressionId expression,
                                                   const ExpressionScope& scope,
                                                   DependencyRecorder& reads) {
    const ProgramExpression& node = program.expression(expression);
    const Names& known = names();
    const auto value_of = [&](const ProgramArgument& entry) {
        return require_value(evaluate_node(program, entry.value, scope, reads),
                             program.expression(entry.value), scope);
    };
    switch (node.helper) {
    case HelperKind::library:
        return evaluate_library(program, node, scope, reads);
    case HelperKind::filter:
    case HelperKind::map:
    case HelperKind::sort_by:
    case HelperKind::distinct_by:
    case HelperKind::group_by:
    case HelperKind::flatten:
    case HelperKind::take_while:
    case HelperKind::window:
    case HelperKind::page:
        return ExpressionValue(collection_view(program, expression, scope, reads));
    case HelperKind::persisted: {
        const ProgramArgument* initial = argument(program, node, known.initial, 1U);
        return initial != nullptr ? evaluate_node(program, initial->value, scope, reads)
                                  : ExpressionValue{};
    }
    case HelperKind::sequence:
        return ExpressionValue(
            composed_action(program, node, scope, ActionCompositionMode::sequence, reads));
    case HelperKind::parallel:
        return ExpressionValue(
            composed_action(program, node, scope, ActionCompositionMode::parallel, reads));
    case HelperKind::choose: {
        const bool condition =
            truthy(argument_value(program, node, scope, known.condition, 0U, reads));
        const ProgramArgument* selected = argument(
            program, node, condition ? known.when_true : known.when_false, condition ? 1U : 2U);
        return selected != nullptr ? evaluate_node(program, selected->value, scope, reads)
                                   : ExpressionValue{};
    }
    case HelperKind::count:
    case HelperKind::any:
    case HelperKind::all: {
        const HelperKind helper = node.helper;
        const ProgramArgument* source_argument = argument(program, node, known.source, 0U);
        if (source_argument == nullptr)
            return ExpressionValue(helper == HelperKind::count ? Value(0.0) : Value(false));
        const ExpressionValue source = evaluate_node(program, source_argument->value, scope, reads);
        const ValueList* items = collection_items(source);
        if (items == nullptr) {
            report(node, scope, "STRATA.DSL.RUNTIME_COLLECTION_SOURCE",
                   "Collection aggregate requires a list or derived view.");
            return ExpressionValue(helper == HelperKind::count ? Value(0.0) : Value(false));
        }
        const ProgramArgument* predicate_argument = argument(program, node, known.predicate, 1U);
        const ExpressionValue predicate_value =
            predicate_argument != nullptr
                ? evaluate_node(program, predicate_argument->value, scope, reads)
                : ExpressionValue{};
        ItemCall predicate(*this, predicate_value.lambda(), reads);
        std::size_t matches = 0U;
        for (const Value& item : items->values) {
            if (!predicate.present() || truthy(predicate(item)))
                ++matches;
            if (helper == HelperKind::any && matches != 0U)
                break;
        }
        if (helper == HelperKind::count)
            return ExpressionValue(Value(static_cast<double>(matches)));
        if (helper == HelperKind::any)
            return ExpressionValue(Value(matches != 0U));
        return ExpressionValue(Value(!items->values.empty() && matches == items->values.size()));
    }
    case HelperKind::min:
    case HelperKind::max: {
        double result = 0.0;
        bool first = true;
        for (const ProgramArgument& entry : program.arguments(node)) {
            const Value value = value_of(entry);
            const double number = value.number() != nullptr ? *value.number() : 0.0;
            result = first                            ? number
                     : node.helper == HelperKind::min ? std::min(result, number)
                                                      : std::max(result, number);
            first = false;
        }
        return ExpressionValue(Value(result));
    }
    case HelperKind::animate: {
        const Value target = argument_value(program, node, scope, known.target, 0U, reads);
        const double heading = target.number() != nullptr ? *target.number() : 0.0;
        if (animation_host_ == nullptr)
            return ExpressionValue(Value(heading));
        const Value duration = argument_value(program, node, scope, known.duration, 1U, reads);
        const Value easing = argument_value(program, node, scope, known.easing, 2U, reads);
        const Value delay = argument_value(program, node, scope, known.delay, 3U, reads);
        const Value from = argument_value(program, node, scope, known.from, 4U, reads);
        AnimateCall call;
        call.instance_path = scope.component_path();
        call.program = program.serial();
        call.expression = expression;
        call.target = heading;
        if (from.number() != nullptr)
            call.from = *from.number();
        if (duration.duration() != nullptr)
            call.duration_nanos = std::max<std::int64_t>(0, duration.duration()->nanoseconds);
        if (delay.duration() != nullptr)
            call.delay_nanos = std::max<std::int64_t>(0, delay.duration()->nanoseconds);
        if (easing.string() != nullptr)
            call.easing = *easing.string();
        const AnimateSample sample = animation_host_->animate(call);
        // While it moves it reads the frame's time, so whatever holds it is described again.
        if (sample.running) {
            reads.host(ExpressionHostDependency{frame_time_path(), false,
                                                frame_time_value(sample.time_nanos), std::nullopt,
                                                0U});
        }
        return ExpressionValue(Value(sample.value));
    }
    case HelperKind::clamp: {
        const Value value_argument = argument_value(program, node, scope, known.value, 0U, reads);
        const Value minimum_argument = argument_value(program, node, scope, known.min, 1U, reads);
        const Value maximum_argument = argument_value(program, node, scope, known.max, 2U, reads);
        const double value = value_argument.number() != nullptr ? *value_argument.number() : 0.0;
        const double minimum =
            minimum_argument.number() != nullptr ? *minimum_argument.number() : value;
        const double maximum =
            maximum_argument.number() != nullptr ? *maximum_argument.number() : value;
        if (minimum > maximum) {
            report(node, scope, "STRATA.DSL.RUNTIME_INVALID_RANGE",
                   "clamp minimum exceeds its maximum.");
            return ExpressionValue(Value(value));
        }
        return ExpressionValue(Value(std::clamp(value, minimum, maximum)));
    }
    case HelperKind::abs:
    case HelperKind::floor:
    case HelperKind::ceil:
    case HelperKind::round: {
        const Value value = argument_value(program, node, scope, known.value, 0U, reads);
        const double number = value.number() != nullptr ? *value.number() : 0.0;
        if (node.helper == HelperKind::round) {
            const Value precision_value =
                argument_value(program, node, scope, known.precision, 1U, reads);
            const int precision =
                precision_value.number() != nullptr
                    ? std::clamp(static_cast<int>(*precision_value.number()), 0, 12)
                    : 0;
            const double scale = std::pow(10.0, static_cast<double>(precision));
            return ExpressionValue(Value(std::round(number * scale) / scale));
        }
        return ExpressionValue(Value(node.helper == HelperKind::abs     ? std::abs(number)
                                     : node.helper == HelperKind::floor ? std::floor(number)
                                                                        : std::ceil(number)));
    }
    case HelperKind::length:
    case HelperKind::size:
    case HelperKind::is_empty: {
        const Value value = argument_value(program, node, scope, known.value, 0U, reads);
        const std::size_t size = value.string() != nullptr   ? utf16_length(*value.string())
                                 : value.list() != nullptr   ? value.list()->values.size()
                                 : value.object() != nullptr ? value.object()->fields.size()
                                                             : 0U;
        return ExpressionValue(node.helper == HelperKind::is_empty
                                   ? Value(size == 0U)
                                   : Value(static_cast<double>(size)));
    }
    case HelperKind::join: {
        const Value values = argument_value(program, node, scope, known.value, 0U, reads);
        const Value separator_value =
            argument_value(program, node, scope, known.separator, 1U, reads);
        const std::string separator = separator_value.kind() == ValueKind::null_value
                                          ? ", "
                                          : display_string(separator_value);
        std::string joined;
        if (const ValueList* list = values.list()) {
            for (std::size_t index = 0U; index < list->values.size(); ++index) {
                if (index != 0U)
                    joined += separator;
                joined += display_string(list->values[index]);
            }
        }
        return ExpressionValue(Value(std::move(joined)));
    }
    case HelperKind::lower:
    case HelperKind::upper:
    case HelperKind::trim:
    case HelperKind::title: {
        std::string value =
            display_string(argument_value(program, node, scope, known.value, 0U, reads));
        if (node.helper == HelperKind::lower) {
            value = lower_ascii(std::move(value));
        } else if (node.helper == HelperKind::upper) {
            value = upper_ascii(std::move(value));
        } else if (node.helper == HelperKind::trim) {
            const std::size_t first = value.find_first_not_of(" \t\r\n");
            const std::size_t last = value.find_last_not_of(" \t\r\n");
            value =
                first == std::string::npos ? std::string{} : value.substr(first, last - first + 1U);
        } else {
            std::istringstream words(value);
            std::string titled;
            for (std::string word; words >> word;) {
                word = lower_ascii(std::move(word));
                if (!word.empty() && word.front() >= 'a' && word.front() <= 'z')
                    word.front() = static_cast<char>(word.front() - 'a' + 'A');
                if (!titled.empty())
                    titled.push_back(' ');
                titled += word;
            }
            value = std::move(titled);
        }
        return ExpressionValue(Value(std::move(value)));
    }
    case HelperKind::contains: {
        const Value value = argument_value(program, node, scope, known.value, 0U, reads);
        const Value needle = argument_value(program, node, scope, known.needle, 1U, reads);
        if (value.string() != nullptr)
            return ExpressionValue(Value(value.string()->contains(display_string(needle))));
        if (value.list() != nullptr) {
            return ExpressionValue(Value(std::ranges::find(value.list()->values, needle) !=
                                         value.list()->values.end()));
        }
        return ExpressionValue(Value(false));
    }
    case HelperKind::starts_with:
    case HelperKind::ends_with: {
        const bool starts = node.helper == HelperKind::starts_with;
        const std::string value =
            display_string(argument_value(program, node, scope, known.value, 0U, reads));
        const std::string part = display_string(
            argument_value(program, node, scope, starts ? known.prefix : known.suffix, 1U, reads));
        return ExpressionValue(Value(starts ? value.starts_with(part) : value.ends_with(part)));
    }
    case HelperKind::format: {
        const std::span<const ProgramArgument> arguments = program.arguments(node);
        if (arguments.empty())
            return ExpressionValue(Value(""));
        std::string formatted = display_string(value_of(arguments[0]));
        for (std::size_t index = 1U; index < arguments.size(); ++index) {
            const std::string marker = "{" + std::to_string(index - 1U) + "}";
            const std::string replacement = display_string(value_of(arguments[index]));
            std::size_t position = 0U;
            while ((position = formatted.find(marker, position)) != std::string::npos) {
                formatted.replace(position, marker.size(), replacement);
                position += replacement.size();
            }
            if (arguments[index].named) {
                const std::string named_marker =
                    "{" + std::string(arguments[index].name.name()) + "}";
                position = 0U;
                while ((position = formatted.find(named_marker, position)) != std::string::npos) {
                    formatted.replace(position, named_marker.size(), replacement);
                    position += replacement.size();
                }
            }
        }
        return ExpressionValue(Value(std::move(formatted)));
    }
    case HelperKind::format_number: {
        const Value value = argument_value(program, node, scope, known.value, 0U, reads);
        const auto optional_number = [&](const Symbol name, const std::size_t position) {
            const Value found = argument_value(program, node, scope, name, position, reads);
            return found.number() != nullptr && std::isfinite(*found.number())
                       ? std::optional<double>(*found.number())
                       : std::nullopt;
        };
        const auto optional_boolean = [&](const Symbol name, const std::size_t position) {
            const Value found = argument_value(program, node, scope, name, position, reads);
            return found.boolean() != nullptr ? std::optional<bool>(*found.boolean())
                                              : std::nullopt;
        };
        const auto text = [&](const Symbol name, const std::size_t position, std::string fallback) {
            const Value found = argument_value(program, node, scope, name, position, reads);
            return found.string() != nullptr ? *found.string() : std::move(fallback);
        };
        NumberFormat format;
        if (const std::optional<double> decimals = optional_number(known.decimals, 1U))
            format.decimals = static_cast<int>(std::clamp(*decimals, 0.0, 12.0));
        format.grouping = optional_boolean(known.grouping, 2U);
        format.compact = optional_boolean(known.compact, 3U).value_or(false);
        format.currency = text(known.currency, 4U, std::string());
        format.percent = optional_boolean(known.percent, 5U).value_or(false);
        format.sign_display = text(known.sign_display, 6U, "auto");
        format.group_separator = text(known.group_separator, 7U, ",");
        format.decimal_separator = text(known.decimal_separator, 8U, ".");
        return ExpressionValue(
            Value(format_number(value.number() != nullptr ? *value.number() : 0.0, format)));
    }
    case HelperKind::format_bytes: {
        const Value value = argument_value(program, node, scope, known.value, 0U, reads);
        const Value decimals = argument_value(program, node, scope, known.decimals, 1U, reads);
        const Value binary = argument_value(program, node, scope, known.binary, 2U, reads);
        const Value separator =
            argument_value(program, node, scope, known.decimal_separator, 3U, reads);
        return ExpressionValue(Value(format_bytes(
            value.number() != nullptr ? *value.number() : 0.0,
            decimals.number() != nullptr && std::isfinite(*decimals.number())
                ? std::optional<int>(static_cast<int>(std::clamp(*decimals.number(), 0.0, 12.0)))
                : std::nullopt,
            binary.boolean() != nullptr && *binary.boolean(),
            separator.string() != nullptr ? std::string_view(*separator.string())
                                          : std::string_view("."))));
    }
    case HelperKind::time_ago: {
        const Value value = argument_value(program, node, scope, known.value, 0U, reads);
        const Value now = argument_value(program, node, scope, known.now, 1U, reads);
        const Value style = argument_value(program, node, scope, known.format_style, 2U, reads);
        return ExpressionValue(
            Value(time_ago(value.number() != nullptr ? *value.number() : 0.0,
                           now.number() != nullptr ? *now.number() : 0.0,
                           style.string() != nullptr && *style.string() == "short")));
    }
    case HelperKind::format_duration: {
        const Value value = argument_value(program, node, scope, known.value, 0U, reads);
        const Value style = argument_value(program, node, scope, known.format_style, 1U, reads);
        return ExpressionValue(
            Value(format_duration(value.number() != nullptr ? *value.number() : 0.0,
                                  style.string() != nullptr ? std::string_view(*style.string())
                                                            : std::string_view("clock"))));
    }
    case HelperKind::rgb:
    case HelperKind::rgba: {
        const std::uint8_t red =
            color_channel(argument_value(program, node, scope, known.red, 0U, reads));
        const std::uint8_t green =
            color_channel(argument_value(program, node, scope, known.green, 1U, reads));
        const std::uint8_t blue =
            color_channel(argument_value(program, node, scope, known.blue, 2U, reads));
        const std::uint8_t alpha =
            node.helper == HelperKind::rgba
                ? color_channel(argument_value(program, node, scope, known.alpha, 3U, reads))
                : UINT8_MAX;
        return ExpressionValue(Value(ColorValue{red, green, blue, alpha}));
    }
    case HelperKind::animation:
        return ExpressionValue(
            Value(display_string(argument_value(program, node, scope, known.name, 0U, reads))));
    case HelperKind::style: {
        std::vector<Value> bases;
        std::vector<std::pair<std::string, Value>> properties;
        for (const ProgramArgument& entry : program.arguments(node)) {
            Value value = value_of(entry);
            if (!entry.named) {
                bases.push_back(std::move(value));
            } else {
                properties.emplace_back(std::string(entry.name.name()), std::move(value));
            }
        }
        properties.emplace_back("$bases", Value(std::move(bases)));
        return ExpressionValue(Value(std::move(properties)));
    }
    case HelperKind::split: {
        const std::string value =
            display_string(argument_value(program, node, scope, known.value, 0U, reads));
        const std::string separator =
            display_string(argument_value(program, node, scope, known.separator, 1U, reads));
        std::vector<Value> parts;
        if (separator.empty()) {
            // An empty separator splits into characters, as most string libraries do.
            for (std::size_t index = 0U; index < value.size();) {
                const std::size_t width =
                    std::max<std::size_t>(utf8_offset(std::string_view(value).substr(index), 1U), 1U);
                parts.emplace_back(value.substr(index, width));
                index += width;
            }
            return ExpressionValue(Value(std::move(parts)));
        }
        std::size_t begin = 0U;
        for (std::size_t found = value.find(separator); found != std::string::npos;
             found = value.find(separator, begin)) {
            parts.emplace_back(value.substr(begin, found - begin));
            begin = found + separator.size();
        }
        parts.emplace_back(value.substr(begin));
        return ExpressionValue(Value(std::move(parts)));
    }
    case HelperKind::replace: {
        std::string value =
            display_string(argument_value(program, node, scope, known.value, 0U, reads));
        const std::string search =
            display_string(argument_value(program, node, scope, known.search, 1U, reads));
        const std::string replacement =
            display_string(argument_value(program, node, scope, known.replacement, 2U, reads));
        if (search.empty())
            return ExpressionValue(Value(std::move(value)));
        std::string result;
        std::size_t begin = 0U;
        for (std::size_t found = value.find(search); found != std::string::npos;
             found = value.find(search, begin)) {
            result.append(value, begin, found - begin);
            result += replacement;
            begin = found + search.size();
        }
        result.append(value, begin);
        return ExpressionValue(Value(std::move(result)));
    }
    case HelperKind::slice: {
        const Value value = argument_value(program, node, scope, known.value, 0U, reads);
        const Value start_value = argument_value(program, node, scope, known.start, 1U, reads);
        const Value end_value = argument_value(program, node, scope, known.end, 2U, reads);
        const double start = start_value.number() != nullptr ? *start_value.number() : 0.0;
        if (const ValueList* list = value.list(); list != nullptr) {
            const std::size_t size = list->values.size();
            const std::size_t from = slice_bound(start, size);
            const std::size_t to =
                end_value.number() != nullptr ? slice_bound(*end_value.number(), size) : size;
            return ExpressionValue(Value(std::vector<Value>(
                list->values.begin() + static_cast<std::ptrdiff_t>(from),
                list->values.begin() + static_cast<std::ptrdiff_t>(std::max(from, to)))));
        }
        const std::string text = display_string(value);
        const std::size_t size = utf16_length(text);
        const std::size_t from = slice_bound(start, size);
        const std::size_t to =
            end_value.number() != nullptr ? slice_bound(*end_value.number(), size) : size;
        const std::size_t byte_from = utf8_offset(text, from);
        const std::size_t byte_to = utf8_offset(text, std::max(from, to));
        return ExpressionValue(Value(text.substr(byte_from, byte_to - byte_from)));
    }
    case HelperKind::index_of: {
        const Value value = argument_value(program, node, scope, known.value, 0U, reads);
        const Value needle = argument_value(program, node, scope, known.needle, 1U, reads);
        if (const ValueList* list = value.list(); list != nullptr) {
            const auto found = std::ranges::find(list->values, needle);
            return ExpressionValue(Value(found == list->values.end()
                                             ? -1.0
                                             : static_cast<double>(found - list->values.begin())));
        }
        const std::string text = display_string(value);
        const std::size_t found = text.find(display_string(needle));
        return ExpressionValue(Value(
            found == std::string::npos
                ? -1.0
                : static_cast<double>(utf16_length(std::string_view(text).substr(0U, found)))));
    }
    case HelperKind::pad_start:
    case HelperKind::pad_end: {
        std::string value =
            display_string(argument_value(program, node, scope, known.value, 0U, reads));
        const Value length_value = argument_value(program, node, scope, known.length, 1U, reads);
        const Value fill_value = argument_value(program, node, scope, known.fill, 2U, reads);
        const std::string fill =
            fill_value.kind() == ValueKind::null_value ? " " : display_string(fill_value);
        const double target = length_value.number() != nullptr ? *length_value.number() : 0.0;
        // A bounded pad: a runaway target would otherwise allocate without limit.
        const std::size_t width = static_cast<std::size_t>(
            std::clamp(std::isfinite(target) ? target : 0.0, 0.0, 4096.0));
        std::size_t current = utf16_length(value);
        const std::size_t fill_units = utf16_length(fill);
        if (fill_units == 0U || current >= width)
            return ExpressionValue(Value(std::move(value)));
        std::string padding;
        while (current + fill_units <= width) {
            padding += fill;
            current += fill_units;
        }
        if (current < width)
            padding += fill.substr(0U, utf8_offset(fill, width - current));
        return ExpressionValue(
            Value(node.helper == HelperKind::pad_start ? padding + value : value + padding));
    }
    case HelperKind::first:
    case HelperKind::last:
    case HelperKind::reverse:
    case HelperKind::sum:
    case HelperKind::find: {
        const ProgramArgument* source_argument = argument(program, node, known.source, 0U);
        const ExpressionValue source =
            source_argument != nullptr
                ? evaluate_node(program, source_argument->value, scope, reads)
                : ExpressionValue{};
        const ValueList* items = collection_items(source);
        if (items == nullptr) {
            report(node, scope, "STRATA.DSL.RUNTIME_COLLECTION_SOURCE",
                   "This helper requires a list or derived view.");
            if (node.helper == HelperKind::sum)
                return ExpressionValue(Value(0.0));
            if (node.helper == HelperKind::reverse)
                return ExpressionValue(Value(std::vector<Value>{}));
            return ExpressionValue(Value{});
        }
        if (node.helper == HelperKind::first || node.helper == HelperKind::last) {
            if (items->values.empty())
                return ExpressionValue(Value{});
            return ExpressionValue(node.helper == HelperKind::first ? items->values.front()
                                                                    : items->values.back());
        }
        if (node.helper == HelperKind::reverse) {
            return ExpressionValue(
                Value(std::vector<Value>(items->values.rbegin(), items->values.rend())));
        }
        const Symbol function_name =
            node.helper == HelperKind::sum ? known.selector : known.predicate;
        const ProgramArgument* function_argument = argument(program, node, function_name, 1U);
        const ExpressionValue function_value =
            function_argument != nullptr
                ? evaluate_node(program, function_argument->value, scope, reads)
                : ExpressionValue{};
        ItemCall function(*this, function_value.lambda(), reads);
        if (node.helper == HelperKind::find) {
            for (const Value& item : items->values) {
                if (function.present() && truthy(function(item)))
                    return ExpressionValue(item);
            }
            return ExpressionValue(Value{});
        }
        double total = 0.0;
        for (const Value& item : items->values) {
            const Value selected = function.present() ? function(item) : item;
            if (selected.number() != nullptr && std::isfinite(*selected.number()))
                total += *selected.number();
        }
        return ExpressionValue(Value(total));
    }
    case HelperKind::range: {
        const Value start_value = argument_value(program, node, scope, known.start, 0U, reads);
        const Value end_value = argument_value(program, node, scope, known.end, 1U, reads);
        const Value step_value = argument_value(program, node, scope, known.step, 2U, reads);
        const double start = start_value.number() != nullptr ? *start_value.number() : 0.0;
        const double end = end_value.number() != nullptr ? *end_value.number() : 0.0;
        const double step = step_value.number() != nullptr ? *step_value.number()
                            : end >= start                 ? 1.0
                                                           : -1.0;
        std::vector<Value> values;
        if (step == 0.0) {
            report(node, scope, "STRATA.DSL.RUNTIME_INVALID_RANGE", "range step is zero.");
            return ExpressionValue(Value(std::move(values)));
        }
        if (!std::isfinite(start) || !std::isfinite(end) || !std::isfinite(step) ||
            (step > 0.0) != (end >= start)) {
            return ExpressionValue(Value(std::move(values)));
        }
        // Ranges are bounded like every eager loop, so a typo cannot allocate without limit.
        constexpr std::size_t maximum = 10'000U;
        for (std::size_t index = 0U;; ++index) {
            const double current = start + step * static_cast<double>(index);
            if (step > 0.0 ? current >= end : current <= end)
                break;
            if (values.size() == maximum) {
                report(node, scope, "STRATA.DSL.RUNTIME_INVALID_RANGE",
                       "range produced more than 10000 values.");
                break;
            }
            values.emplace_back(current);
        }
        return ExpressionValue(Value(std::move(values)));
    }
    case HelperKind::sqrt: {
        const Value value = argument_value(program, node, scope, known.value, 0U, reads);
        const double number = value.number() != nullptr ? *value.number() : 0.0;
        return ExpressionValue(Value(number >= 0.0 ? std::sqrt(number) : 0.0));
    }
    case HelperKind::pow: {
        const Value base = argument_value(program, node, scope, known.base, 0U, reads);
        const Value exponent = argument_value(program, node, scope, known.exponent, 1U, reads);
        const double result = std::pow(base.number() != nullptr ? *base.number() : 0.0,
                                       exponent.number() != nullptr ? *exponent.number() : 1.0);
        return ExpressionValue(Value(std::isfinite(result) ? result : 0.0));
    }
    case HelperKind::format_date: {
        const Value value = argument_value(program, node, scope, known.value, 0U, reads);
        const Value pattern = argument_value(program, node, scope, known.pattern, 1U, reads);
        const Value offset = argument_value(program, node, scope, known.utc_offset_minutes, 2U, reads);
        const auto names = [&](const Symbol name, const std::size_t position) {
            const Value list = argument_value(program, node, scope, name, position, reads);
            std::vector<std::string> result;
            if (const ValueList* values = list.list(); values != nullptr) {
                for (const Value& entry : values->values)
                    result.push_back(display_string(entry));
            }
            return result;
        };
        return ExpressionValue(Value(format_date(
            value.number() != nullptr ? *value.number() : 0.0, display_string(pattern),
            offset.number() != nullptr ? *offset.number() : 0.0, names(known.month_names, 3U),
            names(known.weekday_names, 4U))));
    }
    case HelperKind::delay: {
        const Value after = argument_value(program, node, scope, known.after, 0U, reads);
        const ProgramArgument* step_argument = argument(program, node, known.action, 1U);
        const ExpressionValue step =
            step_argument != nullptr ? evaluate_node(program, step_argument->value, scope, reads)
                                     : ExpressionValue{};
        const auto contract = actions_.contract("action.delay");
        if (step.action() == nullptr || after.duration() == nullptr || contract == nullptr) {
            report(node, scope, "STRATA.DSL.RUNTIME_ACTION_COMPOSITION",
                   "delay requires a duration and a typed action.");
            return ExpressionValue(std::make_shared<const ActionValue>(ActionValue{}));
        }
        auto action = std::make_shared<const Action>(
            contract, Value(std::vector<std::pair<std::string, Value>>{}),
            action_origin(program, node, scope));
        ActionValue delayed{std::move(action), ActionCompositionMode::delay, {*step.action()}};
        delayed.delay_nanoseconds = std::max<std::int64_t>(0, after.duration()->nanoseconds);
        return ExpressionValue(std::make_shared<const ActionValue>(std::move(delayed)));
    }
    case HelperKind::then: {
        const ProgramArgument* first_argument = argument(program, node, known.action, 0U);
        const ProgramArgument* next_argument = argument(program, node, known.next, 1U);
        const ExpressionValue first =
            first_argument != nullptr ? evaluate_node(program, first_argument->value, scope, reads)
                                      : ExpressionValue{};
        const ExpressionValue next =
            next_argument != nullptr ? evaluate_node(program, next_argument->value, scope, reads)
                                     : ExpressionValue{};
        const auto contract = actions_.contract("action.sequence");
        if (first.action() == nullptr || next.action() == nullptr || contract == nullptr) {
            report(node, scope, "STRATA.DSL.RUNTIME_ACTION_COMPOSITION",
                   "then requires two typed actions.");
            return ExpressionValue(std::make_shared<const ActionValue>(ActionValue{}));
        }
        auto action = std::make_shared<const Action>(
            contract, Value(std::vector<std::pair<std::string, Value>>{}),
            action_origin(program, node, scope));
        return ExpressionValue(std::make_shared<const ActionValue>(ActionValue{
            std::move(action), ActionCompositionMode::then, {*first.action(), *next.action()}}));
    }
    case HelperKind::when_style: {
        const Value condition = argument_value(program, node, scope, known.condition, 0U, reads);
        if (condition.boolean() == nullptr || !*condition.boolean())
            return ExpressionValue(Value{});
        return ExpressionValue(argument_value(program, node, scope, known.active, 1U, reads));
    }
    case HelperKind::effect: {
        std::vector<std::pair<std::string, Value>> arguments;
        std::optional<Value> backdrop_source;
        std::optional<Value> refresh_rate;
        for (const ProgramArgument& entry : program.arguments(node)) {
            if (!entry.named || entry.name == known.name)
                continue;
            if (entry.name == known.backdrop_source) {
                backdrop_source = value_of(entry);
                continue;
            }
            if (entry.name == known.refresh_rate) {
                refresh_rate = value_of(entry);
                continue;
            }
            arguments.emplace_back(std::string(entry.name.name()), value_of(entry));
        }
        std::vector<std::pair<std::string, Value>> fields{
            {"arguments", Value(std::move(arguments))},
            {"name", argument_value(program, node, scope, known.name, 0U, reads)},
        };
        if (backdrop_source.has_value())
            fields.emplace_back("backdropSource", std::move(*backdrop_source));
        if (refresh_rate.has_value())
            fields.emplace_back("refreshRate", std::move(*refresh_rate));
        return ExpressionValue(Value(std::move(fields)));
    }
    case HelperKind::unknown:
        break;
    }
    report(node, scope, "STRATA.DSL.RUNTIME_UNKNOWN_HELPER",
           "Helper '" + node.text + "' is not available at runtime.", "registered helper");
    return ExpressionValue{};
}

Value ExpressionRuntime::require_value(const ExpressionValue& evaluated,
                                       const ProgramExpression& node,
                                       const ExpressionScope& scope) {
    if (const Value* value = evaluated.value())
        return *value;
    if (const auto* collection = evaluated.collection())
        return (*collection)->items;
    report(node, scope, "STRATA.DSL.RUNTIME_TYPE_MISMATCH",
           "Expression did not produce a scalar runtime value.");
    return Value{};
}

ExpressionRuntime::ItemCall::ItemCall(ExpressionRuntime& runtime,
                                      const std::shared_ptr<const LambdaValue>* const lambda,
                                      DependencyRecorder& upstream)
    : runtime_(runtime), lambda_(lambda != nullptr ? lambda->get() : nullptr), upstream_(upstream),
      reads_(&upstream, DependencyRecorder::Keep::none, DependencyRecorder::Keep::none) {
    if (lambda_ == nullptr)
        return;
    const ProgramFunction& function = lambda_->declaration();
    const std::span<const ProgramFunctionParameter> parameters =
        lambda_->program->parameters(function);
    fast_ = function.body != no_program_id && lambda_->group == nullptr && !parameters.empty() &&
            std::ranges::all_of(parameters, [](const ProgramFunctionParameter& parameter) {
                return parameter.default_value == no_program_id;
            });
    if (!fast_)
        return;
    scope_ = lambda_->captured;
    frame_ = std::make_shared<ScopeFrame>();
    frame_->parent = scope_.frame();
    frame_->hides_parent_states = !function.script;
    frame_->bindings.reserve(parameters.size());
    for (const ProgramFunctionParameter& parameter : parameters) {
        frame_->bindings.push_back(ScopeBinding{
            parameter.name, true,
            function.script ? ScopeStateBinding::cleared : ScopeStateBinding::inherit,
            ExpressionValue{}, nullptr});
        reads_.hide(parameter.name);
    }
    scope_.set_frame(frame_);
    if (!function.script) {
        scope_.set_component_path(lambda_->captured.component_path() + "/" +
                                  std::string(parameters.front().name.name()));
    }
}

Value ExpressionRuntime::ItemCall::operator()(const Value& item, const Value* const second) {
    if (lambda_ == nullptr)
        return Value{};
    if (!fast_)
        return runtime_.evaluate_lambda(*lambda_, item, upstream_);
    // A closure made by the last call holds the frame: the next item gets a frame of its own.
    if (frame_.use_count() > 2) {
        frame_ = std::make_shared<ScopeFrame>(*frame_);
        scope_.set_frame(frame_);
    }
    frame_->bindings.front().value = ExpressionValue(item);
    if (second != nullptr && frame_->bindings.size() > 1U)
        frame_->bindings[1].value = ExpressionValue(*second);
    const Program& program = *lambda_->program;
    const ExpressionId body = lambda_->declaration().body;
    return runtime_.require_value(runtime_.evaluate_node(program, body, scope_, reads_),
                                  program.expression(body), scope_);
}

Value ExpressionRuntime::evaluate_lambda(const LambdaValue& lambda, const Value& input,
                                         DependencyRecorder& upstream) {
    // The body's input is the helper's data, not a dependency of whoever evaluates it.
    DependencyRecorder reads(&upstream, DependencyRecorder::Keep::none,
                             DependencyRecorder::Keep::none);
    const ProgramFunction& function = lambda.declaration();
    const Program& program = *lambda.program;
    // An item function with an expression body, the common case, binds its item and evaluates:
    // no call machinery per item.
    const std::span<const ProgramFunctionParameter> parameters = program.parameters(function);
    if (function.body != no_program_id && lambda.group == nullptr && !parameters.empty() &&
        std::ranges::all_of(parameters, [](const ProgramFunctionParameter& parameter) {
            return parameter.default_value == no_program_id;
        })) {
        const Symbol parameter = parameters.front().name;
        ExpressionScope nested = lambda.captured;
        ScopeFrame frame;
        frame.hides_parent_states = !function.script;
        frame.bindings.reserve(parameters.size());
        for (std::size_t index = 0U; index < parameters.size(); ++index) {
            frame.bindings.push_back(ScopeBinding{
                parameters[index].name, true,
                function.script ? ScopeStateBinding::cleared : ScopeStateBinding::inherit,
                index == 0U ? ExpressionValue(input) : ExpressionValue{}, nullptr});
            reads.hide(parameters[index].name);
        }
        nested.push(std::move(frame));
        if (!function.script) {
            nested.set_component_path(lambda.captured.component_path() + "/" +
                                      std::string(parameter.name()));
        }
        return require_value(evaluate_node(program, function.body, nested, reads),
                             program.expression(function.body), nested);
    }
    const ExpressionValue result = invoke(lambda, {ExpressionValue(input)}, {}, reads);
    if (const Value* data = result.data_value())
        return *data;
    return Value{};
}

ExpressionValue ExpressionRuntime::evaluate_call(const Program& program,
                                                 const ProgramExpression& node,
                                                 const ExpressionScope& scope,
                                                 DependencyRecorder& reads) {
    const ExpressionValue callee = evaluate_node(program, node.first, scope, reads);
    std::vector<ExpressionValue> positional;
    std::vector<std::pair<Symbol, ExpressionValue>> named;
    for (const ProgramArgument& argument : program.arguments(node)) {
        ExpressionValue value = evaluate_node(program, argument.value, scope, reads);
        if (argument.named)
            named.emplace_back(argument.name, std::move(value));
        else
            positional.push_back(std::move(value));
    }
    if (callee.lambda() != nullptr && *callee.lambda() != nullptr)
        return invoke(**callee.lambda(), std::move(positional), std::move(named), reads);
    // An action value called like a function is performed by the handler running it.
    if (callee.action() != nullptr && *callee.action() != nullptr) {
        const std::shared_ptr<const ActionValue>& action = *callee.action();
        if (action->script != nullptr) {
            for (auto& [name, value] : named)
                positional.push_back(std::move(value));
            return invoke(*action->script, std::move(positional), {}, reads);
        }
        perform_action(action);
        return ExpressionValue{};
    }
    const std::string name(program.expression(node.first).name.name());
    if (scripting_ > 0U || active_run_ != nullptr)
        script_error("STRATA.SCRIPT.NOT_CALLABLE", "'" + name + "' is not a function.");
    report(node, scope, "STRATA.SCRIPT.NOT_CALLABLE", "'" + name + "' is not a function.");
    return ExpressionValue{};
}

ExpressionScope ExpressionRuntime::function_scope(
    const LambdaValue& function, std::vector<ExpressionValue> positional,
    std::vector<std::pair<Symbol, ExpressionValue>> named, DependencyRecorder& reads) {
    const Program& program = *function.program;
    const ProgramFunction& declaration = function.declaration();
    ExpressionScope scope;
    if (function.group != nullptr) {
        scope = function.group->scope;
        scope.keep(function.group);
    } else {
        scope = function.captured;
    }
    const std::span<const ProgramFunctionParameter> parameters = program.parameters(declaration);
    ScopeFrame frame;
    // A helper's item function sees no state binding; a script function reads and writes states.
    frame.hides_parent_states = !declaration.script;
    frame.bindings.reserve(parameters.size());
    for (std::size_t index = 0U; index < parameters.size(); ++index) {
        const ProgramFunctionParameter& parameter = parameters[index];
        reads.hide(parameter.name);
        std::optional<ExpressionValue> value;
        if (index < positional.size())
            value = std::move(positional[index]);
        for (auto& [name, argument] : named) {
            if (name == parameter.name)
                value = std::move(argument);
        }
        if (!value.has_value() && parameter.default_value != no_program_id) {
            // A default sees the parameters before it.
            ExpressionScope defaults = scope;
            ScopeFrame bound = frame;
            defaults.push(std::move(bound));
            value = evaluate_node(program, parameter.default_value, defaults, reads);
        }
        frame.bindings.push_back(ScopeBinding{parameter.name, true, ScopeStateBinding::cleared,
                                              value.value_or(ExpressionValue{}), nullptr});
    }
    scope.push(std::move(frame));
    return scope;
}

ExpressionValue ExpressionRuntime::call(const LambdaValue& function,
                                        std::vector<ExpressionValue> arguments) {
    DependencyRecorder ignored;
    try {
        return invoke(function, std::move(arguments), {}, ignored);
    } catch (const ScriptThrow& thrown) {
        const Value* message = thrown.value.field("message");
        report(RuntimeDiagnostic{
            "STRATA.SCRIPT.UNCAUGHT",
            "A function threw: " +
                (message != nullptr ? display_string(*message) : display_string(thrown.value)),
            function.captured.component_path(),
            std::nullopt,
            DiagnosticSeverity::error,
            std::nullopt,
        });
        return ExpressionValue{};
    }
}

void ExpressionRuntime::script_error(std::string code, std::string message) {
    throw ScriptThrow{Value(std::vector<std::pair<std::string, Value>>{
        {"code", Value(std::move(code))},
        {"message", Value(std::move(message))},
    })};
}

std::shared_ptr<const CollectionViewValue>
ExpressionRuntime::collection_view(const Program& program, const ExpressionId expression,
                                   const ExpressionScope& scope, DependencyRecorder& upstream) {
    const ProgramExpression& helper = program.expression(expression);
    const Names& known = names();
    const std::string& operation = helper.text;
    const HelperKind kind = helper.helper;
    const ProgramArgument* source_argument = argument(program, helper, known.source, 0U);
    const auto empty_view = [&operation] {
        return std::shared_ptr<const CollectionViewValue>(new CollectionViewValue{
            CollectionViewImmutableIdentity{
                Value(std::vector<Value>{}),
                0U,
                0U,
                0U,
                0U,
                operation,
            },
        });
    };
    if (source_argument == nullptr) {
        report(helper, scope, "STRATA.DSL.RUNTIME_COLLECTION_SOURCE",
               "Collection helper requires a source list.");
        return empty_view();
    }
    const ProgramNodeKey key{program.serial(), expression};
    if (const auto bucket = collection_cache_.find(key); bucket != collection_cache_.end()) {
        for (std::size_t position = bucket->second.size(); position-- > 0U;) {
            const CollectionCacheEntry& cached = bucket->second[position];
            bool current = true;
            for (const CollectionDependencyRead& read : cached.dependency_order) {
                if (read.kind == CollectionDependencyKind::lexical) {
                    if (!lexical_read_current(scope, read.lexical,
                                              cached.lexical_dependencies.at(read.lexical).value)) {
                        current = false;
                        break;
                    }
                } else {
                    const ExpressionHostDependency& stored =
                        cached.host_dependencies.at(read.host_key);
                    if (read_host_dependency(stored.key, scope) != stored) {
                        current = false;
                        break;
                    }
                }
            }
            if (!current)
                continue;
            // Whoever reads the view reads what it was derived from.
            for (const CollectionDependencyRead& read : cached.dependency_order) {
                if (read.kind == CollectionDependencyKind::lexical)
                    upstream.lexical(read.lexical,
                                     cached.lexical_dependencies.at(read.lexical).value);
                else
                    upstream.host(cached.host_dependencies.at(read.host_key));
            }
            static_cast<void>(cached.view->cache_hits.fetch_add(1U, std::memory_order_relaxed));
            return cached.view;
        }
    }

    // What derives the view, recorded for its cache entry and told to whoever reads it.
    DependencyRecorder reads(&upstream, DependencyRecorder::Keep::first,
                             DependencyRecorder::Keep::first);
    reads.keep_order();

    const ExpressionValue source = evaluate_node(program, source_argument->value, scope, reads);
    const ValueList* source_items = collection_items(source);
    const auto [total, source_matched] = collection_counts(source);
    if (source_items == nullptr) {
        report(helper, scope, "STRATA.DSL.RUNTIME_COLLECTION_SOURCE",
               "Collection helper requires a list or derived collection view.");
        return empty_view();
    }

    std::vector<Value> scalar_arguments;
    const std::span<const ProgramArgument> arguments = program.arguments(helper);
    for (std::size_t index = 1U; index < arguments.size(); ++index) {
        const ProgramExpression& value = program.expression(arguments[index].value);
        if (value.kind != ExpressionKind::lambda) {
            scalar_arguments.push_back(require_value(
                evaluate_node(program, arguments[index].value, scope, reads), value, scope));
        }
    }

    const ProgramArgument* lambda_argument = nullptr;
    if (kind == HelperKind::filter || kind == HelperKind::take_while)
        lambda_argument = argument(program, helper, known.predicate, 1U);
    else if (kind == HelperKind::map)
        lambda_argument = argument(program, helper, known.transform, 1U);
    else if (kind == HelperKind::sort_by || kind == HelperKind::distinct_by ||
             kind == HelperKind::group_by)
        lambda_argument = argument(program, helper, known.selector, 1U);
    const ExpressionValue lambda_value =
        lambda_argument != nullptr ? evaluate_node(program, lambda_argument->value, scope, reads)
                                   : ExpressionValue{};
    ItemCall call(*this, lambda_value.lambda(), reads);
    const bool has_lambda = call.present();

    std::vector<Value> result;
    if (kind == HelperKind::filter) {
        for (const Value& value : source_items->values) {
            if (has_lambda && truthy(call(value)))
                result.push_back(value);
        }
    } else if (kind == HelperKind::map) {
        result.reserve(source_items->values.size());
        for (const Value& value : source_items->values) {
            result.push_back(has_lambda ? call(value) : Value{});
        }
    } else if (kind == HelperKind::sort_by) {
        const bool descending = !scalar_arguments.empty() &&
                                scalar_arguments.back().boolean() != nullptr &&
                                *scalar_arguments.back().boolean();
        std::vector<std::pair<Value, Value>> decorated;
        decorated.reserve(source_items->values.size());
        for (const Value& value : source_items->values) {
            decorated.emplace_back(value, has_lambda ? call(value) : Value{});
        }
        std::stable_sort(decorated.begin(), decorated.end(),
                         [descending](const auto& left, const auto& right) {
                             const int compared = compare_keys(left.second, right.second);
                             return descending ? compared > 0 : compared < 0;
                         });
        result.reserve(decorated.size());
        for (auto& [value, sort_key] : decorated) {
            static_cast<void>(sort_key);
            result.push_back(std::move(value));
        }
    } else if (kind == HelperKind::distinct_by) {
        std::vector<Value> seen;
        for (const Value& value : source_items->values) {
            const Value distinct = has_lambda ? call(value) : value;
            if (std::ranges::find(seen, distinct) == seen.end()) {
                seen.push_back(distinct);
                result.push_back(value);
            }
        }
    } else if (kind == HelperKind::group_by) {
        std::vector<std::pair<Value, std::vector<Value>>> groups;
        for (const Value& value : source_items->values) {
            const Value group_key = has_lambda ? call(value) : Value{};
            auto found = std::ranges::find_if(
                groups, [&group_key](const auto& group) { return group.first == group_key; });
            if (found == groups.end()) {
                groups.emplace_back(group_key, std::vector<Value>{value});
            } else {
                found->second.push_back(value);
            }
        }
        for (auto& [group_key, items] : groups) {
            result.emplace_back(std::vector<std::pair<std::string, Value>>{
                {"key", group_key},
                {"items", Value(std::move(items))},
            });
        }
    } else if (kind == HelperKind::flatten) {
        for (const Value& value : source_items->values) {
            if (const ValueList* nested = value.list())
                result.insert(result.end(), nested->values.begin(), nested->values.end());
        }
    } else if (kind == HelperKind::take_while) {
        for (const Value& value : source_items->values) {
            if (!has_lambda || !truthy(call(value)))
                break;
            result.push_back(value);
        }
    } else if (kind == HelperKind::window || kind == HelperKind::page) {
        const bool page = kind == HelperKind::page;
        const std::size_t first =
            !scalar_arguments.empty() ? bounded_index(scalar_arguments[0]).value_or(0U) : 0U;
        const std::size_t amount =
            scalar_arguments.size() > 1U
                ? std::min(bounded_index(scalar_arguments[1]).value_or(0U), maximum_derived_items)
                : 0U;
        const std::size_t offset =
            page && amount != 0U && first <= std::numeric_limits<std::size_t>::max() / amount
                ? first * amount
            : page ? std::numeric_limits<std::size_t>::max()
                   : first;
        if (offset < source_items->values.size()) {
            const std::size_t end =
                std::min(source_items->values.size(),
                         offset + std::min(amount, source_items->values.size() - offset));
            result.insert(result.end(),
                          source_items->values.begin() + static_cast<std::ptrdiff_t>(offset),
                          source_items->values.begin() + static_cast<std::ptrdiff_t>(end));
        }
    }
    if (result.size() > maximum_derived_items) {
        result.resize(maximum_derived_items);
        report(helper, scope, "STRATA.DSL.RUNTIME_COLLECTION_BOUND_EXCEEDED",
               "Collection helper output exceeded the runtime bound.");
    }
    if (kind == HelperKind::map && std::ranges::any_of(result, [](const Value& value) {
            if (value.object() == nullptr)
                return false;
            const Value* stable = value.field("key");
            if (stable == nullptr)
                stable = value.field("id");
            return stable == nullptr || stable->kind() == ValueKind::null_value ||
                   stable->kind() == ValueKind::list || stable->kind() == ValueKind::object;
        })) {
        report(helper, scope, "STRATA.DSL.RUNTIME_COLLECTION_UNSTABLE_KEY",
               "Mapped record results must expose a stable 'key' or 'id' field before they are "
               "repeated.",
               "record containing key or id");
    }
    const bool changes_match_count = kind == HelperKind::filter ||
                                     kind == HelperKind::distinct_by ||
                                     kind == HelperKind::group_by || kind == HelperKind::flatten ||
                                     kind == HelperKind::take_while;
    const std::size_t matched = changes_match_count ? result.size() : source_matched;
    std::size_t range_start = 0U;
    if ((kind == HelperKind::window || kind == HelperKind::page) && !scalar_arguments.empty()) {
        range_start = bounded_index(scalar_arguments[0]).value_or(0U);
        if (kind == HelperKind::page && scalar_arguments.size() > 1U) {
            const std::size_t amount = bounded_index(scalar_arguments[1]).value_or(0U);
            range_start =
                amount != 0U && range_start <= std::numeric_limits<std::size_t>::max() / amount
                    ? range_start * amount
                    : std::numeric_limits<std::size_t>::max();
        }
    }
    range_start = std::min(range_start, matched);
    const std::size_t unbounded_end =
        std::numeric_limits<std::size_t>::max() - range_start < result.size()
            ? std::numeric_limits<std::size_t>::max()
            : range_start + result.size();
    const std::size_t range_end = std::min(unbounded_end, std::max(matched, unbounded_end));
    auto view = std::shared_ptr<const CollectionViewValue>(new CollectionViewValue{
        CollectionViewImmutableIdentity{
            Value(std::move(result)),
            total,
            matched,
            range_start,
            range_end,
            operation,
        },
    });
    if (collection_cache_entries_ >= 1024U) {
        collection_cache_.clear();
        collection_cache_entries_ = 0U;
    }
    std::vector<CollectionCacheEntry>& entries = collection_cache_[key];
    // An entry for the same lexical context is superseded: only its host reads differed.
    const auto same_context = [&reads](const CollectionCacheEntry& entry) {
        return std::ranges::equal(entry.lexical_dependencies, reads.lexical_reads(),
                                  [](const auto& left, const auto& right) {
                                      return left.first == right.first &&
                                             same_expression_value(left.second.value,
                                                                   right.second.value);
                                  });
    };
    if (const auto superseded = std::ranges::find_if(entries, same_context);
        superseded != entries.end()) {
        entries.erase(superseded);
        --collection_cache_entries_;
    }
    std::vector<CollectionDependencyRead> dependency_order;
    dependency_order.reserve(reads.order().size());
    for (const DependencyRecorder::Read& read : reads.order()) {
        dependency_order.push_back(CollectionDependencyRead{
            read.host != nullptr ? CollectionDependencyKind::host
                                 : CollectionDependencyKind::lexical,
            read.lexical != nullptr ? *read.lexical : LexicalRead{},
            read.host != nullptr ? *read.host : std::string{},
        });
    }
    entries.push_back(CollectionCacheEntry{
        std::move(reads.lexical_reads()),
        std::move(reads.host_reads()),
        std::move(dependency_order),
        view,
    });
    ++collection_cache_entries_;
    return view;
}

std::shared_ptr<const ActionValue>
ExpressionRuntime::composed_action(const Program& program, const ProgramExpression& node,
                                   const ExpressionScope& scope, const ActionCompositionMode mode,
                                   DependencyRecorder& reads) {
    std::vector<std::shared_ptr<const ActionValue>> children;
    for (const ProgramArgument& argument : program.arguments(node)) {
        const ExpressionValue evaluated = evaluate_node(program, argument.value, scope, reads);
        if (evaluated.action() == nullptr) {
            report(node, scope, "STRATA.DSL.RUNTIME_ACTION_COMPOSITION",
                   "Action composition requires typed actions.");
            return std::make_shared<const ActionValue>(ActionValue{});
        }
        children.push_back(*evaluated.action());
    }
    if (children.empty()) {
        report(node, scope, "STRATA.DSL.RUNTIME_ACTION_COMPOSITION",
               "Action composition must not be empty.");
        return std::make_shared<const ActionValue>(ActionValue{});
    }
    const std::string id =
        mode == ActionCompositionMode::sequence ? "action.sequence" : "action.parallel";
    const auto contract = actions_.contract(id);
    if (contract == nullptr) {
        report(node, scope, "STRATA.DSL.RUNTIME_UNKNOWN_ACTION",
               "Framework composition action is not registered.");
        return std::make_shared<const ActionValue>(ActionValue{});
    }
    Value payload(std::vector<std::pair<std::string, Value>>{});
    auto action = std::make_shared<const Action>(contract, std::move(payload),
                                                 action_origin(program, node, scope));
    return std::make_shared<const ActionValue>(ActionValue{
        std::move(action),
        mode,
        std::move(children),
    });
}

void ExpressionRuntime::report(const ProgramExpression& node, const ExpressionScope& scope,
                               std::string code, std::string message,
                               std::optional<std::string> expected) {
    // Inside a script an error unwinds to the nearest catch instead of reading as null.
    if (scripting_ > 0U)
        script_error(std::move(code), std::move(message));
    const std::string& component_path = scope.component_path();
    const std::optional<std::string_view> path = node.source.find("path").string();
    report(RuntimeDiagnostic{
        std::move(code),
        std::move(message),
        !component_path.empty() ? component_path
        : path.has_value()      ? std::string(*path)
                                : std::string{},
        std::move(expected),
        DiagnosticSeverity::error,
        portable_expression_range(node.source),
    });
}

void ExpressionRuntime::report(RuntimeDiagnostic diagnostic) {
    std::string fingerprint = diagnostic.code;
    fingerprint.push_back('\0');
    fingerprint += diagnostic.path;
    fingerprint.push_back('\0');
    fingerprint += diagnostic.message;
    fingerprint.push_back('\0');
    if (diagnostic.expected.has_value())
        fingerprint += *diagnostic.expected;
    if (!reported_diagnostics_.insert(std::move(fingerprint)).second)
        return;
    diagnostics_.push_back(std::move(diagnostic));
}

std::string format_date_pattern(const double milliseconds, const std::string_view pattern,
                                const double offset_minutes,
                                const std::vector<std::string>& month_names,
                                const std::vector<std::string>& weekday_names) {
    return format_date(milliseconds, pattern, offset_minutes, month_names, weekday_names);
}

} // namespace strata::runtime
