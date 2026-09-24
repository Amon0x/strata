#pragma once

#include <algorithm>
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
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "data/json.hpp"
#include "data/json_view.hpp"
#include "runtime/action.hpp"
#include "runtime/diagnostic.hpp"
#include "runtime/host.hpp"
#include "runtime/program.hpp"
#include "runtime/registry.hpp"
#include "runtime/state.hpp"
#include "runtime/symbol.hpp"
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

    [[nodiscard]] friend bool operator==(const CollectionViewImmutableIdentity&,
                                         const CollectionViewImmutableIdentity&) = default;
};

struct CollectionViewValue final : CollectionViewImmutableIdentity {
    mutable std::atomic<std::uint64_t> cache_hits{0U};
};

[[nodiscard]] inline const CollectionViewImmutableIdentity&
collection_view_immutable_identity(const CollectionViewImmutableIdentity& value) noexcept {
    return value;
}

/** Exact leaf read, including whether the selected root was contextual and whether it existed. */
struct ExpressionHostDependency final {
    std::vector<HostPathSegment> path;
    bool contextual = false;
    std::optional<Value> value;
    std::optional<std::string> snapshot_id;
    std::uint64_t snapshot_generation = 0U;

    [[nodiscard]] friend bool operator==(const ExpressionHostDependency& left,
                                         const ExpressionHostDependency& right) {
        return left.path == right.path && left.contextual == right.contextual &&
               left.value == right.value;
    }
};

[[nodiscard]] std::string canonical_host_dependency_path(std::span<const HostPathSegment> path);

enum class ActionCompositionMode { sequence, parallel };

struct ActionValue final {
    ActionValue() = default;
    ActionValue(std::shared_ptr<const Action> action,
                std::optional<ActionCompositionMode> composition,
                std::vector<std::shared_ptr<const ActionValue>> children,
                std::optional<LexicalStateBinding> lexical_state_binding = std::nullopt)
        : action(std::move(action)), composition(composition), children(std::move(children)),
          lexical_state_binding(std::move(lexical_state_binding)) {}

    std::shared_ptr<const Action> action;
    std::optional<ActionCompositionMode> composition;
    std::vector<std::shared_ptr<const ActionValue>> children;
    std::optional<LexicalStateBinding> lexical_state_binding;
};

struct ExpressionListValue;
struct ExpressionObjectValue;
struct ComponentTemplateValue;
struct LambdaValue;

class ExpressionValue final {
  public:
    using Storage =
        std::variant<Value, std::shared_ptr<const CollectionViewValue>,
                     std::shared_ptr<const LambdaValue>, std::shared_ptr<const ActionValue>,
                     std::shared_ptr<const ExpressionListValue>,
                     std::shared_ptr<const ExpressionObjectValue>,
                     std::shared_ptr<const ComponentTemplateValue>>;

    ExpressionValue();
    ExpressionValue(Value value);
    ExpressionValue(Value value, LexicalStateBinding state_binding);
    ExpressionValue(Value value, std::shared_ptr<const LexicalStateBinding> state_binding);
    explicit ExpressionValue(std::shared_ptr<const CollectionViewValue> value);
    explicit ExpressionValue(std::shared_ptr<const LambdaValue> value);
    explicit ExpressionValue(std::shared_ptr<const ActionValue> value);
    explicit ExpressionValue(std::shared_ptr<const ExpressionListValue> value);
    explicit ExpressionValue(std::shared_ptr<const ExpressionObjectValue> value);
    explicit ExpressionValue(std::shared_ptr<const ComponentTemplateValue> value);

    [[nodiscard]] const Value* value() const noexcept;
    /** Materialized data consumed by widgets, including collection-view items. */
    [[nodiscard]] const Value* data_value() const noexcept;
    [[nodiscard]] const std::shared_ptr<const CollectionViewValue>* collection() const noexcept;
    [[nodiscard]] const std::shared_ptr<const LambdaValue>* lambda() const noexcept;
    [[nodiscard]] const std::shared_ptr<const ActionValue>* action() const noexcept;
    [[nodiscard]] const std::shared_ptr<const ExpressionListValue>* list() const noexcept;
    [[nodiscard]] const std::shared_ptr<const ExpressionObjectValue>* object() const noexcept;
    [[nodiscard]] const std::shared_ptr<const ComponentTemplateValue>*
    component_template() const noexcept;
    /** Whether the value is more than data: a view, lambda, action, or a composite holding one. */
    [[nodiscard]] bool executable() const noexcept;
    /** The retained state a state-binding read of this value targets, if it was one. */
    [[nodiscard]] const LexicalStateBinding* lexical_state_binding() const noexcept {
        return lexical_state_binding_.get();
    }
    [[nodiscard]] const std::shared_ptr<const LexicalStateBinding>&
    shared_lexical_state_binding() const noexcept {
        return lexical_state_binding_;
    }
    /** The same value without its state binding. */
    [[nodiscard]] ExpressionValue without_state_binding() const;

