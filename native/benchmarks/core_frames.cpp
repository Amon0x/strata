// Core frame benchmark: the canonical showcase workload (cold start, idle, tab switching,
// scrolling, ordinary interactions, hover motion) through the public C ABI with a null renderer.
// It measures Strata's own work — compile, describe, layout, render planning, packet encoding and
// decoding — without a GPU or rasterizer in the loop, so runs are comparable on any machine.
//
// usage: strata_core_benchmark <resource-root> <scenario.json> [iterations-scale | cold]
// `cold` measures only the cold start, for profiling startup over many runs.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include <strata/strata.hpp>

#include "../headless/application_host.hpp"
#include "../headless/scenario.hpp"
#include "../host/browser_model.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using strata::headless::ApplicationHost;

constexpr std::int64_t frame_nanos = 16'666'667;

struct Phase final {
    std::string name;
    std::vector<double> frames;
    /** Section path -> nanoseconds summed over this phase's frames, from each frame's samples. */
    std::map<std::string, double> sections;
};

/** Adds the sections the surface's latest frame sampled to `phase`. */
void attribute(const ApplicationHost& host, Phase& phase) {
    const strata::ProfilerSnapshot snapshot = host.surface_profiler();
    for (const strata::ProfilerSection& section : snapshot.sections) {
        if (section.last_sample_frame_index == snapshot.frame_index)
            phase.sections[section.path] += static_cast<double>(section.last_nanoseconds);
    }
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

void breakdown(const Phase& phase) {
    std::vector<std::pair<std::string, double>> ordered(phase.sections.begin(),
                                                        phase.sections.end());
    std::ranges::sort(ordered, [](const auto& left, const auto& right) {
        return left.second > right.second;
    });
    const double frames = static_cast<double>(std::max<std::size_t>(phase.frames.size(), 1U));
    for (std::size_t index = 0U; index < std::min<std::size_t>(ordered.size(), 16U); ++index) {
        std::printf("    %-52s %8.3f ms/frame\n", ordered[index].first.c_str(),
                    ordered[index].second / 1e6 / frames);
    }
}

void report(const Phase& phase) {
    const double total = std::accumulate(phase.frames.begin(), phase.frames.end(), 0.0);
    const double average = phase.frames.empty() ? 0.0 : total / static_cast<double>(phase.frames.size());
    std::printf("%-26s frames %5zu  avg %8.3f ms  p50 %8.3f  p95 %8.3f  p99 %8.3f  max %8.3f\n",
                phase.name.c_str(), phase.frames.size(), average, percentile(phase.frames, 0.5),
                percentile(phase.frames, 0.95), percentile(phase.frames, 0.99),
                phase.frames.empty() ? 0.0 : *std::ranges::max_element(phase.frames));
}

class Driver final {
  public:
    Driver(ApplicationHost& host, const double width, const double height)
        : host_(host), width_(width), height_(height) {}

    /** One frame; returns its wall time in milliseconds. */
    double frame() {
        now_ += frame_nanos;
        const auto started = Clock::now();
        host_.frame(now_);
        return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    }

    [[nodiscard]] std::pair<double, double> locate(const strata::host::Selector& selector) const {
        const strata::host::BrowserModel model =
            strata::host::BrowserModel::build(host_.frame_document(), width_, height_);
        return model.resolve(selector);
    }

    void pointer(const std::uint32_t kind, const double x, const double y) {
        strata_input_event event{};
        event.struct_size = sizeof(event);
        event.version = STRATA_INPUT_EVENT_VERSION_2;
        event.kind = kind;
        event.x = x;
        event.y = y;
        event.timestamp_nanoseconds = now_;
        host_.enqueue(std::span<const strata_input_event>(&event, 1U));
    }

    void scroll(const double x, const double y, const double delta_y) {
        strata_input_event event{};
        event.struct_size = sizeof(event);
        event.version = STRATA_INPUT_EVENT_VERSION_2;
        event.kind = STRATA_INPUT_SCROLL;
        event.x = x;
        event.y = y;
        event.delta_y = delta_y;
        event.timestamp_nanoseconds = now_;
        host_.enqueue(std::span<const strata_input_event>(&event, 1U));
    }

    /** A click and the frames that follow it; the frame the click lands in is `switch_frames`. */
    void click(const std::pair<double, double> at, Phase& switch_frames, Phase& settle_frames,
               const int settle) {
        pointer(STRATA_INPUT_POINTER_MOVE, at.first, at.second);
        pointer(STRATA_INPUT_POINTER_PRESS, at.first, at.second);
        pointer(STRATA_INPUT_POINTER_RELEASE, at.first, at.second);
        switch_frames.frames.push_back(frame());
        attribute(host_, switch_frames);
        for (int index = 0; index < settle; ++index) {
            settle_frames.frames.push_back(frame());
            attribute(host_, settle_frames);
        }
    }

  private:
    ApplicationHost& host_;
    double width_;
    double height_;
    std::int64_t now_ = 0;
};

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3) {
            std::fprintf(stderr, "usage: strata_core_benchmark <resource-root> <scenario.json> [scale]\n");
            return 2;
        }
        const std::filesystem::path resources = argv[1];
        const bool cold_only = argc > 3 && std::string_view(argv[3]) == "cold";
        const int scale = argc > 3 && !cold_only ? std::max(1, std::atoi(argv[3])) : 1;
        strata::headless::Scenario scenario = strata::headless::load_scenario(argv[2]);
        scenario.render_backend = "none";
        scenario.steps.clear();

        const auto cold_started = Clock::now();
        ApplicationHost host(scenario, resources);
        const auto constructed = Clock::now();
        Driver driver(host, scenario.width, scenario.height);
        // An application's first frame captures no trace; the benchmark turns capture on only
        // where it needs the frame document to find its targets.
        host.set_capture_frames(false);
        const double first_frame = driver.frame();
        Phase first{"first frame", {first_frame}};
        attribute(host, first);
        const double construction =
            std::chrono::duration<double, std::milli>(constructed - cold_started).count();
        std::printf("cold start: host + compile + surface %.3f ms, first frame %.3f ms, total %.3f ms\n",
                    construction, first_frame, construction + first_frame);

        breakdown(first);
        if (cold_only) {
            host.close();
            return 0;
        }
        host.set_capture_frames(true);
        Phase warm{"warm-up", {}};
        for (int index = 0; index < 60; ++index)
            warm.frames.push_back(driver.frame());
        report(warm);

        using strata::host::Selector;
        const auto tab = [&](const char* name) {
            return driver.locate(Selector{std::nullopt, std::nullopt, std::string(name),
                                          std::string("tab")});
        };
        const auto forms = tab("FORMS");
        const auto shell = tab("SHELL");
        const auto extend = tab("EXTEND");
        host.set_capture_frames(false);

        Phase idle{"idle", {}};
        for (int index = 0; index < 300 * scale; ++index)
            idle.frames.push_back(driver.frame());
        report(idle);

        Phase switches{"tab-switch (click frame)", {}};
        Phase switch_settle{"tab-switch (settle)", {}};
        for (int iteration = 0; iteration < 20 * scale; ++iteration) {
            driver.click(forms, switches, switch_settle, 4);
            driver.click(shell, switches, switch_settle, 4);
            driver.click(extend, switches, switch_settle, 4);
        }
        report(switches);
        breakdown(switches);
        report(switch_settle);
        breakdown(switch_settle);

        host.set_capture_frames(true);
        static_cast<void>(driver.frame());
        const auto scroll_at = driver.locate(
            Selector{std::nullopt, std::string("targets.page.scroll.extend"), std::nullopt,
                     std::nullopt});
        host.set_capture_frames(false);
        Phase scrolling{"scrolling", {}};
        for (int iteration = 0; iteration < 60 * scale; ++iteration) {
            for (const double delta : {-4.0, 4.0}) {
                driver.scroll(scroll_at.first, scroll_at.second, delta);
                for (int frame = 0; frame < 2; ++frame) {
                    scrolling.frames.push_back(driver.frame());
                    attribute(host, scrolling);
                }
            }
        }
        report(scrolling);
        breakdown(scrolling);

        Phase hover{"hover sweep (moving)", {}};
        for (int iteration = 0; iteration < 40 * scale; ++iteration) {
            for (int step = 0; step <= 20; ++step) {
                const double fraction = static_cast<double>(step) / 20.0;
                driver.pointer(STRATA_INPUT_POINTER_MOVE,
                               forms.first + (extend.first - forms.first) * fraction,
                               forms.second + (extend.second - forms.second) * fraction);
                hover.frames.push_back(driver.frame());
                attribute(host, hover);
            }
        }
        report(hover);
        breakdown(hover);

        // The drawing page animates every frame: native canvases, morphs and loops.
        host.set_capture_frames(true);
        static_cast<void>(driver.frame());
        const auto draw = tab("DRAW");
        host.set_capture_frames(false);
        Phase draw_open{"draw tab (click frame)", {}};
        Phase draw_settle{"draw tab (settle)", {}};
        driver.click(draw, draw_open, draw_settle, 30);
        Phase drawing{"draw tab (animating)", {}};
        Phase slow{"draw tab (frames over 5 ms)", {}};
        for (int index = 0; index < 240 * scale; ++index) {
            drawing.frames.push_back(driver.frame());
            attribute(host, drawing);
            if (drawing.frames.back() > 5.0) {
                slow.frames.push_back(drawing.frames.back());
                attribute(host, slow);
            }
        }
        report(drawing);
        breakdown(drawing);
        report(slow);
        breakdown(slow);
        host.close();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "strata_core_benchmark: %s\n", error.what());
        return 1;
    }
}
