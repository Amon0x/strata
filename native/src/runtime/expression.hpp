#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
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
    /** The path read, shared with the IR node that read it. */
    std::shared_ptr<const HostPathKey> key;
    bool contextual = false;
    std::optional<Value> value;
    std::optional<std::string> snapshot_id;
    std::uint64_t snapshot_generation = 0U;

    [[nodiscard]] const std::vector<HostPathSegment>& path() const noexcept {
        return key->segments;
    }
    /** The canonical key of the path, which reads of it are indexed by. */
    [[nodiscard]] const std::string& canonical() const noexcept {
        return key->canonical;
    }

    [[nodiscard]] friend bool operator==(const ExpressionHostDependency& left,
                                         const ExpressionHostDependency& right) {
        return (left.key == right.key || left.canonical() == right.canonical()) &&
               left.contextual == right.contextual && left.value == right.value;
    }
};

[[nodiscard]] std::string canonical_host_dependency_path(std::span<const HostPathSegment> path);

/**
 * How a composed action runs its steps: in order, together, or (`delay`) its one step once a
 * duration has passed.
 */
/** `then` runs its first step, and its second with the answer once the first's request has one. */
enum class ActionCompositionMode { sequence, parallel, delay, then };

struct LambdaValue;

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
    /** How long a `delay` composition waits before its step runs. */
    std::int64_t delay_nanoseconds = 0;
    /** A script handler: the function run with the event in place of `action`'s dispatch. */
    std::shared_ptr<const LambdaValue> script;
};

/** The contract every script handler's action carries: a framework action run in place. */
[[nodiscard]] const std::shared_ptr<const ActionContract>& script_action_contract();

struct ExpressionListValue;
struct ExpressionObjectValue;
struct ComponentTemplateValue;
struct LambdaValue;
struct FunctionGroup;

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

    /** The value as data: an executable composite's data projection, null for the rest. */
    [[nodiscard]] const Value* value() const;
    /** Materialized data consumed by widgets, including collection-view items. */
    [[nodiscard]] const Value* data_value() const;
    /** Moves the data out of a value that holds plain data (not executable() itself). */
    [[nodiscard]] Value take_value() &&;
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

/**
 * A list literal holding more than data: an action, a lambda, a collection view or a state
 * binding among its elements. A literal of plain data evaluates to a list Value instead. The
 * elements are the one representation; the data projection consumers read is derived from them
 * once, when first asked for, with executable parts read as null and views as their items.
 */
struct ExpressionListValue final {
    explicit ExpressionListValue(std::vector<ExpressionValue> elements)
        : values(std::move(elements)) {}

    std::vector<ExpressionValue> values;

    [[nodiscard]] const Value& materialized() const;

  private:
    mutable std::once_flag materialize_once_;
    mutable Value materialized_;
};

/** A map literal holding more than data; see ExpressionListValue. */
struct ExpressionObjectValue final {
    explicit ExpressionObjectValue(std::vector<std::pair<std::string, ExpressionValue>> entries)
        : fields(std::move(entries)) {}

    std::vector<std::pair<std::string, ExpressionValue>> fields;

    [[nodiscard]] const ExpressionValue* field(std::string_view name) const noexcept;
    [[nodiscard]] const Value& materialized() const;

  private:
    mutable std::once_flag materialize_once_;
    mutable Value materialized_;
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
    /** A script `var`: its current value, which assignment replaces, in place of `value`. */
    std::shared_ptr<ExpressionValue> cell;
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
    /**
     * The functions a block declares, which this frame binds by name. The group is kept alive by
     * the scopes holding the frame (see ExpressionScope::bind_functions), never by the frame,
     * so a group's own scope never holds it.
     */
    const FunctionGroup* functions = nullptr;
};

/** The groups whose functions a scope's frames bind, kept alive with it. */
struct KeptFunctionGroups final {
    std::shared_ptr<const FunctionGroup> group;
    std::shared_ptr<const KeptFunctionGroups> next;
};