  private:
    Storage storage_;
    std::shared_ptr<const LexicalStateBinding> lexical_state_binding_;
};

/** Executable composites retain nested actions/lambdas and expose an immutable scalar projection.
 */
struct ExpressionListValue final {
    Value materialized;
    std::vector<ExpressionValue> values;
};

struct ExpressionObjectValue final {
    Value materialized;
    std::vector<std::pair<std::string, ExpressionValue>> fields;

    [[nodiscard]] const ExpressionValue* field(std::string_view name) const noexcept;
};

struct ComponentTemplateValue final {
    std::string component;
    std::map<std::string, ExpressionValue, std::less<>> arguments;
    Value materialized;
};

/** Whether a binding holds a retained-state binding, clears one, or leaves the outer one. */
enum class ScopeStateBinding : std::uint8_t { inherit, cleared, bound };

/** One lexical binding: a value, a retained-state binding for the name, or both. */
struct ScopeBinding final {
    Symbol name;
    bool has_value = true;
    ScopeStateBinding state = ScopeStateBinding::inherit;
    ExpressionValue value;
    std::shared_ptr<const LexicalStateBinding> binding;
};

/**
 * Bindings added together, over the frame they extend. Frames never change once shared, so a
 * scope extended with a name, or captured by a lambda, costs a frame and not a copy.
 */
struct ScopeFrame final {
    std::shared_ptr<const ScopeFrame> parent;
    std::vector<ScopeBinding> bindings;
    /** A component body's frame: the caller's values are not visible from it, its states are. */
    bool hides_parent_values = false;
    /** A lambda's frame: no retained-state binding reaches its body. */
    bool hides_parent_states = false;
};

using HostRoots = std::map<std::string, Value, std::less<>>;
using FrozenHostReads = std::map<std::string, ExpressionHostDependency, std::less<>>;

/**
 * What an expression can see: its lexical bindings, the host roots its surface overrides, the
 * component path diagnostics and actions name, and the host reads a retained lazy identity froze.
 * Copies share everything.
 */
class ExpressionScope final {
  public:
    ExpressionScope() = default;

    [[nodiscard]] const ExpressionValue* find(Symbol name) const noexcept;
    [[nodiscard]] const ExpressionValue* find(std::string_view name) const;
    /** The retained state a read of the name as a state binding targets. */
    [[nodiscard]] const std::shared_ptr<const LexicalStateBinding>*
    state_binding(Symbol name) const noexcept;

    [[nodiscard]] const std::shared_ptr<const ScopeFrame>& frame() const noexcept {
        return frame_;
    }
    [[nodiscard]] const HostRoots& contextual_host_roots() const noexcept;
    [[nodiscard]] const std::shared_ptr<const HostRoots>&
    shared_contextual_host_roots() const noexcept {
        return contextual_host_roots_;
    }
    [[nodiscard]] const std::string& component_path() const noexcept;
    [[nodiscard]] const std::shared_ptr<const std::string>& shared_component_path() const noexcept {
        return component_path_;
    }
    [[nodiscard]] const FrozenHostReads* host_dependency_overrides() const noexcept {
        return host_dependency_overrides_.get();
    }

    /** Extends the scope by a frame over its current one. */
    void push(ScopeFrame frame);
    /** Binds a value, leaving any retained-state binding of the name visible. */
    void bind(Symbol name, ExpressionValue value);
    void bind(std::string_view name, ExpressionValue value);
    /** Binds a value as a declaration does: the name's state binding becomes `binding`. */
    void declare(Symbol name, ExpressionValue value,
                 std::shared_ptr<const LexicalStateBinding> binding = nullptr);
    /** Replaces the lexical bindings wholesale. */
    void set_frame(std::shared_ptr<const ScopeFrame> frame) noexcept {
        frame_ = std::move(frame);
    }
    void set_component_path(std::shared_ptr<const std::string> path) noexcept {
        component_path_ = std::move(path);
    }
    void set_component_path(std::string path);
    void set_contextual_host_roots(std::shared_ptr<const HostRoots> roots) noexcept {
        contextual_host_roots_ = std::move(roots);
    }
    void set_contextual_host_root(std::string name, Value value);
    void set_host_dependency_overrides(std::shared_ptr<const FrozenHostReads> overrides) noexcept {
        host_dependency_overrides_ = std::move(overrides);
    }

