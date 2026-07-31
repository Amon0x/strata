#include "data/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>

#include "core/utf8.hpp"
#include "data/json_view.hpp"

namespace strata::data {
namespace {

class JsonReader final {
  public:
    JsonReader(const std::string_view source, const JsonLimits& limits)
        : source_(source), limits_(limits) {
        if (source.size() > limits.maximum_input_bytes) {
            fail("input exceeds the configured byte limit");
        }
        if (!strata::core::valid_utf8(source)) {
            fail("input is not valid UTF-8");
        }
    }

    [[nodiscard]] JsonValue parse() {
        skip_whitespace();
        JsonValue value = read_value(0U);
        skip_whitespace();
        if (offset_ != source_.size()) {
            fail("unexpected trailing content");
        }
        return value;
    }

  private:
    [[nodiscard]] JsonValue read_value(const std::size_t depth) {
        if (depth > limits_.maximum_depth) {
            fail("nesting exceeds the configured depth limit");
        }
        ++value_count_;
        if (value_count_ > limits_.maximum_values) {
            fail("value count exceeds the configured limit");
        }
        skip_whitespace();
        switch (peek()) {
        case '{':
            return read_object(depth);
        case '[':
            return read_array(depth);
        case '"':
            return JsonValue(read_string());
        case 't':
            read_literal("true");
            return JsonValue(true);
        case 'f':
            read_literal("false");
            return JsonValue(false);
        case 'n':
            read_literal("null");
            return JsonValue(JsonValue::Null{});
        case '-':
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            return read_number();
        case '\0':
            fail("expected a value");
        default:
            fail("unexpected byte while reading a value");
        }
    }

    [[nodiscard]] JsonValue read_object(const std::size_t depth) {
        consume('{');
        skip_whitespace();
        JsonValue::Object object;
        if (take('}')) {
            return JsonValue(std::move(object));
        }
        while (true) {
            skip_whitespace();
            if (peek() != '"') {
                fail("object key must be a string");
            }
            std::string key = read_string();
            if (std::ranges::any_of(object,
                                    [&key](const auto& entry) { return entry.first == key; })) {
                fail("duplicate object key");
            }
            skip_whitespace();
            consume(':');
            object.emplace_back(std::move(key), read_value(depth + 1U));
            skip_whitespace();
            if (take('}')) {
                break;
            }
            consume(',');
        }
        return JsonValue(std::move(object));
    }

    [[nodiscard]] JsonValue read_array(const std::size_t depth) {
        consume('[');
        skip_whitespace();
        JsonValue::Array array;
        if (take(']')) {
            return JsonValue(std::move(array));
        }
        while (true) {
            array.emplace_back(read_value(depth + 1U));
            skip_whitespace();
            if (take(']')) {
                break;
            }
            consume(',');
        }
        return JsonValue(std::move(array));
    }

    [[nodiscard]] std::string read_string() {
        consume('"');
        std::string value;
        while (true) {
            if (offset_ >= source_.size()) {
                fail("unterminated string");
            }
            const unsigned char byte = static_cast<unsigned char>(source_[offset_++]);
            if (byte == static_cast<unsigned char>('"')) {
                return value;
            }
            if (byte == static_cast<unsigned char>('\\')) {
                read_escape(value);
            } else {
                if (byte < 0x20U) {
                    fail("control character in string");
                }
                value.push_back(static_cast<char>(byte));
            }
            if (value.size() > limits_.maximum_string_bytes) {
                fail("string exceeds the configured byte limit");
            }
        }
    }

    void read_escape(std::string& output) {
        if (offset_ >= source_.size()) {
            fail("unterminated escape");
        }
        switch (source_[offset_++]) {
        case '"':
            output.push_back('"');
            return;
        case '\\':
            output.push_back('\\');
            return;
        case '/':
            output.push_back('/');
            return;
        case 'b':
            output.push_back('\b');
            return;
        case 'f':
            output.push_back('\f');
            return;
        case 'n':
            output.push_back('\n');
            return;
        case 'r':
            output.push_back('\r');
            return;
        case 't':
            output.push_back('\t');
            return;
        case 'u':
            break;
        default:
            fail("invalid string escape");
        }

        std::uint32_t code_point = read_hex_quad();
        if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
            if (offset_ + 2U > source_.size() || source_[offset_] != '\\' ||
                source_[offset_ + 1U] != 'u') {
                fail("high surrogate is not followed by a low surrogate");
            }
            offset_ += 2U;
            const std::uint32_t low = read_hex_quad();
            if (low < 0xDC00U || low > 0xDFFFU) {
                fail("high surrogate is not followed by a low surrogate");
            }
            code_point = 0x10000U + ((code_point - 0xD800U) << 10U) + (low - 0xDC00U);
        } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
            fail("unpaired low surrogate");
        }
        append_utf8(output, code_point);
    }

