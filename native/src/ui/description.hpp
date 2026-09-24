#pragma once

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
#include <vector>

#include "runtime/application.hpp"
#include "runtime/expression.hpp"
#include "runtime/program.hpp"
#include "ui/tree.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {

struct DescriptionBuildResult final {
    std::shared_ptr<const DescriptionNode> root;
    std::vector<runtime::RuntimeDiagnostic> diagnostics;
    std::size_t evaluated_expressions = 0U;
    std::size_t described_nodes = 0U;
};

struct LayerDescriptionRequest final {
    runtime::LayerRole role;
    std::string name;
};

struct DescriptionLayersBuildResult final {
    std::vector<std::shared_ptr<const DescriptionNode>> roots;
    std::vector<runtime::StateScopeSet> layer_state_scopes;
    std::vector<runtime::RuntimeDiagnostic> diagnostics;
    std::size_t evaluated_expressions = 0U;
    std::size_t described_nodes = 0U;
};

/** Materializes a retained description snapshot from the active unit's lowered program. */
class DescriptionBuilder final {
  public:
    explicit DescriptionBuilder(runtime::ApplicationContext& application);
    DescriptionBuilder(runtime::ApplicationContext& application, const WidgetRegistry& widgets);

    [[nodiscard]] DescriptionBuildResult build(runtime::LayerRole role, std::string_view name);
    /** Supplies the prior retained tree to stateful widget expansion for this build. */
    void set_retained_tree(const RetainedTree* tree);
    /** Whether changing one retained value can affect the currently cached description layers. */
    [[nodiscard]] bool observes_retained_value(const RetainedNode& node, std::string_view name,
                                               bool include_lazy_snapshots = true) const;
    /** Supplies request-local host roots without mutating the shared application snapshot. */
    void set_contextual_host_roots(std::map<std::string, runtime::Value, std::less<>> roots);
    [[nodiscard]] DescriptionLayersBuildResult
    build_layers(std::span<const LayerDescriptionRequest> layers);

  private:
    struct RepeaterChildren final {
        std::shared_ptr<const DescriptionChildren> source;
        std::shared_ptr<const runtime::IndexableSequence> sequence;
        DescriptionSequenceGeneration generation;
    };

    struct GeneratedRowEvaluationContext;
    struct LazyRowEvaluationState;
    struct RepeaterIdentityEvaluationState;
    struct WidgetRowEvaluationState;

    struct Scope final {
        struct WidgetDefault final {
            std::optional<runtime::ExpressionValue> style;
            std::optional<runtime::ExpressionValue> variant;
        };
        using WidgetDefaults = std::map<std::string, WidgetDefault, std::less<>>;

        runtime::ExpressionScope expressions;
        /** The instance path: also the runtime state scope and the expressions' component path. */
        std::shared_ptr<const std::string> instance;
        /** The scope states declared here belong to ("component X", "screen X"): the program's. */
        std::string_view declaration_scope;
        std::shared_ptr<const WidgetDefaults> widget_defaults;

        [[nodiscard]] const std::string& instance_path() const noexcept;
        /** Moves the scope to another instance, which its expressions then name. */
        void set_instance(std::string path);
    };

    /** A component call's inputs, in parameter order: compared position by position. */
    using ComponentInputs = std::vector<runtime::ExpressionValue>;

    struct StateBindingEffect final {
        std::string declaration_scope;
        std::string address_scope;
    };

    struct RetainedQuery final {
        std::optional<std::string> key;
        std::string source_path;
        std::string state_scope;
        std::string type;

        [[nodiscard]] friend bool operator==(const RetainedQuery&, const RetainedQuery&) = default;
    };

    struct RetainedValueEffects final {
        RetainedQuery query;
        std::map<std::string, std::optional<runtime::Value>, std::less<>> values;
    };

    struct RetainedSequenceEffect final {
        RetainedQuery query;
        std::weak_ptr<const runtime::IndexableSequence> sequence;
        std::optional<DescriptionSequenceGeneration> generation;
    };

    /** A component cache entry's identity, stable for its cache key while the unit is active. */
    using ComponentId = std::uint64_t;

    struct ComponentEffects final {
        std::map<std::string, runtime::ExpressionHostDependency, std::less<>> host_values;
        std::map<runtime::StateAddress, runtime::Value> state_values;
        std::map<runtime::StateAddress, StateBindingEffect, std::less<>> state_bindings;
        runtime::StateScopeSet owned_state_scopes;
        /** Cached components built directly in this body, in the order they were met. */
        std::vector<ComponentId> direct_descendants;
        /** Every cached component built within this body, sorted. */
        std::vector<ComponentId> descendants;
        std::vector<RetainedValueEffects> retained_values;
        std::vector<RetainedSequenceEffect> retained_sequences;
        bool captures_retained_snapshot = false;
        std::shared_ptr<const RetainedDescriptionSnapshot> retained_snapshot;
        // The share of the aggregated fields above that this body contributed itself, apart from
        // the cached components inside it. A component whose own body is still current then
        // recomputes its aggregate from these and its direct descendants' entries instead of
        // being rebuilt when only a descendant changed.
        std::map<runtime::StateAddress, StateBindingEffect, std::less<>> local_state_bindings;
        runtime::StateScopeSet local_owned_state_scopes;
        bool local_captures_retained_snapshot = false;
    };

