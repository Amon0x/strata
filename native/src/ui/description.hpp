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
#include <vector>

#include "runtime/application.hpp"
#include "runtime/expression.hpp"
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

/** Materializes a retained description snapshot directly from validated portable IR. */
class DescriptionBuilder final {
public:
    explicit DescriptionBuilder(runtime::ApplicationContext& application);
    DescriptionBuilder(
        runtime::ApplicationContext& application,
        const WidgetRegistry& widgets
    );

    [[nodiscard]] DescriptionBuildResult build(runtime::LayerRole role, std::string_view name);
    /** Supplies the prior retained tree to stateful widget expansion for this build. */
    void set_retained_tree(const RetainedTree* tree);
    /** Whether changing one retained value can affect the currently cached description layers. */
    [[nodiscard]] bool observes_retained_value(
        const RetainedNode& node,
        std::string_view name,
        bool include_lazy_snapshots = true
    ) const;
    /** Supplies request-local host roots without mutating the shared application snapshot. */
    void set_contextual_host_roots(
        std::map<std::string, runtime::Value, std::less<>> roots
    );
    [[nodiscard]] DescriptionLayersBuildResult build_layers(
        std::span<const LayerDescriptionRequest> layers
    );

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

        runtime::ExpressionScope expressions;
        std::string runtime_state_scope;
        std::string declaration_scope;
        std::string instance_path;
        std::map<std::string, WidgetDefault, std::less<>> widget_defaults;
    };

    using ComponentInputs = std::vector<
        std::pair<std::string, runtime::ExpressionDependencyValue>
    >;

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

    struct ComponentEffects final {
        std::map<std::string, runtime::ExpressionHostDependency, std::less<>> host_values;
        std::map<runtime::StateAddress, runtime::Value> state_values;
        std::map<runtime::StateAddress, StateBindingEffect> state_bindings;
        runtime::StateScopeSet owned_state_scopes;
        std::set<std::string, std::less<>> direct_descendant_cache_keys;
        std::set<std::string, std::less<>> descendant_cache_keys;
        std::vector<RetainedValueEffects> retained_values;
        std::vector<RetainedSequenceEffect> retained_sequences;
        bool captures_retained_snapshot = false;
        std::shared_ptr<const RetainedDescriptionSnapshot> retained_snapshot;
    };

    struct ComponentCacheEntry final {
        std::string component;
        std::string source_path;
        ComponentInputs inputs;
        std::uint64_t host_invalidation_count = 0U;
        std::uint64_t last_used_epoch = 0U;
        std::map<std::string, runtime::Value, std::less<>> contextual_host_roots;
        ComponentEffects effects;
        std::shared_ptr<const DescriptionNode> root;
        Scope rebuild_scope;
    };

    struct LayerCacheEntry final {
        runtime::LayerRole role;
        std::string name;
        std::string source_path;
        std::uint64_t host_invalidation_count = 0U;
        std::map<std::string, runtime::Value, std::less<>> contextual_host_roots;
        ComponentEffects effects;
        std::shared_ptr<const DescriptionNode> root;
    };

    enum class ComponentRefreshResult {
        unchanged,
        changed,
        invalid,
    };

    void bind_scope_state(const Scope& scope);
    void bind_state_scope(
        std::string_view runtime_scope,
        std::string_view state_name,
        std::string_view declaration_scope,
        std::string_view address_scope
    );
    void own_state_scope(std::string_view scope);
    void observe_state_value(const runtime::StateAddress& address, const runtime::Value& value);
    void observe_host_dependency(const runtime::ExpressionHostDependency& dependency);
    void observe_retained_value(
        const RetainedQuery& query,
        std::string_view name,
        const runtime::Value* value
    );
    void observe_retained_sequence(
        const RetainedQuery& query,
        const RetainedDescriptionSnapshot::Node* retained
    );
    void capture_retained_snapshot();
    void replay_component_effects(const ComponentEffects& effects);
    [[nodiscard]] std::shared_ptr<const DescriptionNode> build_component_body(
        std::string_view component,
        Scope scope,
        ComponentEffects& effects
    );
    [[nodiscard]] ComponentRefreshResult refresh_component_cache_entry(
        const std::string& cache_key
    );
    [[nodiscard]] static std::shared_ptr<const DescriptionNode> replace_component_subtrees(
        const std::shared_ptr<const DescriptionNode>& root,
        const std::map<
            const DescriptionNode*,
            std::shared_ptr<const DescriptionNode>
        >& replacements
    );
    [[nodiscard]] std::optional<ComponentInputs> component_inputs(
        const DescriptionNode::Properties& properties,
        const std::map<std::string, Scope::WidgetDefault, std::less<>>& widget_defaults
    ) const;
    [[nodiscard]] bool component_cache_entry_current(
        const ComponentCacheEntry& entry,
        std::string_view component,
        std::string_view source_path,
        const ComponentInputs& inputs
    ) const;
    [[nodiscard]] bool component_effects_current(
        const ComponentEffects& effects,
        std::uint64_t host_invalidation_count,
        const std::map<std::string, runtime::Value, std::less<>>& contextual_host_roots
    ) const;
    [[nodiscard]] std::shared_ptr<const DescriptionNode> build_layer(
        runtime::LayerRole role,
        std::string_view name
    );

    [[nodiscard]] std::vector<std::shared_ptr<const DescriptionNode>> build_block(
        data::JsonView block,
        Scope scope,
        std::span<const std::size_t> skipped_statement_indices = {}
    );
    [[nodiscard]] RepeaterChildren build_repeater_children(
        data::JsonView block,
        Scope scope,
        const RetainedDescriptionSnapshot::Node* retained_widget
    );
    [[nodiscard]] std::string evaluate_repeater_identity(
        data::JsonView identity,
        const Scope& scope
    );
    [[nodiscard]] const RetainedDescriptionSnapshot::Node* retained_widget(
        const RetainedQuery& query
    ) const noexcept;
    [[nodiscard]] std::shared_ptr<const DescriptionNode> build_call(
        data::JsonView call,
        Scope scope
    );
    [[nodiscard]] std::shared_ptr<const DescriptionNode> build_component_template(
        std::string_view component,
        std::string key,
        WidgetTemplateArguments arguments,
        const Scope& caller
    );
    [[nodiscard]] runtime::ExpressionValue evaluate(
        data::JsonView expression,
        const runtime::ExpressionScope& scope
    );
    [[nodiscard]] runtime::Value require_value(
        const runtime::ExpressionValue& value,
        data::JsonView expression
    );
    [[nodiscard]] std::vector<DescriptionBehavior> build_behaviors(
        data::JsonView expression,
        const runtime::ExpressionScope& scope
    );
    void append_diagnostics(runtime::ExpressionRuntime& expressions);
    [[nodiscard]] runtime::Value resolve_style(
        const runtime::Value& value,
        const runtime::ExpressionScope& scope,
        std::set<std::string, std::less<>>& resolving
    );
    [[nodiscard]] runtime::Value resolve_named_style(
        std::string_view name,
        const runtime::ExpressionScope& scope,
        std::set<std::string, std::less<>>& resolving
    );
    void normalize_layout(
        DescriptionNode::Properties& properties,
        const runtime::ExpressionScope& scope
    );

    runtime::ApplicationContext& application_;
    std::unique_ptr<WidgetRegistry> owned_widgets_;
    const WidgetRegistry& widgets_;
    std::vector<runtime::RuntimeDiagnostic> diagnostics_;
    std::size_t evaluated_expressions_ = 0U;
    std::size_t described_nodes_ = 0U;
    runtime::StateScopeSet current_layer_state_scopes_;
    std::map<std::string, runtime::Value, std::less<>> resolved_styles_;
    std::unique_ptr<runtime::ExpressionRuntime> expressions_;
    std::map<std::string, runtime::Value, std::less<>> contextual_host_roots_;
    std::shared_ptr<const RetainedDescriptionSnapshot> retained_snapshot_;
    std::shared_ptr<const runtime::RuntimeUnit> component_cache_unit_;
    std::map<std::string, ComponentCacheEntry, std::less<>> component_cache_;
    std::map<std::string, LayerCacheEntry, std::less<>> layer_cache_;
    std::set<std::string, std::less<>> visited_component_cache_keys_;
    std::set<std::string, std::less<>> refreshing_component_cache_keys_;
    std::uint64_t component_cache_epoch_ = 0U;
    std::vector<ComponentEffects*> component_effect_stack_;
};

} // namespace strata::ui
