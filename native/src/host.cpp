#include <strata/host.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "data/json.hpp"

namespace strata::host {
namespace {

[[nodiscard]] data::JsonValue to_json_value(const Value& value) {
    if (value.is_null())
        return data::JsonValue{};
    if (const bool* boolean = value.boolean(); boolean != nullptr) {
        return data::JsonValue(*boolean);
    }
    if (const std::int64_t* integer = value.integer(); integer != nullptr) {
        return data::JsonValue(*integer);
    }
    if (const double* number = value.number(); number != nullptr) {
        return data::JsonValue(*number);
    }
    if (const std::string* string = value.string(); string != nullptr) {
        return data::JsonValue(*string);
    }
    if (const Value::Array* values = value.array(); values != nullptr) {
        data::JsonValue::Array result;
        result.reserve(values->size());
        for (const Value& child : *values)
            result.push_back(to_json_value(child));
        return data::JsonValue(std::move(result));
    }
    data::JsonValue::Object result;
    const Value::Object& fields = *value.object();
    result.reserve(fields.size());
    for (const auto& [name, child] : fields) {
        result.emplace_back(name, to_json_value(child));
    }
    return data::JsonValue(std::move(result));
}

[[nodiscard]] Value from_json_value(const data::JsonValue& value) {
    if (value.is_null())
        return Value{};
    if (const bool* boolean = value.boolean(); boolean != nullptr)
        return Value(*boolean);
    if (const std::int64_t* integer = value.integer(); integer != nullptr)
        return Value(*integer);
    if (const double* number = value.number(); number != nullptr)
        return Value(*number);
    if (const std::string* string = value.string(); string != nullptr)
        return Value(*string);
    if (const data::JsonValue::Array* values = value.array(); values != nullptr) {
        Value::Array result;
        result.reserve(values->size());
        for (const data::JsonValue& child : *values)
            result.push_back(from_json_value(child));
        return Value(std::move(result));
    }
    Value::Object result;
    for (const auto& [name, child] : *value.object()) {
        result.insert_or_assign(name, from_json_value(child));
    }
    return Value(std::move(result));
}

[[nodiscard]] std::string copied(const strata_string_view value) {
    return value.size == 0U ? std::string{} : std::string(value.data, value.size);
}

[[nodiscard]] std::optional<std::string> optional_copied(const strata_string_view value) {
    return value.size == 0U ? std::nullopt : std::optional<std::string>(copied(value));
}

[[nodiscard]] std::optional<DragPhase> drag_phase(const std::string_view value) noexcept {
    if (value == "start")
        return DragPhase::start;
    if (value == "enter")
        return DragPhase::enter;
    if (value == "move")
        return DragPhase::move;
    if (value == "leave")
        return DragPhase::leave;
    if (value == "drop")
        return DragPhase::drop;
    if (value == "cancel")
        return DragPhase::cancel;
    return std::nullopt;
}

[[nodiscard]] std::optional<DropPlacement> drop_placement(const std::string_view value) noexcept {
    if (value == "on")
        return DropPlacement::on;
    if (value == "before")
        return DropPlacement::before;
    if (value == "after")
        return DropPlacement::after;
    return std::nullopt;
}

[[nodiscard]] strata_action_handler_result encoded(const ActionResult result) noexcept {
    switch (result) {
    case ActionResult::handled:
        return STRATA_ACTION_HANDLER_HANDLED;
    case ActionResult::forwarded:
        return STRATA_ACTION_HANDLER_FORWARDED;
    case ActionResult::ignored:
        return STRATA_ACTION_HANDLER_IGNORED;
    }
    return STRATA_ACTION_HANDLER_IGNORED;
}

} // namespace

Value::Value() noexcept : storage_(nullptr) {}
Value::Value(std::nullptr_t) noexcept : storage_(nullptr) {}
Value::Value(const bool value) noexcept : storage_(value) {}
Value::Value(const std::int64_t value) noexcept : storage_(value) {}
Value::Value(const double value) : storage_(value) {
    if (!std::isfinite(value))
        throw std::invalid_argument("host number must be finite");
}
Value::Value(const char* const value) : storage_(std::string(value != nullptr ? value : "")) {}
Value::Value(std::string value) : storage_(std::move(value)) {}
Value::Value(const std::string_view value) : storage_(std::string(value)) {}
Value::Value(Array value) : storage_(std::move(value)) {}
Value::Value(Object value) : storage_(std::move(value)) {}

Value Value::array(const std::initializer_list<Value> values) {
    return Value(Array(values));
}

Value Value::object(const std::initializer_list<std::pair<std::string, Value>> fields) {
    Object result;
    for (const auto& [name, value] : fields) {
        const auto [entry, inserted] = result.emplace(name, value);
        static_cast<void>(entry);
        if (!inserted)
            throw std::invalid_argument("host object contains duplicate field '" + name + "'");
    }
    return Value(std::move(result));
}

Value Value::parse(const std::string_view json) {
    return from_json_value(data::parse_json(json));
}

const Value::Storage& Value::storage() const noexcept {
    return storage_;
}
bool Value::is_null() const noexcept {
    return std::holds_alternative<std::nullptr_t>(storage_);
}
const bool* Value::boolean() const noexcept {
    return std::get_if<bool>(&storage_);
}
const std::int64_t* Value::integer() const noexcept {
    return std::get_if<std::int64_t>(&storage_);
}
const double* Value::number() const noexcept {
    return std::get_if<double>(&storage_);
}
const std::string* Value::string() const noexcept {
    return std::get_if<std::string>(&storage_);
}
const Value::Array* Value::array() const noexcept {
    return std::get_if<Array>(&storage_);
}
const Value::Object* Value::object() const noexcept {
    return std::get_if<Object>(&storage_);
}

const Value* Value::find(const std::string_view field) const noexcept {
    const Object* fields = object();
    if (fields == nullptr)
        return nullptr;
    const auto found = fields->find(field);
    return found != fields->end() ? &found->second : nullptr;
}

const Value& Value::require(const std::string_view field) const {
    const Value* value = find(field);
    if (value == nullptr)
        throw std::invalid_argument("host value is missing field '" + std::string(field) + "'");
    return *value;
}

std::string_view Value::require_string(const std::string_view field) const {
    const Value& value = require(field);
    if (value.string() == nullptr) {
        throw std::invalid_argument("host field '" + std::string(field) + "' must be a string");
    }
    return *value.string();
}

std::optional<std::string_view>
Value::optional_string(const std::string_view field) const noexcept {
    const Value* value = find(field);
    return value != nullptr && value->string() != nullptr
               ? std::optional<std::string_view>(*value->string())
               : std::nullopt;
}

std::optional<std::int64_t> Value::optional_integer(const std::string_view field) const noexcept {
    const Value* value = find(field);
    return value != nullptr && value->integer() != nullptr
               ? std::optional<std::int64_t>(*value->integer())
               : std::nullopt;
}

std::optional<double> Value::optional_number(const std::string_view field) const noexcept {
    const Value* value = find(field);
    if (value == nullptr)
        return std::nullopt;
    if (value->number() != nullptr)
        return *value->number();
    return value->integer() != nullptr
               ? std::optional<double>(static_cast<double>(*value->integer()))
               : std::nullopt;
}

std::string Value::json() const {
    return data::encode_canonical_json(to_json_value(*this));
}

bool operator==(const Value& left, const Value& right) noexcept {
    if (left.storage_.index() == right.storage_.index())
        return left.storage_ == right.storage_;
    const auto integer_equals_number = [](const std::int64_t integer, const double number) {
        constexpr double signed_limit = 9'223'372'036'854'775'808.0;
        if (number < -signed_limit || number >= signed_limit || std::trunc(number) != number) {
            return false;
        }
        return static_cast<std::int64_t>(number) == integer;
    };
    if (left.integer() != nullptr && right.number() != nullptr) {
        return integer_equals_number(*left.integer(), *right.number());
    }
    if (left.number() != nullptr && right.integer() != nullptr) {
        return integer_equals_number(*right.integer(), *left.number());
    }
    return false;
}

std::optional<DragEvent> DragEvent::from(const ActionEvent& event) {
    if (event.kind != "drag" || event.value.object() == nullptr)
        return std::nullopt;
    const std::optional<std::string_view> phase_text = event.value.optional_string("phase");
    const Value* payload_value = event.value.find("payload");
    if (!phase_text.has_value() || payload_value == nullptr || payload_value->object() == nullptr) {
        return std::nullopt;
    }
    const std::optional<DragPhase> phase = drag_phase(*phase_text);
    const std::optional<std::string_view> payload_type = payload_value->optional_string("type");
    const Value* payload = payload_value->find("value");
    const std::optional<DropPlacement> placement =
        drop_placement(event.value.optional_string("placement").value_or("on"));
    if (!phase.has_value() || !payload_type.has_value() || payload == nullptr ||
        !placement.has_value()) {
        return std::nullopt;
    }
    const auto copy_optional = [](const std::optional<std::string_view> value) {
        return value.has_value() ? std::optional<std::string>(*value) : std::nullopt;
    };
    const std::optional<std::int64_t> insertion = event.value.optional_integer("insertionIndex");
    return DragEvent{
        *phase,
        std::string(*payload_type),
        *payload,
        copy_optional(event.value.optional_string("targetKey")),
        copy_optional(event.value.optional_string("operation")),
        *placement,
        copy_optional(event.value.optional_string("beforeKey")),
        copy_optional(event.value.optional_string("afterKey")),
        insertion.has_value() && *insertion >= 0
            ? std::optional<std::size_t>(static_cast<std::size_t>(*insertion))
            : std::nullopt,
    };
}

void Revision::changed() {
    if (value_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("host model revision exhausted");
    }
    ++value_;
}

class Bindings::Impl final {
  public:
    struct ActionSlot final {
        Impl* bindings = nullptr;
        Handler handler;
        strata_action_registration* registration = nullptr;

