#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/value.hpp"

namespace strata::runtime {

enum class ValueSchemaKind {
    any,
    null_value,
    boolean,
    number,
    duration,
    string,
    color,
    texture,
    key,
    theme_token,
    list,
    object,
    union_value,
};

class ValueSchema;
using ValueSchemaPtr = std::shared_ptr<const ValueSchema>;

struct ValueSchemaField final {
    std::string name;
    ValueSchemaPtr schema;
    bool required = true;
    bool nullable = false;
};

/** Runtime-owned type contract used by host codecs, state mutation, and action payloads. */
class ValueSchema final {
public:
    static ValueSchemaPtr any();
    static ValueSchemaPtr scalar(ValueSchemaKind kind);
    static ValueSchemaPtr list(
        ValueSchemaPtr element,
        bool element_nullable = false,
        std::optional<std::size_t> maximum_items = std::nullopt
    );
    static ValueSchemaPtr object(
        std::vector<ValueSchemaField> fields,
        bool allow_unknown_fields = false,
        ValueSchemaPtr unknown_field_schema = any()
    );
    static ValueSchemaPtr union_of(std::vector<ValueSchemaPtr> options);

    [[nodiscard]] ValueSchemaKind kind() const noexcept;
    [[nodiscard]] bool accepts(const Value& value) const;
    /** Coerces canonical JSON-shaped values into semantic scalar kinds such as key/texture. */
    [[nodiscard]] std::optional<Value> normalize(const Value& value) const;
    [[nodiscard]] const ValueSchemaPtr& element() const noexcept;
    [[nodiscard]] bool element_nullable() const noexcept;
    [[nodiscard]] const std::optional<std::size_t>& maximum_items() const noexcept;
    [[nodiscard]] const std::vector<ValueSchemaField>& fields() const noexcept;
    [[nodiscard]] bool allow_unknown_fields() const noexcept;
    [[nodiscard]] const ValueSchemaPtr& unknown_field_schema() const noexcept;
    [[nodiscard]] const std::vector<ValueSchemaPtr>& options() const noexcept;
    [[nodiscard]] const ValueSchemaField* field(std::string_view name) const noexcept;

    [[nodiscard]] friend bool operator==(const ValueSchema& left, const ValueSchema& right);

private:
    explicit ValueSchema(ValueSchemaKind kind);

    ValueSchemaKind kind_;
    ValueSchemaPtr element_;
    bool element_nullable_ = false;
    std::optional<std::size_t> maximum_items_;
    std::vector<ValueSchemaField> fields_;
    bool allow_unknown_fields_ = false;
    ValueSchemaPtr unknown_field_schema_;
    std::vector<ValueSchemaPtr> options_;
};

} // namespace strata::runtime