    [[nodiscard]] std::uint32_t read_hex_quad() {
        if (offset_ + 4U > source_.size()) {
            fail("incomplete Unicode escape");
        }
        std::uint32_t value = 0U;
        for (std::size_t index = 0U; index < 4U; ++index) {
            const char digit = source_[offset_++];
            value <<= 4U;
            if (digit >= '0' && digit <= '9') {
                value |= static_cast<std::uint32_t>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
                value |= static_cast<std::uint32_t>(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
                value |= static_cast<std::uint32_t>(digit - 'A' + 10);
            } else {
                fail("invalid Unicode escape");
            }
        }
        return value;
    }

    static void append_utf8(std::string& output, const std::uint32_t code_point) {
        if (code_point <= 0x7FU) {
            output.push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else if (code_point <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
        }
    }

    [[nodiscard]] JsonValue read_number() {
        const std::size_t start = offset_;
        static_cast<void>(take('-'));
        if (take('0')) {
            if (is_digit(peek())) {
                fail("leading zero in number");
            }
        } else {
            read_digits();
        }
        bool integral = true;
        if (take('.')) {
            integral = false;
            read_digits();
        }
        if (peek() == 'e' || peek() == 'E') {
            integral = false;
            ++offset_;
            if (peek() == '+' || peek() == '-') {
                ++offset_;
            }
            read_digits();
        }
        const std::string_view token = source_.substr(start, offset_ - start);
        if (integral) {
            std::int64_t integer = 0;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), integer);
            if (parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size()) {
                return JsonValue(integer);
            }
        }
        double number = 0.0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), number,
                                            std::chars_format::general);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
            !std::isfinite(number)) {
            fail("invalid finite JSON number");
        }
        return JsonValue(number == 0.0 ? 0.0 : number);
    }

    void read_digits() {
        const std::size_t start = offset_;
        while (is_digit(peek())) {
            ++offset_;
        }
        if (offset_ == start) {
            fail("expected a digit");
        }
    }

    void read_literal(const std::string_view literal) {
        if (source_.substr(offset_, literal.size()) != literal) {
            fail("invalid literal");
        }
        offset_ += literal.size();
    }

    void skip_whitespace() noexcept {
        while (peek() == ' ' || peek() == '\n' || peek() == '\r' || peek() == '\t') {
            ++offset_;
        }
    }

    void consume(const char expected) {
        if (!take(expected)) {
            fail("unexpected JSON punctuation");
        }
    }

    [[nodiscard]] bool take(const char expected) noexcept {
        if (peek() != expected) {
            return false;
        }
        ++offset_;
        return true;
    }

    [[nodiscard]] char peek() const noexcept {
        return offset_ < source_.size() ? source_[offset_] : '\0';
    }

    [[nodiscard]] static bool is_digit(const char value) noexcept {
        return value >= '0' && value <= '9';
    }

    [[noreturn]] void fail(const std::string_view message) const {
        throw JsonError(offset_, std::string(message));
    }

    std::string_view source_;
    const JsonLimits& limits_;
    std::size_t offset_ = 0U;
    std::size_t value_count_ = 0U;
};

void append_indent(std::string& output, const std::size_t depth) {
    output.append(depth * 2U, ' ');
}