        ~ActionSlot() {
            if (registration != nullptr)
                strata_action_registration_release(registration);
        }

        static strata_action_handler_result invoke(void* const user_data,
                                                   const strata_action_call* const call) noexcept {
            auto& slot = *static_cast<ActionSlot*>(user_data);
            if (call == nullptr)
                return STRATA_ACTION_HANDLER_IGNORED;
            try {
                const std::string payload_json = copied(call->payload_json);
                const std::string event_json = copied(call->event_value_json);
                const ActionEvent event{
                    copied(call->action_id),
                    Value::parse(payload_json.empty() ? std::string_view("null")
                                                      : std::string_view(payload_json)),
                    copied(call->event_kind),
                    optional_copied(call->source_key),
                    Value::parse(event_json.empty() ? std::string_view("null")
                                                    : std::string_view(event_json)),
                };
                return encoded(slot.handler(event));
            } catch (...) {
                slot.bindings->capture_failure(std::current_exception());
                return STRATA_ACTION_HANDLER_IGNORED;
            }
        }
    };

    struct SnapshotSlot final {
        std::string id;
        RevisionSource revision;
        SnapshotSource source;
        std::optional<std::uint64_t> published_revision;
    };

    Impl(std::shared_ptr<detail::RuntimeControl> runtime, std::string owner)
        : runtime_(std::move(runtime)), owner_(std::move(owner)) {
        if (runtime_ == nullptr || runtime_->value == nullptr)
            throw std::invalid_argument("host bindings require a live runtime");
        if (owner_.empty())
            throw std::invalid_argument("host binding owner must not be empty");
    }

