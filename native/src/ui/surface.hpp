#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/application.hpp"
#include "runtime/host_services.hpp"
#include "runtime/layer.hpp"
#include "runtime/profiler.hpp"
#include "ui/command.hpp"
#include "ui/behavior/registry.hpp"
#include "ui/description.hpp"
#include "ui/input.hpp"
#include "ui/layout.hpp"
#include "ui/motion.hpp"
#include "ui/notification.hpp"
#include "ui/render.hpp"
#include "ui/render/material_registry.hpp"
#include "ui/semantics.hpp"
#include "ui/status.hpp"
#include "ui/text.hpp"
#include "ui/theme.hpp"
#include "ui/tree.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {

enum class SurfaceDensity { compact, comfortable };
enum class PointerPrecision { none, coarse, fine };

struct SurfaceInputCapabilities final {
    bool pointer = false;
    PointerPrecision pointer_precision = PointerPrecision::none;
    bool keyboard = false;
    bool touch = false;
    bool ime = false;
    bool clipboard = false;
    bool controller = false;

    [[nodiscard]] friend bool operator==(
        const SurfaceInputCapabilities&,
        const SurfaceInputCapabilities&
    ) = default;
};

/** Complete caller-owned environment snapshot adopted atomically at a frame boundary. */
struct SurfaceEnvironment final {
    std::uint64_t generation = 1U;
    std::int64_t framebuffer_width = 0;
    std::int64_t framebuffer_height = 0;
    double logical_width = 0.0;
    double logical_height = 0.0;
    double scale = 1.0;
    Edges safe_insets;
    PointSnapPolicy point_snapping = PointSnapPolicy::nearest;
    RectangleSnapPolicy rectangle_snapping = RectangleSnapPolicy::outward;
    SurfaceDensity density = SurfaceDensity::comfortable;
    bool reduced_motion = false;
    SurfaceInputCapabilities input;

    void validate() const;
    [[nodiscard]] LayoutEnvironment layout_environment(std::int64_t frame_time_nanos) const;
    [[nodiscard]] friend bool operator==(const SurfaceEnvironment&, const SurfaceEnvironment&) = default;
};

struct SurfaceOperationCounters final {
    std::size_t rebuilds = 0U;
    std::size_t described_nodes = 0U;
    std::size_t evaluated_expressions = 0U;
    std::size_t layout_measured_nodes = 0U;
    std::size_t layout_arranged_nodes = 0U;
    std::size_t layout_measurement_cache_hits = 0U;
    std::size_t injected_events = 0U;
    std::size_t input_events_processed = 0U;
    std::size_t input_events_deferred = 0U;
    std::size_t input_dispatches = 0U;
    std::size_t input_mutated_nodes = 0U;
    std::size_t input_coalesced_moves = 0U;
    std::size_t behavior_dispatches = 0U;
    std::size_t pointer_geometry_rebuilds = 0U;
    std::size_t input_fast_path_frames = 0U;
    std::uint64_t input_nanos = 0U;
    std::size_t motion_mutated_nodes = 0U;
    std::size_t motion_running_players = 0U;
    std::uint64_t animation_nanos = 0U;
    std::uint64_t layout_nanos = 0U;
    std::size_t resource_reloads = 0U;
    std::uint64_t reload_duration_nanos = 0U;
    RenderOperationCounters render;
    TextOperationCounters text;
};

struct SurfaceFrame final {
    std::uint64_t frame_index = 0U;
    std::int64_t frame_time_nanos = 0;
    SurfaceOperationCounters operations;
    runtime::RuntimeDiagnosticsSnapshot diagnostics;
    InputOperationResult lifecycle_input;
};

struct SurfaceEventRecord final {
    std::uint64_t sequence = 0U;
    std::uint64_t frame_index = 0U;
    data::JsonValue event;
    data::JsonValue action_outcome;
};

struct SurfaceEventDrain final {
    std::vector<SurfaceEventRecord> records;
    std::uint64_t dropped_count = 0U;
};

struct ScrollAnimationRequest final {
    std::string key;
    std::optional<double> x;
    std::optional<double> y;
    std::string timing = std::string(default_motion_timing_name);
    std::optional<std::int64_t> duration_nanos;
};

/** Fully prepared text/layout half of an atomic host-resource reload. */
struct SurfaceResourceReloadPlan final {
    std::shared_ptr<const TextEngine> text_engine;
    LayoutEngine layout_engine;
};