/** What a name denotes in a scope: a value (and the binding holding it), or a function. */
struct ScopeLookup final {
    const ExpressionValue* value = nullptr;
    const ScopeBinding* binding = nullptr;
    const FunctionGroup* group = nullptr;
    std::uint32_t function = std::numeric_limits<std::uint32_t>::max();
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
    /** What the name denotes, a block's function included. */
    [[nodiscard]] ScopeLookup lookup(Symbol name) const noexcept;
    /**
     * The retained state a script reads or writes through the name: a state binding found
     * through lambda frames too, which hide states only from Binding<T> arguments.
     */
    [[nodiscard]] const std::shared_ptr<const LexicalStateBinding>*
    script_state(Symbol name) const noexcept;
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
    /** Binds a script `var`, whose value lives in the returned cell. */
    std::shared_ptr<ExpressionValue> declare_variable(Symbol name, ExpressionValue value);
    /** Binds a block's functions by name, and keeps their group alive with this scope. */
    void bind_functions(const std::shared_ptr<const FunctionGroup>& group);
    /** Keeps a group alive with this scope, as a function of it is called. */
    void keep(std::shared_ptr<const FunctionGroup> group);
    /** The groups this scope keeps, past `group` when it is the newest: what the group closes over. */
    [[nodiscard]] ExpressionScope without_newest(const FunctionGroup* group) const;
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
    std::shared_ptr<const KeptFunctionGroups> kept_;
};

/**
 * The functions one block declares and the scope they close over: the block's, as far as it has
 * been described or run, so they read the states and locals declared around them.
 */
struct FunctionGroup final : std::enable_shared_from_this<FunctionGroup> {
    std::shared_ptr<const Program> program;
    /** Each function's name and its index in the program. */
    std::vector<std::pair<Symbol, std::uint32_t>> functions;
    /** Kept current while the block extends its scope; never keeps this group itself. */
    mutable ExpressionScope scope;

    /** Makes `scope` (which binds this group) what the group's functions close over. */
    void close_over(const ExpressionScope& block_scope) const {
        scope = block_scope.without_newest(this);
    }
};

/**
 * A function value: a lambda, or a declared function. A lambda or a top-level function closes
 * over `captured`; a function a block declares closes over its `group`'s scope.
 */
struct LambdaValue final {
    std::shared_ptr<const Program> program;
    /** The lambda expression, whose free variables tell which captured names matter. */
    ExpressionId expression = no_program_id;
    /** The function's index in the program. */
    std::uint32_t function = no_program_id;
    ExpressionScope captured;
    std::shared_ptr<const FunctionGroup> group;

    [[nodiscard]] const ProgramFunction& declaration() const {
        return program->function(function);
    }
};

/** A value a script throws, or a runtime error's `{ code, message }`, unwinding to a catch. */
struct ScriptThrow final {
    Value value;
};

/** One state a script wrote: where, and what it holds now. */
struct ScriptWrite final {
    std::shared_ptr<const LexicalStateBinding> binding;
    Value value;
};

/**
 * What a handler does between commits: the states it wrote (each once, its latest value) and
 * the actions it performs, in order, once those writes are committed.
 */
struct ScriptTransaction final {
    std::vector<ScriptWrite> writes;
    std::vector<std::shared_ptr<const ActionValue>> actions;
    /** Set by `undoStep`: the writes are one undo step, merged with the last of the same key. */
    std::optional<std::string> undo_label;
    std::optional<std::string> undo_coalesce;

    [[nodiscard]] const Value* written(const StateAddress& address) const noexcept;
    void write(std::shared_ptr<const LexicalStateBinding> binding, Value value);
    [[nodiscard]] bool empty() const noexcept {
        return writes.empty() && actions.empty();
    }
};

/** What an awaited action answered: a value at once, a request answered later, or a failure. */
struct ScriptAnswer final {
    enum class Kind : std::uint8_t { value, pending, failed };
    Kind kind = Kind::value;
    Value value;
    std::string message;
};

/** What a script run can do beyond reading: the host it commits its effects to. */
class ScriptEffects {
  public:
    virtual ~ScriptEffects() = default;
    /** The state's current value, as committed; null when the store holds none. */
    [[nodiscard]] virtual const Value* state(const LexicalStateBinding& binding) = 0;
    /** Commits writes, then performs actions; a message when the commit was rejected. */
    [[nodiscard]] virtual std::optional<std::string> commit(ScriptTransaction transaction) = 0;
    /** Performs an action whose answer the script waits for. */
    [[nodiscard]] virtual ScriptAnswer perform(const std::shared_ptr<const ActionValue>& action) = 0;
};

struct ScriptRun;

/** How a run ended: finished with a value, waiting on an answer, or failed with an error. */
struct ScriptOutcome final {
    enum class Kind : std::uint8_t { finished, waiting, failed };
    Kind kind = Kind::finished;
    Value value;
    /** A failed run's uncaught error, `{ code, message }` or the value thrown. */
    Value error;
    /** A waiting run, resumed with the answer. */
    std::shared_ptr<ScriptRun> run;
};