    void capture_failure(std::exception_ptr failure) noexcept {
        try {
            const std::scoped_lock lock(failure_mutex_);
            if (failure_ == nullptr)
                failure_ = std::move(failure);
        } catch (...) {
        }
    }

    void rethrow_failure() {
        std::exception_ptr failure;
        {
            const std::scoped_lock lock(failure_mutex_);
            failure = std::exchange(failure_, nullptr);
        }
        if (failure != nullptr)
            std::rethrow_exception(failure);
    }

    std::shared_ptr<detail::RuntimeControl> runtime_;
    std::string owner_;
    std::vector<std::unique_ptr<ActionSlot>> actions_;
    std::vector<SnapshotSlot> snapshots_;
    std::mutex failure_mutex_;
    std::exception_ptr failure_;
};

Bindings::Bindings(Runtime& runtime, std::string owner)
    : impl_(std::make_unique<Impl>(runtime.control_, std::move(owner))) {}
Bindings::~Bindings() = default;
Bindings::Bindings(Bindings&&) noexcept = default;
Bindings& Bindings::operator=(Bindings&&) noexcept = default;

void Bindings::on(std::string action_id, Handler handler) {
    if (impl_ == nullptr)
        throw std::logic_error("cannot use moved host bindings");
    if (action_id.empty() || !handler) {
        throw std::invalid_argument("host action binding requires an id and handler");
    }
    auto slot = std::make_unique<Impl::ActionSlot>();
    slot->bindings = impl_.get();
    slot->handler = std::move(handler);
    const strata_action_handler_config config{
        sizeof(strata_action_handler_config),
        strata::view(action_id),
        strata::view(impl_->owner_),
        slot.get(),
        &Impl::ActionSlot::invoke,
    };
    require_ok(strata_runtime_register_action_handler(impl_->runtime_->value, &config,
                                                      &slot->registration),
               "typed host action registration");
    impl_->actions_.push_back(std::move(slot));
}

void Bindings::snapshot(std::string snapshot_id, RevisionSource revision, SnapshotSource source) {
    if (impl_ == nullptr)
        throw std::logic_error("cannot use moved host bindings");
    if (snapshot_id.empty() || !revision || !source) {
        throw std::invalid_argument("host snapshot binding requires an id, revision, and source");
    }
    if (std::ranges::any_of(impl_->snapshots_, [&snapshot_id](const Impl::SnapshotSlot& slot) {
            return slot.id == snapshot_id;
        })) {
        throw std::invalid_argument("duplicate host snapshot binding '" + snapshot_id + "'");
    }
    impl_->snapshots_.push_back(Impl::SnapshotSlot{
        std::move(snapshot_id),
        std::move(revision),
        std::move(source),
        std::nullopt,
    });
}

void Bindings::synchronize() {
    if (impl_ == nullptr)
        throw std::logic_error("cannot use moved host bindings");
    impl_->rethrow_failure();
    for (Impl::SnapshotSlot& slot : impl_->snapshots_) {
        const std::uint64_t revision = slot.revision();
        if (revision == 0U) {
            throw std::logic_error("host snapshot source revision must be nonzero");
        }
        if (slot.published_revision.has_value()) {
            if (revision < *slot.published_revision) {
                throw std::logic_error("host snapshot source revision moved backwards");
            }
            if (revision == *slot.published_revision)
                continue;
        }
        const std::string json = slot.source().json();
        static_cast<void>(detail::publish_host_snapshot(impl_->runtime_, slot.id, json));
        slot.published_revision = revision;
    }
}

} // namespace strata::host
