#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <strata/strata.h>

namespace strata {

enum class ProfilerScope { runtime, surface };

struct ProfilerCounter final {
    std::string name;
    std::uint64_t value = 0U;
};

struct ProfilerSection final {
    std::uint64_t id = 0U;
    std::uint64_t parent_id = 0U;
    std::string name;
    std::string path;
    std::uint64_t sample_count = 0U;
    std::uint64_t last_sample_frame_index = 0U;
    std::int64_t last_nanoseconds = 0;
    std::int64_t average_nanoseconds = 0;
    std::int64_t p50_nanoseconds = 0;
    std::int64_t p95_nanoseconds = 0;
    std::int64_t p99_nanoseconds = 0;
    std::int64_t maximum_nanoseconds = 0;
};

struct ProfilerSpike final {
    std::uint64_t section_id = 0U;
    std::uint64_t parent_id = 0U;
    std::string section_name;
    std::string section_path;
    std::vector<std::uint64_t> stack_section_ids;
    std::uint64_t frame_index = 0U;
    std::int64_t duration_nanoseconds = 0;
    std::int64_t absolute_threshold_nanoseconds = 0;
    std::int64_t effective_threshold_nanoseconds = 0;
    double baseline_multiplier = 0.0;
    std::int64_t rolling_average_nanoseconds = 0;
    bool has_rolling_average = false;
    std::vector<ProfilerCounter> counters;
};

/** Owned copy of one runtime or Surface profiler snapshot. */
struct ProfilerSnapshot final {
    ProfilerScope scope = ProfilerScope::runtime;
    std::string scope_id;
    std::uint64_t frame_index = 0U;
    bool capture_enabled = false;
    std::uint64_t dropped_section_samples = 0U;
    std::uint64_t dropped_timing_samples = 0U;
    std::uint64_t dropped_spikes = 0U;
    std::vector<ProfilerSection> sections;
    std::vector<ProfilerCounter> counters;
    std::vector<ProfilerSpike> spikes;
};

/** Host render/GPU measurements attached to the most recently completed Surface frame. */
struct ProfilerHostFrame final {
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
    std::uint64_t blur_nanoseconds = 0U;
    std::int64_t submit_nanoseconds = 0;

    [[nodiscard]] strata_profiler_host_frame native() const noexcept {
        return strata_profiler_host_frame{
            sizeof(strata_profiler_host_frame),
            STRATA_PROFILER_HOST_FRAME_VERSION_CURRENT,
            0U,
            draw_calls,
            batches,
            vertices,
            texture_creations,
            texture_creation_bytes,
            texture_uploads,
            texture_upload_bytes,
            queued_texture_creations,
            queued_texture_uploads,
            queued_texture_upload_bytes,
            deferred_gpu_resources,
            deferred_gpu_resource_deletions,
            blur_passes,
            blur_target_width,
            blur_target_height,
            blur_nanoseconds,
            submit_nanoseconds,
        };
    }
};

} // namespace strata