/** Steps one handler run may take between commits, and how deep its calls may nest. */
inline constexpr std::uint64_t script_step_budget = 10'000'000U;
inline constexpr std::size_t script_call_depth = 512U;

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

/**
 * What an evaluation reads: the lexical bindings and the host paths its value depends on. The
 * evaluator is handed a recorder by reference and tells it each read where the read is made;
 * nothing is installed on the evaluator. A recorder opened for part of an evaluation (a
 * collection view's inputs, a lambda's body, a Repeater's source) names the recorder it was opened
 * under and forwards every read it does not hide to it, so the chain is explicit and each link is
 * a plain call.
 */
class DependencyRecorder final {
  public:
    /** Which reads of one name or host path a recorder keeps; it forwards every read. */
    enum class Keep : std::uint8_t { none, first, latest };

    using LexicalReads = std::map<Symbol, ExpressionDependencyValue>;
    using HostReads = std::map<std::string, ExpressionHostDependency, std::less<>>;
    /** A kept read, in the order reads were first kept: a lexical name or a host path key. */
    struct Read final {
        Symbol name;
        /** The key of a host read in host_reads(); none for a lexical read. */
        const std::string* host = nullptr;
    };

    /** Keeps and forwards nothing: an evaluation whose reads matter to no one. */
    DependencyRecorder() noexcept = default;
    DependencyRecorder(DependencyRecorder* upstream, Keep lexical, Keep host) noexcept
        : upstream_(upstream), lexical_(lexical), host_(host) {}
    DependencyRecorder(const DependencyRecorder&) = delete;
    DependencyRecorder& operator=(const DependencyRecorder&) = delete;
    DependencyRecorder(DependencyRecorder&&) noexcept = default;
    DependencyRecorder& operator=(DependencyRecorder&&) noexcept = default;

    /** Reads of the name are this recorder's own business: neither kept nor forwarded. */
    void hide(Symbol name) {
        hidden_.push_back(name);
    }
    /**
     * Keeps a lexical read only when it reads the value `outer` binds the name to: a row's
     * inputs are what its body read of the scope enclosing it.
     */
    void keep_only_reads_of(const ExpressionScope* outer) noexcept {
        outer_ = outer;
    }
    /** Remembers the order reads were first kept in (see order()). */
    void keep_order() noexcept {
        ordered_ = true;
    }

    void lexical(Symbol name, const ExpressionValue& value) {
        if (upstream_ != nullptr || lexical_ != Keep::none)
            record_lexical(name, value);
    }
    void host(const ExpressionHostDependency& dependency) {
        if (upstream_ != nullptr || host_ != Keep::none)
            record_host(dependency);
    }

    [[nodiscard]] DependencyRecorder* upstream() const noexcept {
        return upstream_;
    }
    [[nodiscard]] LexicalReads& lexical_reads() {
        return kept().lexical;
    }
    [[nodiscard]] HostReads& host_reads() {
        return kept().host;
    }
    [[nodiscard]] const std::vector<Read>& order() {
        return kept().order;
    }

  private:
    /** Allocated on the first read kept: a recorder that only forwards costs nothing more. */
    struct Kept final {
        LexicalReads lexical;
        HostReads host;
        std::vector<Read> order;
    };

    [[nodiscard]] Kept& kept() {
        if (kept_ == nullptr)
            kept_ = std::make_unique<Kept>();
        return *kept_;
    }
    void record_lexical(Symbol name, const ExpressionValue& value);
    void record_host(const ExpressionHostDependency& dependency);

    DependencyRecorder* upstream_ = nullptr;
    Keep lexical_ = Keep::none;
    Keep host_ = Keep::none;
    bool ordered_ = false;
    const ExpressionScope* outer_ = nullptr;
    std::vector<Symbol> hidden_;
    std::unique_ptr<Kept> kept_;
};

/** Decodes the canonical authored range retained on a portable expression, when present. */
[[nodiscard]] std::optional<DiagnosticRange> portable_expression_range(data::JsonView expression);

/** Evaluates lowered program expressions against explicit runtime state. */
class ExpressionRuntime final {
  public:
    ExpressionRuntime(const HostStore& host, const RuntimeActionRegistry& actions);

