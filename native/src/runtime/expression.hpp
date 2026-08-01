#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "data/json.hpp"
#include "data/json_view.hpp"
#include "runtime/action.hpp"
#include "runtime/diagnostic.hpp"
#include "runtime/host.hpp"
#include "runtime/registry.hpp"
#include "runtime/state.hpp"
#include "runtime/value.hpp"

namespace strata::runtime {

/** Canonical immutable identity shared by collection caches and retained properties. */
struct CollectionViewImmutableIdentity {
    Value items;
    std::size_t total = 0U;
    std::size_t matched = 0U;
    std::size_t range_start = 0U;
    std::size_t range_end_exclusive = 0U;
    std::string operation;
    std::uint64_t rebuilds = 1U;

    [[nodiscard]] friend bool operator==(
        const CollectionViewImmutableIdentity&,
        const CollectionViewImmutableIdentity&
    ) = default;
};

struct CollectionViewValue final : CollectionViewImmutableIdentity {
    mutable std::atomic<std::uint64_t> cache_hits{0U};
};

[[nodiscard]] inline const CollectionViewImmutableIdentity&
collection_view_immutable_identity(
    const CollectionViewImmutableIdentity& value
) noexcept {
    return value;
}

/** Exact leaf read, including whether the selected root was contextual and whether it existed. */
struct ExpressionHostDependency final {
    std::vector<HostPathSegment> path;
    bool contextual = false;
    std::optional<Value> value;
    std::optional<std::string> snapshot_id;
    std::uint64_t snapshot_generation = 0U;

    [[nodiscard]] friend bool operator==(
        const ExpressionHostDependency& left,
        const ExpressionHostDependency& right
    ) {
        return left.path == right.path && left.contextual == right.contextual &&
            left.value == right.value;
    }
};

[[nodiscard]] std::string canonical_host_dependency_path(
    std::span<const HostPathSegment> path
);

enum class ActionCompositionMode { sequence, parallel };

struct ActionValue final {
    ActionValue() = default;
    ActionValue(
        std::shared_ptr<const Action> action,
        std::optional<ActionCompositionMode> composition,
        std::vector<std::shared_ptr<const ActionValue>> children,
        std::optional<LexicalStateBinding> lexical_state_binding = std::nullopt
    ) : action(std::move(action)),
        composition(composition),
        children(std::move(children)),
        lexical_state_binding(std::move(lexical_state_binding)) {}

    std::shared_ptr<const Action> action;
    std::optional<ActionCompositionMode> composition;
    std::vector<std::shared_ptr<const ActionValue>> children;
    std::optional<LexicalStateBinding> lexical_state_binding;
};

struct ExpressionListValue;
struct ExpressionObjectValue;
struct LambdaValue;

class ExpressionValue final {
public:
    using Storage = std::variant<
        Value,
        std::shared_ptr<const CollectionViewValue>,
        std::shared_ptr<const LambdaValue>,
        std::shared_ptr<const ActionValue>,
        std::shared_ptr<const ExpressionListValue>,
        std::shared_ptr<const ExpressionObjectValue>
    >;

    ExpressionValue();
    explicit ExpressionValue(Value value);
    explicit ExpressionValue(std::shared_ptr<const CollectionViewValue> value);
    explicit ExpressionValue(std::shared_ptr<const LambdaValue> value);
    explicit ExpressionValue(std::shared_ptr<const ActionValue> value);
    explicit ExpressionValue(std::shared_ptr<const ExpressionListValue> value);
    explicit ExpressionValue(std::shared_ptr<const ExpressionObjectValue> value);

