// Lottie: parsing, keyframe interpolation (bezier easing, holds, spatial paths), the outline
// arithmetic frames are built from (parametric shapes, trim paths, round corners, the region
// sweep masks and mattes resolve through) and the render commands a frame becomes.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "resource/lottie.hpp"
#include "ui/lottie/geometry.hpp"
#include "ui/lottie/scene.hpp"
#include "ui/widget/lottie_model.hpp"

namespace {

using namespace strata;
namespace model = resource::lottie;
namespace geo = ui::lottie;

void check(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

template <typename Function>
void check_throws(Function&& operation, const std::string_view message) {
    try {
        operation();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

[[nodiscard]] bool near(const double left, const double right, const double tolerance) {
    return std::abs(left - right) <= tolerance;
}

[[nodiscard]] std::string document(const std::string& layers, const std::string& assets = "[]",
                                   const double out = 60.0) {
    return R"({"v":"5.7.4","fr":30,"ip":0,"op":)" + std::to_string(out) +
           R"(,"w":200,"h":100,"nm":"test","assets":)" + assets + R"(,"layers":)" + layers + "}";
}

void test_parsing() {
    const model::Animation animation = model::parse(
        document(R"([
        {"ty":4,"ind":1,"ip":0,"op":60,"st":0,"ks":{"p":{"a":0,"k":[10,20,0]}},
         "shapes":[{"ty":"gr","it":[
            {"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[40,20]},"r":{"a":0,"k":4}},
            {"ty":"fl","c":{"a":0,"k":[255,128,0,255]},"o":{"a":0,"k":100},"r":2},
            {"ty":"st","c":{"a":0,"k":[0,0,1,1]},"o":{"a":0,"k":50},"w":{"a":0,"k":3},"lc":2,"lj":3,
             "d":[{"n":"d","v":{"a":0,"k":4}},{"n":"g","v":{"a":0,"k":2}},{"n":"o","v":{"a":0,"k":1}}]},
            {"ty":"tr","p":{"a":0,"k":[5,5]},"o":{"a":0,"k":80}}]},
          {"ty":"zz"},
          {"ty":"el","hd":true}]},
        {"ty":0,"ind":2,"refId":"inner","w":50,"h":50,"ip":0,"op":60,"st":0,"ks":{},
         "tm":{"a":1,"k":[{"t":0,"s":[0],"o":{"x":[0],"y":[0]},"i":{"x":[1],"y":[1]}},{"t":60,"s":[2]}]}}
    ])",
                 R"([{"id":"inner","layers":[{"ty":3,"ind":1,"ip":0,"op":60,"ks":{}}]}])"));
    check(animation.width == 200.0 && animation.height == 100.0 && animation.frame_rate == 30.0,
          "the composition keeps its size and rate");
    check(animation.root.layers.size() == 2U && animation.precomps.size() == 1U,
          "layers and precompositions are read");
    const model::Layer& shape_layer = animation.root.layers[0];
    check(shape_layer.type == model::LayerType::shape && shape_layer.index == 1,
          "a shape layer keeps its type and index");
    check(shape_layer.shapes.size() == 1U, "unknown and hidden shape items are left out");
    const auto& group = std::get<model::GroupItem>(shape_layer.shapes[0].content);
    check(group.items.size() == 3U, "a group's transform is not one of its items");
    check(group.transform.opacity.scalar(0.0) == 80.0 &&
              group.transform.position_at(0.0) == model::Vec2{5.0, 5.0},
          "a group reads its transform from its tr item");
    const auto& fill = std::get<model::SolidPaintItem>(group.items[1].content);
    std::array<double, 4> color{};
    fill.color.evaluate(0.0, color);
    check(near(color[0], 1.0, 1.0e-9) && near(color[1], 128.0 / 255.0, 1.0e-9),
          "0-255 colours from old exports become 0-1");
    check(fill.rule == model::FillRule::even_odd, "fill rule 2 is even-odd");
    const auto& stroke = std::get<model::SolidPaintItem>(group.items[2].content);
    check(stroke.stroke && stroke.style.cap == model::LineCap::round &&
              stroke.style.join == model::LineJoin::bevel && stroke.style.dashes.size() == 3U &&
              stroke.style.dashes[2].kind == model::Dash::Kind::offset,
          "a stroke reads its caps, joins and dashes");
    const model::Layer& precomp = animation.root.layers[1];
    check(precomp.precomp == 0U && precomp.time_remap.has_value() &&
              precomp.time_remap->scalar(30.0) == 1.0,
          "a precomposition layer resolves its asset and time remap");

    check_throws([] { static_cast<void>(model::parse("[]")); }, "a document must be an object");
    check_throws(
        [] {
            static_cast<void>(
                model::parse("{\"w\":0,\"h\":1,\"fr\":30,\"ip\":0,\"op\":1,\"layers\":[]}"));
        },
        "a composition needs a size");
    check_throws(
        [] {
            static_cast<void>(
                model::parse("{\"w\":1,\"h\":1,\"fr\":30,\"ip\":5,\"op\":5,\"layers\":[]}"));
        },
        "a composition needs frames");
    check_throws([] { static_cast<void>(model::parse("{\"w\":1,")); }, "malformed JSON is refused");
    check_throws(
        [] {
            static_cast<void>(model::parse(document(
                R"([{"ty":0,"refId":"a","ip":0,"op":60,"ks":{}}])",
                R"([{"id":"a","layers":[{"ty":0,"refId":"a","ip":0,"op":60,"ks":{}}]}])")));
        },
        "a precomposition containing itself is refused");
}

void test_easing() {
    // Reference values solved independently (bisection on x(u)) for well-known curves.
    check(near(model::cubic_ease(0.25, 0.1, 0.25, 1.0, 0.5), 0.802403387584857, 1.0e-6),
          "CSS ease at one half");
    check(near(model::cubic_ease(0.42, 0.0, 0.58, 1.0, 0.25), 0.1291619310473198, 1.0e-6),
          "ease-in-out at one quarter");
    check(near(model::cubic_ease(0.34, 1.56, 0.64, 1.0, 0.5), 1.087400670218698, 1.0e-6),
          "an overshooting curve passes its target");
    check(near(model::cubic_ease(0.33, 0.0, 0.67, 1.0, 0.3), 0.2170564858664602, 1.0e-6),
          "After Effects' default easy ease");
    check(model::cubic_ease(0.0, 0.0, 1.0, 1.0, 0.37) == 0.37, "linear handles are linear");
    check(model::cubic_ease(0.5, 0.5, 0.5, 0.5, 0.0) == 0.0 &&
              model::cubic_ease(0.5, 0.5, 0.5, 0.5, 1.0) == 1.0,
          "easing keeps its ends");
}

[[nodiscard]] model::Property parse_property(const std::string& json) {
    const model::Animation animation = model::parse(
        document(R"([{"ty":4,"ip":0,"op":60,"ks":{"p":)" + json + R"(},"shapes":[]}])"));
    return animation.root.layers[0].transform.position;
}

void test_keyframes() {
    const model::Property linear = parse_property(
        R"({"a":1,"k":[{"t":10,"s":[0,0],"o":{"x":[0],"y":[0]},"i":{"x":[1],"y":[1]}},{"t":20,"s":[100,50]}]})");
    check(linear.point(0.0) == model::Vec2{0.0, 0.0}, "before the first keyframe its value holds");
    check(linear.point(15.0) == model::Vec2{50.0, 25.0}, "linear keyframes interpolate");
    check(linear.point(99.0) == model::Vec2{100.0, 50.0},
          "after the last keyframe its value holds");

    // Old exports carry each segment's end value, and the last keyframe its time alone.
    const model::Property old = parse_property(
        R"({"a":1,"k":[{"t":0,"s":[0,0],"e":[10,10],"o":{"x":0.42,"y":0},"i":{"x":0.58,"y":1}},{"t":10}]})");
    check(near(old.point(2.5).x, 10.0 * 0.1291619310473198, 1.0e-5),
          "easing applies, from scalar handles too");
    check(old.point(50.0) == model::Vec2{10.0, 10.0}, "an old export ends on its last end value");

    // Per-dimension easing: x eases in, y is linear.
    const model::Property split = parse_property(
        R"({"a":1,"k":[{"t":0,"s":[0,0],"o":{"x":[0.42,0],"y":[0,0]},"i":{"x":[0.58,1],"y":[1,1]}},{"t":10,"s":[100,100]}]})");
    const model::Vec2 halfway = split.point(2.5);
    check(near(halfway.x, 12.91619310473198, 1.0e-4) && near(halfway.y, 25.0, 1.0e-9),
          "each dimension takes its own handles");

    const model::Property hold =
        parse_property(R"({"a":1,"k":[{"t":0,"s":[1,2],"h":1},{"t":10,"s":[5,6]}]})");
    check(hold.point(9.99) == model::Vec2{1.0, 2.0} && hold.point(10.0) == model::Vec2{5.0, 6.0},
          "a hold keyframe keeps its value until the next");

    // A spatial segment moves along its curve at an even speed: halfway along the arc is the
    // curve's middle (by symmetry), a quarter of the way is where the arc length says.
    const model::Property spatial = parse_property(
        R"({"a":1,"k":[{"t":0,"s":[0,0],"to":[0,-50],"ti":[0,-50],"o":{"x":[0],"y":[0]},"i":{"x":[1],"y":[1]}},{"t":10,"s":[100,0]}]})");
    const model::Vec2 middle = spatial.point(5.0);
    check(near(middle.x, 50.0, 0.05) && near(middle.y, -37.5, 0.05),
          "a spatial keyframe passes the curve's middle halfway");
    const model::Vec2 quarter = spatial.point(2.5);
    check(near(quarter.x, 16.6858, 0.3) && near(quarter.y, -28.8106, 0.3),
          "a spatial keyframe moves evenly along its arc");

    // Shapes interpolate vertex by vertex.
    const model::Animation animation =
        model::parse(document(R"([{"ty":4,"ip":0,"op":60,"ks":{},"shapes":[
        {"ty":"sh","ks":{"a":1,"k":[
          {"t":0,"s":[{"c":true,"v":[[0,0],[10,0],[10,10]],"i":[[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0]]}],"o":{"x":[0],"y":[0]},"i":{"x":[1],"y":[1]}},
          {"t":10,"s":[{"c":true,"v":[[0,0],[30,0],[30,30]],"i":[[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0]]}]}]}}]}])"));
    const auto& path = std::get<model::PathItem>(animation.root.layers[0].shapes[0].content);
    const model::BezierShape morphed = path.shape.evaluate(5.0);
    check(morphed.closed && morphed.vertices.size() == 3U &&
              morphed.vertices[1] == model::Vec2{20.0, 0.0} &&
              morphed.vertices[2] == model::Vec2{20.0, 20.0},
          "a path keyframe morphs vertex by vertex");
}

[[nodiscard]] double area(const std::vector<std::vector<ui::Point>>& polygons) {
    double total = 0.0;
    for (const std::vector<ui::Point>& polygon : polygons) {
        double twice = 0.0;
        for (std::size_t index = 0U; index < polygon.size(); ++index) {
            const ui::Point a = polygon[index];
            const ui::Point b = polygon[(index + 1U) % polygon.size()];
            twice += a.x * b.y - b.x * a.y;
        }
        total += twice * 0.5;
    }
    return total;
}

[[nodiscard]] geo::Contour line(const double x0, const double y0, const double x1,
                                const double y1) {
    return geo::Contour{ui::Point{x0, y0}, {geo::Cubic{{x0, y0}, {x1, y1}, {x1, y1}}}, false};
}

[[nodiscard]] geo::Contour square(const double x, const double y, const double side) {
    geo::Contour result;
    result.start = ui::Point{x, y};
    result.closed = true;
    const ui::Point corners[] = {{x + side, y}, {x + side, y + side}, {x, y + side}, {x, y}};
    ui::Point from = result.start;
    for (const ui::Point to : corners) {
        result.segments.push_back(geo::Cubic{from, to, to});
        from = to;
    }
    return result;
}

void test_geometry() {
    // Parametric shapes start and wind as After Effects draws them.
    const geo::Contour ellipse = geo::ellipse_contour({0.0, 0.0}, {100.0, 100.0}, 1);
    check(ellipse.start == ui::Point{0.0, -50.0} && ellipse.segments.size() == 4U &&
              ellipse.segments[0].to == ui::Point{50.0, 0.0},
          "an ellipse starts at its top and turns clockwise");
    check(near(geo::contour_length(ellipse), std::numbers::pi * 100.0, 0.2),
          "an ellipse's length is its circumference");
    const geo::Contour backwards = geo::ellipse_contour({0.0, 0.0}, {100.0, 100.0}, 3);
    check(backwards.start == ui::Point{0.0, -50.0} &&
              near(backwards.segments[0].to.x, -50.0, 1.0e-9),
          "direction 3 reverses from the same start");
    const geo::Contour rectangle = geo::rectangle_contour({0.0, 0.0}, {40.0, 20.0}, 0.0, 1);
    check(rectangle.start == ui::Point{20.0, -10.0} &&
              near(geo::contour_length(rectangle), 120.0, 1.0e-6),
          "a rectangle starts at the top of its right edge");
    const geo::Contour star =
        geo::polystar_contour(true, {0.0, 0.0}, 5.0, 0.0, 50.0, 20.0, 0.0, 0.0, 1);
    check(star.segments.size() == 10U && near(star.start.x, 0.0, 1.0e-9) &&
              near(star.start.y, -50.0, 1.0e-9),
          "a five-pointed star has ten vertices, the first straight up");
    const geo::Contour hexagon =
        geo::polystar_contour(false, {0.0, 0.0}, 6.0, 0.0, 10.0, 0.0, 0.0, 0.0, 1);
    check(hexagon.segments.size() == 6U && near(geo::contour_length(hexagon), 60.0, 1.0e-6),
          "a regular hexagon's perimeter is six sides");

    // Trim paths.
    const geo::CubicPath middle = geo::trim({line(0.0, 0.0, 100.0, 0.0)}, 0.25, 0.75, 0.0, false);
    check(middle.size() == 1U && near(middle[0].start.x, 25.0, 0.01) &&
              near(geo::contour_length(middle[0]), 50.0, 0.02),
          "a trim keeps its share of the length");
    check(geo::trim({line(0.0, 0.0, 100.0, 0.0)}, 0.0, 1.0, 0.0, false).size() == 1U &&
              geo::trim({line(0.0, 0.0, 100.0, 0.0)}, 0.4, 0.4, 0.0, false).empty(),
          "a whole trim keeps everything and an empty one nothing");
    // Offset past the end of a closed square wraps through its start as one stroke.
    const geo::CubicPath wrapped = geo::trim({square(0.0, 0.0, 100.0)}, 0.0, 0.2, 0.9, false);
    check(wrapped.size() == 1U && near(geo::contour_length(wrapped[0]), 80.0, 1.0e-6) &&
              near(wrapped[0].start.x, 0.0, 1.0e-6) && near(wrapped[0].start.y, 40.0, 1.0e-6),
          "a wrapped trim of a closed path continues through its start");
    // Individually: two lines measured as one path.
    const geo::CubicPath both = geo::trim(
        {line(0.0, 0.0, 100.0, 0.0), line(0.0, 10.0, 100.0, 10.0)}, 0.25, 0.75, 0.0, true);
    check(both.size() == 2U && near(geo::contour_length(both[0]), 50.0, 1.0e-6) &&
              near(geo::contour_length(both[1]), 50.0, 1.0e-6) &&
              near(both[1].start.x, 0.0, 1.0e-6),
          "an individual trim runs across paths in order");
    const auto ranges = geo::trim_ranges(0.8, 1.0, 0.5);
    check(ranges.has_value() && ranges->size() == 1U && near((*ranges)[0].first, 0.3, 1.0e-9) &&
              near((*ranges)[0].second, 0.5, 1.0e-9),
          "an offset beyond a full turn wraps");

    // Round corners: a square's sharp corners become quarter curves.
    const geo::CubicPath rounded = geo::round_corners({square(0.0, 0.0, 100.0)}, 10.0);
    check(rounded.size() == 1U && rounded[0].segments.size() == 8U,
          "each sharp corner becomes a curve between two points");
    check(near(geo::contour_length(rounded[0]), 400.0 - 80.0 + 4.0 * std::numbers::pi * 5.0, 0.3),
          "the rounded perimeter is the square's less its corners plus quarter circles");

    // Sweep: the union and intersection of overlapping squares, and their outline.
    const auto polygon = [](const double x, const double y, const double side) {
        return std::vector<ui::Point>{{x, y}, {x + side, y}, {x + side, y + side}, {x, y + side}};
    };
    const std::vector<geo::PolygonSet> sets{geo::PolygonSet{{polygon(0.0, 0.0, 100.0)}, false},
                                            geo::PolygonSet{{polygon(50.0, 50.0, 100.0)}, false}};
    const auto measure = [](const std::vector<geo::SweepLevel>& levels) {
        double total = 0.0;
        for (const geo::SweepLevel& level : levels)
            total += area(geo::trapezoid_polygons(level.trapezoids)) * level.value;
        return total;
    };
    const auto any = [](const std::span<const std::uint8_t> inside) {
        return inside[0] != 0U || inside[1] != 0U ? 1.0 : 0.0;
    };
    check(near(measure(geo::sweep(sets, any)), 17500.0, 1.0e-6), "the union of two squares");
    check(near(measure(geo::sweep(sets,
                                  [](const std::span<const std::uint8_t> inside) {
                                      return inside[0] != 0U && inside[1] != 0U ? 1.0 : 0.0;
                                  })),
               2500.0, 1.0e-6),
          "the intersection of two squares");
    const std::vector<geo::SweepLevel> graded =
        geo::sweep(sets, [](const std::span<const std::uint8_t> inside) {
            return (inside[0] != 0U ? 0.4 : 0.0) + (inside[1] != 0U ? 0.4 : 0.0);
        });
    check(graded.size() == 2U && near(graded[0].value, 102.0 / 255.0, 1.0e-12),
          "regions group by value");
    const std::vector<std::vector<ui::Point>> outline =
        geo::region_outline(geo::sweep(sets, any)[0].trapezoids);
    check(outline.size() == 1U && outline[0].size() == 8U && near(area(outline), 17500.0, 1.0e-6),
          "the union's outline is one loop of eight corners");
    // Crossing edges: a bow tie's two triangles, even-odd and nonzero alike.
    const std::vector<geo::PolygonSet> bow{
        geo::PolygonSet{{{{0.0, 0.0}, {100.0, 100.0}, {100.0, 0.0}, {0.0, 100.0}}}, true}};
    check(near(measure(geo::sweep(bow,
                                  [](const std::span<const std::uint8_t> inside) {
                                      return inside[0] != 0U ? 1.0 : 0.0;
                                  })),
               5000.0, 1.0e-6),
          "the sweep cuts at crossings");
}

[[nodiscard]] std::size_t count_paths(const std::vector<ui::RenderCommand>& commands) {
    return static_cast<std::size_t>(
        std::ranges::count_if(commands, [](const ui::RenderCommand& command) {
            return std::holds_alternative<ui::PathRenderCommand>(command);
        }));
}

void test_frames(const std::filesystem::path& resources) {
    // A plain rectangle: one path under its layer's transform, scaled into the viewport.
    const model::Animation plain = model::parse(document(R"([{"ty":4,"ind":1,"ip":0,"op":60,"st":0,
        "ks":{"p":{"a":0,"k":[100,50]},"s":{"a":1,"k":[{"t":0,"s":[100,100],"o":{"x":[0],"y":[0]},"i":{"x":[1],"y":[1]}},{"t":60,"s":[200,200]}]}},
        "shapes":[{"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[20,10]},"r":{"a":0,"k":0}},
                  {"ty":"fl","c":{"a":0,"k":[1,0,0,1]},"o":{"a":0,"k":50}}]}])"));
    std::vector<ui::RenderCommand> commands;
    geo::append_frame(commands, plain, 30.0, ui::Rect{0.0, 0.0, 400.0, 200.0},
                      ui::Rect{0.0, 0.0, 400.0, 200.0});
    check(count_paths(commands) == 1U, "one fill draws one path");
    const auto transform = std::ranges::find_if(commands, [](const ui::RenderCommand& command) {
        return std::holds_alternative<ui::TransformPushRenderCommand>(command);
    });
    check(transform != commands.end(), "a path draws under its transform");
    const auto& matrix = std::get<ui::TransformPushRenderCommand>(*transform);
    // Viewport scale 2, layer scale 1.5 at frame 30, placed at (100, 50) of the composition.
    check(near(matrix.m00, 3.0, 1.0e-9) && near(matrix.m11, 3.0, 1.0e-9) &&
              near(matrix.m02, 200.0, 1.0e-9) && near(matrix.m12, 100.0, 1.0e-9),
          "the transform is the viewport's after the layer's");
    const auto path = std::ranges::find_if(commands, [](const ui::RenderCommand& command) {
        return std::holds_alternative<ui::PathRenderCommand>(command);
    });
    const auto& fill = std::get<ui::PathRenderCommand>(*path).shape.fill;
    check(fill.has_value() && fill->color() != nullptr && fill->color()->red == 255U &&
              fill->color()->alpha == 128U,
          "a fill keeps its colour and opacity");

    // The same rectangle under a half-opacity subtract mask over its left half: the right half
    // draws whole, the left at half strength, as two regions.
    const model::Animation masked = model::parse(document(R"([{"ty":4,"ind":1,"ip":0,"op":60,"st":0,
        "ks":{},"masksProperties":[{"mode":"s","inv":false,"o":{"a":0,"k":50},"x":{"a":0,"k":0},
          "pt":{"a":0,"k":{"c":true,"v":[[0,0],[50,0],[50,100],[0,100]],"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]]}}}],
        "shapes":[{"ty":"rc","p":{"a":0,"k":[50,50]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0}},
                  {"ty":"fl","c":{"a":0,"k":[1,1,1,1]},"o":{"a":0,"k":100}}]}])"));
    commands.clear();
    geo::append_frame(commands, masked, 0.0, ui::Rect{0.0, 0.0, 200.0, 100.0},
                      ui::Rect{0.0, 0.0, 200.0, 100.0});
    std::vector<std::uint8_t> alphas;
    for (const ui::RenderCommand& command : commands) {
        if (const auto* drawn = std::get_if<ui::PathRenderCommand>(&command))
            alphas.push_back(drawn->shape.fill->color()->alpha);
    }
    std::ranges::sort(alphas);
    check(alphas == std::vector<std::uint8_t>{128U, 255U},
          "a partial mask splits a fill into regions by coverage");

    // The widget's frame arithmetic: segment ends are exclusive.
    check(near(ui::lottie_frame(ui::LottieSegment{0.0, 60.0}, 0.5), 30.0, 1.0e-12) &&
              ui::lottie_frame(ui::LottieSegment{0.0, 60.0}, 1.0) < 60.0 &&
              near(ui::lottie_frame(ui::LottieSegment{30.0, 0.0}, 0.25), 22.5, 1.0e-12),
          "segments map positions to frames, backwards too");

    // Every sample animation loads (its images decoded) and draws its frames.
    const resource::LottieLibrary library([&resources](const std::string_view id) {
        std::ifstream input(resources / std::filesystem::path(std::string(id)), std::ios::binary);
        if (!input.good())
            throw std::invalid_argument("missing resource");
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), {});
    });
    for (const char* const name : {"spinner", "success", "like", "toggle", "reveal", "badge",
                                   "blend", "feather", "matte", "type", "photo", "effects"}) {
        const std::string id = std::string("assets/strata/lottie/") + name + ".json";
        const std::shared_ptr<const model::Animation> sample = library.find(id);
        check(sample != nullptr, "a sample animation does not load: " + library.error(id));
        std::size_t drawn = 0U;
        for (double frame = sample->in_frame; frame < sample->out_frame; frame += 7.0) {
            commands.clear();
            geo::append_frame(commands, *sample, frame, ui::Rect{0.0, 0.0, 300.0, 300.0},
                              ui::Rect{0.0, 0.0, 300.0, 300.0});
            drawn += count_paths(commands) +
                     static_cast<std::size_t>(
                         std::ranges::count_if(commands, [](const ui::RenderCommand& command) {
                             return std::holds_alternative<ui::ImageRenderCommand>(command);
                         }));
        }
        check(drawn > 0U, std::string("a sample animation draws: ") + name);
    }
    check(library.images().size() == 2U && library.image_generation() == 2U,
          "the photo sample's embedded and file images are decoded once");
}

/** The content effects (isolated layers) a frame opens, in order. */
[[nodiscard]] std::vector<ui::EffectState> layers(const std::vector<ui::RenderCommand>& commands) {
    std::vector<ui::EffectState> result;
    for (const ui::RenderCommand& command : commands) {
        if (const auto* push = std::get_if<ui::ContentEffectPushRenderCommand>(&command))
            result.push_back(push->effect);
    }
    return result;
}

void test_layers() {
    // Blend modes, feather, effects, images and text parse into the model.
    const std::string square =
        R"("shapes":[{"ty":"rc","p":{"a":0,"k":[50,50]},"s":{"a":0,"k":[60,60]},"r":{"a":0,"k":0}},{"ty":"rc","p":{"a":0,"k":[70,50]},"s":{"a":0,"k":[60,60]},"r":{"a":0,"k":0}},{"ty":"fl","c":{"a":0,"k":[1,1,1,1]},"o":{"a":0,"k":100}}])";
    const model::Animation animation = model::parse(document(
        R"([{"ty":4,"ind":1,"ip":0,"op":60,"st":0,"bm":1,"ks":{"o":{"a":0,"k":50}},)" + square +
            R"(,"masksProperties":[{"mode":"a","inv":false,"o":{"a":0,"k":100},"x":{"a":0,"k":0},"f":{"a":0,"k":[6,6]},
                "pt":{"a":0,"k":{"c":true,"v":[[0,0],[100,0],[100,100],[0,100]],"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]]}}}],
            "ef":[{"ty":29,"en":1,"ef":[{"ty":0,"v":{"a":0,"k":10}},{"ty":7,"v":{"a":0,"k":2}}]},
                  {"ty":99,"en":1,"ef":[]},
                  {"ty":21,"en":0,"ef":[]}]},
          {"ty":2,"ind":2,"refId":"pixel","ip":0,"op":60,"st":0,"ks":{}},
          {"ty":5,"ind":3,"ip":0,"op":60,"st":0,"ks":{},"t":{"d":{"k":[{"t":0,"s":{"t":"Hi","f":"ArialMT","s":20,"fc":[1,0,0]}},{"t":30,"s":{"t":"Bye","f":"ArialMT","s":20,"fc":[1,0,0]}}]},
            "a":[{"s":{"s":{"a":0,"k":0},"e":{"a":0,"k":50},"o":{"a":0,"k":0},"r":1,"b":3,"sh":2},"a":{"o":{"a":0,"k":0}}}]}}])",
        R"([{"id":"pixel","w":1,"h":1,"e":1,"u":"","p":"data:image/png;base64,iVBORw0KGgo="}])"));
    const model::Layer& shape = animation.root.layers[0];
    check(shape.blend_mode == 1 && near(shape.masks[0].feather.point(0.0).x, 6.0, 1.0e-12),
          "a layer's blend mode and a mask's feather are read");
    check(shape.effects.size() == 2U && shape.effects[0].type == model::effect_gaussian_blur &&
              near(shape.effects[0].scalar(0U, 0.0, 0.0), 10.0, 1.0e-12),
          "enabled effects are kept with their controls, disabled ones dropped");
    check(animation.layered && animation.filter_reach >= 9.0,
          "an animation that blends and blurs is drawn apart, reaching as far as its blur");
    check(animation.images.size() == 1U && animation.images[0].embedded &&
              animation.root.layers[1].image == std::optional<std::size_t>{0U},
          "an image layer names its embedded asset");
    const model::TextData& text = animation.root.layers[2].text;
    check(text.documents.size() == 2U && text.document(10.0)->text == "Hi" &&
              text.document(45.0)->text == "Bye" && text.animators.size() == 1U &&
              text.animators[0].selector.based_on == 3 && text.animators[0].opacity.has_value(),
          "text documents switch at their times and animators keep their selectors");
    const std::optional<std::vector<std::uint8_t>> bytes =
        model::data_uri_bytes("data:image/png;base64,iVBORw0KGgo=");
    check(bytes.has_value() &&
              *bytes == std::vector<std::uint8_t>{0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU},
          "a base64 data URI decodes to its bytes");

    // The frame: the whole animation one layer cut to the box, the blended layer a multiply layer
    // at its opacity carrying its blur (in widget pixels), and its feathered mask a
    // destination-in layer blurred by its feather.
    std::vector<ui::RenderCommand> commands;
    geo::append_frame(commands, animation, 0.0, ui::Rect{0.0, 0.0, 400.0, 200.0},
                      ui::Rect{0.0, 0.0, 400.0, 200.0});
    const std::vector<ui::EffectState> opened = layers(commands);
    check(opened.size() >= 3U && opened[0].composite == ui::LayerComposite::source_over,
          "a blending animation is drawn apart");
    const auto multiply = std::ranges::find_if(opened, [](const ui::EffectState& effect) {
        return effect.composite == ui::LayerComposite::multiply;
    });
    check(multiply != opened.end() && near(multiply->opacity, 0.5, 1.0e-12) &&
              multiply->filters.size() == 1U &&
              multiply->filters[0].kind == ui::LayerFilterKind::blur &&
              near(multiply->filters[0].values[0], 18.0, 1.0e-9) &&
              near(multiply->filters[0].values[1], 0.0, 1.0e-12),
          "a blended layer composites by its mode at its opacity, with its horizontal blur");
    const auto feather = std::ranges::find_if(opened, [](const ui::EffectState& effect) {
        return effect.composite == ui::LayerComposite::destination_in;
    });
    check(feather != opened.end() && feather->filters.size() == 1U &&
              near(feather->filters[0].values[0], 6.0 * 1.14 * 2.0, 1.0e-9),
          "a feathered mask keeps its layer through a blurred coverage layer");

    // Opacity on one paint needs no layer; on two overlapping paints it does.
    const auto opacity_layers = [](const std::string& shapes) {
        const model::Animation faded = model::parse(
            document(R"([{"ty":4,"ind":1,"ip":0,"op":60,"st":0,"ks":{"o":{"a":0,"k":50}},)" +
                     shapes + "}]"));
        std::vector<ui::RenderCommand> output;
        geo::append_frame(output, faded, 0.0, ui::Rect{0.0, 0.0, 200.0, 100.0},
                          ui::Rect{0.0, 0.0, 200.0, 100.0});
        return layers(output).size();
    };
    check(
        opacity_layers(
            R"("shapes":[{"ty":"rc","p":{"a":0,"k":[50,50]},"s":{"a":0,"k":[60,60]},"r":{"a":0,"k":0}},{"ty":"fl","c":{"a":0,"k":[1,1,1,1]},"o":{"a":0,"k":100}}])") ==
            0U,
        "one faded paint draws in place");
    check(
        opacity_layers(
            R"("shapes":[{"ty":"gr","it":[{"ty":"rc","p":{"a":0,"k":[50,50]},"s":{"a":0,"k":[60,60]},"r":{"a":0,"k":0}},{"ty":"fl","c":{"a":0,"k":[1,1,1,1]},"o":{"a":0,"k":100}}]},{"ty":"rc","p":{"a":0,"k":[70,50]},"s":{"a":0,"k":[60,60]},"r":{"a":0,"k":0}},{"ty":"fl","c":{"a":0,"k":[1,0,0,1]},"o":{"a":0,"k":100}}])") ==
            1U,
        "overlapping faded paints fade as one layer");
}

} // namespace

int strata_test_lottie(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count != 2)
            throw std::runtime_error("usage: lottie RESOURCE_ROOT");
        test_parsing();
        test_easing();
        test_keyframes();
        test_geometry();
        test_frames(arguments[1]);
        test_layers();
        std::cout << "strata_lottie_tests: OK\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "strata_lottie_tests: " << exception.what() << '\n';
        return 1;
    }
}
