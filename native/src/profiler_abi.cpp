#include <strata/strata.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "abi_internal.hpp"
#include "abi_support.hpp"
#include "runtime/profiler.hpp"

namespace {

[[nodiscard]] strata_string_view view(const std::string_view value) noexcept {
    return strata_string_view{value.data(), value.size()};
}

[[nodiscard]] strata_profiler_scope scope(const strata::runtime::ProfilerScope value) noexcept {
    return value == strata::runtime::ProfilerScope::surface
        ? STRATA_PROFILER_SCOPE_SURFACE
        : STRATA_PROFILER_SCOPE_RUNTIME;
}

[[nodiscard]] strata_profiler_counter counter(
    const strata::runtime::ProfilerCounterSnapshot& value
) noexcept {
    return strata_profiler_counter{
        sizeof(strata_profiler_counter),
        STRATA_PROFILER_SNAPSHOT_VERSION_CURRENT,
        0U,
        view(strata::runtime::profiler_counter_name(value.counter)),
        value.value,
    };
}

void emit_snapshot(
    const strata::runtime::ProfilerSnapshot& source,
    const strata_profiler_snapshot_sink& sink
) {
    std::vector<strata_profiler_section> sections;
    sections.reserve(source.sections.size());
    for (const auto& value : source.sections) {
        sections.push_back(strata_profiler_section{
            sizeof(strata_profiler_section),
            STRATA_PROFILER_SNAPSHOT_VERSION_CURRENT,
            0U,
            value.id,
            value.parent_id.value_or(0U),
            view(value.name),
            view(value.path),
            static_cast<std::uint64_t>(value.sample_count),
            value.last_sample_frame_index,
            value.last_nanos,
            value.average_nanos,
            value.p50_nanos,
            value.p95_nanos,
            value.p99_nanos,
            value.maximum_nanos,
        });
    }

    std::vector<strata_profiler_counter> counters;
    counters.reserve(source.counters.size());
    for (const auto& value : source.counters) counters.push_back(counter(value));

    std::vector<std::vector<strata_profiler_counter>> spike_counters(source.spikes.size());
    std::vector<strata_profiler_spike> spikes;
    spikes.reserve(source.spikes.size());
    for (std::size_t index = 0U; index < source.spikes.size(); ++index) {
        const auto& value = source.spikes[index];
        auto& converted = spike_counters[index];
        converted.reserve(value.counters.size());
        for (const auto& item : value.counters) converted.push_back(counter(item));
        spikes.push_back(strata_profiler_spike{
            sizeof(strata_profiler_spike),
            STRATA_PROFILER_SNAPSHOT_VERSION_CURRENT,
            0U,
            value.section_id,
            value.parent_id.value_or(0U),
            view(value.section_name),
            view(value.section_path),
            value.stack_section_ids.data(),
            value.stack_section_ids.size(),
            value.frame_index,
            value.duration_nanos,
            value.absolute_threshold_nanos,
            value.effective_threshold_nanos,
            value.baseline_multiplier,
            value.rolling_average_nanos.value_or(0),
            value.rolling_average_nanos.has_value() ? 1U : 0U,
            0U,
            converted.data(),
            converted.size(),
        });
    }

    const strata_profiler_snapshot snapshot{
        sizeof(strata_profiler_snapshot),
        STRATA_PROFILER_SNAPSHOT_VERSION_CURRENT,
        scope(source.scope),
        view(source.scope_id),
        source.frame_index,
        source.capture_enabled ? 1U : 0U,
        0U,
        source.dropped_section_samples,
        source.dropped_timing_samples,
        source.dropped_spikes,
        sections.data(),
        sections.size(),
        counters.data(),
        counters.size(),
        spikes.data(),
        spikes.size(),
    };
    sink.emit(sink.user_data, &snapshot);
}

[[nodiscard]] bool valid_sink(const strata_profiler_snapshot_sink* const sink) noexcept {
    return sink != nullptr && sink->struct_size >= sizeof(strata_profiler_snapshot_sink) &&
        sink->emit != nullptr;
}

} // namespace

strata_result strata_runtime_read_profiler(
    const strata_runtime* const runtime,
    const strata_profiler_snapshot_sink* const sink
) {
    if (runtime == nullptr || !valid_sink(sink) || !runtime->core.has_application()) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        emit_snapshot(runtime->core.application().profiler().snapshot(), *sink);
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return strata::core::result(STRATA_STATUS_OUT_OF_MEMORY);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INTERNAL_ERROR);
    }
}

strata_result strata_runtime_set_profiler_capture(
    strata_runtime* const runtime,
    const uint32_t enabled
) {
    if (runtime == nullptr || enabled > 1U || !runtime->core.has_application()) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    runtime->core.application().profiler().set_capture_enabled(enabled != 0U);
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_surface_read_profiler(
    const strata_surface* const surface,
    const strata_profiler_snapshot_sink* const sink
) {
    if (surface == nullptr || !valid_sink(sink)) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        emit_snapshot(surface->core.profiler().snapshot(), *sink);
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return strata::core::result(STRATA_STATUS_OUT_OF_MEMORY);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INTERNAL_ERROR);
    }
}

strata_result strata_surface_set_profiler_capture(
    strata_surface* const surface,
    const uint32_t enabled
) {
    if (surface == nullptr || enabled > 1U) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    if (surface->release_packet_prepared) {
        return strata::abi_detail::terminal_surface_failure(*surface);
    }
    surface->core.profiler().set_capture_enabled(enabled != 0U);
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_surface_record_host_frame(
    strata_surface* const surface,
    const strata_profiler_host_frame* const telemetry
) {
    if (surface == nullptr || telemetry == nullptr ||
        telemetry->struct_size < sizeof(strata_profiler_host_frame) ||
        telemetry->version != STRATA_PROFILER_HOST_FRAME_VERSION_CURRENT ||
        telemetry->reserved != 0U || telemetry->submit_nanos < 0) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    if (surface->release_packet_prepared) {
        return strata::abi_detail::terminal_surface_failure(*surface);
    }
    try {
        surface->core.profiler().record_host_frame(strata::runtime::HostFrameProfilerTelemetry{
            telemetry->draw_calls,
            telemetry->batches,
            telemetry->vertices,
            telemetry->texture_creations,
            telemetry->texture_creation_bytes,
            telemetry->texture_uploads,
            telemetry->texture_upload_bytes,
            telemetry->queued_texture_creations,
            telemetry->queued_texture_uploads,
            telemetry->queued_texture_upload_bytes,
            telemetry->deferred_gpu_resources,
            telemetry->deferred_gpu_resource_deletions,
            telemetry->blur_passes,
            telemetry->blur_target_width,
            telemetry->blur_target_height,
            telemetry->blur_nanos,
            telemetry->submit_nanos,
        });
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INTERNAL_ERROR);
    }
}
