#include "runtime/value.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "core/utf8.hpp"

namespace strata::runtime {
namespace {

void validate_text(const std::string_view value, const std::string_view label, const bool non_blank) {
    const core::Utf8Blankness blankness = core::utf8_blankness(value);
    if (blankness == core::Utf8Blankness::malformed) {
        throw std::invalid_argument(std::string(label) + " must be valid UTF-8");
    }
    if (non_blank && blankness == core::Utf8Blankness::blank) {
        throw std::invalid_argument(std::string(label) + " must not be blank");
    }
}

template <typename T>
[[nodiscard]] bool pointer_equal(
    const std::shared_ptr<const T>& left,
    const std::shared_ptr<const T>& right
) {
    return left == right || (left != nullptr && right != nullptr && *left == *right);
}

} // namespace

Value::Value() noexcept : storage_(NullValue{}) {}

Value::Value(NullValue value) noexcept : storage_(value) {}

Value::Value(const bool value) noexcept : storage_(value) {}

Value::Value(const double value) : storage_(value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("runtime number values must be finite");
    }
}

Value::Value(const DurationValue value) noexcept : storage_(value) {}

Value::Value(std::string value) : storage_(std::move(value)) {
    validate_text(*std::get_if<std::string>(&storage_), "runtime string", false);
}

Value::Value(const char* const value) : Value(std::string(value != nullptr ? value : "")) {
    if (value == nullptr) {
        throw std::invalid_argument("runtime string pointer must not be null");
    }
}

Value::Value(const ColorValue value) noexcept : storage_(value) {}

Value::Value(ImageValue value) : storage_(std::move(value)) {
    validate_text(std::get<ImageValue>(storage_).id, "image id", true);
}

Value::Value(KeyValue value) : storage_(std::move(value)) {
    validate_text(std::get<KeyValue>(storage_).value, "node key", true);
}

Value::Value(ThemeTokenValue value) : storage_(std::move(value)) {
    validate_text(std::get<ThemeTokenValue>(storage_).name, "theme token", true);
}

Value::Value(std::vector<Value> values)
    : storage_(std::make_shared<const ValueList>(ValueList{std::move(values)})) {}

Value::Value(std::vector<std::pair<std::string, Value>> fields) {
    for (const auto& [name, value] : fields) {
        static_cast<void>(value);
        validate_text(name, "runtime object field", true);
    }
    std::ranges::sort(fields, {}, &std::pair<std::string, Value>::first);
    const auto duplicate = std::ranges::adjacent_find(fields, {}, &std::pair<std::string, Value>::first);
    if (duplicate != fields.end()) {
        throw std::invalid_argument("runtime object fields must be unique");
    }
    storage_ = std::make_shared<const ValueObject>(ValueObject{std::move(fields)});
}

ValueKind Value::kind() const noexcept {
    return static_cast<ValueKind>(storage_.index());
}

const Value::Storage& Value::storage() const noexcept {
    return storage_;
}

const bool* Value::boolean() const noexcept {
    return std::get_if<bool>(&storage_);
}

const double* Value::number() const noexcept {
    return std::get_if<double>(&storage_);
}

const DurationValue* Value::duration() const noexcept {
    return std::get_if<DurationValue>(&storage_);
}

const std::string* Value::string() const noexcept {
    return std::get_if<std::string>(&storage_);
}

const ColorValue* Value::color() const noexcept {
    return std::get_if<ColorValue>(&storage_);
}

const ImageValue* Value::image() const noexcept {
    return std::get_if<ImageValue>(&storage_);
}

const KeyValue* Value::key() const noexcept {
    return std::get_if<KeyValue>(&storage_);
}

const ThemeTokenValue* Value::theme_token() const noexcept {
    return std::get_if<ThemeTokenValue>(&storage_);
}

const ValueList* Value::list() const noexcept {
    const auto* pointer = std::get_if<ListPtr>(&storage_);
    return pointer != nullptr ? pointer->get() : nullptr;
}

const ValueObject* Value::object() const noexcept {
    const auto* pointer = std::get_if<ObjectPtr>(&storage_);
    return pointer != nullptr ? pointer->get() : nullptr;
}

const void* Value::composite_identity() const noexcept {
    if (const auto* pointer = std::get_if<ListPtr>(&storage_)) return pointer->get();
    if (const auto* pointer = std::get_if<ObjectPtr>(&storage_)) return pointer->get();
    return nullptr;
}

