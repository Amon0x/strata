#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "host/browser_model.hpp"
#include "host.hpp"

struct HWND__;
using HWND = HWND__*;

namespace strata::desktop {

struct PerformanceFramesStep final {
    std::uint32_t count = 1U;
};
struct PerformanceClickStep final {
    host::Selector target;
};
struct PerformanceMoveStep final {
    host::Selector target;
};
struct PerformanceDragStep final {
    host::Selector target;
    double from_fraction = 0.1;
    double to_fraction = 0.9;
    std::uint32_t moves = 1U;
};
struct PerformanceScrollStep final {
    host::Selector target;
    double delta_x = 0.0;
    double delta_y = 0.0;
};
struct PerformanceKeyStep final {
    std::string key;
};

using PerformanceStep = std::variant<
    PerformanceFramesStep,
    PerformanceClickStep,
    PerformanceMoveStep,
    PerformanceDragStep,
    PerformanceScrollStep,
    PerformanceKeyStep
>;

struct PerformancePhase final {
    std::string name;
    std::uint32_t warmup_frames = 0U;
    std::uint32_t warmup_iterations = 0U;
    std::uint32_t iterations = 1U;
    std::vector<PerformanceStep> steps;
};

struct PerformanceScenario final {
    std::uint32_t version = 1U;
    std::string name;
    std::string workload_fingerprint;
    std::uint32_t client_width = 1280U;
    std::uint32_t client_height = 800U;
    bool require_visible = true;
    bool require_foreground = true;
    bool require_draws = true;
    double spike_floor_millis = 2.0;
    double spike_multiplier = 2.5;
    std::size_t maximum_spikes = 32U;
    double maximum_average_regression_percent = 10.0;
    double maximum_p95_regression_percent = 15.0;
    double maximum_p99_regression_percent = 20.0;
    double minimum_regression_millis = 0.1;
    std::vector<PerformanceStep> setup;
    std::vector<PerformancePhase> phases;
};

struct PerformanceStartup final {
    std::chrono::steady_clock::time_point started_at;
    std::int64_t window_create_nanos = 0;
    std::int64_t host_create_nanos = 0;
};

[[nodiscard]] PerformanceScenario load_performance_scenario(
    const std::filesystem::path& path
);

/** Runs scripted phases through the visible uncapped Win32/D3D11 host and writes JSON + HTML. */
class PerformanceRunner final {
public:
    using PumpMessages = std::function<bool()>;

    PerformanceRunner(
        HWND window,
        Host& host,
        PerformanceScenario scenario,
        std::filesystem::path output_root,
        std::filesystem::path baseline_report,
        PerformanceStartup startup
    );
    ~PerformanceRunner();

    PerformanceRunner(const PerformanceRunner&) = delete;
    PerformanceRunner& operator=(const PerformanceRunner&) = delete;

    /** Returns false when the report is invalid (focus loss, occlusion, or interrupted window). */
    [[nodiscard]] bool run(const PumpMessages& pump_messages);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::desktop