    struct ComponentCacheEntry final {
        std::string component;
        std::uint32_t component_index = runtime::no_program_id;
        std::string source_path;
        ComponentInputs inputs;
        std::uint64_t host_invalidation_count = 0U;
        std::uint64_t last_used_epoch = 0U;
        std::shared_ptr<const runtime::HostRoots> contextual_host_roots;
        ComponentEffects effects;
        std::shared_ptr<const DescriptionNode> root;
        Scope rebuild_scope;
        std::string cache_key;
        /** The build that last reached this entry: others are the first evicted. */
        std::uint64_t visited_epoch = 0U;
        bool refreshing = false;
    };

    struct LayerCacheEntry final {
        runtime::LayerRole role;
        std::string name;
        std::string source_path;
        std::uint64_t host_invalidation_count = 0U;
        std::shared_ptr<const runtime::HostRoots> contextual_host_roots;
        ComponentEffects effects;
        std::shared_ptr<const DescriptionNode> root;
    };

    /**
     * A named style resolved with the host values it read. Styles are top-level declarations:
     * they read no locals, so an entry stays good while those values do, and whoever uses it
     * reads them too.
     */
    struct ResolvedStyle final {
        runtime::Value value;
        std::vector<runtime::ExpressionHostDependency> host_values;
        std::uint64_t host_invalidation_count = 0U;
        std::shared_ptr<const runtime::HostRoots> contextual_host_roots;
    };

    enum class ComponentRefreshResult {
        unchanged,
        changed,
        invalid,
    };

    [[nodiscard]] const runtime::Program& program() const noexcept {
        return unit_->program();
    }
    /** Makes `unit` the one descriptions are built from, dropping what the last one cached. */
    void use_unit(const std::shared_ptr<const runtime::RuntimeUnit>& unit);
    /** The widget a call names, looked up once per call site. */
    [[nodiscard]] const WidgetLifecycle* call_widget(runtime::CallId call);

    void bind_state_scope(std::string_view runtime_scope, std::string_view state_name,
                          std::string_view declaration_scope, std::string_view address_scope,
                          bool replayed = false);
    void own_state_scope(std::string_view scope, bool replayed = false);
    void observe_state_value(const runtime::StateAddress& address, const runtime::Value& value);
    void observe_host_dependency(const runtime::ExpressionHostDependency& dependency);
    void observe_retained_value(const RetainedQuery& query, std::string_view name,
                                const runtime::Value* value);
    void observe_retained_sequence(const RetainedQuery& query,
                                   const RetainedDescriptionSnapshot::Node* retained);
    void capture_retained_snapshot();
    void replay_component_effects(const ComponentEffects& effects);
    void absorb_uncached_component_effects(const ComponentEffects& effects);
    [[nodiscard]] bool aggregate_component_effects(ComponentEffects& effects) const;
    [[nodiscard]] std::shared_ptr<const DescriptionNode>
    build_component_body(std::uint32_t component, const Scope& scope, ComponentEffects& effects);
    [[nodiscard]] ComponentRefreshResult refresh_component_cache_entry(ComponentId id);
    /** The id of a cache key, assigned on first use. */
    [[nodiscard]] ComponentId component_id(const std::string& cache_key);
    /** Drops a cache entry and its key's id. */
    void forget_component(ComponentId id);
    static void add_descendant(std::vector<ComponentId>& sorted, ComponentId id);
    static void add_descendants(std::vector<ComponentId>& sorted,
                                const std::vector<ComponentId>& more);
    [[nodiscard]] static std::shared_ptr<const DescriptionNode> replace_component_subtrees(
        const std::shared_ptr<const DescriptionNode>& root,
        const std::map<const DescriptionNode*, std::shared_ptr<const DescriptionNode>>&
            replacements);
    [[nodiscard]] bool component_cache_entry_current(const ComponentCacheEntry& entry,
                                                     std::string_view source_path,
                                                     const ComponentInputs& inputs) const;
    [[nodiscard]] bool component_effects_current(
        const ComponentEffects& effects, std::uint64_t host_invalidation_count,
        const std::shared_ptr<const runtime::HostRoots>& contextual_host_roots) const;
    [[nodiscard]] bool
    same_contextual_host_roots(const std::shared_ptr<const runtime::HostRoots>& roots) const;
    /** Whether a host read would read the same now. */
    [[nodiscard]] bool host_read_current(std::string_view canonical,
                                         const runtime::ExpressionHostDependency& dependency) const;
    [[nodiscard]] std::shared_ptr<const DescriptionNode> build_layer(runtime::LayerRole role,
                                                                     std::string_view name);

