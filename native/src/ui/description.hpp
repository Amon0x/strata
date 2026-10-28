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
#include <unordered_set>
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
    /** Layer and component bodies the build described (generated rows are counted apart). */
    std::size_t described_instances = 0U;
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
    std::size_t described_instances = 0U;
};

/** What a builder did over its life: how far each change reached. */
struct DescriptionTelemetry final {
    std::uint64_t builds = 0U;
    /** Layer and component bodies described. */
    std::uint64_t described_instances = 0U;
    /** Bodies whose output took a changed component's new output without being described. */
    std::uint64_t patched_instances = 0U;
    /** Generated rows described, and rows handed out again as they were. */
    std::uint64_t described_rows = 0U;
    std::uint64_t reused_rows = 0U;
    /** Instances a change they read made stale. */
    std::uint64_t invalidated_instances = 0U;
};

/**
 * Materializes a retained description snapshot from the active unit's lowered program.
 *
 * Every layer, component call, generated child provider and generated row it describes is a
 * component instance: one object owning the instance's identity, the inputs it was called with,
 * what its body read, its output, and the state scopes it binds and owns. Instances form a tree;
 * a provider's rows are child instances of it, described by this builder when they are realized.
 */
class DescriptionBuilder final {
  public:
    explicit DescriptionBuilder(runtime::ApplicationContext& application);
    DescriptionBuilder(runtime::ApplicationContext& application, const WidgetRegistry& widgets);
    ~DescriptionBuilder();
    DescriptionBuilder(const DescriptionBuilder&) = delete;
    DescriptionBuilder& operator=(const DescriptionBuilder&) = delete;

    [[nodiscard]] DescriptionBuildResult build(runtime::LayerRole role, std::string_view name);
    /** Supplies the prior retained tree to stateful widget expansion for this build. */
    void set_retained_tree(const RetainedTree* tree);
    /** Whether changing one retained value can affect the current description. */
    [[nodiscard]] bool observes_retained_value(const RetainedNode& node,
                                               std::string_view name) const;
    /** Supplies request-local host roots without mutating the shared application snapshot. */
    void set_contextual_host_roots(std::map<std::string, runtime::Value, std::less<>> roots);
    [[nodiscard]] DescriptionLayersBuildResult
    build_layers(std::span<const LayerDescriptionRequest> layers);
    [[nodiscard]] const DescriptionTelemetry& telemetry() const noexcept {
        return telemetry_;
    }

  private:
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
        /** The keyed component instances one run of a loop has placed beside its items. */
        using KeyedInstances = std::set<std::string, std::less<>>;
        struct LoopItem final {
            /** The length of the instance path that holds the loop. */
            std::size_t parent_length = 0U;
            std::shared_ptr<KeyedInstances> keyed;
        };
        /** Set while the instance is one item of a loop. */
        std::optional<LoopItem> loop;