    [[nodiscard]] const Value* value() const noexcept;
    /** Materialized data consumed by widgets, including collection-view items. */
    [[nodiscard]] const Value* data_value() const noexcept;
    [[nodiscard]] const std::shared_ptr<const CollectionViewValue>* collection() const noexcept;
    [[nodiscard]] const std::shared_ptr<const LambdaValue>* lambda() const noexcept;
    [[nodiscard]] const std::shared_ptr<const ActionValue>* action() const noexcept;
    [[nodiscard]] const std::shared_ptr<const ExpressionListValue>* list() const noexcept;
    [[nodiscard]] const std::shared_ptr<const ExpressionObjectValue>* object() const noexcept;

private:
    Storage storage_;
};

/** Executable composites retain nested actions/lambdas and expose an immutable scalar projection. */
struct ExpressionListValue final {
    Value materialized;
    std::vector<ExpressionValue> values;
};

struct ExpressionObjectValue final {
    Value materialized;
    std::vector<std::pair<std::string, ExpressionValue>> fields;

    [[nodiscard]] const ExpressionValue* field(std::string_view name) const noexcept;
};

enum class ExpressionDependencyValueKind {
    scalar,
    collection,
    executable_list,
    executable_object,
    unsupported,
};

/** Frozen collection dependency snapshot; immutable view metadata forms its source identity. */
struct ExpressionCollectionDependencyValue final : CollectionViewImmutableIdentity {
    /** Retained for exact frozen evaluation, but not source identity: cache reads mutate it. */
    std::uint64_t cache_hits = 0U;

    [[nodiscard]] friend bool operator==(
        const ExpressionCollectionDependencyValue& left,
        const ExpressionCollectionDependencyValue& right
    ) {
        return collection_view_immutable_identity(left) ==
               collection_view_immutable_identity(right);
    }
};

/**
 * Canonical lexical dependency identity. The kind preserves scalar/collection/executable origin;
 * explicit null remains a scalar and is therefore distinct from a missing binding.
 */
struct ExpressionDependencyValue final {
    ExpressionDependencyValueKind kind = ExpressionDependencyValueKind::unsupported;
    Value scalar;
    ExpressionCollectionDependencyValue collection;
    std::vector<ExpressionDependencyValue> elements;
    std::vector<std::string> field_names;
    std::vector<ExpressionDependencyValue> field_values;

    [[nodiscard]] bool cacheable() const noexcept;
    [[nodiscard]] friend bool operator==(
        const ExpressionDependencyValue&,
        const ExpressionDependencyValue&
    ) = default;
};

[[nodiscard]] ExpressionDependencyValue capture_expression_dependency(
    const ExpressionValue& value
);
[[nodiscard]] ExpressionValue restore_expression_dependency(
    const ExpressionDependencyValue& value
);

struct LambdaValue final {
    std::string parameter;
    data::JsonView body;
    std::map<std::string, Value, std::less<>> captured;
    std::map<std::string, ExpressionValue, std::less<>> captured_executable;
    std::map<std::string, Value, std::less<>> captured_host_roots;
    std::map<std::string, ExpressionHostDependency, std::less<>> captured_host_dependencies;
    std::map<std::string, ExpressionDependencyValue, std::less<>>
        captured_lexical_dependencies;
    std::string component_path;
};

class ExpressionDependencyObserver {
public:
    virtual ~ExpressionDependencyObserver() = default;
    virtual void lexical(std::string_view name, const ExpressionDependencyValue& value) = 0;
    virtual void host(const ExpressionHostDependency& dependency) = 0;
};

struct ExpressionScope final {
    std::map<std::string, Value, std::less<>> values;
    std::map<std::string, ExpressionValue, std::less<>> executable_values;
    /** Surface/request-local host roots which shadow the application host snapshot. */
    std::map<std::string, Value, std::less<>> contextual_host_roots;
    std::string component_path;
    /** State names visible at this lexical expression site, with retained instance addresses. */
    std::map<std::string, LexicalStateBinding, std::less<>> state_bindings;
    /** Exact immutable host reads used by retained lazy identity evaluators. */
    std::map<std::string, ExpressionHostDependency, std::less<>> host_dependency_overrides;
    /** Exact immutable lexical reads used by retained lazy identity evaluators. */
    std::map<std::string, ExpressionDependencyValue, std::less<>> lexical_dependency_overrides;
};

[[nodiscard]] std::optional<ExpressionDependencyValue> expression_scope_dependency(
    const ExpressionScope& scope,
    std::string_view name
);

/** Decodes the canonical authored range retained on a portable expression, when present. */
[[nodiscard]] std::optional<DiagnosticRange> portable_expression_range(
    data::JsonView expression
);

/** Evaluates parser-independent portable IR expressions against explicit runtime state. */
class ExpressionRuntime final {
public:
    ExpressionRuntime(
        const HostStore& host,
        const RuntimeActionRegistry& actions,
        ExpressionScope scope = {}
    );

