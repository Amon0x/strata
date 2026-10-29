// Lottie frames rendered end to end: a .strata module showing Lottie widgets is drawn through the
// public host by every backend this build has, and each animation's frame is measured from the
// framebuffer. White shapes on black make each pixel's red its coverage, so the summed coverage of
// a cell is the area the frame covers there: a wrong keyframe, trim, mask or matte shows as area.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "../headless/application_host.hpp"
#include "../headless/capture_renderer.hpp"
#include "../headless/scenario.hpp"

namespace {

using namespace strata;

void check(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

[[nodiscard]] std::vector<std::string> available_backends() {
    std::vector<std::string> result{"reference"};
#if defined(_WIN32)
    result.emplace_back("d3d11");
#endif
    try {
        const std::unique_ptr<headless::CaptureRenderer> probe =
            headless::create_capture_renderer("vulkan");
        probe->resize(4U, 4U, 4.0, 4.0);
        result.emplace_back("vulkan");
    } catch (const std::exception&) {
    }
    return result;
}

constexpr double cell = 200.0;
constexpr int columns = 4;

const std::string white = R"({"ty":"fl","c":{"a":0,"k":[1,1,1,1]},"o":{"a":0,"k":100}})";
const std::string linear = R"("o":{"x":[0],"y":[0]},"i":{"x":[1],"y":[1]})";

[[nodiscard]] std::string value(const std::string& v) {
    return R"({"a":0,"k":)" + v + "}";
}

[[nodiscard]] std::string rect(const double x, const double y, const double width,
                               const double height) {
    return R"({"ty":"rc","p":)" + value("[" + std::to_string(x) + "," + std::to_string(y) + "]") +
           R"(,"s":)" + value("[" + std::to_string(width) + "," + std::to_string(height) + "]") +
           R"(,"r":)" + value("0") + "}";
}

[[nodiscard]] std::string circle_shape(const double radius) {
    const double k = radius * 0.5523;
    const auto pair = [](const double x, const double y) {
        return "[" + std::to_string(x) + "," + std::to_string(y) + "]";
    };
    return R"({"c":true,"v":[)" + pair(100, 100 - radius) + "," + pair(100 + radius, 100) + "," +
           pair(100, 100 + radius) + "," + pair(100 - radius, 100) + R"(],"i":[)" + pair(-k, 0) +
           "," + pair(0, -k) + "," + pair(k, 0) + "," + pair(0, k) + R"(],"o":[)" + pair(k, 0) +
           "," + pair(0, k) + "," + pair(-k, 0) + "," + pair(0, -k) + "]}";
}

[[nodiscard]] std::string shape_layer(const int index, const std::string& shapes,
                                      const std::string& extra = {}, const std::string& ks = "{}") {
    return R"({"ty":4,"ind":)" + std::to_string(index) + R"(,"ip":0,"op":60,"st":0,"ks":)" + ks +
           R"(,"shapes":[)" + shapes + "]" + extra + "}";
}

[[nodiscard]] std::string composition(const std::string& layers, const std::string& assets = "[]",
                                      const int rate = 30) {
    return R"({"v":"5.7.4","fr":)" + std::to_string(rate) +
           R"(,"ip":0,"op":60,"w":200,"h":200,"assets":)" + assets + R"(,"layers":[)" + layers +
           "]}";
}

/** One measured animation: its document, the progress shown and the area it should cover. */
struct Case final {
    std::string name;
    std::string json;
    double progress = 0.5;
    double area = 0.0;
    double tolerance = 0.01;
};

[[nodiscard]] std::vector<Case> cases() {
    constexpr double pi = std::numbers::pi;
    const double disc = pi * 50.0 * 50.0;
    const double square = 160.0 * 160.0;
    const std::string big = rect(100, 100, 160, 160);
    const std::string mask_circle =
        R"({"mode":"MODE","inv":false,"o":{"a":0,"k":OPACITY},"x":{"a":0,"k":0},"pt":)" +
        value(circle_shape(50.0)) + "}";
    const auto mask = [&](const std::string& mode, const std::string& opacity = "100") {
        std::string result = mask_circle;
        result.replace(result.find("MODE"), 4, mode);
        result.replace(result.find("OPACITY"), 7, opacity);
        return result;
    };
    std::vector<Case> result;
    result.push_back(Case{"rectangle",
                          composition(shape_layer(1, rect(100, 100, 100, 100) + "," + white)), 0.5,
                          10000.0});
    // Scale keyframed 0 -> 100% over the composition: half way, half the size.
    result.push_back(Case{
        "keyframed scale",
        composition(shape_layer(1, rect(0, 0, 100, 100) + "," + white, {},
                                R"({"p":{"a":0,"k":[100,100]},"s":{"a":1,"k":[{"t":0,"s":[0,0],)" +
                                    linear + R"(},{"t":60,"s":[100,100]}]}})")),
        0.5, 2500.0});
    // A 160-long line stroked 10 wide, trimmed to its first half.
    result.push_back(Case{
        "trim paths",
        composition(shape_layer(
            1,
            R"({"ty":"sh","ks":)" +
                value(
                    R"({"c":false,"v":[[20,100],[180,100]],"i":[[0,0],[0,0]],"o":[[0,0],[0,0]]})") +
                R"(},{"ty":"tm","s":{"a":0,"k":0},"e":{"a":0,"k":50},"o":{"a":0,"k":0},"m":1},)" +
                R"({"ty":"st","c":{"a":0,"k":[1,1,1,1]},"o":{"a":0,"k":100},"w":{"a":0,"k":10},"lc":1,"lj":1})")),
        0.5, 800.0});
    result.push_back(Case{
        "mask add",
        composition(shape_layer(1, big + "," + white, R"(,"masksProperties":[)" + mask("a") + "]")),
        0.5, disc});
    result.push_back(Case{
        "mask subtract",
        composition(shape_layer(1, big + "," + white, R"(,"masksProperties":[)" + mask("s") + "]")),
        0.5, square - disc});
    result.push_back(
        Case{"mask opacity",
             composition(shape_layer(1, big + "," + white,
                                     R"(,"masksProperties":[)" + mask("a", "50") + "]")),
             0.5, disc * 128.0 / 255.0});
    // Track mattes: a disc above (td) the 160 square.
    const std::string matte_disc = shape_layer(
        1, R"({"ty":"sh","ks":)" + value(circle_shape(50.0)) + "}," + white, R"(,"td":1)");
    result.push_back(
        Case{"alpha matte",
             composition(matte_disc + "," + shape_layer(2, big + "," + white, R"(,"tt":1)")), 0.5,
             disc});
    result.push_back(
        Case{"inverted alpha matte",
             composition(matte_disc + "," + shape_layer(2, big + "," + white, R"(,"tt":2)")), 0.5,
             square - disc});
    // Luma: a mid-grey matte lets half of the white through.
    const std::string grey =
        shape_layer(1, big + R"(,{"ty":"fl","c":{"a":0,"k":[0.5,0.5,0.5,1]},"o":{"a":0,"k":100}})",
                    R"(,"td":1)");
    result.push_back(Case{"luma matte",
                          composition(grey + "," + shape_layer(2, big + "," + white, R"(,"tt":3)")),
                          0.5, square * 128.0 / 255.0});
    // Four 20 squares, a repeater stepping each 40 to the right.
    result.push_back(Case{
        "repeater",
        composition(shape_layer(
            1,
            R"({"ty":"gr","it":[)" + rect(40, 100, 20, 20) + "," + white +
                R"(,{"ty":"rp","c":{"a":0,"k":4},"o":{"a":0,"k":0},"m":1,"tr":{"ty":"tr","a":{"a":0,"k":[0,0]},"p":{"a":0,"k":[40,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"so":{"a":0,"k":100},"eo":{"a":0,"k":100}}},)" +
                R"({"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}]})")),
        0.5, 1600.0});
    // A precomposition whose square grows 0 -> 100% over its 60 frames, time-remapped to play at
    // half speed: at frame 30 it shows its frame 15, a quarter of the size.
    const std::string inner =
        shape_layer(1, rect(0, 0, 100, 100) + "," + white, {},
                    R"({"p":{"a":0,"k":[100,100]},"s":{"a":1,"k":[{"t":0,"s":[0,0],)" + linear +
                        R"(},{"t":60,"s":[100,100]}]}})");
    result.push_back(Case{
        "time-remapped precomposition",
        composition(
            R"({"ty":0,"ind":1,"refId":"inner","w":200,"h":200,"ip":0,"op":60,"st":0,"ks":{},"tm":{"a":1,"k":[{"t":0,"s":[0],)" +
                linear + R"(},{"t":60,"s":[1]}]}})",
            R"([{"id":"inner","layers":[)" + inner + "]}]"),
        0.5, 625.0});
    // Parenting: a child square at (50, 0) of a parent scaled to 50% at the centre.
    result.push_back(Case{
        "parenting",
        composition(
            shape_layer(2, rect(0, 0, 60, 60) + "," + white, R"(,"parent":1)",
                        R"({"p":{"a":0,"k":[50,0]}})") +
            "," +
            R"({"ty":3,"ind":1,"ip":0,"op":60,"st":0,"ks":{"p":{"a":0,"k":[100,100]},"s":{"a":0,"k":[50,50]}}})"),
        0.5, 900.0});
    return result;
}

