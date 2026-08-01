#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <map>
#include <memory_resource>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "compiler/compile.hpp"
#include "compiler/schema.hpp"
#include "data/json.hpp"
#include "runtime/action.hpp"
#include "runtime/async.hpp"
#include "runtime/diagnostic.hpp"
#include "runtime/durability.hpp"
#include "runtime/host.hpp"
#include "runtime/layer.hpp"
#include "runtime/profiler.hpp"
#include "runtime/registry.hpp"
#include "runtime/services.hpp"
#include "runtime/state.hpp"
#include "runtime/undo.hpp"
#include "runtime/unit.hpp"

namespace strata::runtime {

/** One neutral declaration projected into compiler schemas and native runtime contracts. */
class ApplicationBundle final {
public:
    /**
     * Extension package declarations are applied before the application document so a package owns
     * its widget, behavior, and action contracts and the application only names the packages it
     * activates.
     */
    static std::shared_ptr<const ApplicationBundle> create(
        const data::JsonValue* application_schemas = nullptr,
        ActionPayloadDecoders action_decoders = {},
        std::span<const data::JsonValue> extension_declarations = {}
    );

    [[nodiscard]] const compiler::SchemaRegistry& schema_registry() const noexcept;
    [[nodiscard]] const RuntimeActionRegistry& action_registry() const noexcept;
    [[nodiscard]] std::shared_ptr<const HostSnapshot> host_snapshot(
        std::string id,
        std::uint64_t generation,
        const data::JsonValue& roots
    ) const;
    [[nodiscard]] bool async_binding(std::string_view name) const noexcept;
    [[nodiscard]] ValueSchemaPtr async_value_schema(std::string_view name) const noexcept;
    [[nodiscard]] const std::set<std::string, std::less<>>& async_bindings() const noexcept;

private:
    ApplicationBundle(
        compiler::SchemaRegistry schemas,
        RuntimeActionRegistry actions,
        HostSchemaRoots host_schemas,
        std::set<std::string, std::less<>> async_bindings
    );

    compiler::SchemaRegistry schemas_;
    RuntimeActionRegistry actions_;
    HostSchemaRoots host_schemas_;
    std::set<std::string, std::less<>> async_bindings_;
};

enum class ActivationStatus {
    activated,
    rejected_generation,
    rejected_compile,
    rejected_unit,
    rejected_capability,
};

struct ActivationDiagnostic final {
    ActivationDiagnostic(
        std::string code,
        std::string message,
        std::optional<std::string> source_id = std::nullopt,
        std::optional<std::string> component_path = std::nullopt,
        std::optional<std::string> expected = std::nullopt,
        DiagnosticSeverity severity = DiagnosticSeverity::error,
        std::optional<DiagnosticRange> range = std::nullopt
    ) : code(std::move(code)),
        message(std::move(message)),
        source_id(std::move(source_id)),
        component_path(std::move(component_path)),
        expected(std::move(expected)),
        severity(severity),
        range(std::move(range)) {}

    std::string code;
    std::string message;
    std::optional<std::string> source_id;
    std::optional<std::string> component_path;
    std::optional<std::string> expected;
    DiagnosticSeverity severity;
    std::optional<DiagnosticRange> range;
};

struct ActivationResult final {
    ActivationStatus status;
    std::uint64_t attempted_generation;
    std::optional<std::uint64_t> active_generation;
    bool state_migrated;
    std::vector<ActivationDiagnostic> diagnostics;

    [[nodiscard]] bool activated() const noexcept;
};

struct StateScopeResolution final {
    StateAddress address;
    std::string declaration_scope;
    const UnitStateDeclaration* declaration = nullptr;
};

/**
 * Isolated application-owned Phase 3 state. It intentionally has no process-global default;
 * adapters and later retained surfaces receive this context explicitly.
 */
class ApplicationContext final {
public:
    ApplicationContext(
        std::string id,
        std::shared_ptr<const ApplicationBundle> bundle,
        RuntimeServices::PublishedDiagnosticSink diagnostic_sink = {}
    );
    ~ApplicationContext();

