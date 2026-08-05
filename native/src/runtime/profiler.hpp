#pragma once

#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace strata::runtime {

enum class ProfilerScope : std::uint32_t { runtime = 0U, surface = 1U };

/** Stable, append-only counter identities exported by name through the ABI. */
enum class ProfilerCounter : std::size_t {
    draw_calls,
    batches,
    batch_texture_breaks,
    batch_clip_breaks,
    batch_material_breaks,
    // Canonical zero: retained scopes are flattened before submission and never split a batch.
    batch_scope_breaks,
    batch_effect_breaks,
    encoded_geometry_cache_hits,
    encoded_geometry_cache_misses,
    vertices,
    texture_uploads,
    texture_upload_bytes,
    queued_texture_uploads,
    queued_texture_upload_bytes,
    texture_creations,
    texture_creation_bytes,
    pending_texture_creations,
    deferred_gpu_resources,
    deferred_gpu_resource_deletions,
    reload_duration_nanos,
    // Canonical zeros until the host GPU queue retains a causal hot-reload tag; ordinary
    // create/upload/queued/deferred counters below are still populated with genuine host data.
    hot_reload_gpu_upload_jobs,
    hot_reload_gpu_upload_bytes,
    hot_reload_gpu_upload_deferred_jobs,
    hot_reload_gpu_upload_deferred_bytes,
    hot_reload_gpu_upload_over_budget_jobs,
    hot_reload_gpu_upload_over_budget_bytes,
    hot_reload_gpu_upload_rejected_jobs,
    hot_reload_gpu_upload_rejected_bytes,
    text_layout_requests,
    text_layout_cache_hits,
    text_layout_cache_misses,
    text_layout_cache_lookup_nanos,
    text_layout_cache_restore_nanos,
    text_layout_cache_store_nanos,
    text_shaping_nanos,
    layout_nanos,
    input_nanos,
    input_events_processed,
    input_events_deferred,
    input_dispatches,
    input_mutated_nodes,
    input_coalesced_moves,
    ui_behavior_dispatches,
    layout_measured_nodes,
    layout_reused_nodes,
    layout_arranged_nodes,
    ui_inspector_nanos,
    animation_nanos,
    animation_mutated_nodes,
    animation_running_players,
    render_submission_nanos,
    blur_passes,
    blur_target_width,
    blur_target_height,
    blur_nanos,
    // Native-pipeline detail retained in addition to the canonical profiler contract above.
    frames,
    described_nodes,
    evaluated_expressions,
    rebuilds,
    injected_events,
    render_commands,
    render_fragments_built,
    render_fragments_reused,
    render_nodes_visited,
    render_overlays,
    render_portals,
    packet_bytes,
    host_submissions,
    host_submit_nanos,
    resource_reloads,
    diagnostics,
    input_pointer_geometry_rebuilds,
    input_fast_path_frames,
    text_font_resolution_nanos,
    text_opentype_nanos,
    text_line_assembly_nanos,
    render_retained_subtrees_reused,
    render_retained_subtrees_translated,
    layout_translated_nodes,
    count,
};

[[nodiscard]] std::string_view profiler_counter_name(ProfilerCounter counter) noexcept;

struct ProfilerConfig final {
    std::size_t timing_window_size = 120U;
    std::size_t maximum_sections = 512U;
    std::size_t maximum_recent_spikes = 64U;
    std::int64_t spike_absolute_floor_nanos = 8'000'000;
    double spike_baseline_multiplier = 2.5;
    std::size_t spike_minimum_baseline_samples = 8U;

    void validate() const;
};

struct ProfilerSectionSnapshot final {
    std::uint64_t id = 0U;
    std::optional<std::uint64_t> parent_id;
    std::string name;
    std::string path;
    std::size_t sample_count = 0U;
    std::uint64_t last_sample_frame_index = 0U;
    std::int64_t last_nanos = 0;
    std::int64_t average_nanos = 0;
    std::int64_t p50_nanos = 0;
    std::int64_t p95_nanos = 0;
    std::int64_t p99_nanos = 0;
    std::int64_t maximum_nanos = 0;
};

struct ProfilerCounterSnapshot final {
    ProfilerCounter counter = ProfilerCounter::frames;
    std::uint64_t value = 0U;
};

