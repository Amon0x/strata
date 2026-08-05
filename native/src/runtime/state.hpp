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

/**
 * Lexically resolved target captured by a framework state action while its expression is evaluated.
 * The address owns retained instance identity; declaration_scope owns schema/initializer lookup.
 */
struct LexicalStateBinding final {
    StateAddress address;
    std::string declaration_scope;

    [[nodiscard]] friend bool operator==(const LexicalStateBinding&, const LexicalStateBinding&) = default;
};

struct StateSlot final {
    std::string name;
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

    explicit StateStore(Invalidation invalidation = {});

    [[nodiscard]] const Value& read(const StateAddress& address, const StateSlot& slot);
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
    [[nodiscard]] bool retain_declaration_scopes(const StateScopeSet& attached_scopes);

    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct Stored final {
        std::string type_id;
        Value value;
        std::string declaration_scope;

        [[nodiscard]] friend bool operator==(const Stored&, const Stored&) = default;
    };

    void changed();
    static void validate_address(const StateAddress& address);
    static void validate_slot(const StateSlot& slot);

    std::map<StateAddress, Stored> values_;
    std::map<StateAddress, Value> initial_values_;
    StateScopeSet owned_scopes_;
    StateScopeSet declaration_scopes_;
    Invalidation invalidation_;
    std::uint64_t generation_ = 0U;
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

    [[nodiscard]] StateMutationResult apply(StateStore& state) const;
};

} // namespace strata::runtime
