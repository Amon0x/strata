#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "data/json.hpp"

namespace strata::runtime {

struct NullValue final {
    [[nodiscard]] friend constexpr bool operator==(NullValue, NullValue) noexcept = default;
};

struct DurationValue final {
    std::int64_t nanoseconds;
    [[nodiscard]] friend bool operator==(const DurationValue&, const DurationValue&) = default;
};

struct ColorValue final {
    std::uint8_t red = 0U;
    std::uint8_t green = 0U;
    std::uint8_t blue = 0U;
    std::uint8_t alpha = 0U;
    [[nodiscard]] friend bool operator==(const ColorValue&, const ColorValue&) = default;
};

struct ImageValue final {
    std::string id;
    [[nodiscard]] friend bool operator==(const ImageValue&, const ImageValue&) = default;
};

struct KeyValue final {
    std::string value;
    [[nodiscard]] friend bool operator==(const KeyValue&, const KeyValue&) = default;
};

struct ThemeTokenValue final {
    std::string name;
    [[nodiscard]] friend bool operator==(const ThemeTokenValue&, const ThemeTokenValue&) = default;
};

class Value;
struct ValueList;
struct ValueObject;

enum class ValueKind {
    null_value,
    boolean,
    number,
    duration,
    string,
    color,
    image,
    key,
    theme_token,
    list,
    object,
};

/**
 * Immutable runtime value. Composite storage is shared so snapshots and retained state can reuse
 * values without process-global identities, copy-on-write mutation, or allocator ambiguity.
 */
class Value final {
public:
    using ListPtr = std::shared_ptr<const ValueList>;
    using ObjectPtr = std::shared_ptr<const ValueObject>;
    using Storage = std::variant<
        NullValue,
        bool,
        double,
        DurationValue,
        std::string,
        ColorValue,
        ImageValue,
        KeyValue,
        ThemeTokenValue,
        ListPtr,
        ObjectPtr
    >;

    Value() noexcept;
    explicit Value(NullValue) noexcept;
    explicit Value(bool value) noexcept;
    explicit Value(double value);
    explicit Value(DurationValue value) noexcept;
    explicit Value(std::string value);
    explicit Value(const char* value);
    explicit Value(ColorValue value) noexcept;
    explicit Value(ImageValue value);
    explicit Value(KeyValue value);
    explicit Value(ThemeTokenValue value);
    explicit Value(std::vector<Value> values);
    explicit Value(std::vector<std::pair<std::string, Value>> fields);

    [[nodiscard]] ValueKind kind() const noexcept;
    [[nodiscard]] const Storage& storage() const noexcept;
    [[nodiscard]] const bool* boolean() const noexcept;
    [[nodiscard]] const double* number() const noexcept;
    [[nodiscard]] const DurationValue* duration() const noexcept;
    [[nodiscard]] const std::string* string() const noexcept;
    [[nodiscard]] const ColorValue* color() const noexcept;
    [[nodiscard]] const ImageValue* image() const noexcept;
    [[nodiscard]] const KeyValue* key() const noexcept;
    [[nodiscard]] const ThemeTokenValue* theme_token() const noexcept;
    [[nodiscard]] const ValueList* list() const noexcept;
    [[nodiscard]] const ValueObject* object() const noexcept;
    [[nodiscard]] const void* composite_identity() const noexcept;
    [[nodiscard]] const Value* field(std::string_view name) const noexcept;
    [[nodiscard]] std::string_view state_type_id() const noexcept;

    friend bool operator==(const Value& left, const Value& right);

private:
    Storage storage_;
};

struct ValueList final {
    std::vector<Value> values;
    [[nodiscard]] friend bool operator==(const ValueList&, const ValueList&) = default;
};

struct ValueObject final {
    std::vector<std::pair<std::string, Value>> fields;
    [[nodiscard]] friend bool operator==(const ValueObject&, const ValueObject&) = default;
};

[[nodiscard]] Value value_from_json(const data::JsonValue& value);
[[nodiscard]] data::JsonValue value_to_json(const Value& value);

} // namespace strata::runtime