[[nodiscard]] double covered(const headless::ApplicationHost& host, const double x, const double y,
                             const double width, const double height) {
    const std::span<const std::uint8_t> pixels = host.pixels();
    const std::uint32_t stride = host.framebuffer_width();
    double total = 0.0;
    for (auto row = static_cast<std::uint32_t>(y); row < static_cast<std::uint32_t>(y + height);
         ++row) {
        for (auto column = static_cast<std::uint32_t>(x);
             column < static_cast<std::uint32_t>(x + width); ++column)
            total += pixels[(static_cast<std::size_t>(row) * stride + column) * 4U] / 255.0;
    }
    return total;
}

void write(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
}

[[nodiscard]] headless::Scenario scenario(const std::string& backend, const std::string& module,
                                          const double width, const double height) {
    headless::Scenario result;
    result.application_id = "lottie.tests";
    result.surface_id = "lottie.tests";
    result.module = module;
    result.root = "Frames";
    result.root_role = "screen";
    result.render_backend = backend;
    result.width = width;
    result.height = height;
    result.scale = 1.0;
    result.reduced_motion = false;
    result.clear_color = {0U, 0U, 0U, 255U};
    return result;
}

/** Each case's frame, shown through `progress`, covers its expected area. */
void measure_frames(const std::filesystem::path& root, const std::string& backend) {
    const std::vector<Case> all = cases();
    const int rows = static_cast<int>((all.size() + columns - 1U) / columns);
    std::string module = "screen Frames {\n  root Panel(style: { background: null, border: null }, "
                         "layout: { kind: \"PANEL\", width: " +
                         std::to_string(cell * columns) +
                         ", height: " + std::to_string(cell * rows) + " }) {\n";
    for (std::size_t index = 0U; index < all.size(); ++index) {
        const std::string file = "assets/lottie-tests/case" + std::to_string(index) + ".json";
        write(root / file, all[index].json);
        module += "    Lottie(source: \"" + file +
                  "\", progress: " + std::to_string(all[index].progress) +
                  ", layout: { width: 200, height: 200, "
                  "placement: { x: " +
                  std::to_string(cell * static_cast<double>(index % columns)) +
                  ", y: " + std::to_string(cell * static_cast<double>(index / columns)) + " } })\n";
    }
    module += "  }\n}\n";
    write(root / "assets/lottie-tests/frames.strata", module);
    headless::ApplicationHost host(
        scenario(backend, "assets/lottie-tests/frames.strata", cell * columns, cell * rows), root);
    host.frame(0);
    host.frame(16'666'667);
    std::string failures;
    for (std::size_t index = 0U; index < all.size(); ++index) {
        const Case& entry = all[index];
        const double area = covered(host, cell * static_cast<double>(index % columns),
                                    cell * static_cast<double>(index / columns), cell, cell);
        const double error = std::abs(area - entry.area) / entry.area;
        std::cout << "  " << backend << " " << entry.name << ": area " << area << " of "
                  << entry.area << " (" << error * 100.0 << "%)\n";
        if (error > entry.tolerance) {
            failures += "\n  " + backend + ": " + entry.name + " covers " + std::to_string(area) +
                        " instead of " + std::to_string(entry.area);
        }
    }
    check(failures.empty(), "Lottie frames drew the wrong area:" + failures);
}