namespace surface_detail {

/** Canonical state of one attached lazy producer at a layout/reconcile boundary. */
struct LazyRangeState final {
    std::string structural_path;
    std::size_t child_count = 0U;
    MaterializationRange materialized;
    MaterializationRange visible;
    bool stale_projection = false;

    [[nodiscard]] friend bool operator==(const LazyRangeState&, const LazyRangeState&) = default;
    [[nodiscard]] friend bool operator<(
        const LazyRangeState& left,
        const LazyRangeState& right
    ) noexcept {
        if (left.structural_path != right.structural_path) {
            return left.structural_path < right.structural_path;
        }
        if (left.child_count != right.child_count) return left.child_count < right.child_count;
        if (left.materialized.start != right.materialized.start) {
            return left.materialized.start < right.materialized.start;
        }
        if (left.materialized.end_exclusive != right.materialized.end_exclusive) {
            return left.materialized.end_exclusive < right.materialized.end_exclusive;
        }
        if (left.visible.start != right.visible.start) {
            return left.visible.start < right.visible.start;
        }
        if (left.visible.end_exclusive != right.visible.end_exclusive) {
            return left.visible.end_exclusive < right.visible.end_exclusive;
        }
        return left.stale_projection < right.stale_projection;
    }
};

using LazyRangeSignature = std::vector<LazyRangeState>;

enum class LazyConvergenceStatus {
    progress,
    fixed_point,
    cycle,
};

/**
 * Tracks the deterministic finite range-state machine without an arbitrary retry limit.
 * Every non-terminal observation is new; a repeated signature is a proven cycle.
 */
class LazyConvergenceTracker final {
public:
    [[nodiscard]] LazyConvergenceStatus observe(LazyRangeSignature signature);
    [[nodiscard]] std::size_t observed_state_count() const noexcept;
    /** Saturating upper bound for the range states of every producer observed so far. */
    [[nodiscard]] std::size_t known_state_bound() const noexcept;

private:
    struct SignatureLess final {
        [[nodiscard]] bool operator()(
            const LazyRangeSignature& left,
            const LazyRangeSignature& right
        ) const noexcept;
    };

    std::set<LazyRangeSignature, SignatureLess> observed_;
    std::set<std::pair<std::string, std::size_t>> producers_;
};

/** Lifetime-aware exact-once publication ledger for generated-row transactions. */
enum class MaterializationPublicationClaim {
    publish,
    already_published,
    invalid_transaction,
    duplicate_live_identity,
};

class MaterializationPublicationLedger final {
public:
    [[nodiscard]] MaterializationPublicationClaim claim(
        const std::shared_ptr<const DescriptionMaterialization>& transaction
    );
    void purge_expired();
    [[nodiscard]] std::size_t tracked_count() const noexcept;

private:
    std::map<std::uint64_t, std::weak_ptr<const DescriptionMaterialization>> published_;
};

} // namespace surface_detail

/**
 * One isolated retained surface. Description, reconciliation and layout are a single staged
 * pipeline; later input, semantics and rendering consume these same retained identities.
 */
class Surface final {
public:
    Surface(
        std::string id,
        runtime::ApplicationContext& application,
        runtime::LayerRole root_role,
        std::string root_name,
        SurfaceEnvironment environment,
        std::shared_ptr<const TextEngine> text_engine = {},
        WidgetRegistry widget_registry = WidgetRegistry{},
        BehaviorRegistry behavior_registry = BehaviorRegistry{},
        runtime::HostServices* host_services = nullptr,
        Theme initial_theme = Theme{},
        std::string host_service_owner = {}
    );
    ~Surface();