    [[nodiscard]] std::vector<std::shared_ptr<const DescriptionNode>>
    build_block(runtime::BlockId block, const Scope& scope,
                std::span<const std::size_t> skipped_statement_indices = {});
    [[nodiscard]] RepeaterChildren
    build_repeater_children(runtime::BlockId block, const Scope& scope,
                            const RetainedDescriptionSnapshot::Node* retained_widget);
    [[nodiscard]] std::string evaluate_repeater_identity(runtime::IdentityId identity,
                                                         const Scope& scope);
    [[nodiscard]] const RetainedDescriptionSnapshot::Node*
    retained_widget(const RetainedQuery& query) const noexcept;
    [[nodiscard]] std::shared_ptr<const DescriptionNode> build_call(runtime::CallId call,
                                                                    const Scope& scope);
    [[nodiscard]] std::shared_ptr<const DescriptionNode>
    build_component_call(const runtime::ProgramCall& call, const Scope& scope);
    [[nodiscard]] std::shared_ptr<const DescriptionNode>
    build_component_template(std::string_view component, std::string key,
                             WidgetTemplateArguments arguments, const Scope& caller);
    /**
     * A component instance's scope: its parameters, `supplied` or defaulted, over the caller's
     * state bindings. Appends the parameter values to `inputs` when given.
     */
    [[nodiscard]] Scope component_scope(const runtime::ProgramComponent& component,
                                        std::string instance_path, const Scope& caller,
                                        std::shared_ptr<const Scope::WidgetDefaults> defaults,
                                        std::span<const runtime::ExpressionValue* const> supplied,
                                        ComponentInputs* inputs);
    [[nodiscard]] runtime::ExpressionValue evaluate(runtime::ExpressionId expression,
                                                    const runtime::ExpressionScope& scope);
    [[nodiscard]] runtime::Value require_value(const runtime::ExpressionValue& value,
                                               data::JsonView source);
    [[nodiscard]] std::vector<DescriptionBehavior>
    build_behaviors(const runtime::ProgramCall& call, const runtime::ExpressionScope& scope);
    void append_diagnostics(runtime::ExpressionRuntime& expressions);
    [[nodiscard]] runtime::Value resolve_style(const runtime::Value& value,
                                               std::set<std::string, std::less<>>& resolving);
    [[nodiscard]] runtime::Value resolve_named_style(std::string_view name,
                                                     std::set<std::string, std::less<>>& resolving);
    [[nodiscard]] bool style_current(ResolvedStyle& style) const;
    void normalize_layout(DescriptionNode::Properties& properties);

    runtime::ApplicationContext& application_;
    std::unique_ptr<WidgetRegistry> owned_widgets_;
    const WidgetRegistry& widgets_;
    std::vector<runtime::RuntimeDiagnostic> diagnostics_;
    std::size_t evaluated_expressions_ = 0U;
    std::size_t described_nodes_ = 0U;
    runtime::StateScopeSet current_layer_state_scopes_;
    std::map<std::string, ResolvedStyle, std::less<>> styles_;
    std::unique_ptr<runtime::ExpressionRuntime> expressions_;
    std::shared_ptr<const runtime::HostRoots> contextual_host_roots_;
    std::shared_ptr<const RetainedDescriptionSnapshot> retained_snapshot_;
    /** The unit descriptions are built from; everything cached is for it. */
    std::shared_ptr<const runtime::RuntimeUnit> unit_;
    /** Per call site of the unit's program, the widget it names, for one registry revision. */
    std::vector<std::optional<const WidgetLifecycle*>> call_widgets_;
    std::uint64_t call_widgets_revision_ = 0U;
    struct KeyHash final {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(const std::string_view key) const noexcept {
            return std::hash<std::string_view>{}(key);
        }
    };
    std::unordered_map<ComponentId, ComponentCacheEntry> component_cache_;
    std::unordered_map<std::string, ComponentId, KeyHash, std::equal_to<>> component_ids_;
    ComponentId next_component_id_ = 1U;
    std::map<std::string, LayerCacheEntry, std::less<>> layer_cache_;
    std::uint64_t component_cache_epoch_ = 0U;
    std::vector<ComponentEffects*> component_effect_stack_;
    /** Host paths resolved while checking cached reads, for one host generation. */
    struct HostRead final {
        /** The value at the path, once asked for (none when missing). */
        std::optional<std::optional<runtime::Value>> value;
        /** The snapshot owning the path, once asked for. */
        std::optional<std::optional<std::pair<std::string, std::uint64_t>>> origin;
    };
    mutable std::unordered_map<std::string, HostRead, KeyHash, std::equal_to<>> host_reads_;
    mutable std::uint64_t host_reads_count_ = 0U;
};

} // namespace strata::ui
