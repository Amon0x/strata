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
             "layout: { kind: \"PANEL\", width: 600, height: 600 }) {\n";
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
    // Arbitrary outlines draw from coverage masks: polygons, both fill rules, a translucent stroke
    // whose pieces overlap at its join (a union, so uniform alpha), and dashes.
    const auto masked = [&](const std::string& name, const double column, const double line,
                            const std::string& shape, const double area,
                            const double tolerance = 0.004) {
        const double x = column * box;
        const double y = line * box;
        source += "    Draw(layout: { width: " + std::to_string(box) + ", height: " +
                  std::to_string(box) + ", placement: { x: " + std::to_string(x) + ", y: " +
                  std::to_string(y) + " } }, shapes: [" + shape + "])\n";
        measured.push_back(Measured{name, x, y, box, area, tolerance});
    };
    const std::string star = "points: [{ x: 0.5, y: 0.1 }, { x: 0.74687, y: 0.859787 }, "
                             "{ x: 0.100556, y: 0.390213 }, { x: 0.899444, y: 0.390213 }, "
                             "{ x: 0.25313, y: 0.859787 }]";
    masked("diamond", 2.0, 2.0,
           R"({ kind: "polygon", points: [{ x: 0.5, y: 0.1 }, { x: 0.9, y: 0.5 }, { x: 0.5, y: 0.9 }, { x: 0.1, y: 0.5 }], fill: #FFFFFF })",
           96.0 * 96.0 / 2.0);
    masked("pentagram nonzero", 3.0, 2.0,
           "{ kind: \"polygon\", " + star + ", fill: #FFFFFF }", 2850.4448);
    masked("pentagram even-odd", 4.0, 2.0,
           "{ kind: \"polygon\", " + star + ", fill: #FFFFFF, fillRule: \"evenodd\" }",
           1969.6096);
    masked("translucent zigzag", 0.0, 3.0,
           R"({ kind: "polyline", points: [{ x: 0.15, y: 0.25 }, { x: 0.5, y: 0.8 }, { x: 0.85, y: 0.25 }], stroke: #FFFFFF80, strokeStyle: { width: 12 } })",
           1877.2688 * 128.0 / 255.0, 0.006);
    masked("dashed line", 1.0, 3.0,
           R"({ kind: "polyline", points: [{ x: 0.1, y: 0.5 }, { x: 0.9, y: 0.5 }], stroke: #FFFFFF, strokeStyle: { width: 4, dash: [10, 10] } })",
           5.0 * 10.0 * 4.0);
    // Gradients are evaluated per pixel over the mask, so a gradient's summed value is its exact
    // integral over the shape: every stop, the extend mode and radial distance all show in it.
    const std::string square = R"(kind: "polygon", points: [{ x: 0.1, y: 0.1 }, { x: 0.9, y: 0.1 }, { x: 0.9, y: 0.9 }, { x: 0.1, y: 0.9 }])";
    masked("linear gradient", 2.0, 3.0,
           "{ " + square + R"(, fill: { kind: "linear", from: { x: 0, y: 0.5 }, to: { x: 1, y: 0.5 }, stops: [{ offset: 0, color: #000000 }, { offset: 1, color: #FFFFFF }] } })",
           96.0 * 48.0);
    masked("three-stop gradient", 3.0, 3.0,
           "{ " + square + R"(, fill: { kind: "linear", from: { x: 0, y: 0.5 }, to: { x: 1, y: 0.5 }, stops: [{ offset: 0, color: #FFFFFF }, { offset: 0.5, color: #000000 }, { offset: 1, color: #FFFFFF }] } })",
           96.0 * 38.4);
    masked("radial gradient circle", 4.0, 3.0,
           R"({ kind: "circle", center: { x: 0.5, y: 0.5 }, radius: 0.4, fill: { kind: "radial", center: { x: 0.5, y: 0.5 }, radius: 0.4, stops: [{ offset: 0, color: #FFFFFF }, { offset: 1, color: #000000 }] } })",
           pi * 48.0 * 48.0 / 3.0);
    masked("mirrored gradient", 0.0, 4.0,
           "{ " + square + R"(, fill: { kind: "linear", from: { x: 0, y: 0.5 }, to: { x: 0.25, y: 0.5 }, extend: "mirror", stops: [{ offset: 0, color: #000000 }, { offset: 1, color: #FFFFFF }] } })",
           96.0 * 55.2);
    masked("gradient stroke", 1.0, 4.0,
           R"({ kind: "line", from: { x: 0.1, y: 0.5 }, to: { x: 0.9, y: 0.5 }, stroke: { kind: "linear", from: { x: 0, y: 0.5 }, to: { x: 1, y: 0.5 }, stops: [{ offset: 0, color: #FFFFFF }, { offset: 1, color: #000000 }] }, strokeStyle: { width: 10 } })",
           10.0 * 48.0, 0.006);
    // Rect shapes draw analytically through the rounded-rect mode, the stroke centred on the edge.
    masked("rounded rect shape", 2.0, 4.0,
           R"({ kind: "rect", x: 0.1, y: 0.1, width: 0.8, height: 0.8, radius: 0.2, fill: #FFFFFF })",
           96.0 * 96.0 - (4.0 - pi) * 24.0 * 24.0);
    masked("stroked rect shape", 3.0, 4.0,
           R"({ kind: "rect", x: 0.1, y: 0.1, width: 0.8, height: 0.8, stroke: #FFFFFF, strokeStyle: { width: 6 } })",
           102.0 * 102.0 - 90.0 * 90.0);
    masked("stroked rounded rect shape", 4.0, 4.0,
           R"({ kind: "rect", x: 0.1, y: 0.1, width: 0.8, height: 0.8, radius: 0.2, fill: #000000, stroke: #FFFFFF, strokeStyle: { width: 6 } })",
           (102.0 * 102.0 - (4.0 - pi) * 27.0 * 27.0) - (90.0 * 90.0 - (4.0 - pi) * 21.0 * 21.0));
    source += "  }\n}\n";
    return measured;
}