    /** Evaluates an expression, telling `reads` what it reads. */
    [[nodiscard]] ExpressionValue evaluate(const Program& program, ExpressionId expression,
                                           const ExpressionScope& scope, DependencyRecorder& reads);
    /** Evaluates an expression whose reads matter to no one. */
    [[nodiscard]] ExpressionValue evaluate(const Program& program, ExpressionId expression,
                                           const ExpressionScope& scope);
    /** Lowers a portable expression, once per content, and evaluates it: tooling and tests. */
    [[nodiscard]] ExpressionValue evaluate(data::JsonView expression,
                                           const ExpressionScope& scope = {});
    [[nodiscard]] ExpressionValue evaluate(data::JsonView expression, const ExpressionScope& scope,
                                           DependencyRecorder& reads);
    [[nodiscard]] const std::vector<RuntimeDiagnostic>& diagnostics() const noexcept;
    void clear_diagnostics();
    /** Drops cached collection views, as when the programs they were derived by are replaced. */
    void clear_caches() noexcept;
    [[nodiscard]] ExpressionHostDependency
    read_host_dependency(const std::shared_ptr<const HostPathKey>& path,
                         const ExpressionScope& scope) const;

    /**
     * Runs a handler: the function, called with `arguments`, writes and performs through
     * `effects`. What it writes is committed when it finishes or waits; nothing, when it fails.
     */
    [[nodiscard]] ScriptOutcome run_script(const std::shared_ptr<const LambdaValue>& function,
                                           std::vector<Value> arguments, ScriptEffects& effects);
    /** Resumes a waiting run with its answer, or with the error its request failed with. */
    [[nodiscard]] ScriptOutcome resume_script(const std::shared_ptr<ScriptRun>& run, Value answer,
                                              bool failed, ScriptEffects& effects);
    /** Calls a function with values, outside any handler: pure, so it may not write state. */
    [[nodiscard]] ExpressionValue call(const LambdaValue& function,
                                       std::vector<ExpressionValue> arguments);

  private:
    friend class ScriptMachine;
    enum class CollectionDependencyKind { lexical, host };

    struct CollectionDependencyRead final {
        CollectionDependencyKind kind;
        Symbol name;
        std::string host_key;
    };

    struct CollectionCacheEntry final {
        DependencyRecorder::LexicalReads lexical_dependencies;
        DependencyRecorder::HostReads host_dependencies;
        std::vector<CollectionDependencyRead> dependency_order;
        std::shared_ptr<const CollectionViewValue> view;
    };

    /** A node of one program: an expression, or a host path, by its index. */
    struct ProgramNodeKey final {
        std::uint64_t program = 0U;
        std::uint32_t node = no_program_id;
        [[nodiscard]] friend bool operator==(const ProgramNodeKey&,
                                             const ProgramNodeKey&) = default;
    };

    struct ProgramNodeKeyHash final {
        [[nodiscard]] std::size_t operator()(const ProgramNodeKey& key) const noexcept {
            return std::hash<std::uint64_t>{}(key.program * 0x9E3779B97F4A7C15ULL ^ key.node);
        }
    };

