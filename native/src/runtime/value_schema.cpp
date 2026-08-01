#include "runtime/value_schema.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include "core/utf8.hpp"

namespace strata::runtime {
namespace {

[[nodiscard]] ValueKind value_kind_for(const ValueSchemaKind kind) {
    switch (kind) {
    case ValueSchemaKind::null_value: return ValueKind::null_value;
    case ValueSchemaKind::boolean: return ValueKind::boolean;
    case ValueSchemaKind::number: return ValueKind::number;
    case ValueSchemaKind::duration: return ValueKind::duration;
    case ValueSchemaKind::string: return ValueKind::string;
    case ValueSchemaKind::color: return ValueKind::color;
    case ValueSchemaKind::image: return ValueKind::image;
    case ValueSchemaKind::key: return ValueKind::key;
    case ValueSchemaKind::theme_token: return ValueKind::theme_token;
    case ValueSchemaKind::list: return ValueKind::list;
    case ValueSchemaKind::object: return ValueKind::object;
    case ValueSchemaKind::any:
    case ValueSchemaKind::union_value:
        throw std::logic_error("non-scalar runtime schema has no single value kind");
    }
    throw std::logic_error("invalid runtime schema kind");
}

[[nodiscard]] bool nullable_accepts(const ValueSchema& schema, const Value& value, const bool nullable) {
    return (nullable && value.kind() == ValueKind::null_value) || schema.accepts(value);
}

} // namespace

ValueSchema::ValueSchema(const ValueSchemaKind kind) : kind_(kind) {}

ValueSchemaPtr ValueSchema::any() {
    static const ValueSchemaPtr instance(new ValueSchema(ValueSchemaKind::any));
    return instance;
}

ValueSchemaPtr ValueSchema::scalar(const ValueSchemaKind kind) {
    if (kind == ValueSchemaKind::any || kind == ValueSchemaKind::list ||
        kind == ValueSchemaKind::object || kind == ValueSchemaKind::union_value) {
        throw std::invalid_argument("scalar runtime schema kind is invalid");
    }
    return ValueSchemaPtr(new ValueSchema(kind));
}

ValueSchemaPtr ValueSchema::list(
    ValueSchemaPtr element,
    const bool element_nullable,
    const std::optional<std::size_t> maximum_items
) {
    if (element == nullptr) throw std::invalid_argument("list element schema must not be null");
    auto schema = std::unique_ptr<ValueSchema>(new ValueSchema(ValueSchemaKind::list));
    schema->element_ = std::move(element);
    schema->element_nullable_ = element_nullable;
    schema->maximum_items_ = maximum_items;
    return ValueSchemaPtr(std::move(schema));
}

ValueSchemaPtr ValueSchema::object(
    std::vector<ValueSchemaField> fields,
    const bool allow_unknown_fields,
    ValueSchemaPtr unknown_field_schema
) {
    for (const ValueSchemaField& field : fields) {
        if (field.name.empty() || !core::valid_utf8(field.name) || field.schema == nullptr) {
            throw std::invalid_argument("object field schemas require a valid name and type");
        }
    }
    std::ranges::sort(fields, {}, &ValueSchemaField::name);
    const auto duplicate = std::ranges::adjacent_find(fields, {}, &ValueSchemaField::name);
    if (duplicate != fields.end()) throw std::invalid_argument("object field schemas must be unique");
    if (allow_unknown_fields && unknown_field_schema == nullptr) {
        throw std::invalid_argument("open object schema requires an unknown-field schema");
    }
    auto schema = std::unique_ptr<ValueSchema>(new ValueSchema(ValueSchemaKind::object));
    schema->fields_ = std::move(fields);
    schema->allow_unknown_fields_ = allow_unknown_fields;
    schema->unknown_field_schema_ = std::move(unknown_field_schema);
    return ValueSchemaPtr(std::move(schema));
}

ValueSchemaPtr ValueSchema::union_of(std::vector<ValueSchemaPtr> options) {
    if (options.empty() || std::ranges::any_of(options, [](const ValueSchemaPtr& option) {
            return option == nullptr;
        })) {
        throw std::invalid_argument("union runtime schema requires non-null options");
    }
    auto schema = std::unique_ptr<ValueSchema>(new ValueSchema(ValueSchemaKind::union_value));
    schema->options_ = std::move(options);
    return ValueSchemaPtr(std::move(schema));
}

ValueSchemaKind ValueSchema::kind() const noexcept {
    return kind_;
}

bool ValueSchema::accepts(const Value& value) const {
    if (kind_ == ValueSchemaKind::any) return true;
    if (kind_ == ValueSchemaKind::union_value) {
        return std::ranges::any_of(options_, [&value](const ValueSchemaPtr& option) {
            return option->accepts(value);
        });
    }
    if (kind_ != ValueSchemaKind::list && kind_ != ValueSchemaKind::object) {
        return value.kind() == value_kind_for(kind_);
    }
    if (kind_ == ValueSchemaKind::list) {
        const ValueList* list_value = value.list();
        if (list_value == nullptr ||
            (maximum_items_.has_value() && list_value->values.size() > *maximum_items_)) {
            return false;
        }
        return std::ranges::all_of(list_value->values, [this](const Value& item) {
            return nullable_accepts(*element_, item, element_nullable_);
        });
    }

    const ValueObject* object_value = value.object();
    if (object_value == nullptr) return false;
    for (const ValueSchemaField& field_schema : fields_) {
        const Value* field_value = value.field(field_schema.name);
        if (field_value == nullptr) {
            if (field_schema.required) return false;
            continue;
        }
        if (!nullable_accepts(*field_schema.schema, *field_value, field_schema.nullable)) return false;
    }
    for (const auto& [name, field_value] : object_value->fields) {
        if (field(name) != nullptr) continue;
        if (!allow_unknown_fields_ || unknown_field_schema_ == nullptr ||
            !unknown_field_schema_->accepts(field_value)) {
            return false;
        }
    }
    return true;
}

std::optional<Value> ValueSchema::normalize(const Value& value) const {
    if (kind_ == ValueSchemaKind::any) return value;
    if (kind_ == ValueSchemaKind::union_value) {
        for (const ValueSchemaPtr& option : options_) {
            if (std::optional<Value> normalized = option->normalize(value);
                normalized.has_value()) {
                return normalized;
            }
        }
        return std::nullopt;
    }
    if (kind_ == ValueSchemaKind::key && value.string() != nullptr) {
        return Value(KeyValue{*value.string()});
    }
    if (kind_ == ValueSchemaKind::image && value.string() != nullptr) {
        return Value(ImageValue{*value.string()});
    }
    if (kind_ == ValueSchemaKind::theme_token && value.string() != nullptr) {
        return Value(ThemeTokenValue{*value.string()});
    }
    if (kind_ == ValueSchemaKind::duration && value.number() != nullptr &&
        std::isfinite(*value.number()) &&
        *value.number() >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
        *value.number() < 9'223'372'036'854'775'808.0 &&
        std::trunc(*value.number()) == *value.number()) {
        return Value(DurationValue{static_cast<std::int64_t>(*value.number())});
    }
    if (kind_ == ValueSchemaKind::color && value.list() != nullptr &&
        value.list()->values.size() == 4U) {
        std::array<std::uint8_t, 4U> channels{};
        for (std::size_t index = 0U; index < channels.size(); ++index) {
            const double* channel = value.list()->values[index].number();
            if (channel == nullptr || !std::isfinite(*channel) || *channel < 0.0 ||
                *channel > 255.0 || std::trunc(*channel) != *channel) {
                return std::nullopt;
            }
            channels[index] = static_cast<std::uint8_t>(*channel);
        }
        return Value(ColorValue{channels[0], channels[1], channels[2], channels[3]});
    }
    if (kind_ == ValueSchemaKind::list) {
        if (value.list() == nullptr ||
            (maximum_items_.has_value() && value.list()->values.size() > *maximum_items_)) {
            return std::nullopt;
        }
        std::vector<Value> normalized;
        normalized.reserve(value.list()->values.size());
        for (const Value& item : value.list()->values) {
            if (item.kind() == ValueKind::null_value && element_nullable_) {
                normalized.emplace_back();
                continue;
            }
            std::optional<Value> next = element_->normalize(item);
            if (!next.has_value()) return std::nullopt;
            normalized.push_back(std::move(*next));
        }
        return Value(std::move(normalized));
    }
    if (kind_ == ValueSchemaKind::object) {
        if (value.object() == nullptr) return std::nullopt;
        std::vector<std::pair<std::string, Value>> normalized;
        normalized.reserve(value.object()->fields.size());
        for (const ValueSchemaField& field_schema : fields_) {
            const Value* field_value = value.field(field_schema.name);
            if (field_value == nullptr) {
                if (field_schema.required) return std::nullopt;
                continue;
            }
            if (field_value->kind() == ValueKind::null_value && field_schema.nullable) {
                normalized.emplace_back(field_schema.name, Value{});
                continue;
            }
            std::optional<Value> next = field_schema.schema->normalize(*field_value);
            if (!next.has_value()) return std::nullopt;
            normalized.emplace_back(field_schema.name, std::move(*next));
        }
        for (const auto& [name, field_value] : value.object()->fields) {
            if (field(name) != nullptr) continue;
            if (!allow_unknown_fields_ || unknown_field_schema_ == nullptr) return std::nullopt;
            std::optional<Value> next = unknown_field_schema_->normalize(field_value);
            if (!next.has_value()) return std::nullopt;
            normalized.emplace_back(name, std::move(*next));
        }
        return Value(std::move(normalized));
    }
    return accepts(value) ? std::optional<Value>(value) : std::nullopt;
}

const ValueSchemaPtr& ValueSchema::element() const noexcept { return element_; }
bool ValueSchema::element_nullable() const noexcept { return element_nullable_; }
const std::optional<std::size_t>& ValueSchema::maximum_items() const noexcept { return maximum_items_; }
const std::vector<ValueSchemaField>& ValueSchema::fields() const noexcept { return fields_; }
bool ValueSchema::allow_unknown_fields() const noexcept { return allow_unknown_fields_; }
const ValueSchemaPtr& ValueSchema::unknown_field_schema() const noexcept { return unknown_field_schema_; }
const std::vector<ValueSchemaPtr>& ValueSchema::options() const noexcept { return options_; }

const ValueSchemaField* ValueSchema::field(const std::string_view name) const noexcept {
    const auto found = std::ranges::lower_bound(fields_, name, {}, &ValueSchemaField::name);
    return found != fields_.end() && found->name == name ? &*found : nullptr;
}

bool operator==(const ValueSchema& left, const ValueSchema& right) {
    if (left.kind_ != right.kind_ || left.element_nullable_ != right.element_nullable_ ||
        left.maximum_items_ != right.maximum_items_ ||
        left.allow_unknown_fields_ != right.allow_unknown_fields_ ||
        left.fields_.size() != right.fields_.size() || left.options_.size() != right.options_.size()) {
        return false;
    }
    const auto schema_equal = [](const ValueSchemaPtr& first, const ValueSchemaPtr& second) {
        return first == second || (first != nullptr && second != nullptr && *first == *second);
    };
    if (!schema_equal(left.element_, right.element_) ||
        !schema_equal(left.unknown_field_schema_, right.unknown_field_schema_)) {
        return false;
    }
    for (std::size_t index = 0U; index < left.fields_.size(); ++index) {
        const ValueSchemaField& first = left.fields_[index];
        const ValueSchemaField& second = right.fields_[index];
        if (first.name != second.name || first.required != second.required ||
            first.nullable != second.nullable || !schema_equal(first.schema, second.schema)) {
            return false;
        }
    }
    for (std::size_t index = 0U; index < left.options_.size(); ++index) {
        if (!schema_equal(left.options_[index], right.options_[index])) return false;
    }
    return true;
}

} // namespace strata::runtime
