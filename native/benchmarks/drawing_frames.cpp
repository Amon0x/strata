// Drawing benchmark: the shapes people draw, changing the ways they change, through the public host
// with a null renderer. Each workload is its own surface: a live 10,000-point line whose data
// changes every frame, a 200-slice pie recoloured every frame, 500 arcs animating their sweep, and
// a 2,000-segment outline moving across the surface. It measures describe, layout, shape parsing,
// planning, mask rasterization and packet encoding, and compares each workload's average frame
// with its budget.
//
// usage: strata_drawing_benchmark <scratch-directory> [iterations-scale] [workload-name-part]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <numbers>
#include <numeric>
#include <string>
#include <vector>

#include "../headless/application_host.hpp"
#include "../headless/scenario.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using strata::data::JsonValue;
using strata::headless::ApplicationHost;

constexpr std::int64_t frame_nanos = 16'666'667;

/** One workload: its module body, the values it starts from and the change each frame makes. */
struct Workload final {
    std::string name;
    std::string body;
    std::function<JsonValue(int frame)> values;
    /** The average frame this workload must stay under, in milliseconds. */
    double budget = 0.0;
};

[[nodiscard]] JsonValue number(const double value) {
    return JsonValue(value);
}

[[nodiscard]] JsonValue bench(JsonValue::Object fields) {
    return JsonValue(JsonValue::Object{{"bench", JsonValue(std::move(fields))}});
}