    [[nodiscard]] const std::string& id() const noexcept;
    [[nodiscard]] const std::shared_ptr<const ApplicationBundle>& bundle() const noexcept;
    [[nodiscard]] StateStore& state() noexcept;
    [[nodiscard]] HostStore& host() noexcept;
    [[nodiscard]] LayerStack& layers() noexcept;
    [[nodiscard]] ActionDispatcher& actions() noexcept;
    [[nodiscard]] RuntimeServices& services() noexcept;
    [[nodiscard]] const RuntimeServices& services() const noexcept;
    [[nodiscard]] Profiler& profiler() noexcept;
    [[nodiscard]] const Profiler& profiler() const noexcept;
    [[nodiscard]] DurableState& durability() noexcept;
    [[nodiscard]] const DurableState& durability() const noexcept;
    [[nodiscard]] UndoManager& undo() noexcept;
    [[nodiscard]] const UndoManager& undo() const noexcept;
    [[nodiscard]] AsyncDataService& async() noexcept;
    [[nodiscard]] bool undo_state(std::string_view scope, bool redo = false);
    void synchronize_durable_state();
    void configure_durable_store(DurableStoreAdapter adapter);
    void configure_async_host(AsyncHostAdapter adapter);
    void flush_durable();
    [[nodiscard]] ActionDispatchOutcome dispatch(
        const ActionEvent& event,
        const Action& action,
        std::string_view state_scope = {},
        const LexicalStateBinding* lexical_state_binding = nullptr,
        std::string_view undo_scope = "application",
        std::int64_t timestamp_nanos = 0
    );
    [[nodiscard]] const DeclarativeLayerRegistry& layer_registry() const noexcept;
    [[nodiscard]] const std::shared_ptr<const RuntimeUnit>& active_unit() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> active_generation() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> last_attempted_generation() const noexcept;
    [[nodiscard]] const ActivationResult* last_activation() const noexcept;
    void bind_state_scope(
        std::string runtime_scope,
        std::string state_name,
        std::string declaration_scope,
        std::string address_scope
    );
    void clear_state_scope_bindings() noexcept;
    [[nodiscard]] std::optional<StateScopeResolution> resolve_state_scope(
        std::string_view runtime_scope,
        std::string_view state_name
    ) const;
    /** Evaluates the declaration initializer, never the mutable value currently in the slot. */
    [[nodiscard]] std::optional<Value> state_initial_value(
        const StateScopeResolution& resolution,
        std::string_view runtime_scope
    ) const;
    [[nodiscard]] ActivationResult activate(
        RuntimeUnit::PortableIrStorage portable_ir,
        std::uint64_t generation,
        compiler::CompiledSourceMap source_map = {}
    );
    [[nodiscard]] ActivationResult compile_and_activate(
        const compiler::ModuleSource& entry,
        const compiler::ModuleLoader& loader,
        std::uint64_t generation,
        std::pmr::memory_resource* scratch = std::pmr::get_default_resource()
    );
    [[nodiscard]] std::uint64_t dirty_generation() const noexcept;

private:
    void invalidate();

    std::string id_;
    std::shared_ptr<const ApplicationBundle> bundle_;
    std::uint64_t dirty_generation_ = 0U;
    std::uint64_t async_generation_ = 0U;
    StateStore state_;
    HostStore host_;
    LayerStack layers_;
    ActionDispatcher actions_;
    RuntimeServices services_;
    DurableState durability_;
    UndoManager undo_;
    AsyncDataService async_;
    Profiler profiler_;
    DeclarativeLayerRegistry layer_registry_;
    std::shared_ptr<const RuntimeUnit> active_unit_;
    std::optional<std::uint64_t> active_generation_;
    std::optional<std::uint64_t> last_attempted_generation_;
    std::optional<ActivationResult> last_activation_;
    struct StateScopeBinding final {
        std::string declaration_scope;
        std::string address_scope;
    };
    std::map<StateAddress, StateScopeBinding> state_scope_bindings_;
    bool batching_invalidations_ = false;
    bool batched_invalidation_ = false;
};

} // namespace strata::runtime