/**
 * Playback on the widget's own clock: a one-second scale from nothing to full, played once. Half
 * a second in it is near half the size; past its end it holds the last frame and its onComplete
 * has run (the marker it reveals is drawn).
 */
void measure_playback(const std::filesystem::path& root, const std::string& backend) {
    write(
        root / "assets/lottie-tests/grow.json",
        composition(shape_layer(1, rect(0, 0, 100, 100) + "," + white, {},
                                R"({"p":{"a":0,"k":[100,100]},"s":{"a":1,"k":[{"t":0,"s":[0,0],)" +
                                    linear + R"(},{"t":60,"s":[100,100]}]}})"),
                    "[]", 60));
    write(root / "assets/lottie-tests/playback.strata",
          "component Player() {\n"
          "  state done = false;\n"
          "  Panel(style: { background: null, border: null }, layout: { kind: \"PANEL\", width: "
          "400, height: 200 }) {\n"
          "    Lottie(source: \"assets/lottie-tests/grow.json\", loop: false, onComplete: "
          "action(\"state.toggle\", name: \"done\"), layout: { width: 200, height: 200, placement: "
          "{ x: 0, y: 0 } })\n"
          "    if done {\n"
          "      Panel(style: { background: #FFFFFF, border: null }, layout: { kind: \"PANEL\", "
          "width: 40, height: 40, placement: { x: 280, y: 80 } })\n"
          "    }\n"
          "  }\n"
          "}\n"
          "screen Frames {\n  root Player()\n}\n");
    headless::ApplicationHost host(
        scenario(backend, "assets/lottie-tests/playback.strata", 400.0, 200.0), root);
    constexpr std::int64_t frame = 16'666'667;
    std::int64_t now = 0;
    for (int index = 0; index <= 30; ++index)
        host.frame(now += frame);
    const double half = covered(host, 0.0, 0.0, 200.0, 200.0);
    const double marker_early = covered(host, 200.0, 0.0, 200.0, 200.0);
    for (int index = 0; index < 60; ++index)
        host.frame(now += frame);
    const double full = covered(host, 0.0, 0.0, 200.0, 200.0);
    const double marker = covered(host, 200.0, 0.0, 200.0, 200.0);
    std::cout << "  " << backend << " playback: half a second " << half << ", end " << full
              << ", completion marker " << marker_early << " -> " << marker << "\n";
    // Half a second of a linear 0-100% scale, give or take the frames before the clock starts.
    check(half > 1600.0 && half < 2700.0, backend + ": playback did not reach half way in time");
    check(std::abs(full - 10000.0) / 10000.0 < 0.01, backend + ": playback did not hold its end");
    check(marker_early < 1.0 && std::abs(marker - 1600.0) < 16.0,
          backend + ": onComplete did not run once at the end");
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count < 2)
            throw std::invalid_argument("usage: strata_lottie_render_tests <scratch-directory>");
        const std::filesystem::path root = arguments[1];
        for (const std::string& backend : available_backends()) {
            measure_frames(root, backend);
            measure_playback(root, backend);
        }
        std::cout << "strata_lottie_render_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_lottie_render_tests: " << error.what() << '\n';
        return 1;
    }
}
