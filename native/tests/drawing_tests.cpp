// Drawing accuracy: shapes rendered through the public host and every backend this build has,
// measured from the framebuffer. White shapes on black make each pixel's value its coverage, so a
// shape's summed coverage is its area: any edge drawn outside (or inside) the geometry shows up as
// area, a thousandth of it at a time.

#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numbers>
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

/** One measured shape: its Draw's box in logical pixels and its true area there. */
struct Measured final {
    std::string name;
    double x = 0.0;
    double y = 0.0;
    double size = 0.0;
    double area = 0.0;
    /** The share of the area the measurement may miss by. */
    double tolerance = 0.004;
};

constexpr double box = 120.0;

/** Every shape sits alone in a `box`-sized Draw on a grid, white on black. */
[[nodiscard]] std::vector<Measured> layout_shapes(std::string& source) {
    struct Shape final {
        std::string name;
        std::string shapes;
        double area;
        double tolerance = 0.004;
        std::string style = {};
    };
    constexpr double pi = std::numbers::pi;
    const std::vector<Shape> shapes{
        {"circle", R"({ kind: "circle", center: { x: 0.5, y: 0.5 }, radius: 0.35, fill: #FFFFFF })",
         pi * 42.0 * 42.0},
        {"ellipse",
         R"({ kind: "ellipse", center: { x: 0.5, y: 0.5 }, radiusX: 0.4, radiusY: 0.2, fill: #FFFFFF })",
         pi * 48.0 * 24.0},
        {"ring stroke",
         R"({ kind: "circle", center: { x: 0.5, y: 0.5 }, radius: 0.35, stroke: #FFFFFF, strokeStyle: { width: 6 } })",
         2.0 * pi * 42.0 * 6.0},
        {"hairline ring",
         R"({ kind: "circle", center: { x: 0.5, y: 0.5 }, radius: 0.35, stroke: #FFFFFF, strokeStyle: { width: 0.5 } })",
         2.0 * pi * 42.0 * 0.5, 0.02},
        {"pie quarter",
         R"({ kind: "sector", center: { x: 0.5, y: 0.5 }, radius: 0.4, start: 0, sweep: 90, fill: #FFFFFF })",
         pi * 48.0 * 48.0 / 4.0},
        {"annular sector",
         R"({ kind: "sector", center: { x: 0.5, y: 0.5 }, radius: 0.4, innerRadius: 0.2, start: -60, sweep: 200, fill: #FFFFFF })",
         pi * (48.0 * 48.0 - 24.0 * 24.0) * 200.0 / 360.0},
        {"round arc",
         R"({ kind: "arc", center: { x: 0.5, y: 0.5 }, radius: 0.3, start: 30, sweep: 120, stroke: #FFFFFF, strokeStyle: { width: 10, cap: "round" } })",
         36.0 * 2.0 * pi / 3.0 * 10.0 + pi * 25.0},
        {"line", R"({ kind: "line", from: { x: 0.1, y: 0.5 }, to: { x: 0.9, y: 0.5 }, stroke: #FFFFFF, strokeStyle: { width: 3 } })",
         96.0 * 3.0},
        {"hairline", R"({ kind: "line", from: { x: 0.1, y: 0.3 }, to: { x: 0.9, y: 0.7 }, stroke: #FFFFFF, strokeStyle: { width: 0.5 } })",
         std::hypot(96.0, 48.0) * 0.5, 0.02},
        {"rotated line",
         R"({ kind: "line", from: { x: 0.15, y: 0.5 }, to: { x: 0.85, y: 0.5 }, stroke: #FFFFFF, strokeStyle: { width: 4, cap: "square" } })",
         (84.0 + 4.0) * 4.0, 0.004, "rotate: 33"},
    };
    std::vector<Measured> measured;
    source = "overlay Shapes {\n  root Panel(style: { background: null, border: null }, "
             "layout: { kind: \"PANEL\", width: 600, height: 360 }) {\n";
    for (std::size_t index = 0U; index < shapes.size(); ++index) {
        const double x = static_cast<double>(index % 5U) * box;
        const double y = static_cast<double>(index / 5U) * box;
        source += "    Draw(style: { " + shapes[index].style + " }, layout: { width: " +
                  std::to_string(box) + ", height: " + std::to_string(box) +
                  ", placement: { x: " + std::to_string(x) + ", y: " + std::to_string(y) +
                  " } }, shapes: [" + shapes[index].shapes + "])\n";
        measured.push_back(Measured{shapes[index].name, x, y, box, shapes[index].area,
                                    shapes[index].tolerance});
    }
    // Rounded rects and borders at fractional positions, where a cut-off edge ramp shows as area.
    const double row = 2.0 * box;
    source += "    Panel(style: { background: #FFFFFF, border: null, radius: 14 }, layout: { kind: "
              "\"PANEL\", width: 90.4, height: 60.6, placement: { x: 10.3, y: " +
              std::to_string(row + 20.7) + " } })\n";
    measured.push_back(Measured{"rounded rect", 0.0, row, box,
                                90.4 * 60.6 - (4.0 - pi) * 14.0 * 14.0});
    source += "    Panel(style: { background: null, border: { width: 3, color: #FFFFFF }, radius: "
              "10 }, layout: { kind: \"PANEL\", width: 90.4, height: 60.6, placement: { x: " +
              std::to_string(box + 10.3) + ", y: " + std::to_string(row + 20.7) + " } })\n";
    measured.push_back(Measured{
        "rounded border", box, row, box,
        (90.4 * 60.6 - (4.0 - pi) * 10.0 * 10.0) - (84.4 * 54.6 - (4.0 - pi) * 7.0 * 7.0)});
    source += "  }\n}\n";
    return measured;
}