void append_quoted(std::string& output, const std::string_view value) {
    output.push_back('"');
    constexpr char hexadecimal[] = "0123456789abcdef";
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"':
            output.append("\\\"");
            break;
        case '\\':
            output.append("\\\\");
            break;
        case '\b':
            output.append("\\b");
            break;
        case '\f':
            output.append("\\f");
            break;
        case '\n':
            output.append("\\n");
            break;
        case '\r':
            output.append("\\r");
            break;
        case '\t':
            output.append("\\t");
            break;
        default:
            if (byte < 0x20U) {
                output.append("\\u00");
                output.push_back(hexadecimal[byte >> 4U]);
                output.push_back(hexadecimal[byte & 0x0FU]);
            } else {
                output.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    output.push_back('"');
}

[[nodiscard]] std::string plain_decimal(double value) {
    if (value == 0.0) {
        return "0";
    }
    char buffer[128]{};
    const auto converted =
        std::to_chars(std::begin(buffer), std::end(buffer), value, std::chars_format::general);
    if (converted.ec != std::errc{}) {
        throw std::runtime_error("failed to encode finite JSON number");
    }
    std::string shortest(buffer, converted.ptr);
    const std::size_t exponent_at = shortest.find_first_of("eE");
    if (exponent_at == std::string::npos) {
        return shortest;
    }

    const bool negative = shortest.front() == '-';
    const std::size_t mantissa_start = negative ? 1U : 0U;
    const std::string_view mantissa(shortest.data() + mantissa_start, exponent_at - mantissa_start);
    const std::size_t dot = mantissa.find('.');
    const std::size_t original_point = dot == std::string_view::npos ? mantissa.size() : dot;
    std::string digits(mantissa);
    if (dot != std::string_view::npos) {
        digits.erase(dot, 1U);
    }

    const std::string_view exponent_text(shortest.data() + exponent_at + 1U,
                                         shortest.size() - exponent_at - 1U);
    const bool exponent_negative = exponent_text.front() == '-';
    const std::size_t exponent_digits_start =
        exponent_text.front() == '-' || exponent_text.front() == '+' ? 1U : 0U;
    const std::string_view exponent_digits = exponent_text.substr(exponent_digits_start);
    int exponent_magnitude = 0;
    const auto parsed =
        std::from_chars(exponent_digits.data(), exponent_digits.data() + exponent_digits.size(),
                        exponent_magnitude);
    if (parsed.ec != std::errc{} || parsed.ptr != exponent_digits.data() + exponent_digits.size()) {
        throw std::runtime_error("failed to decode JSON number exponent");
    }
    const int exponent = exponent_negative ? -exponent_magnitude : exponent_magnitude;
    const auto decimal_point = static_cast<std::int64_t>(original_point) + exponent;
    std::string result;
    if (negative) {
        result.push_back('-');
    }
    if (decimal_point <= 0) {
        result.append("0.");
        result.append(static_cast<std::size_t>(-decimal_point), '0');
        result.append(digits);
    } else if (static_cast<std::size_t>(decimal_point) >= digits.size()) {
        result.append(digits);
        result.append(static_cast<std::size_t>(decimal_point) - digits.size(), '0');
    } else {
        result.append(digits.substr(0U, static_cast<std::size_t>(decimal_point)));
        result.push_back('.');
        result.append(digits.substr(static_cast<std::size_t>(decimal_point)));
    }
    return result;
}

void append_view(std::string& output, const JsonView value, const std::size_t depth) {
    switch (value.kind()) {
    case JsonViewKind::invalid:
        throw std::invalid_argument("canonical JSON rejects an invalid view");
    case JsonViewKind::null_value:
        output.append("null");
        return;
    case JsonViewKind::boolean:
        output.append(*value.boolean() ? "true" : "false");
        return;
    case JsonViewKind::integer: {
        char buffer[32]{};
        const auto converted =
            std::to_chars(std::begin(buffer), std::end(buffer), *value.integer());
        output.append(buffer, converted.ptr);
        return;
    }
    case JsonViewKind::number: {
        const double number = *value.number();
        if (!std::isfinite(number)) {
            throw std::invalid_argument("canonical JSON rejects non-finite numbers");
        }
        output.append(plain_decimal(number));
        return;
    }
    case JsonViewKind::string:
        append_quoted(output, *value.string());
        return;
    case JsonViewKind::array: {
        const JsonArrayView array = *value.array();
        if (array.empty()) {
            output.append("[]");
            return;
        }
        output.append("[\n");
        for (std::size_t index = 0U; index < array.size(); ++index) {
            append_indent(output, depth + 1U);
            append_view(output, array[index], depth + 1U);
            if (index + 1U != array.size())
                output.push_back(',');
            output.push_back('\n');
        }
        append_indent(output, depth);
        output.push_back(']');
        return;
    }
    case JsonViewKind::object: {
        const JsonObjectView object = *value.object();
        if (object.empty()) {
            output.append("{}");
            return;
        }
        std::vector<JsonObjectView::Entry> sorted(object.begin(), object.end());
        std::ranges::sort(sorted, {}, &JsonObjectView::Entry::first);
        output.append("{\n");
        for (std::size_t index = 0U; index < sorted.size(); ++index) {
            append_indent(output, depth + 1U);
            append_quoted(output, sorted[index].first);
            output.append(": ");
            append_view(output, sorted[index].second, depth + 1U);
            if (index + 1U != sorted.size())
                output.push_back(',');
            output.push_back('\n');
        }
        append_indent(output, depth);
        output.push_back('}');
        return;
    }
    }
    throw std::logic_error("unknown JSON view kind");
}

void append_compact_value(std::string& output, const JsonValue& value) {
    const JsonValue::Storage& storage = value.storage();
    if (std::holds_alternative<JsonValue::Null>(storage)) {
        output.append("null");
    } else if (const auto* boolean = std::get_if<bool>(&storage)) {
        output.append(*boolean ? "true" : "false");
    } else if (const auto* integer = std::get_if<std::int64_t>(&storage)) {
        char buffer[32]{};
        const auto converted = std::to_chars(std::begin(buffer), std::end(buffer), *integer);
        output.append(buffer, converted.ptr);
    } else if (const auto* number = std::get_if<double>(&storage)) {
        if (!std::isfinite(*number)) {
            throw std::invalid_argument("compact JSON rejects non-finite numbers");
        }
        output.append(plain_decimal(*number));
    } else if (const auto* string = std::get_if<std::string>(&storage)) {
        append_quoted(output, *string);
    } else if (const auto* array = std::get_if<JsonValue::Array>(&storage)) {
        output.push_back('[');
        for (std::size_t index = 0U; index < array->size(); ++index) {
            if (index != 0U)
                output.push_back(',');
            append_compact_value(output, (*array)[index]);
        }
        output.push_back(']');
    } else {
        const auto& object = std::get<JsonValue::Object>(storage);
        std::vector<const JsonValue::ObjectEntry*> sorted;
        sorted.reserve(object.size());
        for (const auto& entry : object)
            sorted.push_back(&entry);
        std::ranges::sort(sorted, {},
                          [](const auto* entry) -> const std::string& { return entry->first; });
        output.push_back('{');
        for (std::size_t index = 0U; index < sorted.size(); ++index) {
            if (index != 0U)
                output.push_back(',');
            append_quoted(output, sorted[index]->first);
            output.push_back(':');
            append_compact_value(output, sorted[index]->second);
        }
        output.push_back('}');
    }
}

void append_value(std::string& output, const JsonValue& value, const std::size_t depth) {
    const JsonValue::Storage& storage = value.storage();
    if (std::holds_alternative<JsonValue::Null>(storage)) {
        output.append("null");
    } else if (const auto* boolean = std::get_if<bool>(&storage)) {
        output.append(*boolean ? "true" : "false");
    } else if (const auto* integer = std::get_if<std::int64_t>(&storage)) {
        char buffer[32]{};
        const auto converted = std::to_chars(std::begin(buffer), std::end(buffer), *integer);
        output.append(buffer, converted.ptr);
    } else if (const auto* number = std::get_if<double>(&storage)) {
        if (!std::isfinite(*number)) {
            throw std::invalid_argument("canonical JSON rejects non-finite numbers");
        }
        output.append(plain_decimal(*number));
    } else if (const auto* string = std::get_if<std::string>(&storage)) {
        append_quoted(output, *string);
    } else if (const auto* array = std::get_if<JsonValue::Array>(&storage)) {
        if (array->empty()) {
            output.append("[]");
            return;
        }
        output.append("[\n");
        for (std::size_t index = 0U; index < array->size(); ++index) {
            append_indent(output, depth + 1U);
            append_value(output, (*array)[index], depth + 1U);
            if (index + 1U != array->size()) {
                output.push_back(',');
            }
            output.push_back('\n');
        }
        append_indent(output, depth);
        output.push_back(']');
    } else {
        const auto& object = std::get<JsonValue::Object>(storage);
        if (object.empty()) {
            output.append("{}");
            return;
        }
        std::vector<const JsonValue::ObjectEntry*> sorted;
        sorted.reserve(object.size());
        for (const auto& entry : object) {
            sorted.push_back(&entry);
        }
        std::ranges::sort(sorted, {},
                          [](const auto* entry) -> const std::string& { return entry->first; });
        output.append("{\n");
        for (std::size_t index = 0U; index < sorted.size(); ++index) {
            append_indent(output, depth + 1U);
            append_quoted(output, sorted[index]->first);
            output.append(": ");
            append_value(output, sorted[index]->second, depth + 1U);
            if (index + 1U != sorted.size()) {
                output.push_back(',');
            }
            output.push_back('\n');
        }
        append_indent(output, depth);
        output.push_back('}');
    }
}

} // namespace