        [[nodiscard]] const std::string& instance_path() const noexcept;
        /** Moves the scope to another instance, which its expressions then name. */
        void set_instance(std::string path);
        /** Moves the scope to one item, identified by `identity`, of a loop repeating in it. */
        void enter_loop_item(std::string_view item, std::string_view identity,
                             std::shared_ptr<KeyedInstances> keyed);
    };

    /** A component call's instance path in `scope`; see the definition for keyed calls. */
    [[nodiscard]] static std::string keyed_instance_path(const Scope& scope, std::string_view type,
                                                         const std::optional<std::string>& key,
                                                         std::string_view source_path);

    /** A component call's inputs, in parameter order: compared position by position. */
    using ComponentInputs = std::vector<runtime::ExpressionValue>;

    struct StateBindingEffect final {
        std::string declaration_scope;
        std::string address_scope;

        [[nodiscard]] friend bool operator==(const StateBindingEffect&,
                                             const StateBindingEffect&) = default;
    };
    using StateBindings = std::map<runtime::StateAddress, StateBindingEffect, std::less<>>;

    struct RetainedQuery final {
        std::optional<std::string> key;
        std::string source_path;
        std::string state_scope;
        std::string type;

        [[nodiscard]] friend bool operator==(const RetainedQuery&, const RetainedQuery&) = default;
    };

    /** One retained value a body read, as it read it (none when the node or value was absent). */
    struct RetainedRead final {
        RetainedQuery query;
        std::string name;
        std::optional<runtime::Value> value;
    };

    /** What one body read itself: the instances it called read for themselves. */
    struct InstanceReads final {
        std::map<std::string, runtime::ExpressionHostDependency, std::less<>> host;
        std::map<runtime::StateAddress, runtime::Value> state;
        std::vector<RetainedRead> retained;
        /**
         * The body raised an error. Build diagnostics are reported by every build, so such a body
         * is described again by each one; the bodies around it keep their output and take its new
         * one.
         */
        bool raised_errors = false;
    };

    using InstanceId = std::uint64_t;

    enum class InstanceKind : std::uint8_t {
        /** A screen or overlay declaration's body. */
        layer,
        /** A component call's body. */
        component,
        /** A widget call that provides its children as generated rows (a Repeater, a table). */
        generator,
        /** One generated row of a generator. */
        row,
    };

    /**
     * What the caller does to an instance's output where it calls it: projects its content into
     * the slots the body declares, attaches behaviors to its root, and, for the first item of a
     * loop, names the item and its position. The instance's emitted output is its body's output
     * so placed; a body described alone is placed again the same way.
     */
    struct CallSite final {
        std::string component_path;
        std::map<std::string, std::vector<std::shared_ptr<const DescriptionNode>>, std::less<>>
            projected;
        std::vector<DescriptionBehavior> behaviors;
        std::optional<std::string> materialization_key;
        std::optional<double> loop_position;
    };

    /** A child instance and its output as the parent's output holds it. */
    struct ChildRef final {
        InstanceId id = 0U;
        std::shared_ptr<const DescriptionNode> seen;
    };

    struct RowSource;

    struct ComponentInstance final {
        InstanceId id = 0U;
        InstanceKind kind = InstanceKind::component;
        /** The registry key: kind, type and instance path. */
        std::string key;
        InstanceId parent = 0U;
        std::uint32_t depth = 0U;
        /** The generated row this instance is described within, if any (a row is its own). */
        InstanceId row = 0U;
        /** The layer whose state scopes this instance's count toward; none within a row. */
        InstanceId scope_layer = 0U;

        /** The scope its body is described in; its instance path is the instance's identity. */
        Scope scope;
        // A component's inputs.
        std::uint32_t component_index = runtime::no_program_id;
        std::string source_path;
        ComponentInputs inputs;
        std::shared_ptr<const runtime::HostRoots> contextual_host_roots;
        // A layer's declaration.
        runtime::LayerRole role = runtime::LayerRole::screen;
        std::string name;

        InstanceReads reads;
        /** The body's output, and the output as the caller places it. */
        std::shared_ptr<const DescriptionNode> root;
        CallSite call_site;
        std::shared_ptr<const DescriptionNode> emitted;
        /** The slots `root` declares itself, for the root they were collected from. */
        std::set<std::string, std::less<>> declared_slots;
        std::shared_ptr<const DescriptionNode> declared_slots_root;

        /** The state scopes the body binds and owns itself. */
        StateBindings bindings;
        runtime::StateScopeSet owned_scopes;
        /** What of those the application holds now: bound for the instance, counted for a layer. */
        StateBindings bound;
        runtime::StateScopeSet counted_scopes;
        InstanceId counted_layer = 0U;
        /** A layer's count of the state scopes its instances own. */
        std::map<std::string, std::size_t, std::less<>> layer_scope_counts;
        std::optional<runtime::StateScopeSet> layer_scopes;

        std::vector<ChildRef> children;

        /** What it read changed since it was described. */
        bool dirty = false;
        /** Reached from a layer the last build described. */
        bool attached = false;
        /** The build that last described it, or confirmed it current. */
        std::uint64_t settled_epoch = 0U;
        std::uint64_t last_used_epoch = 0U;

        // A generator's rows: the provider they are described from, and the rows by key.
        std::shared_ptr<RowSource> rows_source;
        std::map<std::string, InstanceId, std::less<>> rows;
        /** A Repeater's last generation, kept again while what it read stays the same. */
        std::shared_ptr<const runtime::IndexableSequence> sequence;
        std::optional<DescriptionSequenceGeneration> sequence_generation;
        /** A row changed: the generator hands out a new provider over the same source. */
        bool reprovision = false;

        // A row's inputs: the source it was described from, its index there, and the values of
        // the enclosing scope its body read.
        std::shared_ptr<const RowSource> row_source;
        std::size_t row_index = 0U;
        std::map<runtime::Symbol, runtime::ExpressionDependencyValue> outer_reads;
    };

    /** The body being described, and what it records. */
    struct Frame final {
        ComponentInstance* instance = nullptr;
        InstanceReads reads;
        std::vector<ChildRef> children;
        /** Children by the output this body holds of them. */
        std::unordered_map<const DescriptionNode*, std::size_t> child_index;
        StateBindings bindings;
        runtime::StateScopeSet owned_scopes;
        /** A row's enclosing scope: reads of its bindings are the row's inputs. */
        const runtime::ExpressionScope* outer = nullptr;
        std::map<runtime::Symbol, runtime::ExpressionDependencyValue> outer_reads;
    };

    /** A state a body declared, to be stored when the build that described it commits. */
    struct StateInitialization final {
        runtime::StateAddress address;
        runtime::StateSlot slot;
        /** The durable value that becomes the state's first value. */
        std::optional<runtime::Value> persisted;
    };

    /**
     * What a build (or one row's realization) changes outside the builder. Describing reads only;
     * these are applied once the whole description succeeded.
     */
    struct Transaction final {
        std::vector<StateInitialization> states;
        std::vector<std::string> erased_application_values;
        std::vector<std::pair<std::string, std::string>> erased_widget_values;
        std::vector<runtime::RuntimeDiagnostic> reports;
        /** Instances whose bindings, owned scopes or attachment may have changed. */
        std::vector<InstanceId> touched;
    };

    /** Records the reads expressions make into the body being described. */
    class FrameObserver final : public runtime::ExpressionDependencyObserver {
      public:
        explicit FrameObserver(DescriptionBuilder& builder) noexcept : builder_(builder) {}
        void lexical(runtime::Symbol name, const runtime::ExpressionValue& value) override;
        void host(const runtime::ExpressionHostDependency& dependency) override;

      private:
        DescriptionBuilder& builder_;
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

    /** A Repeater's generation and the provider of its rows. */
    struct RepeaterChildren final {
        std::shared_ptr<const DescriptionChildren> source;
        std::shared_ptr<const runtime::IndexableSequence> sequence;
        DescriptionSequenceGeneration generation;
    };

    [[nodiscard]] const runtime::Program& program() const noexcept {
        return unit_->program();
    }
    /** Makes `unit` the one descriptions are built from, dropping what the last one cached. */
    void use_unit(const std::shared_ptr<const runtime::RuntimeUnit>& unit);
    /**
     * The registry as generated rows describe with it after this build: rows may be realized
     * after the registry changes, so they keep a copy, made once per registry revision.
     */
    [[nodiscard]] std::shared_ptr<const WidgetRegistry> widget_snapshot();
    /** The widget a call names, looked up once per call site. */
    [[nodiscard]] const WidgetLifecycle* call_widget(runtime::CallId call);

    // Describing (description.cpp).
    [[nodiscard]] std::vector<std::shared_ptr<const DescriptionNode>>
    build_block(runtime::BlockId block, const Scope& scope,
                std::span<const std::size_t> skipped_statement_indices = {});
    /** Gives a loop item's node its item key and position, where its owner places it. */
    [[nodiscard]] std::shared_ptr<const DescriptionNode>
    annotate_loop_item(const std::shared_ptr<const DescriptionNode>& node,
                       std::optional<std::string> materialization_key,
                       std::optional<double> loop_position);
    [[nodiscard]] RepeaterChildren
    build_repeater_children(runtime::BlockId block, const Scope& scope,
                            ComponentInstance& generator,
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
    [[nodiscard]] bool
    same_contextual_host_roots(const std::shared_ptr<const runtime::HostRoots>& roots) const;
    /** Whether a host read would read the same now. */
    [[nodiscard]] bool host_read_current(std::string_view canonical,
                                         const runtime::ExpressionHostDependency& dependency) const;

    // Recording what the body being described reads and declares.
    void bind_state_scope(std::string_view runtime_scope, std::string_view state_name,
                          std::string_view declaration_scope, std::string_view address_scope);
    void own_state_scope(std::string_view scope);
    void observe_state_value(const runtime::StateAddress& address, const runtime::Value& value);
    void observe_host_dependency(const runtime::ExpressionHostDependency& dependency);
    void observe_retained_value(const RetainedQuery& query, std::string_view name,
                                const runtime::Value* value);
    [[nodiscard]] Frame* frame() noexcept {
        return frames_.empty() ? nullptr : frames_.back();
    }

    // Instances (description_instances.cpp).
    [[nodiscard]] ComponentInstance* find_instance(InstanceId id) noexcept;
    [[nodiscard]] const ComponentInstance* find_instance(InstanceId id) const noexcept;
    /** The instance a key names, made on first use and placed under `parent`. */
    [[nodiscard]] ComponentInstance& instance(InstanceKind kind, std::string key,
                                              ComponentInstance* parent);
    /** The body being described holds `child`'s emitted output among its own. */
    void hold_child(ComponentInstance& child);
    /** Describes a layer, component or row body in its stored scope. */
    void describe(ComponentInstance& instance);
    void describe_layer(ComponentInstance& layer);
    void describe_component(ComponentInstance& component);
    /** Runs `body` as `instance`'s description and installs what it recorded. */
    template <typename Body>
    [[nodiscard]] std::shared_ptr<const DescriptionNode>
    describe_body(ComponentInstance& instance, const runtime::ExpressionScope* outer, Body&& body);
    void install_frame(ComponentInstance& instance, Frame& frame);
    /** Places the body's output as its caller does: what the caller's output holds of it. */
    void emit(ComponentInstance& instance);
    [[nodiscard]] const std::set<std::string, std::less<>>&
    declared_slots(ComponentInstance& instance);
    void attach(ComponentInstance& instance);
    void detach(ComponentInstance& instance);
    /** Notes that the instance's bindings or owned scopes may differ from what is applied. */
    void touch(ComponentInstance& instance);
    void mark_dirty(ComponentInstance& instance);
    /** Queues a stale instance: a body to describe, or a row whose generator hands it out again. */
    void schedule(ComponentInstance& instance);
    void forget(ComponentInstance& instance);
    /** Withdraws the bindings and counted scopes the application holds for an instance. */
    void release(ComponentInstance& instance) noexcept;
    void count_scopes(ComponentInstance& instance);
    void uncount_scopes(ComponentInstance& instance) noexcept;
    [[nodiscard]] const runtime::StateScopeSet& layer_scopes(ComponentInstance& layer);
    /** Whether everything the instance's body read would read the same now. */
    [[nodiscard]] bool reads_current(const ComponentInstance& instance) const;
    /** Marks stale every instance whose reads changed since the last build. */
    void collect_invalidations();
    void flush_reprovisions();
    /** Describes the stale instances, shallowest first. */
    void describe_stale();
    /** Hands a generator's rows out through a new provider, so a changed row is asked again. */
    void reprovision(ComponentInstance& generator);
    /** Takes changed outputs into their callers', up to the layers. */
    void patch_ancestors();
    [[nodiscard]] bool patch(ComponentInstance& instance);
    /** Applies what the build changes outside the builder. */
    void commit(Transaction& transaction);
    void evict();
    /** Drops every instance, as after a build that failed part way. */
    void reset() noexcept;

    // Generated rows (description_instances.cpp).
    /** The generator that provides a widget call's rows, under the body being described. */
    [[nodiscard]] ComponentInstance& generator(const Scope& scope, const runtime::ProgramCall& call,
                                               const std::optional<std::string>& key);
    [[nodiscard]] std::shared_ptr<const DescriptionChildren>
    provide(ComponentInstance& generator, std::shared_ptr<RowSource> source);
    [[nodiscard]] static std::shared_ptr<const DescriptionNode>
    realize(const std::shared_ptr<const RowSource>& source, std::size_t index);
    [[nodiscard]] std::shared_ptr<const DescriptionNode>
    realize_row(const std::shared_ptr<const RowSource>& source, std::size_t index);
    [[nodiscard]] bool row_current(const ComponentInstance& row, const RowSource& source,
                                   std::size_t index) const;
    [[nodiscard]] std::shared_ptr<const DescriptionNode>
    describe_row(ComponentInstance& row, const std::shared_ptr<const RowSource>& source,
                 std::size_t index);
    [[nodiscard]] runtime::StateScopeSet row_owned_scopes(const ComponentInstance& row) const;

    runtime::ApplicationContext& application_;
    /** This builder while it lives: generated rows describe with it, or alone once it is gone. */
    std::shared_ptr<DescriptionBuilder*> self_;
    std::unique_ptr<WidgetRegistry> owned_widgets_;
    const WidgetRegistry& widgets_;
    std::vector<runtime::RuntimeDiagnostic> diagnostics_;
    std::size_t evaluated_expressions_ = 0U;
    std::size_t described_nodes_ = 0U;
    std::size_t described_instances_ = 0U;
    std::map<std::string, ResolvedStyle, std::less<>> styles_;
    std::unique_ptr<runtime::ExpressionRuntime> expressions_;
    FrameObserver frame_observer_;
    std::shared_ptr<const runtime::HostRoots> contextual_host_roots_;
    bool contextual_host_roots_changed_ = false;
    std::shared_ptr<const RetainedDescriptionSnapshot> retained_snapshot_;
    /** The unit descriptions are built from; everything cached is for it. */
    std::shared_ptr<const runtime::RuntimeUnit> unit_;
    /** Per call site of the unit's program, the widget it names, for one registry revision. */
    std::vector<std::optional<const WidgetLifecycle*>> call_widgets_;
    std::uint64_t call_widgets_revision_ = 0U;
    std::shared_ptr<const WidgetRegistry> widget_snapshot_;
    std::uint64_t widget_snapshot_revision_ = 0U;

    struct KeyHash final {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(const std::string_view key) const noexcept {
            return std::hash<std::string_view>{}(key);
        }
    };
    std::unordered_map<InstanceId, std::unique_ptr<ComponentInstance>> instances_;
    std::unordered_map<std::string, InstanceId, KeyHash, std::equal_to<>> instance_ids_;
    InstanceId next_instance_id_ = 1U;
    /** The layers the last build described, in order. */
    std::vector<InstanceId> layers_;
    std::uint64_t epoch_ = 0U;
    std::vector<Frame*> frames_;
    Transaction* transaction_ = nullptr;
    /** Stale instances, and instances whose emitted output changed, this build. */
    std::vector<InstanceId> stale_;
    std::vector<InstanceId> emitted_changed_;
    std::vector<InstanceId> reprovisioned_;
    std::size_t eviction_threshold_ = 1024U;
    DescriptionTelemetry telemetry_;

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