struct HostFrameProfilerTelemetry final {
    std::uint64_t draw_calls = 0U;
    std::uint64_t batches = 0U;
    std::uint64_t vertices = 0U;
    std::uint64_t texture_creations = 0U;
    std::uint64_t texture_creation_bytes = 0U;
    std::uint64_t texture_uploads = 0U;
    std::uint64_t texture_upload_bytes = 0U;
    std::uint64_t queued_texture_creations = 0U;
    std::uint64_t queued_texture_uploads = 0U;
    std::uint64_t queued_texture_upload_bytes = 0U;
    std::uint64_t deferred_gpu_resources = 0U;
    std::uint64_t deferred_gpu_resource_deletions = 0U;
    std::uint64_t blur_passes = 0U;
    std::uint64_t blur_target_width = 0U;
    std::uint64_t blur_target_height = 0U;
    std::uint64_t blur_nanos = 0U;
    std::int64_t submit_nanos = 0;
};

struct ProfilerSpikeSnapshot final {
    std::uint64_t section_id = 0U;
    std::optional<std::uint64_t> parent_id;
    std::string section_name;
    std::string section_path;
    std::vector<std::uint64_t> stack_section_ids;
    std::uint64_t frame_index = 0U;
    std::int64_t duration_nanos = 0;
    std::int64_t absolute_threshold_nanos = 0;
    std::int64_t effective_threshold_nanos = 0;
    double baseline_multiplier = 0.0;
    std::optional<std::int64_t> rolling_average_nanos;
    std::vector<ProfilerCounterSnapshot> counters;
};

struct ProfilerSnapshot final {
    ProfilerScope scope = ProfilerScope::runtime;
    std::string scope_id;
    std::uint64_t frame_index = 0U;
    bool capture_enabled = true;
    std::uint64_t dropped_section_samples = 0U;
    std::uint64_t dropped_timing_samples = 0U;
    std::uint64_t dropped_spikes = 0U;
    std::vector<ProfilerSectionSnapshot> sections;
    std::vector<ProfilerCounterSnapshot> counters;
    std::vector<ProfilerSpikeSnapshot> spikes;
};

/**
 * Runtime-local profiler. Section nesting is owner-thread bound while snapshots and counters are
 * safe to query from host threads. All retained collections are bounded by ProfilerConfig.
 */
class Profiler final {
public:
    using Clock = std::function<std::int64_t()>;

    class Section final {
    public:
        Section() noexcept = default;
        Section(const Section&) = delete;
        Section& operator=(const Section&) = delete;
        Section(Section&& other) noexcept;
        Section& operator=(Section&& other) noexcept;
        ~Section();

        void close() noexcept;
        [[nodiscard]] bool active() const noexcept;

    private:
        friend class Profiler;
        Section(
            Profiler* profiler,
            std::uint64_t generation,
            std::uint64_t token,
            std::string_view name
        ) noexcept;

        Profiler* profiler_ = nullptr;
        std::uint64_t generation_ = 0U;
        std::uint64_t token_ = 0U;
        std::uint32_t trace_id_ = 0U;
        std::int32_t trace_active_ = 0;
    };

    class Frame final {
    public:
        Frame() noexcept = default;
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
        Frame(Frame&& other) noexcept;
        Frame& operator=(Frame&& other) noexcept;
        ~Frame();

        void close() noexcept;
        [[nodiscard]] bool active() const noexcept;

    private:
        friend class Profiler;
        Frame(Profiler* profiler, std::uint64_t generation) noexcept;

        Profiler* profiler_ = nullptr;
        std::uint64_t generation_ = 0U;
    };

    explicit Profiler(
        ProfilerScope scope,
        std::string scope_id,
        ProfilerConfig config = {},
        Clock clock = {}
    );