JsonError::JsonError(const std::size_t offset, std::string message)
    : std::runtime_error(std::move(message) + " at offset " + std::to_string(offset)),
      offset_(offset) {}

std::size_t JsonError::offset() const noexcept {
    return offset_;
}

JsonValue::JsonValue() noexcept : storage_(Null{}) {}
JsonValue::JsonValue(Null) noexcept : storage_(Null{}) {}
JsonValue::JsonValue(const bool value) noexcept : storage_(value) {}
JsonValue::JsonValue(const std::int64_t value) noexcept : storage_(value) {}

JsonValue::JsonValue(const double value) : storage_(value == 0.0 ? 0.0 : value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("JSON number must be finite");
    }
}

JsonValue::JsonValue(const char* const value) : JsonValue(std::string(value)) {}

JsonValue::JsonValue(std::string value) : storage_(std::move(value)) {
    if (!strata::core::valid_utf8(std::get<std::string>(storage_))) {
        throw std::invalid_argument("JSON string must be valid UTF-8");
    }
}

JsonValue::JsonValue(Array value) : storage_(std::move(value)) {}
JsonValue::JsonValue(Object value) : storage_(std::move(value)) {}

bool operator==(const JsonValue& left, const JsonValue& right) {
    if (left.storage_.index() != right.storage_.index()) {
        const auto integer_and_number_equal = [](const JsonValue::Storage& integer_storage,
                                                 const JsonValue::Storage& number_storage) {
            const auto* integer = std::get_if<std::int64_t>(&integer_storage);
            const auto* number = std::get_if<double>(&number_storage);
            if (integer == nullptr || number == nullptr) {
                return false;
            }
            char integer_buffer[32]{};
            const auto converted =
                std::to_chars(std::begin(integer_buffer), std::end(integer_buffer), *integer);
            return std::string_view(integer_buffer,
                                    static_cast<std::size_t>(converted.ptr - integer_buffer)) ==
                   plain_decimal(*number);
        };
        if (integer_and_number_equal(left.storage_, right.storage_) ||
            integer_and_number_equal(right.storage_, left.storage_)) {
            return true;
        }
        return false;
    }
    if (const auto* left_object = std::get_if<JsonValue::Object>(&left.storage_)) {
        const auto& right_object = std::get<JsonValue::Object>(right.storage_);
        if (left_object->size() != right_object.size()) {
            return false;
        }
        return std::ranges::all_of(*left_object, [&right_object](const auto& entry) {
            const auto found =
                std::ranges::find(right_object, entry.first, &JsonValue::ObjectEntry::first);
            return found != right_object.end() && found->second == entry.second;
        });
    }
    return left.storage_ == right.storage_;
}