const Value* Value::field(const std::string_view name) const noexcept {
    const ValueObject* value = object();
    if (value == nullptr) return nullptr;
    const auto found = std::ranges::lower_bound(
        value->fields,
        name,
        {},
        &std::pair<std::string, Value>::first
    );
    return found != value->fields.end() && found->first == name ? &found->second : nullptr;
}

std::string_view Value::state_type_id() const noexcept {
    switch (kind()) {
    case ValueKind::null_value: return "dsl.null";
    case ValueKind::boolean: return "dsl.boolean";
    case ValueKind::number: return "dsl.number";
    case ValueKind::duration: return "dsl.duration";
    case ValueKind::string: return "dsl.string";
    case ValueKind::color: return "dsl.color";
    case ValueKind::image: return "dsl.image";
    case ValueKind::key: return "dsl.key";
    case ValueKind::theme_token: return "dsl.theme-token";
    case ValueKind::list: return "dsl.list";
    case ValueKind::object: return "dsl.map";
    }
    return "dsl.unknown";
}

bool operator==(const Value& left, const Value& right) {
    if (left.storage_.index() != right.storage_.index()) return false;
    if (const auto* left_list = std::get_if<Value::ListPtr>(&left.storage_)) {
        return pointer_equal(*left_list, std::get<Value::ListPtr>(right.storage_));
    }
    if (const auto* left_object = std::get_if<Value::ObjectPtr>(&left.storage_)) {
        return pointer_equal(*left_object, std::get<Value::ObjectPtr>(right.storage_));
    }
    return left.storage_ == right.storage_;
}

Value value_from_json(const data::JsonValue& value) {
    if (value.is_null()) return Value{};
    if (const bool* boolean = value.boolean()) return Value(*boolean);
    if (const std::int64_t* integer = value.integer()) return Value(static_cast<double>(*integer));
    if (const double* number = value.number()) return Value(*number);
    if (const std::string* string = value.string()) return Value(*string);
    if (const data::JsonValue::Array* array = value.array()) {
        std::vector<Value> values;
        values.reserve(array->size());
        for (const data::JsonValue& item : *array) values.push_back(value_from_json(item));
        return Value(std::move(values));
    }
    const data::JsonValue::Object* object = value.object();
    if (object == nullptr) throw std::logic_error("JSON value has no runtime representation");
    std::vector<std::pair<std::string, Value>> fields;
    fields.reserve(object->size());
    for (const auto& [name, field] : *object) {
        fields.emplace_back(name, value_from_json(field));
    }
    return Value(std::move(fields));
}

data::JsonValue value_to_json(const Value& value) {
    switch (value.kind()) {
    case ValueKind::null_value: return data::JsonValue{};
    case ValueKind::boolean: return data::JsonValue(*value.boolean());
    case ValueKind::number: return data::JsonValue(*value.number());
    case ValueKind::duration:
        return data::JsonValue(value.duration()->nanoseconds);
    case ValueKind::string: return data::JsonValue(*value.string());
    case ValueKind::color: {
        const ColorValue& color = *value.color();
        data::JsonValue::Array channels;
        channels.emplace_back(static_cast<std::int64_t>(color.red));
        channels.emplace_back(static_cast<std::int64_t>(color.green));
        channels.emplace_back(static_cast<std::int64_t>(color.blue));
        channels.emplace_back(static_cast<std::int64_t>(color.alpha));
        return data::JsonValue(std::move(channels));
    }
    case ValueKind::image: return data::JsonValue(value.image()->id);
    case ValueKind::key: return data::JsonValue(value.key()->value);
    case ValueKind::theme_token: return data::JsonValue(value.theme_token()->name);
    case ValueKind::list: {
        data::JsonValue::Array array;
        array.reserve(value.list()->values.size());
        for (const Value& item : value.list()->values) array.push_back(value_to_json(item));
        return data::JsonValue(std::move(array));
    }
    case ValueKind::object: {
        data::JsonValue::Object object;
        object.reserve(value.object()->fields.size());
        for (const auto& [name, field] : value.object()->fields) {
            object.emplace_back(name, value_to_json(field));
        }
        return data::JsonValue(std::move(object));
    }
    }
    throw std::logic_error("invalid runtime value kind");
}

} // namespace strata::runtime
