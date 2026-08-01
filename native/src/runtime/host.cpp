#include "runtime/host.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "core/utf8.hpp"

namespace strata::runtime {

struct HostScalarData final {
    explicit HostScalarData(
        std::function<Value()> source_provider,
        HostNode::EvaluationCounter source_counter
    )
        : provider(std::move(source_provider)), counter(std::move(source_counter)) {}

    std::function<Value()> provider;
    HostNode::EvaluationCounter counter;
    mutable std::mutex mutex;
    mutable std::optional<Value> cached;
};

namespace {

void validate_name(const std::string_view value, const std::string_view label) {
    if (value.empty() || !core::valid_utf8(value)) {
        throw std::invalid_argument(std::string(label) + " must be non-empty valid UTF-8");
    }
}

[[nodiscard]] std::vector<std::string_view> split_path(const std::string_view path) {
    if (path.empty()) throw std::invalid_argument("host path must not be empty");
    std::vector<std::string_view> result;
    std::size_t start = 0U;
    while (start <= path.size()) {
        const std::size_t dot = path.find('.', start);
        const std::size_t end = dot == std::string_view::npos ? path.size() : dot;
        if (end == start) throw std::invalid_argument("host path segments must not be empty");
        result.push_back(path.substr(start, end - start));
        if (dot == std::string_view::npos) break;
        start = dot + 1U;
    }
    return result;
}

[[nodiscard]] HostNode schema_host_node(
    const data::JsonValue& value,
    const ValueSchemaPtr& schema,
    const HostNode::EvaluationCounter& counter
) {
    if (schema == nullptr || schema->kind() == ValueSchemaKind::any) {
        return HostNode::from_json(value, counter);
    }
    if (value.is_null()) return HostNode{};
    if (schema->kind() == ValueSchemaKind::key) {
        if (const std::string* text = value.string(); text != nullptr) {
            return HostNode::constant(Value(KeyValue{*text}), counter);
        }
        return HostNode::from_json(value, counter);
    }
    if (schema->kind() == ValueSchemaKind::list) {
        const data::JsonValue::Array* array = value.array();
        if (array == nullptr) return HostNode::from_json(value, counter);
        std::vector<HostNode> items;
        items.reserve(array->size());
        for (const data::JsonValue& item : *array) {
            items.push_back(schema_host_node(item, schema->element(), counter));
        }
        return HostNode::list(std::move(items));
    }
    if (schema->kind() == ValueSchemaKind::object) {
        const data::JsonValue::Object* object = value.object();
        if (object == nullptr) return HostNode::from_json(value, counter);
        std::vector<std::pair<std::string, HostNode>> fields;
        fields.reserve(object->size());
        for (const auto& [name, field] : *object) {
            const ValueSchemaField* field_schema = schema->field(name);
            fields.emplace_back(
                name,
                schema_host_node(
                    field,
                    field_schema != nullptr
                        ? field_schema->schema
                        : schema->unknown_field_schema(),
                    counter
                )
            );
        }
        return HostNode::object(std::move(fields));
    }
    return HostNode::from_json(value, counter);
}

[[nodiscard]] HostNode schema_host_roots(
    const data::JsonValue& roots,
    const HostSchemaRoots& schemas,
    const HostNode::EvaluationCounter& counter
) {
    const data::JsonValue::Object* object = roots.object();
    if (object == nullptr) return HostNode::from_json(roots, counter);
    std::vector<std::pair<std::string, HostNode>> fields;
    fields.reserve(object->size());
    for (const auto& [name, value] : *object) {
        const auto schema = schemas.find(name);
        fields.emplace_back(
            name,
            schema_host_node(
                value,
                schema != schemas.end() ? schema->second : ValueSchemaPtr{},
                counter
            )
        );
    }
    return HostNode::object(std::move(fields));
}

} // namespace

HostPathSegment HostPathSegment::named(std::string field) {
    if (field.empty()) throw std::invalid_argument("host field segment must not be empty");
    return HostPathSegment{HostPathSegmentKind::field, std::move(field), std::nullopt};
}

HostPathSegment HostPathSegment::lookup(
    std::string field,
    const std::optional<std::size_t> index
) {
    return HostPathSegment{HostPathSegmentKind::lookup, std::move(field), index};
}

HostConstantData::HostConstantData(Value source_value, HostEvaluationCounter source_counter)
    : value(std::move(source_value)), counter(std::move(source_counter)) {}

HostConstantData::HostConstantData(HostConstantData&& other)
    : value(std::move(other.value)),
      counter(std::move(other.counter)),
      evaluated(other.evaluated.load(std::memory_order_relaxed)) {}

HostConstantData& HostConstantData::operator=(HostConstantData&& other) {
    if (this == &other) return *this;
    value = std::move(other.value);
    counter = std::move(other.counter);
    evaluated.store(
        other.evaluated.load(std::memory_order_relaxed),
        std::memory_order_relaxed
    );
    return *this;
}

HostNode::HostNode() noexcept : storage_(NullValue{}) {}

HostNode::HostNode(Storage storage) : storage_(std::move(storage)) {}

HostNode HostNode::scalar(std::function<Value()> provider, EvaluationCounter counter) {
    if (!provider) throw std::invalid_argument("host scalar provider must be callable");
    return HostNode(std::make_shared<HostScalarData>(std::move(provider), std::move(counter)));
}

HostNode HostNode::constant(Value value, EvaluationCounter counter) {
    return HostNode(HostConstantData(std::move(value), std::move(counter)));
}

HostNode HostNode::list(std::vector<HostNode> items) {
    return HostNode(std::make_shared<const HostListData>(HostListData{std::move(items)}));
}

HostNode HostNode::object(std::vector<std::pair<std::string, HostNode>> fields) {
    for (const auto& [name, node] : fields) {
        static_cast<void>(node);
        validate_name(name, "host object field");
    }
    std::ranges::sort(fields, {}, &std::pair<std::string, HostNode>::first);
    const auto duplicate = std::ranges::adjacent_find(
        fields,
        {},
        &std::pair<std::string, HostNode>::first
    );
    if (duplicate != fields.end()) throw std::invalid_argument("host object fields must be unique");
    return HostNode(std::make_shared<const HostObjectData>(HostObjectData{std::move(fields)}));
}

HostNode HostNode::from_json(const data::JsonValue& value, EvaluationCounter counter) {
    if (value.is_null()) return HostNode{};
    if (const data::JsonValue::Array* array = value.array()) {
        std::vector<HostNode> items;
        items.reserve(array->size());
        for (const data::JsonValue& item : *array) items.push_back(from_json(item, counter));
        return list(std::move(items));
    }
    if (const data::JsonValue::Object* object_value = value.object()) {
        std::vector<std::pair<std::string, HostNode>> fields;
        fields.reserve(object_value->size());
        for (const auto& [name, field_value] : *object_value) {
            fields.emplace_back(name, from_json(field_value, counter));
        }
        return object(std::move(fields));
    }
    return constant(value_from_json(value), std::move(counter));
}

HostNodeKind HostNode::kind() const noexcept {
    switch (storage_.index()) {
    case 0U: return HostNodeKind::null_value;
    case 1U:
    case 2U: return HostNodeKind::scalar;
    case 3U: return HostNodeKind::list;
    case 4U: return HostNodeKind::object;
    default: std::unreachable();
    }
}

const HostNode* HostNode::field(const std::string_view name) const noexcept {
    const auto* object_pointer = std::get_if<4U>(&storage_);
    if (object_pointer == nullptr || *object_pointer == nullptr) return nullptr;
    const auto& fields = (*object_pointer)->fields;
    const auto found = std::ranges::lower_bound(
        fields,
        name,
        {},
        &std::pair<std::string, HostNode>::first
    );
    return found != fields.end() && found->first == name ? &found->second : nullptr;
}

const HostNode* HostNode::item(const std::size_t index) const noexcept {
    const auto* list_pointer = std::get_if<3U>(&storage_);
    if (list_pointer == nullptr || *list_pointer == nullptr || index >= (*list_pointer)->items.size()) {
        return nullptr;
    }
    return &(*list_pointer)->items[index];
}

std::size_t HostNode::size() const noexcept {
    if (const auto* list_pointer = std::get_if<3U>(&storage_)) {
        return *list_pointer != nullptr ? (*list_pointer)->items.size() : 0U;
    }
    if (const auto* object_pointer = std::get_if<4U>(&storage_)) {
        return *object_pointer != nullptr ? (*object_pointer)->fields.size() : 0U;
    }
    return 0U;
}

Value HostNode::materialize() const {
    if (std::holds_alternative<NullValue>(storage_)) return Value{};
    if (const auto* scalar_pointer = std::get_if<std::shared_ptr<HostScalarData>>(&storage_)) {
        HostScalarData& scalar_data = **scalar_pointer;
        std::scoped_lock lock(scalar_data.mutex);
        if (!scalar_data.cached.has_value()) {
            Value value = scalar_data.provider();
            scalar_data.cached = std::move(value);
            if (scalar_data.counter != nullptr) {
                static_cast<void>(scalar_data.counter->fetch_add(1U, std::memory_order_relaxed));
            }
        }
        return *scalar_data.cached;
    }
    if (const auto* constant = std::get_if<HostConstantData>(&storage_)) {
        if (!constant->evaluated.exchange(true, std::memory_order_relaxed) &&
            constant->counter != nullptr) {
            static_cast<void>(
                constant->counter->fetch_add(1U, std::memory_order_relaxed)
            );
        }
        return constant->value;
    }
    if (const auto* list_pointer = std::get_if<3U>(&storage_)) {
        std::vector<Value> values;
        values.reserve((*list_pointer)->items.size());
        for (const HostNode& item_node : (*list_pointer)->items) values.push_back(item_node.materialize());
        return Value(std::move(values));
    }
    const auto& object_data = *std::get<4U>(storage_);
    std::vector<std::pair<std::string, Value>> fields;
    fields.reserve(object_data.fields.size());
    for (const auto& [name, field_node] : object_data.fields) {
        fields.emplace_back(name, field_node.materialize());
    }
    return Value(std::move(fields));
}

HostSnapshot::HostSnapshot(std::string id, const std::uint64_t generation, HostNode roots)
    : id_(std::move(id)),
      generation_(generation),
      roots_(std::move(roots)),
      evaluation_counter_(std::make_shared<std::atomic<std::uint64_t>>(0U)) {
    validate_name(id_, "host snapshot id");
    if (roots_.kind() != HostNodeKind::object) {
        throw std::invalid_argument("host snapshot roots must be an object");
    }
}

std::shared_ptr<const HostSnapshot> HostSnapshot::from_json(
    std::string id,
    const std::uint64_t generation,
    const data::JsonValue& roots
) {
    auto counter = std::make_shared<std::atomic<std::uint64_t>>(0U);
    HostNode root_node = HostNode::from_json(roots, counter);
    auto snapshot = std::shared_ptr<HostSnapshot>(
        new HostSnapshot(std::move(id), generation, std::move(root_node))
    );
    snapshot->evaluation_counter_ = std::move(counter);
    return snapshot;
}

std::shared_ptr<const HostSnapshot> HostSnapshot::from_json(
    std::string id,
    const std::uint64_t generation,
    const data::JsonValue& roots,
    const HostSchemaRoots& schemas
) {
    auto counter = std::make_shared<std::atomic<std::uint64_t>>(0U);
    HostNode root_node = schema_host_roots(roots, schemas, counter);
    auto snapshot = std::shared_ptr<HostSnapshot>(
        new HostSnapshot(std::move(id), generation, std::move(root_node))
    );
    snapshot->evaluation_counter_ = std::move(counter);
    return snapshot;
}

const std::string& HostSnapshot::id() const noexcept { return id_; }
std::uint64_t HostSnapshot::generation() const noexcept { return generation_; }

std::optional<Value> HostSnapshot::resolve(const std::string_view dotted_path) const {
    const std::vector<std::string_view> path = split_path(dotted_path);
    return resolve(path);
}

std::optional<Value> HostSnapshot::resolve(const std::span<const std::string_view> path) const {
    if (path.empty()) throw std::invalid_argument("host path must contain at least one segment");
    const HostNode* current = &roots_;
    for (const std::string_view segment : path) {
        if (segment.empty()) throw std::invalid_argument("host path segments must not be empty");
        current = current->field(segment);
        if (current == nullptr) return std::nullopt;
    }
    return current->materialize();
}

std::optional<Value> HostSnapshot::resolve(
    const std::span<const HostPathSegment> path
) const {
    if (path.empty()) throw std::invalid_argument("host path must contain at least one segment");
    const HostNode* current = &roots_;
    for (const HostPathSegment& segment : path) {
        if (segment.kind == HostPathSegmentKind::field) {
            if (segment.field.empty()) {
                throw std::invalid_argument("host field segment must not be empty");
            }
            current = current->field(segment.field);
        } else if (current->kind() == HostNodeKind::list) {
            current = segment.index.has_value() ? current->item(*segment.index) : nullptr;
        } else {
            current = current->field(segment.field);
        }
        if (current == nullptr) return std::nullopt;
    }
    return current->materialize();
}

bool HostSnapshot::contains(const std::span<const HostPathSegment> path) const {
    if (path.empty()) throw std::invalid_argument("host path must contain at least one segment");
    const HostNode* current = &roots_;
    for (const HostPathSegment& segment : path) {
        if (segment.kind == HostPathSegmentKind::field) {
            if (segment.field.empty()) {
                throw std::invalid_argument("host field segment must not be empty");
            }
            current = current->field(segment.field);
        } else if (current->kind() == HostNodeKind::list) {
            current = segment.index.has_value() ? current->item(*segment.index) : nullptr;
        } else {
            current = current->field(segment.field);
        }
        if (current == nullptr) return false;
    }
    return true;
}

std::uint64_t HostSnapshot::evaluated_scalar_count() const noexcept {
    return evaluation_counter_->load(std::memory_order_relaxed);
}

HostStore::HostStore(Invalidation invalidation) : invalidation_(std::move(invalidation)) {}

bool HostStore::adopt(std::shared_ptr<const HostSnapshot> snapshot) {
    if (snapshot == nullptr) throw std::invalid_argument("host snapshot must not be null");
    if (snapshot_ == snapshot) return false;
    const std::string id = snapshot->id();
    const auto previous = snapshots_.find(id);
    if (previous != snapshots_.end() &&
        snapshot->generation() <= previous->second->generation()) {
        throw std::invalid_argument(
            "host snapshot generations must increase strictly within one snapshot id"
        );
    }
    snapshots_.insert_or_assign(id, snapshot);
    std::erase(adoption_order_, id);
    adoption_order_.push_back(id);
    snapshot_ = std::move(snapshot);
    ++invalidation_count_;
    if (invalidation_) invalidation_(snapshot_->generation());
    return true;
}

const std::shared_ptr<const HostSnapshot>& HostStore::snapshot() const noexcept { return snapshot_; }

std::optional<std::uint64_t> HostStore::generation(const std::string_view id) const noexcept {
    const auto found = snapshots_.find(id);
    return found != snapshots_.end()
        ? std::optional<std::uint64_t>(found->second->generation())
        : std::nullopt;
}

std::vector<std::shared_ptr<const HostSnapshot>> HostStore::snapshots() const {
    std::vector<std::shared_ptr<const HostSnapshot>> result;
    result.reserve(adoption_order_.size());
    for (const std::string& id : adoption_order_) result.push_back(snapshots_.at(id));
    return result;
}

std::optional<Value> HostStore::resolve(const std::string_view dotted_path) const {
    for (auto id = adoption_order_.rbegin(); id != adoption_order_.rend(); ++id) {
        if (std::optional<Value> value = snapshots_.at(*id)->resolve(dotted_path);
            value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<Value> HostStore::resolve(
    const std::span<const HostPathSegment> path
) const {
    for (auto id = adoption_order_.rbegin(); id != adoption_order_.rend(); ++id) {
        if (std::optional<Value> value = snapshots_.at(*id)->resolve(path); value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<HostResolution> HostStore::resolve_with_origin(
    const std::span<const HostPathSegment> path
) const {
    for (auto id = adoption_order_.rbegin(); id != adoption_order_.rend(); ++id) {
        const std::shared_ptr<const HostSnapshot>& snapshot = snapshots_.at(*id);
        if (std::optional<Value> value = snapshot->resolve(path); value.has_value()) {
            return HostResolution{std::move(*value), snapshot->id(), snapshot->generation()};
        }
    }
    return std::nullopt;
}

std::optional<std::pair<std::string, std::uint64_t>> HostStore::origin(
    const std::span<const HostPathSegment> path
) const {
    for (auto id = adoption_order_.rbegin(); id != adoption_order_.rend(); ++id) {
        const std::shared_ptr<const HostSnapshot>& snapshot = snapshots_.at(*id);
        if (snapshot->contains(path)) {
            return std::pair<std::string, std::uint64_t>{
                snapshot->id(), snapshot->generation(),
            };
        }
    }
    return std::nullopt;
}

std::uint64_t HostStore::invalidation_count() const noexcept { return invalidation_count_; }

} // namespace strata::runtime
