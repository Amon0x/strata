#include "runtime/state.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include "core/utf8.hpp"

namespace strata::runtime {
namespace {

void validate_non_blank(const std::string_view value, const std::string_view label) {
    if (value.empty() || !core::valid_utf8(value)) {
        throw std::invalid_argument(std::string(label) + " must be non-empty valid UTF-8");
    }
}

[[nodiscard]] const Value* argument(
    const std::vector<std::pair<std::string, Value>>& arguments,
    const std::string_view name
) {
    const auto found = std::ranges::find(arguments, name, &std::pair<std::string, Value>::first);
    return found != arguments.end() ? &found->second : nullptr;
}

[[nodiscard]] std::optional<std::size_t> index_argument(
    const std::vector<std::pair<std::string, Value>>& arguments
) {
    const Value* value = argument(arguments, "index");
    const double* number = value != nullptr ? value->number() : nullptr;
    if (number == nullptr || *number < 0.0 ||
        *number > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::trunc(*number));
}

[[nodiscard]] StateMutationResult rejected(std::string message) {
    return StateMutationResult{StateMutationStatus::rejected, std::move(message)};
}

} // namespace

StateSlot StateSlot::from_initial(
    std::string name,
    Value initial_value,
    std::string declaration_scope
) {
    return StateSlot{
        std::move(name),
        std::string(initial_value.state_type_id()),
        std::move(initial_value),
        std::move(declaration_scope),
    };
}

StateStore::StateStore(Invalidation invalidation) : invalidation_(std::move(invalidation)) {}

const Value& StateStore::read(const StateAddress& address, const StateSlot& slot) {
    validate_address(address);
    validate_slot(slot);
    initial_values_.insert_or_assign(address, slot.initial_value);
    const std::string declaration_scope = slot.declaration_scope.empty()
                                              ? address.scope
                                              : slot.declaration_scope;
    auto [found, inserted] = values_.try_emplace(
        address,
        Stored{slot.type_id, slot.initial_value, declaration_scope}
    );
    if (!inserted && found->second.type_id != slot.type_id) {
        throw std::logic_error(
            "state slot '" + address.scope + "/" + address.name + "' changed type from '" +
            found->second.type_id + "' to '" + slot.type_id + "'"
        );
    }
    if (!inserted && found->second.declaration_scope != declaration_scope) {
        throw std::logic_error("state slot changed declaration ownership");
    }
    return found->second.value;
}

bool StateStore::write(const StateAddress& address, const StateSlot& slot, Value value) {
    validate_address(address);
    validate_slot(slot);
    initial_values_.insert_or_assign(address, slot.initial_value);
    if (value.state_type_id() != slot.type_id) {
        throw std::invalid_argument("state value does not match its slot type id");
    }
    const std::string declaration_scope = slot.declaration_scope.empty()
                                              ? address.scope
                                              : slot.declaration_scope;
    auto found = values_.find(address);
    if (found == values_.end()) {
        values_.emplace(address, Stored{slot.type_id, std::move(value), declaration_scope});
        changed();
        return true;
    }
    if (found->second.declaration_scope != declaration_scope) {
        throw std::logic_error("state slot changed declaration ownership");
    }
    if (found->second.type_id != slot.type_id) {
        throw std::logic_error(
            "state slot '" + address.scope + "/" + address.name + "' changed type from '" +
            found->second.type_id + "' to '" + slot.type_id + "'"
        );
    }
    if (found->second.value == value) return false;
    found->second.value = std::move(value);
    changed();
    return true;
}

bool StateStore::reset(const StateAddress& address) {
    validate_address(address);
    if (values_.erase(address) == 0U) return false;
    changed();
    return true;
}

const Value* StateStore::find(const StateAddress& address) const noexcept {
    const auto found = values_.find(address);
    return found != values_.end() ? &found->second.value : nullptr;
}

const Value* StateStore::initial(const StateAddress& address) const noexcept {
    const auto found = initial_values_.find(address);
    return found != initial_values_.end() ? &found->second : nullptr;
}

StateSnapshot StateStore::snapshot() const {
    std::vector<StateSnapshotEntry> entries;
    entries.reserve(values_.size());
    for (const auto& [address, stored] : values_) {
        entries.push_back(StateSnapshotEntry{
            address,
            stored.type_id,
            stored.value,
            stored.declaration_scope,
        });
    }
    return StateSnapshot{std::move(entries)};
}

bool StateStore::restore(const StateSnapshot& snapshot_value) {
    std::map<StateAddress, Stored> restored;
    for (const StateSnapshotEntry& entry : snapshot_value.entries) {
        validate_address(entry.address);
        validate_non_blank(entry.type_id, "state snapshot type id");
        const std::string declaration_scope = entry.declaration_scope.empty()
                                                  ? entry.address.scope
                                                  : entry.declaration_scope;
        validate_non_blank(declaration_scope, "state snapshot declaration scope");
        if (entry.value.state_type_id() != entry.type_id) {
            throw std::invalid_argument("state snapshot value does not match its type id");
        }
        if (!restored.emplace(
                         entry.address,
                         Stored{entry.type_id, entry.value, declaration_scope}
                     )
                 .second) {
            throw std::invalid_argument("state snapshot addresses must be unique");
        }
    }
    if (values_ == restored) return false;
    values_ = std::move(restored);
    changed();
    return true;
}

bool StateStore::migrate_declarations(const std::vector<StateDeclarationSchema>& declarations) {
    std::map<StateAddress, std::string> schemas;
    StateScopeSet scopes;
    for (const StateDeclarationSchema& declaration : declarations) {
        validate_address(StateAddress{declaration.scope, declaration.name});
        validate_non_blank(declaration.type_id, "state declaration type id");
        scopes.insert(declaration.scope);
        if (!schemas.emplace(
                         StateAddress{declaration.scope, declaration.name},
                         declaration.type_id
                     )
                 .second) {
            throw std::invalid_argument("state declaration addresses must be unique");
        }
    }

    bool removed = false;
    for (auto value = values_.begin(); value != values_.end();) {
        const bool declaration_owned = declaration_scopes_.contains(value->second.declaration_scope) ||
                                       scopes.contains(value->second.declaration_scope);
        if (!declaration_owned) {
            ++value;
            continue;
        }
        const auto schema = schemas.find(StateAddress{
            value->second.declaration_scope,
            value->first.name,
        });
        if (schema == schemas.end() ||
            (schema->second != "dsl.unknown" && schema->second != value->second.type_id)) {
            initial_values_.erase(value->first);
            value = values_.erase(value);
            removed = true;
        } else {
            ++value;
        }
    }
    declaration_scopes_ = std::move(scopes);
    if (removed) changed();
    return removed;
}

void StateStore::mark_owned_scope(std::string scope) {
    validate_non_blank(scope, "owned state scope");
    owned_scopes_.insert(std::move(scope));
}

bool StateStore::retain_owned_scopes(const StateScopeSet& attached_scopes) {
    bool removed = false;
    for (auto iterator = owned_scopes_.begin(); iterator != owned_scopes_.end();) {
        if (attached_scopes.contains(*iterator)) {
            ++iterator;
            continue;
        }
        const std::string scope = *iterator;
        iterator = owned_scopes_.erase(iterator);
        for (auto value = values_.begin(); value != values_.end();) {
            if (value->first.scope == scope) {
                initial_values_.erase(value->first);
                value = values_.erase(value);
                removed = true;
            } else {
                ++value;
            }
        }
    }
    if (removed) changed();
    return removed;
}

bool StateStore::retain_declaration_scopes(const StateScopeSet& attached_scopes) {
    bool removed = false;
    for (const std::string& scope : declaration_scopes_) {
        if (attached_scopes.contains(scope)) continue;
        for (auto value = values_.begin(); value != values_.end();) {
            if (value->second.declaration_scope == scope) {
                initial_values_.erase(value->first);
                value = values_.erase(value);
                removed = true;
            } else {
                ++value;
            }
        }
    }
    declaration_scopes_ = attached_scopes;
    if (removed) changed();
    return removed;
}

std::uint64_t StateStore::generation() const noexcept { return generation_; }
std::size_t StateStore::size() const noexcept { return values_.size(); }

void StateStore::changed() {
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("state generation exhausted");
    }
    ++generation_;
    if (invalidation_) invalidation_();
}

