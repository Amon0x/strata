#include <strata/host.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <exception>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "data/json.hpp"
#include "host_json.hpp"

namespace strata::host {
namespace {

/**
 * Writes `value` as JSON straight from its fields: canonical order (an Object's keys are sorted
 * already) and numbers, without whitespace or a copy into data::JsonValue first. A snapshot of
 * thousands of rows is this text, which the runtime parses, so its size and cost are the
 * snapshot's.
 */
void append_compact(std::string& output, const Value& value) {
    if (value.is_null()) {
        output.append("null");
    } else if (const bool* boolean = value.boolean(); boolean != nullptr) {
        output.append(*boolean ? "true" : "false");
    } else if (const std::int64_t* integer = value.integer(); integer != nullptr) {
        char buffer[32]{};
        const auto converted = std::to_chars(std::begin(buffer), std::end(buffer), *integer);
        output.append(buffer, converted.ptr);
    } else if (const double* number = value.number(); number != nullptr) {
        data::append_json_number(output, *number);
    } else if (const std::string* string = value.string(); string != nullptr) {
        data::append_json_string(output, *string);
    } else if (const Value::Array* values = value.array(); values != nullptr) {
        output.push_back('[');
        for (std::size_t index = 0U; index < values->size(); ++index) {
            if (index != 0U)
                output.push_back(',');
            append_compact(output, (*values)[index]);
        }
        output.push_back(']');
    } else {
        output.push_back('{');
        bool first = true;
        for (const auto& [name, child] : *value.object()) {
            if (!first)
                output.push_back(',');
            first = false;
            data::append_json_string(output, name);
            output.push_back(':');
            append_compact(output, child);
        }
        output.push_back('}');
    }
}

} // namespace

Value from_json_value(const data::JsonValue& value) {
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

namespace {

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
    // The host parses documents it already holds, including this library's own canonical frame
    // snapshots, which outgrow the runtime's untrusted-input byte budget on large surfaces. Depth,
    // value-count and string limits still bound the structure.
    data::JsonLimits limits;
    limits.maximum_input_bytes = std::max(limits.maximum_input_bytes, json.size());
    return from_json_value(data::parse_json(json, limits));
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
    std::string output;
    append_compact(output, *this);
    return output;
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

namespace {

/** Independent synchronized error storage; preparation endpoints never reach into owner maps. */
class BindingFailures final {
  public:
    void capture(std::exception_ptr failure) noexcept {
        try {
            const std::scoped_lock lock(mutex_);
            if (failure_ == nullptr)
                failure_ = std::move(failure);
        } catch (...) {
        }
    }

    void rethrow() {
        std::exception_ptr failure;
        {
            const std::scoped_lock lock(mutex_);
            failure = std::exchange(failure_, nullptr);
        }
        if (failure != nullptr)
            std::rethrow_exception(failure);
    }

  private:
    std::mutex mutex_;
    std::exception_ptr failure_;
};

struct ActionEndpoint;

/** Only the owner retains the handler; weak preparation endpoints cannot destroy UI callbacks. */
struct BindingAction final {
    std::string id;
    Bindings::Handler handler;
    std::shared_ptr<ActionEndpoint> endpoint;
    strata_action_registration* registration = nullptr;

    ~BindingAction() {
        if (registration != nullptr)
            strata_action_registration_release(registration);
    }
};

/** Immutable identity and weak dispatch target, enabled only by the serialized adopting owner. */
struct ActionEndpoint final {
    std::string id;
    std::weak_ptr<BindingAction> target;
    std::shared_ptr<BindingFailures> failures;
    std::atomic_bool adopted{false};

    static strata_action_handler_result invoke(void* const context,
                                               const strata_action_call* const call) noexcept {
        auto& endpoint = *static_cast<ActionEndpoint*>(context);
        if (call == nullptr)
            return STRATA_ACTION_HANDLER_IGNORED;
        try {
            if (!endpoint.adopted.load(std::memory_order_acquire))
                throw std::logic_error("prepared host action dispatched before owner adoption");
            const auto handler = endpoint.target.lock();
            if (handler == nullptr)
                throw std::logic_error("host action binding has retired");
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
            return encoded(handler->handler(event));
        } catch (...) {
            endpoint.failures->capture(std::current_exception());
            return STRATA_ACTION_HANDLER_IGNORED;
        }
    }
};

std::shared_ptr<ActionEndpoint> action_endpoint(const std::shared_ptr<BindingAction>& action,
                                                const std::shared_ptr<BindingFailures>& failures) {
    auto endpoint = std::make_shared<ActionEndpoint>();
    endpoint->id = action->id;
    endpoint->target = action;
    endpoint->failures = failures;
    return endpoint;
}

void install_action(strata_runtime* const runtime, const std::string_view owner,
                    ActionEndpoint& endpoint, strata_action_registration*& registration) {
    const strata_action_handler_config config{
        sizeof(strata_action_handler_config), strata::view(endpoint.id), strata::view(owner),
        &endpoint, &ActionEndpoint::invoke,
    };
    require_ok(strata_runtime_register_action_handler(runtime, &config, &registration),
               "typed host action registration");
}

} // namespace

struct ActionBindings::Impl final {
    struct Entry final {
        std::shared_ptr<ActionEndpoint> endpoint;
        strata_action_registration* registration = nullptr;
    };

    ~Impl() { release(); }

    void release() noexcept {
        for (Entry& entry : entries) {
            if (entry.registration != nullptr)
                strata_action_registration_release(std::exchange(entry.registration, nullptr));
        }
        runtime = nullptr;
    }

    std::string owner;
    std::shared_ptr<BindingFailures> failures;
    std::vector<Entry> entries;
    strata_runtime* runtime = nullptr;
};

ActionBindings::ActionBindings() noexcept = default;
ActionBindings::~ActionBindings() = default;
ActionBindings::ActionBindings(ActionBindings&&) noexcept = default;
ActionBindings& ActionBindings::operator=(ActionBindings&&) noexcept = default;

bool ActionBindings::empty() const noexcept {
    return impl_ == nullptr || impl_->entries.empty();
}

void ActionBindings::install(Runtime& runtime) {
    if (empty())
        return;
    if (impl_->runtime != nullptr)
        throw std::logic_error("captured actions are already installed");
    try {
        for (Impl::Entry& entry : impl_->entries) {
            if (entry.endpoint->target.expired())
                throw std::logic_error("captured host action owner has retired");
            install_action(runtime.native_handle(), impl_->owner, *entry.endpoint, entry.registration);
        }
        impl_->runtime = runtime.native_handle();
    } catch (...) {
        impl_->release();
        throw;
    }
}

void ActionBindings::rethrow_failure() {
    if (impl_ != nullptr)
        impl_->failures->rethrow();
}

class Bindings::Impl final {
  public:
    struct PendingRevision final {
        std::uint64_t revision;
        HostPublication publication;
    };

    struct SnapshotSlot final {
        std::string id;
        RevisionSource revision;
        SnapshotSource source;
        std::optional<std::uint64_t> published_revision;
        std::optional<std::uint64_t> observed_revision;
        std::vector<PendingRevision> pending;
    };

    /** Consume every terminal receipt, including failures preceding a newer accepted revision. */
    static void adopt_revisions(SnapshotSlot& slot) {
        while (!slot.pending.empty()) {
            const auto info = slot.pending.front().publication.info();
            if (info.status == STRATA_HOST_PUBLICATION_PENDING) return;
            PendingRevision completed = std::move(slot.pending.front());
            slot.pending.erase(slot.pending.begin());
            if (info.status == STRATA_HOST_PUBLICATION_ADOPTED) {
                slot.published_revision = completed.revision;
            } else if (info.status == STRATA_HOST_PUBLICATION_FAILED) {
                throw AbiError(info.result,
                               "host snapshot publication: " + std::string(info.message.data, info.message.size));
            } else {
                throw std::runtime_error("host snapshot publication was cancelled");
            }
        }
    }

    explicit Impl(std::string owner) : owner_(std::move(owner)) {
        if (owner_.empty())
            throw std::invalid_argument("host binding owner must not be empty");
    }

    void rethrow_failure() {
        failures_->rethrow();
    }

    std::shared_ptr<detail::RuntimeControl> runtime_;
    std::string owner_;
    std::vector<std::shared_ptr<BindingAction>> actions_;
    std::vector<SnapshotSlot> snapshots_;
    std::shared_ptr<BindingFailures> failures_ = std::make_shared<BindingFailures>();
};

Bindings::Bindings(std::string owner) : impl_(std::make_unique<Impl>(std::move(owner))) {}
Bindings::Bindings(Runtime& runtime, std::string owner) : Bindings(std::move(owner)) {
    attach(runtime);
}
Bindings::~Bindings() = default;
Bindings::Bindings(Bindings&&) noexcept = default;
Bindings& Bindings::operator=(Bindings&&) noexcept = default;

std::vector<CapturedSnapshot> Bindings::capture_snapshots() {
    if (impl_ == nullptr)
        throw std::logic_error("cannot use moved host bindings");
    impl_->rethrow_failure();
    std::vector<CapturedSnapshot> result;
    result.reserve(impl_->snapshots_.size());
    for (const Impl::SnapshotSlot& slot : impl_->snapshots_) {
        const std::uint64_t revision = slot.revision();
        if (revision == 0U)
            throw std::logic_error("host snapshot source revision must be nonzero");
        result.push_back(CapturedSnapshot{{slot.id, revision}, slot.source()});
    }
    return result;
}

ActionBindings Bindings::capture_actions() const {
    if (impl_ == nullptr || impl_->runtime_ != nullptr)
        throw std::logic_error("only unattached owner bindings can prepare action availability");
    ActionBindings result;
    if (impl_->actions_.empty())
        return result;
    result.impl_ = std::make_unique<ActionBindings::Impl>();
    result.impl_->owner = impl_->owner_;
    result.impl_->failures = std::make_shared<BindingFailures>();
    result.impl_->entries.reserve(impl_->actions_.size());
    for (const auto& slot : impl_->actions_)
        result.impl_->entries.push_back({action_endpoint(slot, result.impl_->failures), nullptr});
    return result;
}

void Bindings::attach(Runtime& runtime, const std::span<const SnapshotRevision> published,
                      ActionBindings actions) {
    if (impl_ == nullptr || impl_->runtime_ != nullptr)
        throw std::logic_error("host bindings must be unattached before adoption");
    if (runtime.control_ == nullptr || runtime.control_->value == nullptr)
        throw std::invalid_argument("host bindings require a live runtime");
    const std::size_t installed = actions.empty() ? 0U : actions.impl_->entries.size();
    if (installed != 0U) {
        if (actions.impl_->runtime != runtime.control_->value || installed > impl_->actions_.size())
            throw std::invalid_argument("captured actions belong to another runtime or owner");
        for (std::size_t index = 0U; index < installed; ++index) {
            if (actions.impl_->entries[index].endpoint->target.lock() != impl_->actions_[index])
                throw std::invalid_argument("captured actions must return to their original bindings");
        }
        actions.rethrow_failure();
    }
    std::size_t cursor = 0U;
    for (const Impl::SnapshotSlot& slot : impl_->snapshots_) {
        if (cursor < published.size() && published[cursor].id == slot.id) {
            if (published[cursor].revision == 0U)
                throw std::invalid_argument("an adopted snapshot revision must be nonzero");
            ++cursor;
        }
    }
    if (cursor != published.size())
        throw std::invalid_argument("adopted snapshot revisions must follow capture order");
    const auto failures = installed != 0U ? actions.impl_->failures : impl_->failures_;
    try {
        for (std::size_t index = installed; index < impl_->actions_.size(); ++index) {
            BindingAction& slot = *impl_->actions_[index];
            slot.endpoint = action_endpoint(impl_->actions_[index], failures);
            install_action(runtime.control_->value, impl_->owner_, *slot.endpoint, slot.registration);
        }
    } catch (...) {
        for (const auto& slot : impl_->actions_) {
            if (slot->registration != nullptr)
                strata_action_registration_release(std::exchange(slot->registration, nullptr));
        }
        throw;
    }
    for (std::size_t index = 0U; index < installed; ++index) {
        impl_->actions_[index]->endpoint = std::move(actions.impl_->entries[index].endpoint);
        impl_->actions_[index]->registration =
            std::exchange(actions.impl_->entries[index].registration, nullptr);
    }
    impl_->failures_ = failures;
    impl_->runtime_ = runtime.control_;
    for (const auto& slot : impl_->actions_)
        slot->endpoint->adopted.store(true, std::memory_order_release);
    cursor = 0U;
    for (Impl::SnapshotSlot& slot : impl_->snapshots_) {
        if (cursor < published.size() && published[cursor].id == slot.id) {
            slot.published_revision = published[cursor++].revision;
            slot.observed_revision = slot.published_revision;
        }
    }
}

void Bindings::on(const std::string_view action_id, Handler handler) {
    if (impl_ == nullptr)
        throw std::logic_error("cannot use moved host bindings");
    if (action_id.empty() || !handler) {
        throw std::invalid_argument("host action binding requires an id and handler");
    }
    auto slot = std::make_shared<BindingAction>();
    slot->id = action_id;
    slot->handler = std::move(handler);
    if (impl_->runtime_ != nullptr) {
        slot->endpoint = action_endpoint(slot, impl_->failures_);
        install_action(impl_->runtime_->value, impl_->owner_, *slot->endpoint, slot->registration);
        slot->endpoint->adopted.store(true, std::memory_order_release);
    }
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
        std::nullopt,
        {},
    });
}

void Bindings::synchronize() {
    if (impl_ == nullptr)
        throw std::logic_error("cannot use moved host bindings");
    if (impl_->runtime_ == nullptr)
        throw std::logic_error("host bindings are not attached");
    impl_->rethrow_failure();
    require_ok(strata_runtime_poll_host_snapshots(impl_->runtime_->value), "host snapshot adoption");
    for (Impl::SnapshotSlot& slot : impl_->snapshots_) {
        Impl::adopt_revisions(slot);
        const std::uint64_t revision = slot.revision();
        if (revision == 0U) {
            throw std::logic_error("host snapshot source revision must be nonzero");
        }
        if (slot.observed_revision.has_value() && revision < *slot.observed_revision)
            throw std::logic_error("host snapshot source revision moved backwards");
        slot.observed_revision = revision;
        if (slot.published_revision == revision ||
            (!slot.pending.empty() && slot.pending.back().revision == revision))
            continue;
        const std::string json = slot.source().json();
        if (slot.pending.size() == slot.pending.capacity())
            slot.pending.reserve(std::max<std::size_t>(2U, slot.pending.size() * 2U));
        slot.pending.push_back(Impl::PendingRevision{
            revision, detail::publish_host_snapshot(impl_->runtime_, slot.id, json)});
        Impl::adopt_revisions(slot);
    }
}

} // namespace strata::host