    [[nodiscard]] const std::string& id() const noexcept;
    [[nodiscard]] runtime::ApplicationContext& application() const noexcept;
    [[nodiscard]] const SurfaceEnvironment& environment() const noexcept;
    [[nodiscard]] runtime::Profiler& profiler() noexcept;
    [[nodiscard]] const runtime::Profiler& profiler() const noexcept;
    [[nodiscard]] std::string_view viewport_class() const noexcept;
    [[nodiscard]] bool adopt_environment(SurfaceEnvironment environment);
    /** Adopts only framebuffer/logical scale fields and advances their generation domain. */
    [[nodiscard]] bool adopt_scale_context(const SurfaceEnvironment& environment);
    /** Adopts input, inset, density, and motion preferences as a separate generation domain. */
    [[nodiscard]] bool adopt_environment_preferences(const SurfaceEnvironment& environment);
    /** Invalidates resource-derived text/style/render state without rebuilding the application. */
    void invalidate_resources();
    /** Performs every potentially throwing text/layout step without changing the live surface. */
    [[nodiscard]] SurfaceResourceReloadPlan prepare_resource_reload(
        std::shared_ptr<const TextEngine> text_engine
    ) const;
    /** Installs a matching prepared text/layout state; frame-time invalidation remains retryable. */
    void commit_resource_reload(SurfaceResourceReloadPlan plan) noexcept;
    /** Atomically adopts a fully constructed text engine and invalidates dependent caches. */
    void replace_text_engine(std::shared_ptr<const TextEngine> text_engine);
    /** Completes the reload measurement queued by replace_text_engine for the next frame. */
    void note_resource_reload_duration(std::uint64_t duration_nanos) noexcept;
    void invalidate() noexcept;
    /** Releases all surface-owned interaction state; lifecycle output is published next frame. */
    void cancel_interactions();
    /** Injects an action as a surface event and publishes its event/outcome on the next frame. */
    [[nodiscard]] runtime::ActionDispatchOutcome dispatch_action(
        std::string action_id,
        runtime::Value payload,
        std::string event_kind,
        std::optional<std::string> source_key,
        runtime::Value event_value,
        bool dynamic = false
    );
    [[nodiscard]] SurfaceFrame frame(std::int64_t frame_time_nanos);
    /** Drains the bounded ordered event/outcome publication stream exactly once. */
    [[nodiscard]] SurfaceEventDrain drain_events();
    /** Queues work completed before begin-frame for publication on the next frame call. */
    void report_diagnostic(runtime::RuntimeDiagnostic diagnostic);
    /** Clears Surface-owned pending diagnostics and its last-frame diagnostic projection only. */
    void clear_diagnostics() noexcept;

    [[nodiscard]] const RetainedTree& tree() const noexcept;
    [[nodiscard]] RetainedTree& tree() noexcept;
    [[nodiscard]] const LayoutResult& layout() const noexcept;
    [[nodiscard]] const SurfaceFrame& last_frame() const noexcept;
    [[nodiscard]] InputRouter& input() noexcept;
    [[nodiscard]] const InputRouter& input() const noexcept;
    [[nodiscard]] const SemanticsEngine& semantics() const noexcept;
    [[nodiscard]] const CommandIndex& commands() const noexcept;
    [[nodiscard]] const WidgetRegistry& widget_registry() const noexcept;
    [[nodiscard]] const BehaviorRegistry& behavior_registry() const noexcept;
    [[nodiscard]] const MotionRuntime& motion() const noexcept;
    [[nodiscard]] const Theme& theme() const noexcept;
    [[nodiscard]] const std::shared_ptr<const Theme>* registered_theme(
        std::string_view name
    ) const noexcept;
    /** Registers/replaces a named immutable theme and invalidates materialized theme state. */
    [[nodiscard]] bool register_theme(Theme theme);
    /** Registers and selects a root theme. */
    [[nodiscard]] bool set_theme(Theme theme);
    /** Unknown names and the active root are not removable. */
    [[nodiscard]] bool unregister_theme(std::string_view name);
    /** Installs an immutable local theme scope at a keyed description node. */
    [[nodiscard]] bool set_scoped_theme(std::string node_key, Theme theme);
    [[nodiscard]] bool clear_scoped_theme(std::string_view node_key);
    /** Interruptibly animates a keyed scroll container through the canonical input mutation path. */
    [[nodiscard]] bool animate_scroll_to(ScrollAnimationRequest request);
    [[nodiscard]] std::size_t active_scroll_animation_count() const noexcept;
    [[nodiscard]] NotificationService& notifications() noexcept;
    [[nodiscard]] const NotificationService& notifications() const noexcept;
    [[nodiscard]] const TextEngine* text_engine() const noexcept;
    [[nodiscard]] const RenderCommandBuffer& render_commands() const noexcept;
    [[nodiscard]] std::vector<runtime::LayerSnapshot> layer_snapshot() const;
    [[nodiscard]] bool inspect_select(std::string_view key);
    [[nodiscard]] bool inspect_pick(Point position);
    void inspect_clear() noexcept;
    [[nodiscard]] const RetainedNode* inspected_node() const noexcept;
    [[nodiscard]] bool set_focus_containment(std::optional<std::string_view> key);
    [[nodiscard]] bool focus_contained() const noexcept;

private:
    void advance_environment_generation();
    void queue_resource_invalidation() noexcept;
    void apply_pending_resource_invalidation();
    void invalidate_frame() noexcept;
    void invalidate_description() noexcept;
    [[nodiscard]] std::shared_ptr<const DescriptionNode> describe(
        DescriptionBuildResult& result
    );
    [[nodiscard]] runtime::ActionDispatchOutcome execute_environment_action(
        const runtime::Action& action
    );
    [[nodiscard]] bool rebuild_tree(
        SurfaceFrame& frame,
        std::optional<std::string_view>& restore_focus
    );
    [[nodiscard]] std::shared_ptr<const DescriptionNode> project_description_theme();
    [[nodiscard]] ReconcileStats realize_virtual_children(const LayoutResult& layout);
    void report_unknown_theme_timing(
        std::string_view name,
        const DescriptionNode& node
    );
    void advance_scroll_animations(std::int64_t frame_time_nanos);
    void retain_scroll_animations();
    void sample_motion(
        SurfaceFrame& frame,
        std::int64_t frame_time_nanos,
        bool temporal
    );
    void layout_tree(SurfaceFrame& frame, std::int64_t frame_time_nanos);
    void commit_lazy_materializations(SurfaceFrame& frame);
    void collect_input_profiler_counters(SurfaceFrame& frame);
    void notifications_changed(const NotificationChange& change);
    void record_profiler_counters(const SurfaceFrame& frame);
    [[nodiscard]] SurfaceFrame complete_frame(SurfaceFrame frame);