/** A scene to measure: its module source, its surface size and the shapes in it. */
struct Scene final {
    std::string source;
    double width = 0.0;
    double height = 0.0;
    std::vector<Measured> shapes;
};

/** The grid of every shape kind above, at sizes that fit an atlas page. */
[[nodiscard]] Scene grid_scene() {
    Scene scene;
    scene.shapes = layout_shapes(scene.source);
    scene.width = 600.0;
    scene.height = 600.0;
    return scene;
}

/**
 * One shape larger than an atlas page, which draws in tiles: seams between tiles, empty and fully
 * covered tiles and tiles turned with the draw must all leave the area exact.
 */
[[nodiscard]] Scene large_scene(const std::string& name, const std::string& style,
                                const std::string& shape, const double area) {
    constexpr double side = 1040.0;
    Scene scene;
    scene.width = side + 20.0;
    scene.height = side + 20.0;
    scene.source = "overlay Shapes {\n  root Panel(style: { background: null, border: null }, "
                   "layout: { kind: \"PANEL\", width: " +
                   std::to_string(scene.width) + ", height: " + std::to_string(scene.height) +
                   " }) {\n    Draw(style: { " + style + " }, layout: { width: " +
                   std::to_string(side) + ", height: " + std::to_string(side) +
                   ", placement: { x: 10, y: 10 } }, shapes: [" + shape + "])\n  }\n}\n";
    scene.shapes.push_back(Measured{name, 0.0, 0.0, scene.width, area});
    return scene;
}

void measure(const std::filesystem::path& root, const std::string& backend, const double scale,
             const Scene& scene) {
    const std::string& source = scene.source;
    const std::vector<Measured>& shapes = scene.shapes;
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
    scenario.width = scene.width;
    scenario.height = scene.height;
    scenario.scale = scale;
    scenario.reduced_motion = true;
    scenario.clear_color = {0U, 0U, 0U, 255U};
    headless::ApplicationHost host(scenario, root);
    host.frame(0);
    const std::span<const std::uint8_t> pixels = host.pixels();
    const std::uint32_t width = host.framebuffer_width();
    const std::uint32_t height = host.framebuffer_height();
    check(!pixels.empty(), "the drawing host produced no pixels");
    std::string failures;
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
        if (error > shape.tolerance) {
            failures += "\n  " + backend + " at scale " + std::to_string(scale) + ": " +
                        shape.name + " covers " + std::to_string(covered) + " instead of " +
                        std::to_string(expected);
        }
    }
    // Every shape is reported before failing, so one regression shows its whole extent.
    check(failures.empty(), "shapes drew the wrong area:" + failures);
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count < 2)
            throw std::invalid_argument("usage: strata_drawing_tests <scratch-directory>");
        const std::filesystem::path root = arguments[1];
        // Tiled shapes scale the grid's pentagram (area 2850.4448 in a 120 box) and radial
        // gradient (a third of the disc) up past a page.
        const double large = 1040.0 / box;
        const std::string star = R"({ kind: "polygon", points: [{ x: 0.5, y: 0.1 }, { x: 0.74687, y: 0.859787 }, { x: 0.100556, y: 0.390213 }, { x: 0.899444, y: 0.390213 }, { x: 0.25313, y: 0.859787 }], fill: #FFFFFF })";
        const std::vector<Scene> scenes{
            grid_scene(),
            large_scene("tiled pentagram", "", star, 2850.4448 * large * large),
            large_scene("rotated tiled pentagram", "rotate: 33", star,
                        2850.4448 * large * large),
            large_scene(
                "tiled radial gradient", "",
                R"({ kind: "circle", center: { x: 0.5, y: 0.5 }, radius: 0.4, fill: { kind: "radial", center: { x: 0.5, y: 0.5 }, radius: 0.4, stops: [{ offset: 0, color: #FFFFFF }, { offset: 1, color: #000000 }] } })",
                std::numbers::pi * 416.0 * 416.0 / 3.0),
        };
        for (const std::string& backend : available_backends()) {
            for (const double scale : {1.0, 1.25, 2.0}) {
                for (const Scene& scene : scenes)
                    measure(root, backend, scale, scene);
            }
        }
        std::cout << "strata_drawing_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_drawing_tests: " << error.what() << '\n';
        return 1;
    }
}