[[nodiscard]] std::vector<Workload> workloads() {
    std::vector<Workload> result;

    // A live series: every point moves every frame, as a streaming chart's do.
    result.push_back(Workload{
        "live line, 10,000 points",
        "    Draw(layout: { width: 1180, height: 380, placement: { x: 10, y: 10 } }, shapes: [\n"
        "      { kind: \"polyline\", points: bench.line, stroke: #56E1C6FF,\n"
        "        strokeStyle: { width: 2, join: \"round\" } }\n"
        "    ])\n",
        [](const int frame) {
            JsonValue::Array points;
            points.reserve(10'000U);
            for (int index = 0; index < 10'000; ++index) {
                const double x = static_cast<double>(index) / 9'999.0;
                const double phase = static_cast<double>(frame) * 0.05;
                const double y = 0.5 + 0.3 * std::sin(x * 40.0 + phase) +
                                 0.1 * std::sin(x * 517.0 + phase * 3.0);
                points.push_back(JsonValue(JsonValue::Object{{"x", number(x)}, {"y", number(y)}}));
            }
            return bench({{"line", JsonValue(std::move(points))}});
        },
        12.0,
    });

    // A recolour: every slice's paint changes, nothing else does.
    {
        std::string body = "    Draw(layout: { width: 760, height: 760, placement: { x: 220, y: 20 } }, "
                           "shapes: [\n";
        for (int index = 0; index < 200; ++index) {
            const double start = static_cast<double>(index) * 1.8;
            body += "      { kind: \"sector\", center: { x: 0.5, y: 0.5 }, radius: 0.48, "
                    "innerRadius: 0.2, start: " +
                    std::to_string(start) + ", sweep: 1.7, fill: bench.flip ? #3B82F6FF : #F59E0BFF },\n";
        }
        body += "    ])\n";
        result.push_back(Workload{
            "pie, 200 slices recoloured",
            std::move(body),
            [](const int frame) { return bench({{"flip", JsonValue(frame % 2 == 0)}}); },
            2.0,
        });
    }

    // An animation: every arc's sweep changes every frame (circular arcs: a square Draw).
    {
        std::string body = "    Draw(layout: { width: 780, height: 780, placement: { x: 210, y: 10 } }, "
                           "shapes: [\n";
        for (int index = 0; index < 500; ++index) {
            const double x = 0.025 + static_cast<double>(index % 25) * 0.039;
            const double y = 0.025 + static_cast<double>(index / 25) * 0.049;
            body += "      { kind: \"arc\", center: { x: " + std::to_string(x) + ", y: " +
                    std::to_string(y) +
                    " }, radius: 0.012, start: -90, sweep: bench.sweep, stroke: #A78BFAFF, "
                    "strokeStyle: { width: 3, cap: \"round\" } },\n";
        }
        body += "    ])\n";
        result.push_back(Workload{
            "500 arcs animating",
            std::move(body),
            [](const int frame) {
                return bench({{"sweep", number(20.0 + std::fmod(static_cast<double>(frame) * 3.0, 330.0))}});
            },
            3.0,
        });
    }

    // A complex outline in motion: the same shape at a new position every frame.
    {
        std::string commands = "M 0.02 0.5";
        for (int index = 1; index <= 2'000; ++index) {
            const double t = static_cast<double>(index) / 5999.0;
            const double angle = t * std::numbers::pi * 2.0 * 7.0;
            const double radius = 0.25 + 0.2 * std::sin(t * std::numbers::pi * 2.0 * 23.0);
            commands += " L " + std::to_string(0.5 + radius * std::cos(angle) * (0.4 + t * 0.6)) +
                        " " + std::to_string(0.5 + radius * std::sin(angle) * (0.4 + t * 0.6));
        }
        commands += " Z";
        result.push_back(Workload{
            "2,000-segment outline moving",
            "    Draw(layout: { width: 360, height: 360, placement: { x: bench.x, y: 200 } }, "
            "shapes: [\n      { kind: \"path\", commands: \"" + commands +
                "\", fill: #22C55E99, stroke: #FFFFFFCC, strokeStyle: { width: 1.5 } }\n    ])\n",
            [](const int frame) {
                return bench({{"x", number(20.0 + std::fmod(static_cast<double>(frame) * 2.37, 800.0))}});
            },
            1.0,
        });
    }
    return result;
}

[[nodiscard]] double percentile(std::vector<double> values, const double fraction) {
    if (values.empty())
        return 0.0;
    std::ranges::sort(values);
    const auto index = static_cast<std::size_t>(
        std::min<double>(static_cast<double>(values.size() - 1U),
                         fraction * static_cast<double>(values.size() - 1U) + 0.5));
    return values[index];
}

/** Runs one workload; returns whether its average frame stayed within budget. */
[[nodiscard]] bool run(const std::filesystem::path& root, const Workload& workload, const int scale,
                       const bool breakdown) {
    const std::filesystem::path module = root / "assets" / "drawing" / "bench.strata";
    std::filesystem::create_directories(module.parent_path());
    {
        std::ofstream file(module, std::ios::binary);
        file << "screen Bench {\n  root Panel(style: { background: #0B0F16FF, border: null }, "
                "layout: { kind: \"PANEL\", width: 1200, height: 800 }) {\n"
             << workload.body << "  }\n}\n";
    }
    strata::headless::Scenario scenario;
    scenario.application_id = "drawing.bench";
    scenario.surface_id = "drawing.bench";
    scenario.module = "assets/drawing/bench.strata";
    scenario.root = "Bench";
    scenario.root_role = "screen";
    scenario.render_backend = "none";
    scenario.width = 1200.0;
    scenario.height = 800.0;
    scenario.reduced_motion = true;
    scenario.snapshots.push_back(strata::headless::SnapshotConfig{"bench", workload.values(0)});

    const auto started = Clock::now();
    ApplicationHost host(scenario, root);
    host.set_capture_frames(false);
    std::int64_t now = 0;
    try {
        host.frame(now);
    } catch (const std::exception&) {
        for (const strata::headless::CapturedDiagnostic& diagnostic : host.diagnostics())
            std::fprintf(stderr, "%s: %s (%s)\n", diagnostic.code.c_str(),
                         diagnostic.message.c_str(), diagnostic.component_path.c_str());
        throw;
    }
    const double first =
        std::chrono::duration<double, std::milli>(Clock::now() - started).count();

    std::vector<double> changing;
    std::vector<double> idle;
    std::map<std::string, double> sections;
    const int frames = 120 * scale;
    for (int frame = 1; frame <= frames; ++frame) {
        host.publish(strata::headless::SnapshotConfig{"bench", workload.values(frame)});
        now += frame_nanos;
        const auto begun = Clock::now();
        host.frame(now);
        changing.push_back(std::chrono::duration<double, std::milli>(Clock::now() - begun).count());
        const strata::ProfilerSnapshot snapshot = host.surface_profiler();
        for (const strata::ProfilerSection& section : snapshot.sections) {
            if (section.last_sample_frame_index == snapshot.frame_index)
                sections[section.path] += static_cast<double>(section.last_nanoseconds);
        }
    }
    for (int frame = 0; frame < 60; ++frame) {
        now += frame_nanos;
        const auto begun = Clock::now();
        host.frame(now);
        idle.push_back(std::chrono::duration<double, std::milli>(Clock::now() - begun).count());
    }
    host.close();
    const double average =
        std::accumulate(changing.begin(), changing.end(), 0.0) / static_cast<double>(changing.size());
    const bool within = average <= workload.budget;
    std::printf("%-32s first %8.3f ms  changing avg %7.3f  p95 %7.3f  max %7.3f  idle avg %6.3f  "
                "budget %5.1f  %s\n",
                workload.name.c_str(), first, average, percentile(changing, 0.95),
                *std::ranges::max_element(changing),
                std::accumulate(idle.begin(), idle.end(), 0.0) / static_cast<double>(idle.size()),
                workload.budget, within ? "ok" : "OVER");
    if (breakdown) {
        std::vector<std::pair<std::string, double>> ordered(sections.begin(), sections.end());
        std::ranges::sort(ordered, [](const auto& left, const auto& right) {
            return left.second > right.second;
        });
        for (std::size_t index = 0U; index < std::min<std::size_t>(ordered.size(), 12U); ++index)
            std::printf("    %-52s %8.3f ms/frame\n", ordered[index].first.c_str(),
                        ordered[index].second / 1e6 / static_cast<double>(frames));
    }
    return within;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::fprintf(stderr, "usage: strata_drawing_benchmark <scratch-directory> [scale]\n");
            return 2;
        }
        const std::filesystem::path root = argv[1];
        const int scale = argc > 2 ? std::max(1, std::atoi(argv[2])) : 1;
        const std::string only = argc > 3 ? argv[3] : "";
        bool within = true;
        for (const Workload& workload : workloads()) {
            if (workload.name.find(only) != std::string::npos)
                within = run(root, workload, scale, !only.empty()) && within;
        }
        return within ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "strata_drawing_benchmark: %s\n", error.what());
        return 1;
    }
}