const JsonValue::Storage& JsonValue::storage() const noexcept {
    return storage_;
}

bool JsonValue::is_null() const noexcept {
    return std::holds_alternative<Null>(storage_);
}

const bool* JsonValue::boolean() const noexcept {
    return std::get_if<bool>(&storage_);
}

const std::int64_t* JsonValue::integer() const noexcept {
    return std::get_if<std::int64_t>(&storage_);
}

const double* JsonValue::number() const noexcept {
    return std::get_if<double>(&storage_);
}

const std::string* JsonValue::string() const noexcept {
    return std::get_if<std::string>(&storage_);
}

const JsonValue::Array* JsonValue::array() const noexcept {
    return std::get_if<Array>(&storage_);
}

const JsonValue::Object* JsonValue::object() const noexcept {
    return std::get_if<Object>(&storage_);
}

const JsonValue* JsonValue::find(const std::string_view key) const noexcept {
    const Object* entries = object();
    if (entries == nullptr) {
        return nullptr;
    }
    const auto found = std::ranges::find(*entries, key, &ObjectEntry::first);
    return found == entries->end() ? nullptr : &found->second;
}

JsonValue parse_json(const std::string_view source, const JsonLimits& limits) {
    return JsonReader(source, limits).parse();
}

std::string encode_canonical_json(const JsonValue& value) {
    std::string output;
    append_value(output, value, 0U);
    output.push_back('\n');
    return output;
}

std::string encode_canonical_json(const JsonView value) {
    std::string output;
    append_view(output, value, 0U);
    output.push_back('\n');
    return output;
}

std::string encode_json_line(const JsonValue& value) {
    std::string output;
    append_compact_value(output, value);
    output.push_back('\n');
    return output;
}

} // namespace strata::data
