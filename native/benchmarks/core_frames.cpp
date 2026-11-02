// Core frame benchmark: the canonical showcase workload (cold start, idle, tab switching,
// scrolling, ordinary interactions, hover motion) through the public C ABI with a null renderer.
// It measures Strata's own work — compile, describe, layout, render planning, packet encoding and
// decoding — without a GPU or rasterizer in the loop, so runs are comparable on any machine.
//
// usage: strata_core_benchmark <resource-root> <scenario.json> [scale | cold | first | scroll | hover] [stress-frames]
// `cold` measures only the cold start, for profiling startup over many runs. `first` opens every
// showcase tab once, in order, and reports each first opening: the hitch a user sees the first
// time a page appears, with the sections it spent its time in.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
    /** Work counters summed over this phase's frames, independent of clock speed. */
    std::map<std::string, double> counters;
};

/** Adds the sections the surface's latest frame sampled to `phase`. */
void attribute(const ApplicationHost& host, Phase& phase) {
    const strata::ProfilerSnapshot snapshot = host.surface_profiler();
    for (const strata::ProfilerSection& section : snapshot.sections) {
        if (section.last_sample_frame_index == snapshot.frame_index)
            phase.sections[section.path] += static_cast<double>(section.last_nanoseconds);
    }
    for (const strata::ProfilerCounter& counter : snapshot.counters) {
        if (counter.name.starts_with("text.") || counter.name.starts_with("layout.") ||
            counter.name.starts_with("render.") || counter.name.starts_with("input."))
            phase.counters[counter.name] += static_cast<double>(counter.value);
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
    for (const auto& [name, total] : phase.counters) {
        if (name.ends_with("_nanos") || name.ends_with(".nanos"))
            std::printf("    %-52s %8.3f ms/frame\n", name.c_str(), total / 1e6 / frames);
        else
            std::printf("    %-52s %8.1f /frame\n", name.c_str(), total / frames);
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
            std::fprintf(stderr, "usage: strata_core_benchmark <resource-root> <scenario.json> [scale|cold|switch|first|scroll|hover] [stress-frames]\n");
            return 2;
        }
        // Comparing worker counts: the pool's size is fixed when it starts.
        // Opening only some tabs keeps the caches as cold as a user's first visit to them.
        std::string only_tabs;
        if (std::string_view(argv[argc - 1]).starts_with("--only=")) {
            only_tabs = "," + std::string(argv[--argc] + 7) + ",";
        }
        if (std::string_view(argv[argc - 1]).starts_with("--workers=")) {
            static_cast<void>(strata_limit_task_workers(
                static_cast<std::uint32_t>(std::atoi(argv[--argc] + 10))));
        }
        const std::filesystem::path resources = argv[1];
        const bool cold_only = argc > 3 && std::string_view(argv[3]) == "cold";
        // "switch" measures only tab switching, at length: what profiling the cold path wants.
        const bool switch_only = argc > 3 && std::string_view(argv[3]) == "switch";
        const bool first_opens = argc > 3 && std::string_view(argv[3]) == "first";
        const bool scroll_stress = argc > 3 && std::string_view(argv[3]) == "scroll";
        const bool hover_stress = argc > 3 && std::string_view(argv[3]) == "hover";
        const int scale = switch_only ? 10
                          : argc > 3 && !cold_only && !first_opens ? std::max(1, std::atoi(argv[3]))
                                                                   : 1;
        strata::headless::Scenario scenario = strata::headless::load_scenario(argv[2]);
        scenario.render_backend = "none";
        scenario.steps.clear();

        const auto cold_started = Clock::now();
        strata::headless::ApplicationHostOptions host_options;
        host_options.frame_slicing = first_opens;
        ApplicationHost host(scenario, resources, host_options);
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
        if (scroll_stress || hover_stress) {
            for (int index = 0; index < 60; ++index)
                static_cast<void>(driver.frame());
            Phase changed{scroll_stress ? "scroll stress" : "hover stress", {}};
            const int frames = argc > 4 ? std::max(1, std::atoi(argv[4])) : 600;
            changed.frames.reserve(static_cast<std::size_t>(frames));
            for (int index = 0; index < frames; ++index) {
                if (scroll_stress) {
                    driver.scroll(scenario.width * 0.5, scenario.height * 0.5,
                                  (index / 30) % 2 == 0 ? -4.0 : 4.0);
                } else {
                    const int cell = index % 2000;
                    driver.pointer(STRATA_INPUT_POINTER_MOVE,
                                   12.0 + static_cast<double>(cell % 50) * 24.0,
                                   10.0 + static_cast<double>(cell / 50) * 20.0);
                }
                changed.frames.push_back(driver.frame());
                attribute(host, changed);
            }
            report(changed);
            breakdown(changed);
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
        if (first_opens) {
            // Each tab's first opening, in the order the tab bar lists them; the page shown at
            // start (EXTEND) has opened already.
            constexpr std::array<const char*, 20U> tabs{
                "MENU", "BLUEPRINT", "INTERACT", "TYPE", "LAYOUT", "DRAW", "STATE", "CONTROL",
                "FORMS", "MANIPULATE", "ADAPT", "INSPECT", "DATA", "SHELL", "SYSTEM", "MEDIA",
                "DESKTOP", "NET", "LOTTIE", "CONTENT"};
            std::vector<std::pair<const char*, std::pair<double, double>>> targets;
            for (const char* const name : tabs) {
                if (only_tabs.empty() || only_tabs.contains("," + std::string(name) + ","))
                    targets.emplace_back(name, tab(name));
            }
            host.set_capture_frames(false);
            for (const auto& [name, at] : targets) {
                Phase opened{std::string("first ") + name, {}};
                Phase after{std::string("after ") + name, {}};
                const auto clicked = Clock::now();
                driver.click(at, opened, after, 1);
                std::printf("frame after click %s:\n", name);
                breakdown(after);
                // Real time until every preparation the page started has been adopted.
                Phase worst{std::string("worst ") + name, {}};
                Phase heaviest_layout{std::string("heaviest layout ") + name, {}};
                const auto layout_time = [](const Phase& phase) {
                    const auto found = phase.sections.find("frame/layout");
                    return found != phase.sections.end() ? found->second : 0.0;
                };
                // The UI thread's share: decoding a packet is the render thread's in a real host.
                const auto ui_time = [](const Phase& phase) {
                    const auto section = [&phase](const char* const path) {
                        const auto found = phase.sections.find(path);
                        return found != phase.sections.end() ? found->second : 0.0;
                    };
                    return (section("frame") + section("frame/packet-encode")) / 1'000'000.0;
                };
                const auto frame_after = [&] {
                    Phase sample{worst.name, {driver.frame()}};
                    attribute(host, sample);
                    if (heaviest_layout.frames.empty() ||
                        layout_time(sample) > layout_time(heaviest_layout))
                        heaviest_layout = sample;
                    if (worst.frames.empty() || ui_time(sample) > ui_time(worst))
                        worst = std::move(sample);
                };
                while (host.pending_tasks() != 0U &&
                       Clock::now() - clicked < std::chrono::seconds(3))
                    frame_after();
                const double settled =
                    std::chrono::duration<double, std::milli>(Clock::now() - clicked).count();
                for (int index = 0; index < 30; ++index)
                    frame_after();
                std::printf("%-12s open %8.3f ms   settled %8.3f ms   after max %8.3f ms, total %8.3f ms\n",
                            name, ui_time(opened), settled, ui_time(worst),
                            std::accumulate(after.frames.begin(), after.frames.end(), 0.0));
                breakdown(opened);
                std::printf("%s frame:\n", worst.name.c_str());
                breakdown(worst);
                std::printf("%s frame:\n", heaviest_layout.name.c_str());
                breakdown(heaviest_layout);
            }
            host.close();
            return 0;
        }
        const auto forms = tab("FORMS");
        const auto shell = tab("SHELL");
        const auto extend = tab("EXTEND");
        host.set_capture_frames(false);

        Phase idle{"idle", {}};
        for (int index = 0; index < (switch_only ? 0 : 300 * scale); ++index)
            idle.frames.push_back(driver.frame());
        if (!switch_only)
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
        if (switch_only) {
            host.close();
            return 0;
        }

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