  private:
    std::shared_ptr<const ScopeFrame> frame_;
    std::shared_ptr<const HostRoots> contextual_host_roots_;
    std::shared_ptr<const std::string> component_path_;
    std::shared_ptr<const FrozenHostReads> host_dependency_overrides_;
};

/** A lambda: its body, the program it is part of, and the scope it closed over. */
struct LambdaValue final {
    std::shared_ptr<const Program> program;
    /** The lambda expression, whose free variables tell which captured names matter. */
    ExpressionId expression = no_program_id;
    ExpressionId body = no_program_id;
    Symbol parameter;
    ExpressionScope captured;
};

/**
 * Whether two evaluated actions dispatch identically: same contract, payload, origin, state
 * binding and composition. Evaluation is pure, so this is also their identity as an input.
 */
[[nodiscard]] bool same_action(const std::shared_ptr<const ActionValue>& left,
                               const std::shared_ptr<const ActionValue>& right);
/**
 * Whether two expression values mean the same: equal data, the same collection view identity,
 * the same action, and lambdas with one body over equal values of the names it reads.
 */
[[nodiscard]] bool same_expression_value(const ExpressionValue& left, const ExpressionValue& right);

enum class ExpressionDependencyValueKind {
    scalar,
    collection,
    executable_list,
    executable_object,
    component_template,
    action,
    lambda,
};

/**
 * A lexical read kept to tell later whether it is still current. Values never change once
 * evaluated, so the value itself is kept and compared by meaning.
 */
struct ExpressionDependencyValue final {
    ExpressionValue value;

    [[nodiscard]] ExpressionDependencyValueKind kind() const noexcept;
    [[nodiscard]] friend bool operator==(const ExpressionDependencyValue& left,
                                         const ExpressionDependencyValue& right) {
        return same_expression_value(left.value, right.value);
    }
};

[[nodiscard]] ExpressionDependencyValue capture_expression_dependency(const ExpressionValue& value);
[[nodiscard]] ExpressionValue restore_expression_dependency(const ExpressionDependencyValue& value);

class ExpressionDependencyObserver {
  public:
    virtual ~ExpressionDependencyObserver() = default;
    virtual void lexical(Symbol name, const ExpressionValue& value) = 0;
    virtual void host(const ExpressionHostDependency& dependency) = 0;
};

[[nodiscard]] std::optional<ExpressionDependencyValue>
expression_scope_dependency(const ExpressionScope& scope, Symbol name);

/** Decodes the canonical authored range retained on a portable expression, when present. */
[[nodiscard]] std::optional<DiagnosticRange> portable_expression_range(data::JsonView expression);

/** Evaluates lowered program expressions against explicit runtime state. */
class ExpressionRuntime final {
  public:
    ExpressionRuntime(const HostStore& host, const RuntimeActionRegistry& actions);

    [[nodiscard]] ExpressionValue evaluate(const Program& program, ExpressionId expression,
                                           const ExpressionScope& scope);
    /** Lowers a portable expression, once per content, and evaluates it: tooling and tests. */
    [[nodiscard]] ExpressionValue evaluate(data::JsonView expression,
                                           const ExpressionScope& scope = {});
    [[nodiscard]] const std::vector<RuntimeDiagnostic>& diagnostics() const noexcept;
    void clear_diagnostics();
    /** Drops cached collection views, as when the programs they were derived by are replaced. */
    void clear_caches() noexcept;
    /** Installs a synchronous observer and returns the previous observer. */
    ExpressionDependencyObserver*
    exchange_dependency_observer(ExpressionDependencyObserver* observer) noexcept;
    [[nodiscard]] ExpressionDependencyObserver* dependency_observer() const noexcept {
        return dependency_observer_;
    }
    [[nodiscard]] ExpressionHostDependency
    read_host_dependency(std::span<const HostPathSegment> path, const ExpressionScope& scope) const;

  private:
    enum class CollectionDependencyKind { lexical, host };