    [[nodiscard]] ExpressionValue evaluate(data::JsonView expression);
    [[nodiscard]] ExpressionValue evaluate_in(
        data::JsonView expression,
        const ExpressionScope& scope
    );
    [[nodiscard]] const std::vector<RuntimeDiagnostic>& diagnostics() const noexcept;
    void clear_diagnostics();
    /** Installs a synchronous observer and returns the previous observer. */
    ExpressionDependencyObserver* exchange_dependency_observer(
        ExpressionDependencyObserver* observer
    ) noexcept;
    [[nodiscard]] ExpressionHostDependency read_host_dependency(
        std::span<const HostPathSegment> path,
        const ExpressionScope& scope
    ) const;

private:
    struct HostAccess final {
        std::vector<HostPathSegment> path;
    };

    enum class CollectionDependencyKind { lexical, host };

    struct CollectionDependencyRead final {
        CollectionDependencyKind kind;
        std::string key;
    };

    struct CollectionCacheEntry final {
        std::string path;
        std::string expression_fingerprint;
        std::map<std::string, ExpressionDependencyValue, std::less<>> lexical_dependencies;
        std::map<std::string, ExpressionHostDependency, std::less<>> host_dependencies;
        std::vector<CollectionDependencyRead> dependency_order;
        std::shared_ptr<const CollectionViewValue> view;
    };

    [[nodiscard]] ExpressionValue evaluate(data::JsonView expression, const ExpressionScope& scope);
    [[nodiscard]] ExpressionValue evaluate_literal(data::JsonView expression);
    [[nodiscard]] ExpressionValue evaluate_helper(data::JsonView expression, const ExpressionScope& scope);
    [[nodiscard]] ExpressionValue evaluate_action(data::JsonView expression, const ExpressionScope& scope);
    [[nodiscard]] Value require_value(const ExpressionValue& value, data::JsonView expression);
    [[nodiscard]] Value argument(
        data::JsonView helper,
        const ExpressionScope& scope,
        std::string_view name,
        std::size_t position
    );
    [[nodiscard]] data::JsonView argument_expression(
        data::JsonView helper,
        std::string_view name,
        std::size_t position
    ) const;
    [[nodiscard]] Value evaluate_lambda(const LambdaValue& lambda, const Value& input);
    [[nodiscard]] std::optional<HostAccess> host_access(
        data::JsonView expression,
        const ExpressionScope& scope
    );
    [[nodiscard]] std::shared_ptr<const CollectionViewValue> collection_view(
        data::JsonView helper,
        const ExpressionScope& scope
    );
    [[nodiscard]] std::shared_ptr<const ActionValue> composed_action(
        data::JsonView helper,
        const ExpressionScope& scope,
        ActionCompositionMode mode
    );
    void report(
        data::JsonView expression,
        std::string code,
        std::string message,
        std::optional<std::string> expected = std::nullopt
    );
    void report(RuntimeDiagnostic diagnostic);

    const HostStore& host_;
    const RuntimeActionRegistry& actions_;
    ExpressionScope scope_;
    const ExpressionScope* active_scope_ = nullptr;
    std::vector<RuntimeDiagnostic> diagnostics_;
    std::set<std::string, std::less<>> reported_diagnostics_;
    std::vector<CollectionCacheEntry> collection_cache_;
    ExpressionDependencyObserver* dependency_observer_ = nullptr;
};

[[nodiscard]] bool truthy(const Value& value) noexcept;
[[nodiscard]] std::string display_string(const Value& value);

} // namespace strata::runtime