    [[nodiscard]] Frame frame(std::uint64_t frame_index);
    [[nodiscard]] Section section(std::string_view name);
    void set_capture_enabled(bool enabled);
    [[nodiscard]] bool capture_enabled() const noexcept;
    void increment(ProfilerCounter counter, std::uint64_t amount = 1U);
    void record(ProfilerCounter counter, std::uint64_t value);
    void record_counters(
        std::initializer_list<std::pair<ProfilerCounter, std::uint64_t>> values
    );
    /** Atomically attaches host-GPU telemetry to the latest completed surface frame. */
    void record_host_frame(const HostFrameProfilerTelemetry& telemetry);
    /** Records a host-owned phase against the latest completed frame under the canonical frame root. */
    void record_external_timing(std::string_view name, std::int64_t duration_nanos);
    [[nodiscard]] ProfilerSnapshot snapshot() const;
    void reset();

private:
    struct RollingWindow final {
        explicit RollingWindow(std::size_t capacity);
        /** Returns true when the bounded window evicts its oldest retained sample. */
        [[nodiscard]] bool add(std::int64_t value);
        [[nodiscard]] std::int64_t average() const noexcept;
        [[nodiscard]] std::int64_t maximum() const noexcept;
        [[nodiscard]] std::int64_t percentile(double fraction) const;

        std::size_t capacity;
        std::deque<std::int64_t> samples;
        std::vector<std::int64_t> ordered_samples;
        std::uint64_t sum = 0U;
        std::int64_t maximum_value = 0;
    };

    struct SectionRecord final {
        std::uint64_t id = 0U;
        std::optional<std::uint64_t> parent_id;
        std::string name;
        std::string path;
        RollingWindow timings;
        std::uint64_t last_sample_frame_index = 0U;
    };

    struct SectionKey final {
        std::optional<std::uint64_t> parent_id;
        std::string name;
        [[nodiscard]] friend auto operator<=>(const SectionKey&, const SectionKey&) = default;
    };

    struct OpenSection final {
        std::uint64_t token = 0U;
        std::uint64_t section_id = 0U;
        std::int64_t started_at_nanos = 0;
        std::uint64_t generation = 0U;
    };

    [[nodiscard]] std::int64_t now() const noexcept;
    void begin_frame(std::uint64_t frame_index);
    void end_frame(std::uint64_t generation);
    void close_section(std::uint64_t generation, std::uint64_t token);
    [[nodiscard]] std::optional<std::uint64_t> find_or_create_section_locked(
        std::string_view name,
        std::optional<std::uint64_t> parent_id
    );
    void close_top_locked(std::int64_t ended_at_nanos);
    void record_timing_locked(std::uint64_t section_id, std::int64_t duration_nanos);
    void record_spike_locked(
        const SectionRecord& section,
        std::int64_t duration_nanos,
        std::int64_t baseline_nanos
    );
    [[nodiscard]] std::vector<std::uint64_t> stack_locked(std::uint64_t section_id) const;
    [[nodiscard]] std::vector<ProfilerCounterSnapshot> counters_locked(bool nonzero_only) const;
    [[nodiscard]] ProfilerSnapshot snapshot_locked() const;
    void invalidate_completed_snapshot_locked();
    void publish_completed_snapshot_locked() const;
    [[nodiscard]] SectionRecord* section_locked(std::uint64_t id) noexcept;
    [[nodiscard]] const SectionRecord* section_locked(std::uint64_t id) const noexcept;
    void reset_counters_locked() noexcept;
    static void increment_saturated(std::uint64_t& value, std::uint64_t amount = 1U) noexcept;

    ProfilerScope scope_;
    std::string scope_id_;
    ProfilerConfig config_;
    Clock clock_;
    mutable std::mutex mutex_;
    bool enabled_ = true;
    bool frame_active_ = false;
    std::thread::id owner_thread_;
    std::uint64_t frame_index_ = 0U;
    std::uint64_t generation_ = 0U;
    std::uint64_t next_section_id_ = 1U;
    std::uint64_t next_token_ = 1U;
    std::optional<std::uint64_t> frame_root_id_;
    std::vector<SectionRecord> sections_;
    std::map<SectionKey, std::uint64_t> section_ids_;
    std::vector<OpenSection> open_sections_;
    std::array<std::uint64_t, static_cast<std::size_t>(ProfilerCounter::count)> counters_{};
    std::deque<ProfilerSpikeSnapshot> spikes_;
    std::uint64_t dropped_section_samples_ = 0U;
    std::uint64_t dropped_timing_samples_ = 0U;
    std::uint64_t dropped_spikes_ = 0U;
    mutable ProfilerSnapshot completed_snapshot_;
    mutable bool has_completed_snapshot_ = false;
    mutable bool completed_snapshot_dirty_ = false;
};

} // namespace strata::runtime
