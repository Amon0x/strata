#include "runtime/expression.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace strata::runtime {
namespace {

constexpr std::size_t maximum_derived_items = 100'000U;
using JsonValue = data::JsonView;
using JsonArray = data::JsonArrayView;
using JsonObject = data::JsonObjectView;

[[nodiscard]] std::string operator+(const char* const left, const std::string_view right) {
    std::string result(left);
    result.append(right);
    return result;
}

[[nodiscard]] JsonValue required(const JsonValue value, const std::string_view field) {
    const JsonValue found = value.find(field);
    if (!found)
        throw std::runtime_error("runtime IR is missing field '" + std::string(field) + "'");
    return found;
}

[[nodiscard]] std::string_view string_field(const JsonValue value, const std::string_view field) {
    const std::optional<std::string_view> text = required(value, field).string();
    if (!text.has_value())
        throw std::runtime_error("runtime IR field must be a string");
    return *text;
}

[[nodiscard]] JsonArray array_field(const JsonValue value, const std::string_view field) {
    const std::optional<JsonArray> array = required(value, field).array();
    if (!array.has_value())
        throw std::runtime_error("runtime IR field must be an array");
    return *array;
}

[[nodiscard]] JsonObject object_field(const JsonValue value, const std::string_view field) {
    const std::optional<JsonObject> object = required(value, field).object();
    if (!object.has_value())
        throw std::runtime_error("runtime IR field must be an object");
    return *object;
}

[[nodiscard]] double json_number(const JsonValue value) {
    if (const std::optional<double> number = value.number(); number.has_value())
        return *number;
    if (const std::optional<std::int64_t> integer = value.integer(); integer.has_value()) {
        return static_cast<double>(*integer);
    }
    throw std::runtime_error("runtime IR number is invalid");
}

[[nodiscard]] std::optional<std::size_t> bounded_index(const Value& value) {
    const double* number = value.number();
    if (number == nullptr || *number < 0.0 ||
        *number > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::trunc(*number));
}

[[nodiscard]] int hexadecimal(const char value) noexcept {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

[[nodiscard]] ColorValue parse_color(const std::string_view rgba) {
    if (rgba.size() != 8U)
        throw std::runtime_error("portable IR color must contain RGBA bytes");
    std::uint8_t channels[4]{};
    for (std::size_t index = 0U; index < 4U; ++index) {
        const int high = hexadecimal(rgba[index * 2U]);
        const int low = hexadecimal(rgba[index * 2U + 1U]);
        if (high < 0 || low < 0)
            throw std::runtime_error("portable IR color is not hexadecimal");
        channels[index] = static_cast<std::uint8_t>(high * 16 + low);
    }
    return ColorValue{channels[0], channels[1], channels[2], channels[3]};
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

struct CollectionDependencyTrace final : ExpressionDependencyObserver {
    explicit CollectionDependencyTrace(ExpressionDependencyObserver* source_parent)
        : parent(source_parent) {}

    void lexical(const std::string_view name, const ExpressionDependencyValue& value) override {
        if (parent != nullptr)
            parent->lexical(name, value);
        if (!value.cacheable()) {
            cacheable = false;
            return;
        }
        const auto [stored, inserted] = lexical_values.insert_or_assign(std::string(name), value);
        if (inserted)
            order.emplace_back(false, stored->first);
    }

    void host(const ExpressionHostDependency& dependency) override {
        if (parent != nullptr)
            parent->host(dependency);
        const auto [stored, inserted] = host_values.insert_or_assign(
            canonical_host_dependency_path(dependency.path), dependency);
        if (inserted)
            order.emplace_back(true, stored->first);
    }

    ExpressionDependencyObserver* parent;
    bool cacheable = true;
    std::map<std::string, ExpressionDependencyValue, std::less<>> lexical_values;
    std::map<std::string, ExpressionHostDependency, std::less<>> host_values;
    std::vector<std::pair<bool, std::string>> order;
};

/** Hides one helper's data-domain input while preserving outer/nested lexical dependencies. */
struct LambdaDependencyFilter final : ExpressionDependencyObserver {
    LambdaDependencyFilter(ExpressionDependencyObserver* source_parent,
                           std::string_view source_parameter)
        : parent(source_parent), parameter(source_parameter) {}

    void lexical(const std::string_view name, const ExpressionDependencyValue& value) override {
        if (parent != nullptr && name != parameter)
            parent->lexical(name, value);
    }

    void host(const ExpressionHostDependency& dependency) override {
        if (parent != nullptr)
            parent->host(dependency);
    }

    ExpressionDependencyObserver* parent;
    std::string_view parameter;
};

[[nodiscard]] std::pair<std::size_t, std::size_t> collection_counts(const ExpressionValue& value) {
    if (const auto* collection = value.collection())
        return {(*collection)->total, (*collection)->matched};
    const ValueList* list = value.value() != nullptr ? value.value()->list() : nullptr;
    return list != nullptr
               ? std::pair<std::size_t, std::size_t>{list->values.size(), list->values.size()}
               : std::pair<std::size_t, std::size_t>{0U, 0U};
}

[[nodiscard]] std::optional<ActionOrigin> action_origin(const JsonValue expression,
                                                        const ExpressionScope& scope) {
    const JsonValue span = expression.find("span");
    const JsonValue start = span.find("start");
    const JsonValue end = span.find("end");
    const std::optional<std::string_view> source_id = span.find("sourceId").string();
    if (!source_id.has_value() || !start || !end)
        return std::nullopt;
    const auto position = [](const JsonValue value, const std::string_view field) {
        const std::optional<std::int64_t> number = value.find(field).integer();
        return number.has_value() && *number > 0 && *number <= static_cast<std::int64_t>(UINT32_MAX)
                   ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(*number))
                   : std::nullopt;
    };
    std::optional<std::string> component_path;
    if (scope.component_path.starts_with("/component/")) {
        component_path = scope.component_path;
    } else if (const std::optional<std::string_view> path = expression.find("path").string();
               path.has_value()) {
        const std::size_t arguments = path->find("/arguments/");
        component_path = arguments == std::string_view::npos
                             ? std::string(*path)
                             : std::string(path->substr(0U, arguments));
    } else if (!scope.component_path.empty()) {
        component_path = scope.component_path;
    }
    constexpr std::string_view instance_root_suffix = "/root";
    if (component_path.has_value() && component_path->starts_with("/component/") &&
        component_path->ends_with(instance_root_suffix)) {
        component_path->erase(component_path->size() - instance_root_suffix.size());
    }
    return ActionOrigin{
        std::string(*source_id), position(start, "line"), position(start, "column"),
        position(end, "line"),   position(end, "column"), std::move(component_path),
    };
}

} // namespace

std::optional<DiagnosticRange> portable_expression_range(const JsonValue expression) {
    const JsonValue span = expression.find("span");
    const std::optional<std::string_view> source_id = span.find("sourceId").string();
    const JsonValue start = span.find("start");
    const JsonValue end = span.find("end");
    if (!source_id.has_value() || !start || !end)
        return std::nullopt;
    const auto position = [](const JsonValue value) -> std::optional<DiagnosticPosition> {
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
      lexical_state_binding_(std::move(state_binding)) {}
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
const Value* ExpressionValue::value() const noexcept {
    if (const Value* scalar = std::get_if<Value>(&storage_))
        return scalar;
    if (const auto* list_value = list())
        return &(**list_value).materialized;
    if (const auto* object_value = object())
        return &(**object_value).materialized;
    if (const auto* component_value = component_template())
        return &(**component_value).materialized;
    return nullptr;
}
const Value* ExpressionValue::data_value() const noexcept {
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
const std::optional<LexicalStateBinding>&
ExpressionValue::lexical_state_binding() const noexcept {
    return lexical_state_binding_;
}

bool ExpressionDependencyValue::cacheable() const noexcept {
    if (kind == ExpressionDependencyValueKind::unsupported)
        return false;
    if (kind == ExpressionDependencyValueKind::executable_list) {
        return std::ranges::all_of(elements, &ExpressionDependencyValue::cacheable);
    }
    if (kind == ExpressionDependencyValueKind::executable_object) {
        return field_names.size() == field_values.size() &&
               std::ranges::all_of(field_values, &ExpressionDependencyValue::cacheable);
    }
    if (kind == ExpressionDependencyValueKind::component_template) {
        return field_names.size() == field_values.size() &&
               std::ranges::all_of(field_values, &ExpressionDependencyValue::cacheable);
    }
    return true;
}

ExpressionDependencyValue capture_expression_dependency(const ExpressionValue& value) {
    ExpressionDependencyValue result;
    if (const auto* collection = value.collection()) {
        result.kind = ExpressionDependencyValueKind::collection;
        result.collection = ExpressionCollectionDependencyValue{
            collection_view_immutable_identity(**collection),
            (*collection)->cache_hits.load(std::memory_order_relaxed),
        };
        return result;
    }
    if (const auto* list = value.list()) {
        result.kind = ExpressionDependencyValueKind::executable_list;
        result.elements.reserve((**list).values.size());
        for (const ExpressionValue& element : (**list).values) {
            result.elements.push_back(capture_expression_dependency(element));
        }
        return result;
    }
    if (const auto* object = value.object()) {
        result.kind = ExpressionDependencyValueKind::executable_object;
        result.field_names.reserve((**object).fields.size());
        result.field_values.reserve((**object).fields.size());
        for (const auto& [name, field] : (**object).fields) {
            result.field_names.push_back(name);
            result.field_values.push_back(capture_expression_dependency(field));
        }
        return result;
    }
    if (const auto* component = value.component_template()) {
        result.kind = ExpressionDependencyValueKind::component_template;
        result.component = (**component).component;
        result.field_names.reserve((**component).arguments.size());
        result.field_values.reserve((**component).arguments.size());
        for (const auto& [name, argument] : (**component).arguments) {
            result.field_names.push_back(name);
            result.field_values.push_back(capture_expression_dependency(argument));
        }
        return result;
    }
    if (const Value* scalar = value.value()) {
        result.kind = ExpressionDependencyValueKind::scalar;
        result.scalar = *scalar;
    }
    return result;
}

ExpressionValue restore_expression_dependency(const ExpressionDependencyValue& value) {
    switch (value.kind) {
    case ExpressionDependencyValueKind::scalar:
        return ExpressionValue(value.scalar);
    case ExpressionDependencyValueKind::collection: {
        auto restored = std::shared_ptr<CollectionViewValue>(new CollectionViewValue{
            collection_view_immutable_identity(value.collection),
        });
        restored->cache_hits.store(value.collection.cache_hits, std::memory_order_relaxed);
        return ExpressionValue(std::shared_ptr<const CollectionViewValue>(std::move(restored)));
    }
    case ExpressionDependencyValueKind::executable_list: {
        std::vector<ExpressionValue> executable;
        std::vector<Value> materialized;
        executable.reserve(value.elements.size());
        materialized.reserve(value.elements.size());
        for (const ExpressionDependencyValue& element : value.elements) {
            ExpressionValue restored = restore_expression_dependency(element);
            materialized.push_back(materialized_value(restored));
            executable.push_back(std::move(restored));
        }
        return ExpressionValue(std::make_shared<const ExpressionListValue>(ExpressionListValue{
            Value(std::move(materialized)),
            std::move(executable),
        }));
    }
    case ExpressionDependencyValueKind::executable_object: {
        std::vector<std::pair<std::string, ExpressionValue>> executable;
        std::vector<std::pair<std::string, Value>> materialized;
        if (value.field_names.size() != value.field_values.size()) {
            throw std::logic_error("executable object dependency fields are inconsistent");
        }
        executable.reserve(value.field_values.size());
        materialized.reserve(value.field_values.size());
        for (std::size_t index = 0U; index < value.field_values.size(); ++index) {
            ExpressionValue restored = restore_expression_dependency(value.field_values[index]);
            materialized.emplace_back(value.field_names[index], materialized_value(restored));
            executable.emplace_back(value.field_names[index], std::move(restored));
        }
        return ExpressionValue(std::make_shared<const ExpressionObjectValue>(ExpressionObjectValue{
            Value(std::move(materialized)),
            std::move(executable),
        }));
    }
    case ExpressionDependencyValueKind::component_template: {
        if (value.field_names.size() != value.field_values.size()) {
            throw std::logic_error("component-template dependency arguments are inconsistent");
        }
        std::map<std::string, ExpressionValue, std::less<>> arguments;
        for (std::size_t index = 0U; index < value.field_values.size(); ++index) {
            arguments.insert_or_assign(
                value.field_names[index],
                restore_expression_dependency(value.field_values[index])
            );
        }
        return ExpressionValue(std::make_shared<const ComponentTemplateValue>(
            ComponentTemplateValue{
                value.component,
                std::move(arguments),
                Value(value.component),
            }
        ));
    }
    case ExpressionDependencyValueKind::unsupported:
        throw std::logic_error("unsupported expression dependency cannot be restored");
    }
    throw std::logic_error("unknown expression dependency kind");
}

std::optional<ExpressionDependencyValue> expression_scope_dependency(const ExpressionScope& scope,
                                                                     const std::string_view name) {
    if (const auto frozen = scope.lexical_dependency_overrides.find(name);
        frozen != scope.lexical_dependency_overrides.end()) {
        return frozen->second;
    }
    if (const auto executable = scope.executable_values.find(name);
        executable != scope.executable_values.end()) {
        return capture_expression_dependency(executable->second);
    }
    if (const auto scalar = scope.values.find(name); scalar != scope.values.end()) {
        ExpressionDependencyValue result;
        result.kind = ExpressionDependencyValueKind::scalar;
        result.scalar = scalar->second;
        return result;
    }
    return std::nullopt;
}

const ExpressionValue* ExpressionObjectValue::field(const std::string_view name) const noexcept {
    const auto found = std::ranges::find(fields, name, &decltype(fields)::value_type::first);
    return found != fields.end() ? &found->second : nullptr;
}

ExpressionRuntime::ExpressionRuntime(const HostStore& host, const RuntimeActionRegistry& actions,
                                     ExpressionScope scope)
    : host_(host), actions_(actions), scope_(std::move(scope)) {}

std::string canonical_host_dependency_path(const std::span<const HostPathSegment> path) {
    std::string result;
    for (const HostPathSegment& segment : path) {
        result.push_back(segment.kind == HostPathSegmentKind::field ? 'f' : 'l');
        result.append(std::to_string(segment.field.size()));
        result.push_back(':');
        result.append(segment.field);
        if (segment.kind == HostPathSegmentKind::lookup) {
            result.push_back('#');
            result.append(segment.index.has_value() ? std::to_string(*segment.index) : "-");
        }
        result.push_back('/');
    }
    return result;
}

ExpressionValue ExpressionRuntime::evaluate(const JsonValue expression) {
    return evaluate(expression, scope_);
}

ExpressionValue ExpressionRuntime::evaluate_in(const JsonValue expression,
                                               const ExpressionScope& scope) {
    return evaluate(expression, scope);
}

const std::vector<RuntimeDiagnostic>& ExpressionRuntime::diagnostics() const noexcept {
    return diagnostics_;
}

void ExpressionRuntime::clear_diagnostics() {
    diagnostics_.clear();
}

ExpressionDependencyObserver* ExpressionRuntime::exchange_dependency_observer(
    ExpressionDependencyObserver* const observer) noexcept {
    return std::exchange(dependency_observer_, observer);
}

ExpressionHostDependency
ExpressionRuntime::read_host_dependency(const std::span<const HostPathSegment> path,
                                        const ExpressionScope& scope) const {
    if (path.empty() || path.front().kind != HostPathSegmentKind::field ||
        path.front().field.empty()) {
        throw std::invalid_argument("host dependency path requires a named root");
    }
    const std::string canonical = canonical_host_dependency_path(path);
    if (const auto frozen = scope.host_dependency_overrides.find(canonical);
        frozen != scope.host_dependency_overrides.end()) {
        return frozen->second;
    }

    const auto contextual = scope.contextual_host_roots.find(path.front().field);
    if (contextual == scope.contextual_host_roots.end()) {
        std::optional<HostResolution> resolution = host_.resolve_with_origin(path);
        return ExpressionHostDependency{
            std::vector<HostPathSegment>(path.begin(), path.end()),
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
        std::vector<HostPathSegment>(path.begin(), path.end()),
        true,
        current != nullptr ? std::optional<Value>(*current) : std::nullopt,
        std::nullopt,
        0U,
    };
}

ExpressionValue ExpressionRuntime::evaluate(const JsonValue expression,
                                            const ExpressionScope& scope) {
    struct ActiveScopeRestore final {
        const ExpressionScope*& slot;
        const ExpressionScope* previous;
        ~ActiveScopeRestore() {
            slot = previous;
        }
    } restore{active_scope_, active_scope_};
    active_scope_ = &scope;
    const std::string_view kind = string_field(expression, "kind");
    if (kind == "literal")
        return evaluate_literal(expression);
    if (kind == "variable" || kind == "property" || kind == "index") {
        if (std::optional<HostAccess> access = host_access(expression, scope); access.has_value()) {
            const ExpressionHostDependency dependency = read_host_dependency(access->path, scope);
            bool computed_property = false;
            if (!dependency.value.has_value() && kind == "property") {
                const std::string_view name = string_field(expression, "name");
                if (std::optional<HostAccess> receiver =
                        host_access(required(expression, "receiver"), scope);
                    receiver.has_value()) {
                    const ExpressionHostDependency receiver_dependency =
                        read_host_dependency(receiver->path, scope);
                    const Value* value = receiver_dependency.value.has_value()
                                             ? &*receiver_dependency.value
                                             : nullptr;
                    computed_property =
                        (value != nullptr && value->list() != nullptr &&
                         (name == "size" || name == "length" || name == "isEmpty")) ||
                        (value != nullptr && value->string() != nullptr &&
                         (name == "length" || name == "isEmpty"));
                }
            }
            if (!computed_property) {
                if (dependency_observer_ != nullptr)
                    dependency_observer_->host(dependency);
                if (dependency.value.has_value())
                    return ExpressionValue(*dependency.value);
                const bool root = access->path.size() == 1U;
                report(expression,
                       root ? "STRATA.DSL.RUNTIME_MISSING_HOST_ROOT"
                            : "STRATA.DSL.RUNTIME_MISSING_PROPERTY",
                       root ? "Host binding '" + access->path.front().field + "' is not available."
                            : "Host path selected by the expression is not available.",
                       root ? std::optional<std::string>("host adapter root") : std::nullopt);
                return ExpressionValue{};
            }
        }
    }
    if (kind == "variable") {
        const std::string_view name = string_field(expression, "name");
        const auto frozen = scope.lexical_dependency_overrides.find(name);
        if (frozen != scope.lexical_dependency_overrides.end()) {
            if (dependency_observer_ != nullptr) {
                dependency_observer_->lexical(name, frozen->second);
            }
            return restore_expression_dependency(frozen->second);
        }
        const auto executable = scope.executable_values.find(name);
        if (executable != scope.executable_values.end()) {
            if (dependency_observer_ != nullptr) {
                dependency_observer_->lexical(name,
                                              capture_expression_dependency(executable->second));
            }
            return executable->second;
        }
        const auto local = scope.values.find(name);
        if (local != scope.values.end()) {
            const JsonValue state_binding_value = expression.find("stateBinding");
            const std::optional<bool> preserve_state_binding =
                state_binding_value ? state_binding_value.boolean() : std::optional<bool>{};
            const auto state_binding =
                preserve_state_binding.value_or(false)
                    ? scope.state_bindings.find(name)
                    : scope.state_bindings.end();
            const ExpressionValue value =
                state_binding != scope.state_bindings.end()
                    ? ExpressionValue(local->second, state_binding->second)
                    : ExpressionValue(local->second);
            if (dependency_observer_ != nullptr) {
                dependency_observer_->lexical(name, capture_expression_dependency(value));
            }
            return value;
        }
        const std::string_view binding = string_field(expression, "binding");
        if (binding == "host") {
            throw std::logic_error("host variable bypassed structural host resolution");
        } else if (binding == "style" || binding == "animation" || binding == "component") {
            return ExpressionValue(Value(std::string(name)));
        } else {
            report(expression, "STRATA.DSL.RUNTIME_MISSING_BINDING",
                   "Binding '" + name + "' is not available.",
                   "local state, component parameter, or host root");
        }
        return ExpressionValue{};
    }
    if (kind == "list") {
        std::vector<ExpressionValue> executable;
        std::vector<Value> materialized;
        for (const JsonValue element : array_field(expression, "elements")) {
            ExpressionValue value = evaluate(element, scope);
            materialized.push_back(materialized_value(value));
            executable.push_back(std::move(value));
        }
        return ExpressionValue(std::make_shared<const ExpressionListValue>(ExpressionListValue{
            Value(std::move(materialized)),
            std::move(executable),
        }));
    }
    if (kind == "map") {
        std::vector<std::pair<std::string, ExpressionValue>> executable;
        std::vector<std::pair<std::string, Value>> materialized;
        for (const auto& [name, child] : object_field(expression, "entries")) {
            ExpressionValue value = evaluate(child, scope);
            materialized.emplace_back(std::string(name), materialized_value(value));
            executable.emplace_back(std::string(name), std::move(value));
        }
        return ExpressionValue(std::make_shared<const ExpressionObjectValue>(ExpressionObjectValue{
            Value(std::move(materialized)),
            std::move(executable),
        }));
    }
    if (kind == "componentTemplate") {
        std::map<std::string, ExpressionValue, std::less<>> arguments;
        for (const JsonValue argument : array_field(expression, "arguments")) {
            arguments.insert_or_assign(
                std::string(string_field(argument, "name")),
                evaluate(required(argument, "value"), scope)
            );
        }
        return ExpressionValue(std::make_shared<const ComponentTemplateValue>(
            ComponentTemplateValue{
                std::string(string_field(expression, "component")),
                std::move(arguments),
                Value(std::string(string_field(expression, "component"))),
            }
        ));
    }
    if (kind == "lambda") {
        return ExpressionValue(std::make_shared<const LambdaValue>(LambdaValue{
            std::string(string_field(expression, "parameter")),
            required(expression, "body"),
            scope.values,
            scope.executable_values,
            scope.contextual_host_roots,
            scope.host_dependency_overrides,
            scope.lexical_dependency_overrides,
            scope.component_path,
        }));
    }
    if (kind == "action")
        return evaluate_action(expression, scope);
    if (kind == "helper")
        return evaluate_helper(expression, scope);
    if (kind == "property") {
        const ExpressionValue receiver = evaluate(required(expression, "receiver"), scope);
        const std::string_view name = string_field(expression, "name");
        if (const auto* object_value = receiver.object()) {
            if (const ExpressionValue* field = (**object_value).field(name); field != nullptr) {
                return *field;
            }
        }
        if (const auto* list_value = receiver.list()) {
            if (name == "size" || name == "length") {
                return ExpressionValue(Value(static_cast<double>((**list_value).values.size())));
            }
            if (name == "isEmpty")
                return ExpressionValue(Value((**list_value).values.empty()));
        }
        if (const Value* value = receiver.value()) {
            if (const Value* field = value->field(name))
                return ExpressionValue(*field);
            if (const ValueList* list = value->list()) {
                if (name == "size" || name == "length")
                    return ExpressionValue(Value(static_cast<double>(list->values.size())));
                if (name == "isEmpty")
                    return ExpressionValue(Value(list->values.empty()));
            }
            if (const std::string* text = value->string()) {
                if (name == "length")
                    return ExpressionValue(Value(static_cast<double>(utf16_length(*text))));
                if (name == "isEmpty")
                    return ExpressionValue(Value(text->empty()));
            }
        } else if (const auto* view_pointer = receiver.collection()) {
            const CollectionViewValue& view = **view_pointer;
            if (name == "items")
                return ExpressionValue(view.items);
            if (name == "size" || name == "length" || name == "matched")
                return ExpressionValue(Value(static_cast<double>(view.matched)));
            if (name == "total")
                return ExpressionValue(Value(static_cast<double>(view.total)));
            if (name == "rangeStart")
                return ExpressionValue(Value(static_cast<double>(view.range_start)));
            if (name == "rangeEnd")
                return ExpressionValue(Value(static_cast<double>(view.range_end_exclusive)));
            if (name == "isEmpty")
                return ExpressionValue(Value(view.matched == 0U));
            if (name == "cacheHits")
                return ExpressionValue(
                    Value(static_cast<double>(view.cache_hits.load(std::memory_order_relaxed))));
            if (name == "rebuilds")
                return ExpressionValue(Value(static_cast<double>(view.rebuilds)));
        }
        report(expression, "STRATA.DSL.RUNTIME_MISSING_PROPERTY",
               "Property '" + name + "' is not available on this value.");
        return ExpressionValue{};
    }
    if (kind == "index") {
        const ExpressionValue receiver = evaluate(required(expression, "receiver"), scope);
        const Value index =
            require_value(evaluate(required(expression, "index"), scope), expression);
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
        const Value scalar_receiver = require_value(receiver, expression);
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
    if (kind == "conditional") {
        const Value condition =
            require_value(evaluate(required(expression, "condition"), scope), expression);
        return evaluate(required(expression, truthy(condition) ? "then" : "else"), scope);
    }
    if (kind == "unary") {
        const Value operand =
            require_value(evaluate(required(expression, "operand"), scope), expression);
        const std::string_view operation = string_field(expression, "operator");
        if (operation == "not")
            return ExpressionValue(Value(!truthy(operand)));
        if (operation == "negate") {
            if (operand.number() != nullptr)
                return ExpressionValue(Value(-*operand.number()));
            if (operand.duration() != nullptr)
                return ExpressionValue(Value(DurationValue{-operand.duration()->nanoseconds}));
        }
        report(expression, "STRATA.DSL.RUNTIME_TYPE_MISMATCH",
               "Unary operator received an incompatible value.");
        return ExpressionValue(Value(0.0));
    }
    if (kind == "group")
        return evaluate(required(expression, "expression"), scope);
    if (kind == "binary") {
        const std::string_view operation = string_field(expression, "operator");
        const Value left = require_value(evaluate(required(expression, "left"), scope), expression);
        if (operation == "and" && !truthy(left))
            return ExpressionValue(Value(false));
        if (operation == "or" && truthy(left))
            return ExpressionValue(Value(true));
        if (operation == "coalesce" && left.kind() != ValueKind::null_value)
            return ExpressionValue(left);
        const Value right =
            require_value(evaluate(required(expression, "right"), scope), expression);
        if (operation == "and" || operation == "or")
            return ExpressionValue(Value(truthy(right)));
        if (operation == "coalesce")
            return ExpressionValue(right);
        if (operation == "equal")
            return ExpressionValue(Value(left == right));
        if (operation == "not_equal")
            return ExpressionValue(Value(!(left == right)));
        if (operation == "add" && (left.string() != nullptr || right.string() != nullptr)) {
            return ExpressionValue(Value(display_string(left) + display_string(right)));
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
        if ((operation == "less" || operation == "less_equal" || operation == "greater" ||
             operation == "greater_equal") &&
            numeric) {
            return ExpressionValue(Value(operation == "less"         ? left_numeric < right_numeric
                                         : operation == "less_equal" ? left_numeric <= right_numeric
                                         : operation == "greater" ? left_numeric > right_numeric
                                                                  : left_numeric >= right_numeric));
        }
        if (numeric) {
            double result = 0.0;
            if (operation == "add")
                result = left_numeric + right_numeric;
            else if (operation == "subtract")
                result = left_numeric - right_numeric;
            else if (operation == "multiply")
                result = left_numeric * right_numeric;
            else if (operation == "divide")
                result = right_numeric == 0.0 ? 0.0 : left_numeric / right_numeric;
            else if (operation == "modulo")
                result = right_numeric == 0.0 ? 0.0 : std::fmod(left_numeric, right_numeric);
            else {
                report(expression, "STRATA.DSL.RUNTIME_UNKNOWN_OPERATOR",
                       "Binary operator '" + operation + "' is not supported.");
                return ExpressionValue{};
            }
            if (!std::isfinite(result))
                result = 0.0;
            return left.duration() != nullptr || right.duration() != nullptr
                       ? ExpressionValue(Value(DurationValue{static_cast<std::int64_t>(result)}))
                       : ExpressionValue(Value(result));
        }
        report(expression, "STRATA.DSL.RUNTIME_TYPE_MISMATCH",
               "Binary operator received incompatible values.");
        return ExpressionValue(Value(0.0));
    }
    if (kind == "error")
        return ExpressionValue{};
    if (kind == "materialReference" || kind == "materialCall") {
        std::vector<std::pair<std::string, Value>> material{
            {"id", Value(std::string(string_field(expression, "id")))},
        };
        if (kind == "materialCall") {
            std::vector<std::pair<std::string, Value>> parameters;
            for (const JsonValue parameter : array_field(expression, "parameters")) {
                parameters.emplace_back(
                    std::string(string_field(parameter, "name")),
                    require_value(evaluate(required(parameter, "value"), scope), parameter));
            }
            material.emplace_back("parameters", Value(std::move(parameters)));
        }
        return ExpressionValue(Value(std::move(material)));
    }
    report(expression, "STRATA.DSL.RUNTIME_UNKNOWN_EXPRESSION",
           "Portable expression kind '" + kind + "' is not supported.");
    return ExpressionValue{};
}

ExpressionValue ExpressionRuntime::evaluate_literal(const JsonValue expression) {
    const JsonValue literal = required(expression, "value");
    const std::string_view kind = string_field(literal, "kind");
    if (kind == "null")
        return ExpressionValue{};
    if (kind == "boolean")
        return ExpressionValue(Value(*required(literal, "value").boolean()));
    if (kind == "number")
        return ExpressionValue(Value(json_number(required(literal, "value"))));
    if (kind == "duration")
        return ExpressionValue(Value(DurationValue{*required(literal, "nanos").integer()}));
    if (kind == "string")
        return ExpressionValue(Value(std::string(string_field(literal, "value"))));
    if (kind == "image")
        return ExpressionValue(Value(ImageValue{std::string(string_field(literal, "value"))}));
    if (kind == "key")
        return ExpressionValue(Value(KeyValue{std::string(string_field(literal, "value"))}));
    if (kind == "color")
        return ExpressionValue(Value(parse_color(string_field(literal, "rgba"))));
    if (kind == "themeToken")
        return ExpressionValue(Value(ThemeTokenValue{std::string(string_field(literal, "name"))}));
    if (kind == "styleReference" || kind == "animation") {
        return ExpressionValue(Value(std::string(string_field(literal, "name"))));
    }
    report(expression, "STRATA.DSL.RUNTIME_UNKNOWN_LITERAL",
           "Portable literal kind '" + kind + "' is not supported.");
    return ExpressionValue{};
}

ExpressionValue ExpressionRuntime::evaluate_action(const JsonValue expression,
                                                   const ExpressionScope& scope) {
    const std::string_view id = string_field(expression, "id");
    const auto contract = actions_.contract(id);
    if (contract == nullptr) {
        report(expression, "STRATA.DSL.RUNTIME_UNKNOWN_ACTION",
               "Action '" + id + "' is not registered.");
        return ExpressionValue{};
    }
    Value payload;
    if (contract->payload_contract != "no payload") {
        std::vector<std::pair<std::string, Value>> fields;
        for (const JsonValue argument_value : array_field(expression, "arguments")) {
            fields.emplace_back(
                std::string(string_field(argument_value, "name")),
                require_value(evaluate(required(argument_value, "value"), scope), argument_value));
        }
        payload = Value(std::move(fields));
    }
    const std::optional<ActionOrigin> origin = action_origin(expression, scope);
    try {
        payload = actions_.decode_payload(id, std::move(payload));
        auto action = std::make_shared<const Action>(contract, std::move(payload), origin);
        std::optional<LexicalStateBinding> state_binding;
        if (id.starts_with("state.")) {
            const Value* state_name_value = action->payload.field("name");
            const std::string* state_name =
                state_name_value != nullptr ? state_name_value->string() : nullptr;
            if (state_name != nullptr) {
                const auto binding = scope.state_bindings.find(*state_name);
                if (binding != scope.state_bindings.end())
                    state_binding = binding->second;
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
            const DiagnosticPosition end = origin->end_line.has_value()
                ? DiagnosticPosition{
                      *origin->end_line, origin->end_column.value_or(1U), std::nullopt,
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

ExpressionValue ExpressionRuntime::evaluate_helper(const JsonValue expression,
                                                   const ExpressionScope& scope) {
    const std::string_view name = string_field(expression, "name");
    if (name == "filter" || name == "map" || name == "sortBy" || name == "distinctBy" ||
        name == "groupBy" || name == "flatten" || name == "takeWhile" || name == "window" ||
        name == "page") {
        return ExpressionValue(collection_view(expression, scope));
    }
    if (name == "persisted") {
        const JsonValue initial = argument_expression(expression, "initial", 1U);
        return initial ? evaluate(initial, scope) : ExpressionValue{};
    }
    if (name == "sequence")
        return ExpressionValue(composed_action(expression, scope, ActionCompositionMode::sequence));
    if (name == "parallel")
        return ExpressionValue(composed_action(expression, scope, ActionCompositionMode::parallel));
    if (name == "choose" || name == "chooseAction") {
        const bool condition = truthy(argument(expression, scope, "condition", 0U));
        const JsonValue selected = argument_expression(
            expression, condition ? "whenTrue" : "whenFalse", condition ? 1U : 2U);
        return selected ? evaluate(selected, scope) : ExpressionValue{};
    }
    if (name == "count" || name == "any" || name == "all") {
        const JsonValue source_expression = argument_expression(expression, "source", 0U);
        if (!source_expression)
            return ExpressionValue(name == "count" ? Value(0.0) : Value(false));
        const ExpressionValue source = evaluate(source_expression, scope);
        const ValueList* items = collection_items(source);
        if (items == nullptr) {
            report(expression, "STRATA.DSL.RUNTIME_COLLECTION_SOURCE",
                   "Collection aggregate requires a list or derived view.");
            return ExpressionValue(name == "count" ? Value(0.0) : Value(false));
        }
        const JsonValue predicate_expression = argument_expression(expression, "predicate", 1U);
        const auto predicate_value =
            predicate_expression ? evaluate(predicate_expression, scope) : ExpressionValue{};
        const auto* predicate = predicate_value.lambda();
        std::size_t matches = 0U;
        for (const Value& item : items->values) {
            if (predicate == nullptr || truthy(evaluate_lambda(**predicate, item)))
                ++matches;
            if (name == "any" && matches != 0U)
                break;
        }
        if (name == "count")
            return ExpressionValue(Value(static_cast<double>(matches)));
        if (name == "any")
            return ExpressionValue(Value(matches != 0U));
        return ExpressionValue(Value(!items->values.empty() && matches == items->values.size()));
    }
    if (name == "min" || name == "max") {
        double result = 0.0;
        bool first = true;
        for (const JsonValue entry : array_field(expression, "arguments")) {
            const Value value = require_value(evaluate(required(entry, "value"), scope), entry);
            const double number = value.number() != nullptr ? *value.number() : 0.0;
            result = first           ? number
                     : name == "min" ? std::min(result, number)
                                     : std::max(result, number);
            first = false;
        }
        return ExpressionValue(Value(result));
    }
    if (name == "clamp") {
        const Value value_argument = argument(expression, scope, "value", 0U);
        const Value minimum_argument = argument(expression, scope, "min", 1U);
        const Value maximum_argument = argument(expression, scope, "max", 2U);
        const double value = value_argument.number() != nullptr ? *value_argument.number() : 0.0;
        const double minimum =
            minimum_argument.number() != nullptr ? *minimum_argument.number() : value;
        const double maximum =
            maximum_argument.number() != nullptr ? *maximum_argument.number() : value;
        if (minimum > maximum) {
            report(expression, "STRATA.DSL.RUNTIME_INVALID_RANGE",
                   "clamp minimum exceeds its maximum.");
            return ExpressionValue(Value(value));
        }
        return ExpressionValue(Value(std::clamp(value, minimum, maximum)));
    }
    if (name == "abs" || name == "floor" || name == "ceil" || name == "round") {
        const Value value = argument(expression, scope, "value", 0U);
        const double number = value.number() != nullptr ? *value.number() : 0.0;
        if (name == "round") {
            const Value precision_value = argument(expression, scope, "precision", 1U);
            const int precision =
                precision_value.number() != nullptr
                    ? std::clamp(static_cast<int>(*precision_value.number()), 0, 12)
                    : 0;
            const double scale = std::pow(10.0, static_cast<double>(precision));
            return ExpressionValue(Value(std::round(number * scale) / scale));
        }
        return ExpressionValue(Value(name == "abs"     ? std::abs(number)
                                     : name == "floor" ? std::floor(number)
                                                       : std::ceil(number)));
    }
    if (name == "length" || name == "size" || name == "isEmpty") {
        const Value value = argument(expression, scope, "value", 0U);
        std::size_t size = value.string() != nullptr   ? utf16_length(*value.string())
                           : value.list() != nullptr   ? value.list()->values.size()
                           : value.object() != nullptr ? value.object()->fields.size()
                                                       : 0U;
        return ExpressionValue(name == "isEmpty" ? Value(size == 0U)
                                                 : Value(static_cast<double>(size)));
    }
    if (name == "join") {
        const Value values = argument(expression, scope, "value", 0U);
        const Value separator_value = argument(expression, scope, "separator", 1U);
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
    if (name == "lower" || name == "upper" || name == "trim" || name == "title") {
        std::string value = display_string(argument(expression, scope, "value", 0U));
        if (name == "lower")
            value = lower_ascii(std::move(value));
        else if (name == "upper")
            value = upper_ascii(std::move(value));
        else if (name == "trim") {
            const std::size_t first = value.find_first_not_of(" \t\r\n");
            const std::size_t last = value.find_last_not_of(" \t\r\n");
            value =
                first == std::string::npos ? std::string{} : value.substr(first, last - first + 1U);
        } else {
            std::istringstream words(value);
            std::string titled;
            for (std::string word; words >> word;) {
                word = lower_ascii(std::move(word));
                if (!word.empty() && word.front() >= 'a' && word.front() <= 'z') {
                    word.front() = static_cast<char>(word.front() - 'a' + 'A');
                }
                if (!titled.empty())
                    titled.push_back(' ');
                titled += word;
            }
            value = std::move(titled);
        }
        return ExpressionValue(Value(std::move(value)));
    }
    if (name == "contains") {
        const Value value = argument(expression, scope, "value", 0U);
        const Value needle = argument(expression, scope, "needle", 1U);
        if (value.string() != nullptr)
            return ExpressionValue(Value(value.string()->contains(display_string(needle))));
        if (value.list() != nullptr)
            return ExpressionValue(Value(std::ranges::find(value.list()->values, needle) !=
                                         value.list()->values.end()));
        return ExpressionValue(Value(false));
    }
    if (name == "startsWith" || name == "endsWith") {
        const std::string value = display_string(argument(expression, scope, "value", 0U));
        const std::string part = display_string(
            argument(expression, scope, name == "startsWith" ? "prefix" : "suffix", 1U));
        return ExpressionValue(
            Value(name == "startsWith" ? value.starts_with(part) : value.ends_with(part)));
    }
    if (name == "format") {
        const auto& arguments = array_field(expression, "arguments");
        if (arguments.empty())
            return ExpressionValue(Value(""));
        std::string formatted = display_string(
            require_value(evaluate(required(arguments[0], "value"), scope), arguments[0]));
        for (std::size_t index = 1U; index < arguments.size(); ++index) {
            const std::string marker = "{" + std::to_string(index - 1U) + "}";
            const std::string replacement = display_string(require_value(
                evaluate(required(arguments[index], "value"), scope), arguments[index]));
            std::size_t position = 0U;
            while ((position = formatted.find(marker, position)) != std::string::npos) {
                formatted.replace(position, marker.size(), replacement);
                position += replacement.size();
            }
            const JsonValue argument_name = required(arguments[index], "name");
            if (const std::optional<std::string_view> encoded_name = argument_name.string();
                encoded_name.has_value()) {
                const std::string named_marker = "{" + std::string(*encoded_name) + "}";
                position = 0U;
                while ((position = formatted.find(named_marker, position)) != std::string::npos) {
                    formatted.replace(position, named_marker.size(), replacement);
                    position += replacement.size();
                }
            }
        }
        return ExpressionValue(Value(std::move(formatted)));
    }
    if (name == "formatNumber") {
        const Value value = argument(expression, scope, "value", 0U);
        const Value precision_value = argument(expression, scope, "precision", 1U);
        const double number = value.number() != nullptr ? *value.number() : 0.0;
        const int precision = precision_value.number() != nullptr
                                  ? std::clamp(static_cast<int>(*precision_value.number()), 0, 12)
                                  : 2;
        std::ostringstream formatted;
        formatted.imbue(std::locale::classic());
        formatted << std::fixed << std::setprecision(precision) << number;
        return ExpressionValue(Value(formatted.str()));
    }
    if (name == "rgb" || name == "rgba") {
        const std::uint8_t red = color_channel(argument(expression, scope, "red", 0U));
        const std::uint8_t green = color_channel(argument(expression, scope, "green", 1U));
        const std::uint8_t blue = color_channel(argument(expression, scope, "blue", 2U));
        const std::uint8_t alpha =
            name == "rgba" ? color_channel(argument(expression, scope, "alpha", 3U)) : UINT8_MAX;
        return ExpressionValue(Value(ColorValue{red, green, blue, alpha}));
    }
    if (name == "animation") {
        return ExpressionValue(Value(display_string(argument(expression, scope, "name", 0U))));
    }
    if (name == "style") {
        std::vector<Value> bases;
        std::vector<std::pair<std::string, Value>> properties;
        for (const JsonValue entry : array_field(expression, "arguments")) {
            const Value value = require_value(evaluate(required(entry, "value"), scope), entry);
            const JsonValue argument_name = required(entry, "name");
            if (const std::optional<std::string_view> encoded_name = argument_name.string();
                !encoded_name.has_value()) {
                bases.push_back(value);
            } else {
                properties.emplace_back(std::string(*encoded_name), value);
            }
        }
        properties.emplace_back("$bases", Value(std::move(bases)));
        return ExpressionValue(Value(std::move(properties)));
    }
    if (name == "whenStyle") {
        const Value condition = argument(expression, scope, "condition", 0U);
        if (condition.boolean() == nullptr || !*condition.boolean()) {
            return ExpressionValue(Value{});
        }
        return ExpressionValue(argument(expression, scope, "active", 1U));
    }
    if (name == "effect") {
        std::vector<std::pair<std::string, Value>> arguments;
        std::optional<Value> refresh_rate;
        for (const JsonValue entry : array_field(expression, "arguments")) {
            const JsonValue argument_name = required(entry, "name");
            const std::optional<std::string_view> encoded_name = argument_name.string();
            if (!encoded_name.has_value() || *encoded_name == "name")
                continue;
            if (*encoded_name == "refreshRate") {
                refresh_rate = require_value(evaluate(required(entry, "value"), scope), entry);
                continue;
            }
            arguments.emplace_back(std::string(*encoded_name),
                                   require_value(evaluate(required(entry, "value"), scope), entry));
        }
        std::vector<std::pair<std::string, Value>> fields{
            {"arguments", Value(std::move(arguments))},
            {"name", argument(expression, scope, "name", 0U)},
        };
        if (refresh_rate.has_value()) {
            fields.emplace_back("refreshRate", std::move(*refresh_rate));
        }
        return ExpressionValue(Value(std::move(fields)));
    }
    report(expression, "STRATA.DSL.RUNTIME_UNKNOWN_HELPER",
           "Helper '" + name + "' is not available at runtime.", "registered helper");
    return ExpressionValue{};
}

Value ExpressionRuntime::require_value(const ExpressionValue& evaluated,
                                       const JsonValue expression) {
    if (const Value* value = evaluated.value())
        return *value;
    if (const auto* collection = evaluated.collection())
        return (*collection)->items;
    report(expression, "STRATA.DSL.RUNTIME_TYPE_MISMATCH",
           "Expression did not produce a scalar runtime value.");
    return Value{};
}

Value ExpressionRuntime::argument(const JsonValue helper, const ExpressionScope& scope,
                                  const std::string_view name, const std::size_t position) {
    const JsonValue expression = argument_expression(helper, name, position);
    return expression ? require_value(evaluate(expression, scope), expression) : Value{};
}

JsonValue ExpressionRuntime::argument_expression(const JsonValue helper,
                                                 const std::string_view name,
                                                 const std::size_t position) const {
    const JsonArray arguments = array_field(helper, "arguments");
    for (const JsonValue argument_value : arguments) {
        const JsonValue argument_name = required(argument_value, "name");
        if (argument_name.string() == std::optional<std::string_view>(name)) {
            return required(argument_value, "value");
        }
    }
    if (position < arguments.size() && required(arguments[position], "name").is_null()) {
        return required(arguments[position], "value");
    }
    return {};
}

Value ExpressionRuntime::evaluate_lambda(const LambdaValue& lambda, const Value& input) {
    ExpressionScope nested{
        lambda.captured,
        lambda.captured_executable,
        lambda.captured_host_roots,
        lambda.component_path + "/" + lambda.parameter,
        {},
        lambda.captured_host_dependencies,
        lambda.captured_lexical_dependencies,
    };
    nested.values.insert_or_assign(lambda.parameter, input);
    nested.executable_values.erase(lambda.parameter);
    nested.lexical_dependency_overrides.erase(lambda.parameter);
    LambdaDependencyFilter dependency_filter(dependency_observer_, lambda.parameter);
    struct DependencyObserverRestore final {
        ExpressionDependencyObserver*& slot;
        ExpressionDependencyObserver* previous;
        ~DependencyObserverRestore() {
            slot = previous;
        }
    } dependency_restore{dependency_observer_, dependency_observer_};
    dependency_observer_ = &dependency_filter;
    return require_value(evaluate(lambda.body, nested), lambda.body);
}

std::optional<ExpressionRuntime::HostAccess>
ExpressionRuntime::host_access(const JsonValue expression, const ExpressionScope& scope) {
    const std::string_view kind = string_field(expression, "kind");
    if (kind == "variable" && string_field(expression, "binding") == "host") {
        const std::string_view name = string_field(expression, "name");
        if (scope.values.contains(name) || scope.executable_values.contains(name) ||
            scope.lexical_dependency_overrides.contains(name)) {
            return std::nullopt;
        }
        return HostAccess{{HostPathSegment::named(std::string(name))}};
    }
    if (kind != "property" && kind != "index")
        return std::nullopt;
    std::optional<HostAccess> receiver = host_access(required(expression, "receiver"), scope);
    if (!receiver.has_value())
        return std::nullopt;
    if (kind == "property") {
        receiver->path.push_back(
            HostPathSegment::named(std::string(string_field(expression, "name"))));
        return receiver;
    }
    const Value lookup = require_value(evaluate(required(expression, "index"), scope), expression);
    receiver->path.push_back(HostPathSegment::lookup(
        lookup.string() != nullptr ? *lookup.string() : display_string(lookup),
        bounded_index(lookup)));
    return receiver;
}

std::shared_ptr<const CollectionViewValue>
ExpressionRuntime::collection_view(const JsonValue helper, const ExpressionScope& scope) {
    const std::string_view operation = string_field(helper, "name");
    const JsonValue source_expression = argument_expression(helper, "source", 0U);
    if (!source_expression) {
        report(helper, "STRATA.DSL.RUNTIME_COLLECTION_SOURCE",
               "Collection helper requires a source list.");
        return std::shared_ptr<const CollectionViewValue>(new CollectionViewValue{
            CollectionViewImmutableIdentity{
                Value(std::vector<Value>{}),
                0U,
                0U,
                0U,
                0U,
                std::string(operation),
            },
        });
    }
    const std::string_view path = string_field(helper, "path");
    const std::string expression_fingerprint = data::encode_canonical_json(helper);
    for (CollectionCacheEntry& cached : collection_cache_) {
        if (cached.path != path || cached.expression_fingerprint != expression_fingerprint) {
            continue;
        }
        bool dependencies_current = true;
        for (const CollectionDependencyRead& read : cached.dependency_order) {
            if (read.kind == CollectionDependencyKind::lexical) {
                const auto stored = cached.lexical_dependencies.find(read.key);
                const std::optional<ExpressionDependencyValue> current =
                    expression_scope_dependency(scope, read.key);
                if (stored == cached.lexical_dependencies.end() || !current.has_value() ||
                    *current != stored->second) {
                    dependencies_current = false;
                    break;
                }
            } else {
                const auto stored = cached.host_dependencies.find(read.key);
                if (stored == cached.host_dependencies.end()) {
                    dependencies_current = false;
                    break;
                }
                const ExpressionHostDependency current =
                    read_host_dependency(stored->second.path, scope);
                if (canonical_host_dependency_path(current.path) != read.key ||
                    current != stored->second) {
                    dependencies_current = false;
                    break;
                }
            }
        }
        if (!dependencies_current)
            continue;
        if (dependency_observer_ != nullptr) {
            for (const CollectionDependencyRead& read : cached.dependency_order) {
                if (read.kind == CollectionDependencyKind::lexical) {
                    dependency_observer_->lexical(read.key,
                                                  cached.lexical_dependencies.at(read.key));
                } else {
                    dependency_observer_->host(cached.host_dependencies.at(read.key));
                }
            }
        }
        static_cast<void>(cached.view->cache_hits.fetch_add(1U, std::memory_order_relaxed));
        return cached.view;
    }

    CollectionDependencyTrace dependencies(dependency_observer_);
    struct DependencyObserverRestore final {
        ExpressionDependencyObserver*& slot;
        ExpressionDependencyObserver* previous;
        ~DependencyObserverRestore() {
            slot = previous;
        }
    } dependency_restore{dependency_observer_, dependency_observer_};
    dependency_observer_ = &dependencies;

    const ExpressionValue source = evaluate(source_expression, scope);
    const ValueList* source_items = collection_items(source);
    const auto [total, source_matched] = collection_counts(source);
    if (source_items == nullptr) {
        report(helper, "STRATA.DSL.RUNTIME_COLLECTION_SOURCE",
               "Collection helper requires a list or derived collection view.");
        return std::shared_ptr<const CollectionViewValue>(new CollectionViewValue{
            CollectionViewImmutableIdentity{
                Value(std::vector<Value>{}),
                0U,
                0U,
                0U,
                0U,
                std::string(operation),
            },
        });
    }

    std::vector<Value> scalar_arguments;
    const auto& arguments = array_field(helper, "arguments");
    for (std::size_t index = 1U; index < arguments.size(); ++index) {
        const JsonValue argument_value = required(arguments[index], "value");
        if (string_field(argument_value, "kind") != "lambda") {
            scalar_arguments.push_back(
                require_value(evaluate(argument_value, scope), argument_value));
        }
    }

    JsonValue lambda_expression;
    if (operation == "filter" || operation == "takeWhile")
        lambda_expression = argument_expression(helper, "predicate", 1U);
    else if (operation == "map")
        lambda_expression = argument_expression(helper, "transform", 1U);
    else if (operation == "sortBy" || operation == "distinctBy" || operation == "groupBy")
        lambda_expression = argument_expression(helper, "selector", 1U);
    ExpressionValue lambda_value =
        lambda_expression ? evaluate(lambda_expression, scope) : ExpressionValue{};
    const auto* lambda_pointer = lambda_value.lambda();

    std::vector<Value> result;
    if (operation == "filter") {
        for (const Value& value : source_items->values) {
            if (lambda_pointer != nullptr && truthy(evaluate_lambda(**lambda_pointer, value)))
                result.push_back(value);
        }
    } else if (operation == "map") {
        for (const Value& value : source_items->values) {
            result.push_back(lambda_pointer != nullptr ? evaluate_lambda(**lambda_pointer, value)
                                                       : Value{});
        }
    } else if (operation == "sortBy") {
        const bool descending = !scalar_arguments.empty() &&
                                scalar_arguments.back().boolean() != nullptr &&
                                *scalar_arguments.back().boolean();
        std::vector<std::pair<Value, Value>> decorated;
        decorated.reserve(source_items->values.size());
        for (const Value& value : source_items->values) {
            decorated.emplace_back(value, lambda_pointer != nullptr
                                              ? evaluate_lambda(**lambda_pointer, value)
                                              : Value{});
        }
        std::stable_sort(decorated.begin(), decorated.end(),
                         [this, descending](const auto& left, const auto& right) {
                             const int compared = compare_keys(left.second, right.second);
                             return descending ? compared > 0 : compared < 0;
                         });
        result.reserve(decorated.size());
        for (auto& [value, key] : decorated) {
            static_cast<void>(key);
            result.push_back(std::move(value));
        }
    } else if (operation == "distinctBy") {
        std::vector<Value> seen;
        for (const Value& value : source_items->values) {
            const Value key =
                lambda_pointer != nullptr ? evaluate_lambda(**lambda_pointer, value) : value;
            if (std::ranges::find(seen, key) == seen.end()) {
                seen.push_back(key);
                result.push_back(value);
            }
        }
    } else if (operation == "groupBy") {
        std::vector<std::pair<Value, std::vector<Value>>> groups;
        for (const Value& value : source_items->values) {
            const Value key =
                lambda_pointer != nullptr ? evaluate_lambda(**lambda_pointer, value) : Value{};
            auto found = std::ranges::find_if(
                groups, [&key](const auto& group) { return group.first == key; });
            if (found == groups.end()) {
                groups.emplace_back(key, std::vector<Value>{value});
            } else {
                found->second.push_back(value);
            }
        }
        for (auto& [key, items] : groups) {
            result.emplace_back(std::vector<std::pair<std::string, Value>>{
                {"key", key},
                {"items", Value(std::move(items))},
            });
        }
    } else if (operation == "flatten") {
        for (const Value& value : source_items->values) {
            if (const ValueList* nested = value.list()) {
                result.insert(result.end(), nested->values.begin(), nested->values.end());
            }
        }
    } else if (operation == "takeWhile") {
        for (const Value& value : source_items->values) {
            if (lambda_pointer == nullptr || !truthy(evaluate_lambda(**lambda_pointer, value)))
                break;
            result.push_back(value);
        }
    } else if (operation == "window" || operation == "page") {
        const std::size_t first =
            !scalar_arguments.empty() ? bounded_index(scalar_arguments[0]).value_or(0U) : 0U;
        const std::size_t amount =
            scalar_arguments.size() > 1U
                ? std::min(bounded_index(scalar_arguments[1]).value_or(0U), maximum_derived_items)
                : 0U;
        const std::size_t offset = operation == "page" && amount != 0U &&
                                           first <= std::numeric_limits<std::size_t>::max() / amount
                                       ? first * amount
                                   : operation == "page" ? std::numeric_limits<std::size_t>::max()
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
        report(helper, "STRATA.DSL.RUNTIME_COLLECTION_BOUND_EXCEEDED",
               "Collection helper output exceeded the runtime bound.");
    }
    if (operation == "map" && std::ranges::any_of(result, [](const Value& value) {
            if (value.object() == nullptr)
                return false;
            const Value* key = value.field("key");
            if (key == nullptr)
                key = value.field("id");
            return key == nullptr || key->kind() == ValueKind::null_value ||
                   key->kind() == ValueKind::list || key->kind() == ValueKind::object;
        })) {
        report(helper, "STRATA.DSL.RUNTIME_COLLECTION_UNSTABLE_KEY",
               "Mapped record results must expose a stable 'key' or 'id' field before they are "
               "repeated.",
               "record containing key or id");
    }
    const bool changes_match_count = operation == "filter" || operation == "distinctBy" ||
                                     operation == "groupBy" || operation == "flatten" ||
                                     operation == "takeWhile";
    const std::size_t matched = changes_match_count ? result.size() : source_matched;
    std::size_t range_start = 0U;
    if ((operation == "window" || operation == "page") && !scalar_arguments.empty()) {
        range_start = bounded_index(scalar_arguments[0]).value_or(0U);
        if (operation == "page" && scalar_arguments.size() > 1U) {
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
            std::string(operation),
        },
    });
    if (dependencies.cacheable) {
        if (collection_cache_.size() >= 1024U)
            collection_cache_.erase(collection_cache_.begin());
        std::vector<CollectionDependencyRead> dependency_order;
        dependency_order.reserve(dependencies.order.size());
        for (auto& [host, key] : dependencies.order) {
            dependency_order.push_back(CollectionDependencyRead{
                host ? CollectionDependencyKind::host : CollectionDependencyKind::lexical,
                std::move(key),
            });
        }
        collection_cache_.push_back(CollectionCacheEntry{
            std::string(path),
            expression_fingerprint,
            std::move(dependencies.lexical_values),
            std::move(dependencies.host_values),
            std::move(dependency_order),
            view,
        });
    }
    return view;
}

std::shared_ptr<const ActionValue>
ExpressionRuntime::composed_action(const JsonValue helper, const ExpressionScope& scope,
                                   const ActionCompositionMode mode) {
    std::vector<std::shared_ptr<const ActionValue>> children;
    for (const JsonValue argument_value : array_field(helper, "arguments")) {
        const ExpressionValue evaluated = evaluate(required(argument_value, "value"), scope);
        if (evaluated.action() == nullptr) {
            report(helper, "STRATA.DSL.RUNTIME_ACTION_COMPOSITION",
                   "Action composition requires typed actions.");
            return std::make_shared<const ActionValue>(ActionValue{});
        }
        children.push_back(*evaluated.action());
    }
    if (children.empty()) {
        report(helper, "STRATA.DSL.RUNTIME_ACTION_COMPOSITION",
               "Action composition must not be empty.");
        return std::make_shared<const ActionValue>(ActionValue{});
    }
    const std::string id =
        mode == ActionCompositionMode::sequence ? "action.sequence" : "action.parallel";
    const auto contract = actions_.contract(id);
    if (contract == nullptr) {
        report(helper, "STRATA.DSL.RUNTIME_UNKNOWN_ACTION",
               "Framework composition action is not registered.");
        return std::make_shared<const ActionValue>(ActionValue{});
    }
    Value payload(std::vector<std::pair<std::string, Value>>{});
    auto action =
        std::make_shared<const Action>(contract, std::move(payload), action_origin(helper, scope));
    return std::make_shared<const ActionValue>(ActionValue{
        std::move(action),
        mode,
        std::move(children),
    });
}

void ExpressionRuntime::report(const JsonValue expression, std::string code, std::string message,
                               std::optional<std::string> expected) {
    const std::optional<std::string_view> path = expression.find("path").string();
    const std::string diagnostic_path =
        active_scope_ != nullptr && !active_scope_->component_path.empty()
            ? active_scope_->component_path
        : path.has_value() ? std::string(*path)
                           : std::string{};
    report(RuntimeDiagnostic{
        std::move(code),
        std::move(message),
        diagnostic_path,
        std::move(expected),
        DiagnosticSeverity::error,
        portable_expression_range(expression),
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

bool truthy(const Value& value) noexcept {
    switch (value.kind()) {
    case ValueKind::null_value:
        return false;
    case ValueKind::boolean:
        return *value.boolean();
    case ValueKind::number:
        return *value.number() != 0.0;
    case ValueKind::duration:
        return value.duration()->nanoseconds != 0;
    case ValueKind::string:
        return !value.string()->empty();
    case ValueKind::list:
        return !value.list()->values.empty();
    case ValueKind::object:
        return !value.object()->fields.empty();
    case ValueKind::color:
    case ValueKind::image:
    case ValueKind::key:
    case ValueKind::theme_token:
        return true;
    }
    return false;
}

std::string display_string(const Value& value) {
    switch (value.kind()) {
    case ValueKind::null_value:
        return {};
    case ValueKind::boolean:
        return *value.boolean() ? "true" : "false";
    case ValueKind::number: {
        char buffer[64]{};
        const auto converted = std::to_chars(std::begin(buffer), std::end(buffer), *value.number());
        return converted.ec == std::errc{} ? std::string(buffer, converted.ptr) : std::string{"0"};
    }
    case ValueKind::duration:
        return std::to_string(value.duration()->nanoseconds);
    case ValueKind::string:
        return *value.string();
    case ValueKind::color: {
        static constexpr char digits[] = "0123456789abcdef";
        const ColorValue color = *value.color();
        const std::uint8_t channels[] = {color.red, color.green, color.blue, color.alpha};
        std::string displayed = "#";
        displayed.reserve(9U);
        for (const std::uint8_t channel : channels) {
            displayed.push_back(digits[channel >> 4U]);
            displayed.push_back(digits[channel & 0x0FU]);
        }
        return displayed;
    }
    case ValueKind::image:
        return value.image()->id;
    case ValueKind::key:
        return value.key()->value;
    case ValueKind::theme_token:
        return "theme." + value.theme_token()->name;
    case ValueKind::list: {
        std::string displayed = "[";
        for (std::size_t index = 0U; index < value.list()->values.size(); ++index) {
            if (index != 0U)
                displayed += ", ";
            displayed += display_string(value.list()->values[index]);
        }
        return displayed + "]";
    }
    case ValueKind::object: {
        std::string displayed = "{";
        for (std::size_t index = 0U; index < value.object()->fields.size(); ++index) {
            if (index != 0U)
                displayed += ", ";
            displayed += value.object()->fields[index].first + "=" +
                         display_string(value.object()->fields[index].second);
        }
        return displayed + "}";
    }
    }
    return {};
}

} // namespace strata::runtime
