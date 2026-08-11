#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "runtime/profiler.hpp"

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

const strata::runtime::ProfilerSectionSnapshot& section(
    const strata::runtime::ProfilerSnapshot& snapshot,
    const std::string_view path
) {
    for (const auto& value : snapshot.sections) {
        if (value.path == path) return value;
    }
    throw std::runtime_error("missing profiler section");
}

void nested_rolling_statistics_are_canonical() {
    std::int64_t now = 0;
    strata::runtime::ProfilerConfig config;
    config.timing_window_size = 4U;
    strata::runtime::Profiler profiler(
        strata::runtime::ProfilerScope::surface,
        "showcase",
        config,
        [&now] { return now; }
    );

    for (std::uint64_t frame_index = 1U; frame_index <= 5U; ++frame_index) {
        now = static_cast<std::int64_t>(frame_index * 1'000U);
        auto frame = profiler.frame(frame_index);
        {
            auto input = profiler.section("input");
            now += static_cast<std::int64_t>(frame_index * 10U);
            {
                auto dispatch = profiler.section("dispatch");
                now += static_cast<std::int64_t>(frame_index * 10U);
            }
        }
        now += 1;
    }

    const auto snapshot = profiler.snapshot();
    require(snapshot.scope == strata::runtime::ProfilerScope::surface, "scope was not retained");
    require(snapshot.scope_id == "showcase", "scope id was not retained");
    const auto& input = section(snapshot, "frame/input");
    require(input.sample_count == 4U, "rolling timing window was not bounded");
    require(input.last_nanos == 100, "last nested duration is wrong");
    require(input.average_nanos == 70, "rolling average is wrong");
    require(input.p50_nanos == 60, "nearest-rank p50 is wrong");
    require(input.p95_nanos == 100, "nearest-rank p95 is wrong");
    require(input.p99_nanos == 100, "nearest-rank p99 is wrong");
    require(input.maximum_nanos == 100, "rolling maximum is wrong");
    const auto& dispatch = section(snapshot, "frame/input/dispatch");
    require(dispatch.parent_id == input.id, "nested parent identity is wrong");
}

void adaptive_spikes_freeze_stack_baseline_and_counters() {
    std::int64_t now = 0;
    strata::runtime::ProfilerConfig config;
    config.spike_absolute_floor_nanos = 100;
    config.spike_baseline_multiplier = 2.0;
    config.spike_minimum_baseline_samples = 3U;
    strata::runtime::Profiler profiler(
        strata::runtime::ProfilerScope::runtime,
        "application",
        config,
        [&now] { return now; }
    );

    for (std::uint64_t frame_index = 1U; frame_index <= 4U; ++frame_index) {
        auto frame = profiler.frame(frame_index);
        auto render = profiler.section("render");
        auto encode = profiler.section("encode");
        profiler.record(strata::runtime::ProfilerCounter::render_commands, frame_index * 2U);
        now += frame_index == 4U ? 250 : 50;
    }

    const auto snapshot = profiler.snapshot();
    const auto found = std::ranges::find(
        snapshot.spikes,
        std::string("frame/render/encode"),
        &strata::runtime::ProfilerSpikeSnapshot::section_path
    );
    require(found != snapshot.spikes.end(), "nested adaptive spike was not captured");
    const auto& spike = *found;
    require(spike.section_path == "frame/render/encode", "spike section path is wrong");
    require(spike.rolling_average_nanos == 50, "pre-spike baseline was not frozen");
    require(spike.effective_threshold_nanos == 100, "effective threshold is wrong");
    require(spike.stack_section_ids.size() == 3U, "full section stack was not captured");
    require(!spike.counters.empty(), "spike counter context was not captured");
}

void bounded_drops_and_external_host_submission_are_visible() {
    std::int64_t now = 0;
    strata::runtime::ProfilerConfig config;
    config.maximum_sections = 2U;
    config.maximum_recent_spikes = 1U;
    config.spike_absolute_floor_nanos = 1;
    config.spike_baseline_multiplier = 1.0;
    config.spike_minimum_baseline_samples = 1U;
    strata::runtime::Profiler profiler(
        strata::runtime::ProfilerScope::surface,
        "surface",
        config,
        [&now] { return now; }
    );

    {
        auto frame = profiler.frame(1U);
        auto first = profiler.section("first");
        now += 1;
    }
    {
        auto frame = profiler.frame(2U);
        auto first = profiler.section("first");
        now += 2;
        auto overflow = profiler.section("overflow");
    }
    profiler.record_external_timing("host-submit", 77);

    const auto snapshot = profiler.snapshot();
    require(snapshot.dropped_section_samples == 2U, "bounded section drops were not accounted");
    require(snapshot.sections.size() == 2U, "section registry exceeded its configured bound");
}

void disabled_capture_is_independent_and_preserves_history() {
    std::int64_t now = 0;
    strata::runtime::Profiler profiler(
        strata::runtime::ProfilerScope::surface,
        "surface",
        {},
        [&now] { return now; }
    );
    {
        auto frame = profiler.frame(1U);
        auto input = profiler.section("input");
        now += 5;
    }
    profiler.set_capture_enabled(false);
    {
        auto frame = profiler.frame(2U);
        auto ignored = profiler.section("ignored");
        now += 50;
    }
    const auto snapshot = profiler.snapshot();
    require(!snapshot.capture_enabled, "capture gate was not exported");
    require(snapshot.frame_index == 1U, "disabled capture advanced the canonical frame");
    require(section(snapshot, "frame/input").maximum_nanos == 5, "capture gate erased history");
}

void snapshots_never_publish_an_open_frame() {
    std::int64_t now = 0;
    strata::runtime::Profiler profiler(
        strata::runtime::ProfilerScope::surface,
        "surface",
        {},
        [&now] { return now; }
    );
    {
        auto first = profiler.frame(1U);
        now += 10;
    }
    auto second = profiler.frame(2U);
    auto input = profiler.section("input");
    now += 50;
    const auto while_open = profiler.snapshot();
    require(while_open.frame_index == 1U, "snapshot exposed a partially open frame");
    input.close();
    second.close();
    require(profiler.snapshot().frame_index == 2U, "completed frame was not published");
}

} // namespace

int strata_test_profiler() {
    try {
        nested_rolling_statistics_are_canonical();
        adaptive_spikes_freeze_stack_baseline_and_counters();
        bounded_drops_and_external_host_submission_are_visible();
        disabled_capture_is_independent_and_preserves_history();
        snapshots_never_publish_an_open_frame();
        std::cout << "profiler tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "profiler tests failed: " << error.what() << '\n';
        return 1;
    }
}
