#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/value.hpp"
#include "runtime/value_schema.hpp"

namespace strata::runtime {

/** Canonical ordered state-scope set with transparent lookup. */
using StateScopeSet = std::set<std::string, std::less<>>;

struct StateAddress final {
    std::string scope;
    std::string name;

    [[nodiscard]] friend bool operator==(const StateAddress&, const StateAddress&) = default;
    [[nodiscard]] friend auto operator<=>(const StateAddress&, const StateAddress&) = default;
};

/** A borrowed StateAddress, for looking one up without building its strings. */
struct StateAddressView final {
    std::string_view scope;
    std::string_view name;

    [[nodiscard]] friend bool operator==(const StateAddressView&,
                                         const StateAddressView&) = default;
    [[nodiscard]] friend auto operator<=>(const StateAddressView&,
                                          const StateAddressView&) = default;
    [[nodiscard]] friend auto operator<=>(const StateAddress& left,
                                          const StateAddressView& right) noexcept {
        return StateAddressView{left.scope, left.name} <=> right;
    }
    [[nodiscard]] friend bool operator==(const StateAddress& left,
                                         const StateAddressView& right) noexcept {
        return left.scope == right.scope && left.name == right.name;
    }
};

/**
 * Lexically resolved target captured by a framework state action while its expression is evaluated.
 * The address owns retained instance identity; declaration_scope owns schema/initializer lookup.
 */
struct LexicalStateBinding final {
    StateAddress address;
    std::string declaration_scope;

    [[nodiscard]] friend bool operator==(const LexicalStateBinding&, const LexicalStateBinding&) = default;
};

/**
 * The type id of a state whose values need not share one kind (a nullable or application type,
 * or an initializer read from elsewhere): its declared schema, not its kind, says what it holds.
 */
inline constexpr std::string_view any_state_type_id = "dsl.any";

/** Whether a slot of `type_id` holds `value`: its kind, unless the slot takes any. */
[[nodiscard]] bool state_type_accepts(std::string_view type_id, const Value& value) noexcept;

struct StateSlot final {
    std::string name;
    /** Stable for the declaration: a value kind (`Value::state_type_id`) or `any_state_type_id`. */
    std::string type_id;
    Value initial_value;
    std::string declaration_scope;

    static StateSlot from_initial(
        std::string name,
        Value initial_value,
        std::string declaration_scope = {}
    );
};

struct StateSnapshotEntry final {
    StateAddress address;
    std::string type_id;
    Value value;
    std::string declaration_scope;

    [[nodiscard]] friend bool operator==(const StateSnapshotEntry&, const StateSnapshotEntry&) = default;
};

struct StateSnapshot final {
    std::vector<StateSnapshotEntry> entries;
    [[nodiscard]] friend bool operator==(const StateSnapshot&, const StateSnapshot&) = default;
};

struct StateDeclarationSchema final {
    std::string scope;
    std::string name;
    std::string type_id;

    [[nodiscard]] friend bool operator==(const StateDeclarationSchema&, const StateDeclarationSchema&) = default;
};

class StateStore final {
public:
    using Invalidation = std::function<void()>;
    /**
     * Told the address of each value written, reset or dropped, or null when a restore or a
     * migration replaced any number of them at once. An observer must not change the store.
     */
    using ChangeObserver = std::function<void(const StateAddress* address)>;

    explicit StateStore(Invalidation invalidation = {});

    [[nodiscard]] const Value& read(const StateAddress& address, const StateSlot& slot);
    /**
     * The value the slot holds, checked against the slot as read() checks it, without storing
     * the slot's initial value: null while it holds none.
     */
    [[nodiscard]] const Value* peek(const StateAddress& address, const StateSlot& slot) const;
    [[nodiscard]] bool write(const StateAddress& address, const StateSlot& slot, Value value);
    [[nodiscard]] bool reset(const StateAddress& address);
    [[nodiscard]] const Value* find(const StateAddress& address) const noexcept;
    /** Initial value evaluated in the live lexical component instance that owns this address. */
    [[nodiscard]] const Value* initial(const StateAddress& address) const noexcept;
    [[nodiscard]] StateSnapshot snapshot() const;
    [[nodiscard]] bool restore(const StateSnapshot& snapshot);
    /** Drops only declaration-owned values removed or made type-incompatible by activation. */
    [[nodiscard]] bool migrate_declarations(const std::vector<StateDeclarationSchema>& declarations);

    void mark_owned_scope(std::string scope);
    [[nodiscard]] bool retain_owned_scopes(const StateScopeSet& attached_scopes);

    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    /** Registers an observer of changed addresses; the token stops it. */
    [[nodiscard]] std::uint64_t observe_changes(ChangeObserver observer);
    void stop_observing_changes(std::uint64_t token) noexcept;

private:
    struct Stored final {
        std::string type_id;
        Value value;
        std::string declaration_scope;

        [[nodiscard]] friend bool operator==(const Stored&, const Stored&) = default;
    };

    void changed();
    void notify(const StateAddress* address) const;
    static void validate_address(const StateAddress& address);
    static void validate_slot(const StateSlot& slot);

    std::map<StateAddress, Stored> values_;
    std::map<StateAddress, Value> initial_values_;
    StateScopeSet owned_scopes_;
    StateScopeSet declaration_scopes_;
    Invalidation invalidation_;
    std::uint64_t generation_ = 0U;
    std::vector<std::pair<std::uint64_t, ChangeObserver>> change_observers_;
    std::uint64_t next_change_observer_ = 1U;
};

enum class StateMutationKind {
    set,
    toggle,
    adjust,
    reset,
    list_append,
    list_insert,
    list_remove_value,
    list_remove_at,
    list_toggle,
    list_clear,
    record_set,
};

enum class StateMutationStatus { changed, unchanged, rejected };

struct StateMutationResult final {
    StateMutationStatus status;
    std::string message;
};

struct StateMutation final {
    StateAddress address;
    Value initial_value;
    ValueSchemaPtr declared_schema;
    StateMutationKind kind;
    std::vector<std::pair<std::string, Value>> arguments;
    std::string declaration_scope{};
    /** The declaration's slot type id; empty takes the initial value's kind. */
    std::string type_id{};

    [[nodiscard]] StateMutationResult apply(StateStore& state) const;
};

} // namespace strata::runtime