void StateStore::validate_address(const StateAddress& address) {
    validate_non_blank(address.scope, "state scope");
    validate_non_blank(address.name, "state slot name");
}

void StateStore::validate_slot(const StateSlot& slot) {
    validate_non_blank(slot.name, "state slot name");
    validate_non_blank(slot.type_id, "state slot type id");
    if (slot.initial_value.state_type_id() != slot.type_id) {
        throw std::invalid_argument("state slot initial value does not match its type id");
    }
    if (!slot.declaration_scope.empty()) {
        validate_non_blank(slot.declaration_scope, "state declaration scope");
    }
}

StateMutationResult StateMutation::apply(StateStore& state) const {
    const StateSlot slot = StateSlot::from_initial(
        address.name,
        initial_value,
        declaration_scope
    );
    if (kind == StateMutationKind::reset) {
        return StateMutationResult{
            state.reset(address) ? StateMutationStatus::changed : StateMutationStatus::unchanged,
            {},
        };
    }
    const Value& current = state.read(address, slot);
    Value next;
    switch (kind) {
    case StateMutationKind::set: {
        const Value* value = argument(arguments, "value");
        if (value == nullptr || value->state_type_id() != slot.type_id ||
            (declared_schema != nullptr && !declared_schema->accepts(*value))) {
            return rejected("state.set value does not satisfy the retained slot type");
        }
        next = *value;
        break;
    }
    case StateMutationKind::toggle: {
        const bool* value = current.boolean();
        if (value == nullptr) return rejected("state.toggle requires boolean state");
        next = Value(!*value);
        break;
    }
    case StateMutationKind::adjust: {
        const double* value = current.number();
        const Value* amount_value = argument(arguments, "amount");
        const double* amount = amount_value != nullptr ? amount_value->number() : nullptr;
        if (value == nullptr || amount == nullptr || !std::isfinite(*value + *amount)) {
            return rejected("state.adjust requires a finite numeric result");
        }
        next = Value(*value + *amount);
        break;
    }
    case StateMutationKind::list_append:
    case StateMutationKind::list_insert:
    case StateMutationKind::list_remove_value:
    case StateMutationKind::list_remove_at:
    case StateMutationKind::list_toggle:
    case StateMutationKind::list_clear: {
        const ValueList* list = current.list();
        if (list == nullptr) return rejected("list mutation requires list state");
        std::vector<Value> values = list->values;
        const Value* value = argument(arguments, "value");
        const ValueSchema* element_schema = declared_schema != nullptr &&
                                                    declared_schema->kind() == ValueSchemaKind::list
                                                ? declared_schema->element().get()
                                                : nullptr;
        if (value != nullptr && element_schema != nullptr && !element_schema->accepts(*value)) {
            return rejected("list mutation value does not satisfy the element type");
        }
        if (kind == StateMutationKind::list_append) {
            if (value == nullptr) return rejected("state.listAppend requires value");
            values.push_back(*value);
        } else if (kind == StateMutationKind::list_insert) {
            const std::optional<std::size_t> index = index_argument(arguments);
            if (value == nullptr || !index.has_value() || *index > values.size()) {
                return rejected("state.listInsert index or value is invalid");
            }
            values.insert(values.begin() + static_cast<std::ptrdiff_t>(*index), *value);
        } else if (kind == StateMutationKind::list_remove_value) {
            if (value == nullptr) return rejected("state.listRemoveValue requires value");
            const auto found = std::ranges::find(values, *value);
            if (found != values.end()) values.erase(found);
        } else if (kind == StateMutationKind::list_remove_at) {
            const std::optional<std::size_t> index = index_argument(arguments);
            if (!index.has_value() || *index >= values.size()) {
                return rejected("state.listRemoveAt index is outside the list");
            }
            values.erase(values.begin() + static_cast<std::ptrdiff_t>(*index));
        } else if (kind == StateMutationKind::list_toggle) {
            if (value == nullptr) return rejected("state.listToggle requires value");
            const auto found = std::ranges::find(values, *value);
            if (found != values.end()) {
                values.erase(found);
            } else {
                values.push_back(*value);
            }
        } else {
            values.clear();
        }
        if (declared_schema != nullptr && declared_schema->maximum_items().has_value() &&
            values.size() > *declared_schema->maximum_items()) {
            return rejected("list mutation exceeds the declared maximum item count");
        }
        next = Value(std::move(values));
        break;
    }
    case StateMutationKind::record_set: {
        const ValueObject* object = current.object();
        const Value* field_value = argument(arguments, "field");
        const Value* value = argument(arguments, "value");
        const std::string* field_name = field_value != nullptr ? field_value->string() : nullptr;
        if (object == nullptr || field_name == nullptr || value == nullptr || field_name->empty()) {
            return rejected("state.recordSet requires object state, field, and value");
        }
        if (declared_schema != nullptr && declared_schema->kind() == ValueSchemaKind::object) {
            const ValueSchemaField* field_schema = declared_schema->field(*field_name);
            if (field_schema == nullptr && !declared_schema->allow_unknown_fields()) {
                return rejected("state.recordSet field is not declared");
            }
            const ValueSchemaPtr& schema = field_schema != nullptr
                                               ? field_schema->schema
                                               : declared_schema->unknown_field_schema();
            if (schema != nullptr && !schema->accepts(*value)) {
                return rejected("state.recordSet value does not satisfy the field type");
            }
        }
        std::vector<std::pair<std::string, Value>> fields = object->fields;
        const auto found = std::ranges::lower_bound(
            fields,
            *field_name,
            {},
            &std::pair<std::string, Value>::first
        );
        if (found != fields.end() && found->first == *field_name) {
            found->second = *value;
        } else {
            fields.insert(found, std::pair<std::string, Value>{*field_name, *value});
        }
        next = Value(std::move(fields));
        break;
    }
    case StateMutationKind::reset:
        throw std::logic_error("reset mutation was not handled");
    }

    const bool did_change = state.write(address, slot, std::move(next));
    return StateMutationResult{
        did_change ? StateMutationStatus::changed : StateMutationStatus::unchanged,
        {},
    };
}

} // namespace strata::runtime