void measure(const std::filesystem::path& root, const std::string& backend, const double scale) {
    std::string source;
    const std::vector<Measured> shapes = layout_shapes(source);
    std::filesystem::create_directories(root / "assets" / "drawing");
    {
        std::ofstream module(root / "assets" / "drawing" / "shapes.strata", std::ios::binary);
        module << source;
    }
    headless::Scenario scenario;
    scenario.application_id = "drawing.tests";
    scenario.surface_id = "drawing.tests";
    scenario.module = "assets/drawing/shapes.strata";
    scenario.root = "Shapes";
    scenario.render_backend = backend;
    scenario.width = 600.0;
    scenario.height = 360.0;
    scenario.scale = scale;
    scenario.reduced_motion = true;
    scenario.clear_color = {0U, 0U, 0U, 255U};
    headless::ApplicationHost host(scenario, root);
    host.frame(0);
    const std::span<const std::uint8_t> pixels = host.pixels();
    const std::uint32_t width = host.framebuffer_width();
    const std::uint32_t height = host.framebuffer_height();
    check(!pixels.empty(), "the drawing host produced no pixels");
    for (const Measured& shape : shapes) {
        const auto left = static_cast<std::uint32_t>(std::floor(shape.x * scale));
        const auto top = static_cast<std::uint32_t>(std::floor(shape.y * scale));
        const auto right = std::min(width, static_cast<std::uint32_t>(
                                               std::ceil((shape.x + shape.size) * scale)));
        const auto bottom = std::min(height, static_cast<std::uint32_t>(
                                                 std::ceil((shape.y + shape.size) * scale)));
        double covered = 0.0;
        for (std::uint32_t y = top; y < bottom; ++y) {
            for (std::uint32_t x = left; x < right; ++x)
                covered += pixels[(static_cast<std::size_t>(y) * width + x) * 4U] / 255.0;
        }
        const double expected = shape.area * scale * scale;
        const double error = std::abs(covered - expected) / expected;
        std::cout << "  " << backend << " x" << scale << " " << shape.name << ": area "
                  << covered << " of " << expected << " (" << error * 100.0 << "%)\n";
        check(error <= shape.tolerance,
              backend + " at scale " + std::to_string(scale) + ": " + shape.name +
                  " covers " + std::to_string(covered) + " instead of " +
                  std::to_string(expected));
    }
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count < 2)
            throw std::invalid_argument("usage: strata_drawing_tests <scratch-directory>");
        const std::filesystem::path root = arguments[1];
        for (const std::string& backend : available_backends()) {
            for (const double scale : {1.0, 1.25, 2.0})
                measure(root, backend, scale);
        }
        std::cout << "strata_drawing_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_drawing_tests: " << error.what() << '\n';
        return 1;
    }
}