    [[nodiscard]] ExpressionValue evaluate_node(const Program& program, ExpressionId expression,
                                                const ExpressionScope& scope,
                                                DependencyRecorder& reads);
    [[nodiscard]] ExpressionValue evaluate_variable(const Program& program,
                                                    const ProgramExpression& node,
                                                    const ExpressionScope& scope,
                                                    DependencyRecorder& reads);
    [[nodiscard]] ExpressionValue evaluate_property(const Program& program,
                                                    const ProgramExpression& node,
                                                    const ExpressionScope& scope,
                                                    DependencyRecorder& reads);
    [[nodiscard]] ExpressionValue evaluate_index(const Program& program,
                                                 const ProgramExpression& node,
                                                 const ExpressionScope& scope,
                                                 DependencyRecorder& reads);
    [[nodiscard]] ExpressionValue evaluate_binary(const Program& program,
                                                  const ProgramExpression& node,
                                                  const ExpressionScope& scope,
                                                  DependencyRecorder& reads);
    [[nodiscard]] ExpressionValue evaluate_helper(const Program& program, ExpressionId expression,
                                                  const ExpressionScope& scope,
                                                  DependencyRecorder& reads);
    [[nodiscard]] ExpressionValue evaluate_action(const Program& program,
                                                  const ProgramExpression& node,
                                                  const ExpressionScope& scope,
                                                  DependencyRecorder& reads);
    /** The host read a host-rooted node makes, unless a local shadows its root. */
    [[nodiscard]] std::optional<ExpressionHostDependency> host_read(const Program& program,
                                                                    const ProgramExpression& node,
                                                                    const ExpressionScope& scope,
                                                                    DependencyRecorder& reads);
    [[nodiscard]] Value require_value(const ExpressionValue& value, const ProgramExpression& node,
                                      const ExpressionScope& scope);
    [[nodiscard]] const ProgramArgument* argument(const Program& program,
                                                  const ProgramExpression& node, Symbol name,
                                                  std::size_t position) const;
    [[nodiscard]] Value argument_value(const Program& program, const ProgramExpression& node,
                                       const ExpressionScope& scope, Symbol name,
                                       std::size_t position, DependencyRecorder& reads);
    [[nodiscard]] Value evaluate_lambda(const LambdaValue& lambda, const Value& input,
                                        DependencyRecorder& upstream);
    /** A helper of StrataScript's library, by name (library.cpp). */
    [[nodiscard]] ExpressionValue evaluate_library(const Program& program,
                                                   const ProgramExpression& node,
                                                   const ExpressionScope& scope,
                                                   DependencyRecorder& reads);
    [[nodiscard]] ExpressionValue evaluate_call(const Program& program,
                                                const ProgramExpression& node,
                                                const ExpressionScope& scope,
                                                DependencyRecorder& reads);
    /**
     * Calls a function: arguments by position, then by parameter name, the rest defaulted.
     * A block body runs as a nested script of the run in progress, if any.
     */
    [[nodiscard]] ExpressionValue invoke(const LambdaValue& function,
                                         std::vector<ExpressionValue> positional,
                                         std::vector<std::pair<Symbol, ExpressionValue>> named,
                                         DependencyRecorder& reads);
    /** The scope a function's body runs in, its parameters bound from the arguments. */
    [[nodiscard]] ExpressionScope function_scope(const LambdaValue& function,
                                                 std::vector<ExpressionValue> positional,
                                                 std::vector<std::pair<Symbol, ExpressionValue>> named,
                                                 DependencyRecorder& reads);
    /** A script error, unwinding to the nearest catch: `{ code, message }`. */
    [[noreturn]] void script_error(std::string code, std::string message);
    /** A state's value as the running handler sees it: its own write, else the store's. */
    [[nodiscard]] const Value* script_state_value(const LexicalStateBinding& binding);
    /** Queues an action for the running handler to perform once its writes commit. */
    void perform_action(const std::shared_ptr<const ActionValue>& action);
    /** Names the running handler's undo step; false outside a handler. */
    [[nodiscard]] bool name_undo_step(std::string label, std::optional<std::string> coalesce);
    [[nodiscard]] std::shared_ptr<const CollectionViewValue>
    collection_view(const Program& program, ExpressionId expression, const ExpressionScope& scope,
                    DependencyRecorder& upstream);
    [[nodiscard]] std::shared_ptr<const ActionValue> composed_action(const Program& program,
                                                                     const ProgramExpression& node,
                                                                     const ExpressionScope& scope,
                                                                     ActionCompositionMode mode,
                                                                     DependencyRecorder& reads);
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
    /** How deep evaluation is inside the left of `??`, where a missing property is just null. */
    std::size_t probing_ = 0U;
    /** How deep evaluation is inside a script's statements, where an error unwinds to a catch. */
    std::size_t scripting_ = 0U;
    /** The handler run whose transaction writes and actions go to; null outside one. */
    ScriptRun* active_run_ = nullptr;
    /** How deep function calls nest. */
    std::size_t call_depth_ = 0U;
    /** Views per collection expression, one per lexical context, newest last. */
    std::unordered_map<ProgramNodeKey, std::vector<CollectionCacheEntry>, ProgramNodeKeyHash>
        collection_cache_;
    /**
     * The last path each host path with a computed step resolved to: evaluated again with the
     * same lookups, it reads through the same shared key.
     */
    std::unordered_map<ProgramNodeKey, std::shared_ptr<const HostPathKey>, ProgramNodeKeyHash>
        dynamic_host_paths_;
    std::size_t collection_cache_entries_ = 0U;
    /** Programs lowered from standalone expressions, by content. */
    std::unordered_map<std::string, std::shared_ptr<const Program>> standalone_programs_;
};

/**
 * What the `formatDate` helper produces: a UTC millisecond timestamp shifted by an offset, in a
 * pattern of yyyy yy, MMMM MMM MM M, dd d, EEEE EEE, HH H, hh h, mm m, ss s, SSS and a, with
 * names from the lists given (English when they are not twelve and seven long).
 */
[[nodiscard]] std::string format_date_pattern(double milliseconds, std::string_view pattern,
                                              double offset_minutes,
                                              const std::vector<std::string>& month_names,
                                              const std::vector<std::string>& weekday_names);

} // namespace strata::runtime
