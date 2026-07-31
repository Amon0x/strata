#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace strata::data {

class JsonView;

struct JsonLimits final {
    std::size_t maximum_input_bytes = 16U * 1024U * 1024U;
    std::size_t maximum_depth = 256U;
    std::size_t maximum_values = 1'000'000U;
    std::size_t maximum_string_bytes = 8U * 1024U * 1024U;
};

class JsonError final : public std::runtime_error {
  public:
    JsonError(std::size_t offset, std::string message);

    [[nodiscard]] std::size_t offset() const noexcept;

  private:
    std::size_t offset_;
};

class JsonValue final {
  public:
    struct Null final {
        [[nodiscard]] friend constexpr bool operator==(Null, Null) noexcept = default;
    };

    using Array = std::vector<JsonValue>;
    using ObjectEntry = std::pair<std::string, JsonValue>;
    using Object = std::vector<ObjectEntry>;
    using Storage = std::variant<Null, bool, std::int64_t, double, std::string, Array, Object>;

    JsonValue() noexcept;
    explicit JsonValue(Null) noexcept;
    explicit JsonValue(bool value) noexcept;
    explicit JsonValue(std::int64_t value) noexcept;
    explicit JsonValue(double value);
    explicit JsonValue(const char* value);
    explicit JsonValue(std::string value);
    explicit JsonValue(Array value);
    explicit JsonValue(Object value);

    [[nodiscard]] const Storage& storage() const noexcept;
    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] const bool* boolean() const noexcept;
    [[nodiscard]] const std::int64_t* integer() const noexcept;
    [[nodiscard]] const double* number() const noexcept;
    [[nodiscard]] const std::string* string() const noexcept;
    [[nodiscard]] const Array* array() const noexcept;
    [[nodiscard]] const Object* object() const noexcept;
    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;

    [[nodiscard]] friend bool operator==(const JsonValue& left, const JsonValue& right);

  private:
    Storage storage_;
};

[[nodiscard]] JsonValue parse_json(std::string_view source,
                                   const JsonLimits& limits = JsonLimits{});

[[nodiscard]] std::string encode_canonical_json(const JsonValue& value);
[[nodiscard]] std::string encode_canonical_json(JsonView value);
/** Canonically ordered JSON without insignificant whitespace, terminated by one newline. */
[[nodiscard]] std::string encode_json_line(const JsonValue& value);

} // namespace strata::data