    struct CollectionDependencyRead final {
        CollectionDependencyKind kind;
        Symbol name;
        std::string host_key;
    };

    struct CollectionCacheEntry final {
        std::map<Symbol, ExpressionValue> lexical_dependencies;
        std::map<std::string, ExpressionHostDependency, std::less<>> host_dependencies;
        std::vector<CollectionDependencyRead> dependency_order;
        std::shared_ptr<const CollectionViewValue> view;
    };

    struct CollectionCacheKey final {
        std::uint64_t program = 0U;
        ExpressionId expression = no_program_id;
        [[nodiscard]] friend bool operator==(const CollectionCacheKey&,
                                             const CollectionCacheKey&) = default;
    };

    struct CollectionCacheKeyHash final {
        [[nodiscard]] std::size_t operator()(const CollectionCacheKey& key) const noexcept {
            return std::hash<std::uint64_t>{}(key.program * 0x9E3779B97F4A7C15ULL ^ key.expression);
        }
    };

    [[nodiscard]] ExpressionValue evaluate_node(const Program& program, ExpressionId expression,
                                                const ExpressionScope& scope);
    [[nodiscard]] ExpressionValue evaluate_variable(const Program& program,
                                                    const ProgramExpression& node,
                                                    const ExpressionScope& scope);
    [[nodiscard]] ExpressionValue evaluate_property(const Program& program,
                                                    const ProgramExpression& node,
                                                    const ExpressionScope& scope);
    [[nodiscard]] ExpressionValue evaluate_index(const Program& program,
                                                 const ProgramExpression& node,
                                                 const ExpressionScope& scope);
    [[nodiscard]] ExpressionValue evaluate_binary(const Program& program,
                                                  const ProgramExpression& node,
                                                  const ExpressionScope& scope);
    [[nodiscard]] ExpressionValue evaluate_helper(const Program& program, ExpressionId expression,
                                                  const ExpressionScope& scope);
    [[nodiscard]] ExpressionValue evaluate_action(const Program& program,
                                                  const ProgramExpression& node,
                                                  const ExpressionScope& scope);
    /** The host read a host-rooted node makes, unless a local shadows its root. */
    [[nodiscard]] std::optional<ExpressionHostDependency>
    host_read(const Program& program, const ProgramExpression& node, const ExpressionScope& scope);
    [[nodiscard]] Value require_value(const ExpressionValue& value, const ProgramExpression& node,
                                      const ExpressionScope& scope);
    [[nodiscard]] const ProgramArgument* argument(const Program& program,
                                                  const ProgramExpression& node, Symbol name,
                                                  std::size_t position) const;
    [[nodiscard]] Value argument_value(const Program& program, const ProgramExpression& node,
                                       const ExpressionScope& scope, Symbol name,
                                       std::size_t position);
    [[nodiscard]] Value evaluate_lambda(const LambdaValue& lambda, const Value& input);
    [[nodiscard]] std::shared_ptr<const CollectionViewValue>
    collection_view(const Program& program, ExpressionId expression, const ExpressionScope& scope);
    [[nodiscard]] std::shared_ptr<const ActionValue> composed_action(const Program& program,
                                                                     const ProgramExpression& node,
                                                                     const ExpressionScope& scope,
                                                                     ActionCompositionMode mode);
    [[nodiscard]] std::optional<ActionOrigin> action_origin(const Program& program,
                                                            const ProgramExpression& node,
                                                            const ExpressionScope& scope) const;
    void report(const ProgramExpression& node, const ExpressionScope& scope, std::string code,
                std::string message, std::optional<std::string> expected = std::nullopt);
    void report(RuntimeDiagnostic diagnostic);

    const HostStore& host_;
    const RuntimeActionRegistry& actions_;
    std::vector<RuntimeDiagnostic> diagnostics_;
    std::set<std::string, std::less<>> reported_diagnostics_;
    /** Views per collection expression, one per lexical context, newest last. */
    std::unordered_map<CollectionCacheKey, std::vector<CollectionCacheEntry>,
                       CollectionCacheKeyHash>
        collection_cache_;
    std::size_t collection_cache_entries_ = 0U;
    /** Programs lowered from standalone expressions, by content. */
    std::unordered_map<std::string, std::shared_ptr<const Program>> standalone_programs_;
    ExpressionDependencyObserver* dependency_observer_ = nullptr;
};

} // namespace strata::runtime