    std::string id_;
    runtime::Profiler profiler_;
    runtime::ApplicationContext& application_;
    runtime::LayerRole root_role_;
    std::string root_name_;
    SurfaceEnvironment environment_;
    std::uint64_t adopted_environment_generation_ = 0U;
    std::string viewport_class_;
    BehaviorRegistry behaviors_;
    WidgetRegistry widgets_;
    DescriptionBuilder descriptions_;
    ThemeCatalog themes_;
    ThemeMaterializationCache theme_materialization_cache_;
    std::shared_ptr<const DescriptionNode> raw_description_;
    RetainedTree tree_;
    std::shared_ptr<const TextEngine> text_engine_;
    MotionRuntime motion_;
    LayoutEngine layout_engine_;
    StatusFeedbackService status_feedback_;
    NotificationService notifications_;
    InputRouter input_;
    CommandIndex commands_;
    SemanticsEngine semantics_;
    MaterialRegistry material_registry_;
    RenderEngine render_engine_;
    RenderCommandBuffer render_commands_;
    std::uint64_t observed_application_generation_ = 0U;
    std::optional<std::string> observed_active_screen_;
    std::map<std::string, std::string, std::less<>> focus_by_screen_;
    std::map<std::string, runtime::StateScopeSet, std::less<>> state_scopes_by_layer_;
    std::vector<std::shared_ptr<const DescriptionMaterialization>>
        pending_lazy_materializations_;
    surface_detail::MaterializationPublicationLedger materialization_publications_;
    std::set<std::string, std::less<>> reported_theme_motion_diagnostics_;
    std::set<std::string, std::less<>> reported_lazy_convergence_diagnostics_;
    struct ScrollAnimation final {
        std::uint64_t identity = 0U;
        Point start;
        Point target;
        MotionTiming timing;
        std::optional<std::int64_t> started_nanos;
    };
    std::map<std::string, ScrollAnimation, std::less<>> scroll_animations_;
    std::optional<std::uint64_t> inspected_identity_;
    std::uint64_t scale_context_generation_ = 0U;
    /** Surface frame index whose temporal motion clocks were sampled; discovery is separate. */
    std::uint64_t motion_sampled_frame_index_ = 0U;
    std::size_t pending_resource_reloads_ = 0U;
    std::uint64_t pending_reload_duration_nanos_ = 0U;
    bool pending_reload_timing_recorded_ = false;
    std::size_t queued_resource_invalidations_ = 0U;
    bool resource_invalidation_pending_ = false;
    bool advancing_scroll_animation_ = false;
    bool frame_invalidated_ = true;
    bool description_invalidated_ = true;
    bool declarative_environment_pending_ = false;
    InputOperationResult pending_lifecycle_input_;
    runtime::RuntimeGenerationSnapshot observed_service_generations_;
    SurfaceFrame last_frame_;
    std::deque<SurfaceEventRecord> published_events_;
    std::uint64_t next_event_sequence_ = 1U;
    std::uint64_t dropped_event_count_ = 0U;
};

[[nodiscard]] std::string_view surface_density_name(SurfaceDensity value) noexcept;
[[nodiscard]] std::string_view pointer_precision_name(PointerPrecision value) noexcept;

} // namespace strata::ui
