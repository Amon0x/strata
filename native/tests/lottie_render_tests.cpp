// Lottie frames rendered end to end: a .strata module showing Lottie widgets is drawn through the
// public host by every backend this build has, and each animation's frame is measured from the
// framebuffer. White shapes on black make each pixel's red its coverage, so the summed coverage of
// a cell is the area the frame covers there: a wrong keyframe, trim, mask or matte shows as area.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "../headless/application_host.hpp"
#include "../headless/capture_renderer.hpp"
#include "../headless/png.hpp"
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

/** A pixel of a case's cell whose red channel (0-1) must fall within [low, high]. */
struct Probe final {
    double x = 0.0;
    double y = 0.0;
    double low = 0.0;
    double high = 1.0;
};

/**
 * One measured animation: its document, the progress shown and the area it should cover. With a
 * `relative` case, the area is that case's measured area times `area`.
 */
struct Case final {
    std::string name;
    std::string json;
    double progress = 0.5;
    double area = 0.0;
    double tolerance = 0.01;
    std::vector<Probe> probes{};
    int relative = -1;
};

[[nodiscard]] std::string fill_color(const double grey, const double opacity = 100.0) {
    return R"({"ty":"fl","c":{"a":0,"k":[)" + std::to_string(grey) + "," + std::to_string(grey) +
           "," + std::to_string(grey) + R"(,1]},"o":{"a":0,"k":)" + std::to_string(opacity) + "}}";
}

/** An effect (`ef`) of `type` whose controls hold `values` (each a number or an array). */
[[nodiscard]] std::string effect(const int type, const std::vector<std::string>& values) {
    std::string controls;
    for (const std::string& entry : values) {
        if (!controls.empty())
            controls += ",";
        controls += R"({"ty":0,"v":{"a":0,"k":)" + entry + "}}";
    }
    return R"(,"ef":[{"ty":)" + std::to_string(type) + R"(,"en":1,"ef":[)" + controls + "]}]";
}

/** A text layer showing `text` in `font` at `size`, white, with `animators`. */
[[nodiscard]] std::string text_layer(const std::string& text, const std::string& font,
                                     const double size, const std::string& animators = "[]",
                                     const std::string& ks = R"({"p":{"a":0,"k":[20,120]}})") {
    return R"({"ty":5,"ind":1,"ip":0,"op":60,"st":0,"ks":)" + ks +
           R"(,"t":{"d":{"k":[{"t":0,"s":{"t":")" + text + R"(","f":")" + font + R"(","s":)" +
           std::to_string(size) + R"(,"fc":[1,1,1],"j":0,"tr":0,"lh":)" +
           std::to_string(size * 1.2) +
           R"(,"ls":0}}]},"p":{},"m":{"g":1,"a":{"a":0,"k":[0,0]}},"a":)" + animators + "}}";
}

/** Base64 of `bytes`, for a data URI. */
[[nodiscard]] std::string base64(const std::vector<std::uint8_t>& bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t index = 0U; index < bytes.size(); index += 3U) {
        const std::uint32_t chunk =
            (static_cast<std::uint32_t>(bytes[index]) << 16U) |
            (index + 1U < bytes.size() ? static_cast<std::uint32_t>(bytes[index + 1U]) << 8U : 0U) |
            (index + 2U < bytes.size() ? static_cast<std::uint32_t>(bytes[index + 2U]) : 0U);
        result.push_back(alphabet[(chunk >> 18U) & 63U]);
        result.push_back(alphabet[(chunk >> 12U) & 63U]);
        result.push_back(index + 1U < bytes.size() ? alphabet[(chunk >> 6U) & 63U] : '=');
        result.push_back(index + 2U < bytes.size() ? alphabet[chunk & 63U] : '=');
    }
    return result;
}

/** An 8 x 8 opaque white PNG, written beside the cases and read back. */
[[nodiscard]] std::vector<std::uint8_t> white_png(const std::filesystem::path& path) {
    const std::vector<std::uint8_t> pixels(8U * 8U * 4U, 255U);
    std::filesystem::create_directories(path.parent_path());
    headless::write_png(path, 8U, 8U, pixels);
    std::ifstream input(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), {});
}

[[nodiscard]] std::vector<Case> cases(const std::filesystem::path& root) {
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

    // Layer opacity fades the layer as one picture: two overlapping 100 squares at half opacity
    // cover their union at half, not their overlap at three quarters.
    result.push_back(Case{
        "group opacity",
        composition(shape_layer(
            1, rect(70, 100, 100, 100) + "," + white + "," + rect(130, 100, 100, 100) + "," + white,
            {}, R"({"o":{"a":0,"k":50}})")),
        0.5,
        16000.0 * 128.0 / 255.0,
        0.01,
        {Probe{100, 100, 0.47, 0.53}}});
    // Blend modes: a grey 100 square over a grey 160 square, both half grey.
    const std::string under = shape_layer(2, big + "," + fill_color(0.5));
    const auto blended = [&](const int mode) {
        return composition(shape_layer(1, rect(100, 100, 100, 100) + "," + fill_color(0.5),
                                       R"(,"bm":)" + std::to_string(mode)) +
                           "," + under);
    };
    const double half_grey = 128.0 / 255.0;
    result.push_back(Case{"multiply blend",
                          blended(1),
                          0.5,
                          (25600.0 - 10000.0) * half_grey + 10000.0 * half_grey * half_grey,
                          0.015,
                          {Probe{100, 100, 0.22, 0.28}}});
    result.push_back(
        Case{"screen blend",
             blended(2),
             0.5,
             (25600.0 - 10000.0) * half_grey + 10000.0 * (2.0 * half_grey - half_grey * half_grey),
             0.015,
             {Probe{100, 100, 0.72, 0.78}}});
    result.push_back(
        Case{"add blend",
             blended(16),
             0.5,
             (25600.0 - 10000.0) * half_grey + 10000.0 * std::min(1.0, 2.0 * half_grey),
             0.015,
             {Probe{100, 100, 0.97, 1.0}}});
    result.push_back(
        Case{"difference blend",
             composition(shape_layer(1, rect(100, 100, 100, 100) + "," + white, R"(,"bm":10)") +
                         "," + under),
             0.5,
             (25600.0 - 10000.0) * half_grey + 10000.0 * (1.0 - half_grey),
             0.015,
             {Probe{100, 100, 0.47, 0.53}}});
    // Layers nested past where Lottie used to draw them in place: eight precompositions, each a
    // screen layer (over nothing it shows as it is) holding the next, around a matte and a
    // multiply. Every level isolates, and the innermost keeps its matte and its blend.
    const auto nested = [&](const std::string& innermost) {
        std::string assets = "[";
        for (int level = 1; level <= 8; ++level) {
            const std::string layers =
                level < 8 ? R"({"ty":0,"ind":1,"refId":"p)" + std::to_string(level + 1) +
                                R"(","w":200,"h":200,"ip":0,"op":60,"st":0,"ks":{},"bm":2})"
                          : innermost;
            assets += (level > 1 ? "," : "") + std::string(R"({"id":"p)") +
                      std::to_string(level) + R"(","layers":[)" + layers + "]}";
        }
        return composition(
            R"({"ty":0,"ind":1,"refId":"p1","w":200,"h":200,"ip":0,"op":60,"st":0,"ks":{},"bm":2})",
            assets + "]");
    };
    result.push_back(
        Case{"deeply nested alpha matte",
             nested(matte_disc + "," + shape_layer(2, big + "," + white, R"(,"tt":1)")), 0.5,
             disc});
    result.push_back(Case{"deeply nested multiply blend",
                          nested(shape_layer(1, rect(100, 100, 100, 100) + "," + fill_color(0.5),
                                             R"(,"bm":1)") +
                                 "," + under),
                          0.5,
                          (25600.0 - 10000.0) * half_grey + 10000.0 * half_grey * half_grey,
                          0.015,
                          {Probe{100, 100, 0.22, 0.28}}});
    // A feathered mask: the disc's area stays, its edge ramps (half way on the outline).
    const auto feathered = [&](const bool inverted) {
        std::string result_mask = mask("a");
        result_mask.replace(result_mask.find(R"("x":{"a":0,"k":0})"), 17,
                            R"("x":{"a":0,"k":0},"f":{"a":0,"k":[20,20]})");
        if (inverted)
            result_mask.replace(result_mask.find(R"("inv":false)"), 11, R"("inv":true)");
        return result_mask;
    };
    result.push_back(
        Case{"feathered mask",
             composition(shape_layer(1, big + "," + white,
                                     R"(,"masksProperties":[)" + feathered(false) + "]")),
             0.5,
             disc,
             0.03,
             {Probe{100, 100, 0.97, 1.0}, Probe{150, 100, 0.35, 0.65}, Probe{138, 100, 0.65, 0.97},
              Probe{162, 100, 0.03, 0.35}}});
    result.push_back(
        Case{"inverted feathered mask",
             composition(shape_layer(1, big + "," + white,
                                     R"(,"masksProperties":[)" + feathered(true) + "]")),
             0.5,
             square - disc,
             0.03,
             {Probe{100, 100, 0.0, 0.03}, Probe{150, 100, 0.35, 0.65}, Probe{25, 25, 0.97, 1.0}}});
    // A luma matte whose shape is a gradient from white (left) to black (right): what shows of a
    // white square follows the gradient, not its mean.
    const std::string gradient_matte = shape_layer(
        1,
        big +
            R"(,{"ty":"gf","o":{"a":0,"k":100},"r":1,"s":{"a":0,"k":[20,100]},"e":{"a":0,"k":[180,100]},"t":1,"g":{"p":2,"k":{"a":0,"k":[0,1,1,1,1,0,0,0]}}})",
        R"(,"td":1)");
    result.push_back(Case{
        "gradient luma matte",
        composition(gradient_matte + "," + shape_layer(2, big + "," + white, R"(,"tt":3)")),
        0.5,
        square * 0.5,
        0.02,
        {Probe{30, 100, 0.85, 0.97}, Probe{100, 100, 0.42, 0.58}, Probe{170, 100, 0.03, 0.15}}});
    // Effects.
    const std::string plain = rect(100, 100, 100, 100) + "," + white;
    result.push_back(
        Case{"fill effect",
             composition(shape_layer(
                 1, plain, effect(21, {"0", "0", "[0.5,0.5,0.5,1]", "0", "0", "0", "1"}))),
             0.5, 10000.0 * half_grey, 0.015});
    result.push_back(Case{
        "tint effect",
        composition(shape_layer(1, plain, effect(20, {"[0,0,0,1]", "[0.25,0.25,0.25,1]", "100"}))),
        0.5, 10000.0 * 64.0 / 255.0, 0.02});
    result.push_back(
        Case{"tritone effect",
             composition(shape_layer(
                 1, plain, effect(23, {"[0.5,0.5,0.5,1]", "[0.2,0.2,0.2,1]", "[0,0,0,1]", "0"}))),
             0.5, 10000.0 * half_grey, 0.015});
    std::vector<std::string> levels(36U, "0");
    for (const std::size_t first : {3U, 10U, 17U, 24U, 31U}) {
        levels[first] = "0";
        levels[first + 1U] = "1";
        levels[first + 2U] = "1";
        levels[first + 3U] = "0";
        levels[first + 4U] = "1";
    }
    levels[7U] = "0.5";
    result.push_back(Case{"levels effect", composition(shape_layer(1, plain, effect(24, levels))),
                          0.5, 10000.0 * half_grey, 0.015});
    result.push_back(Case{
        "gaussian blur effect",
        composition(shape_layer(1, plain, effect(29, {"10", "1", "0"}))),
        0.5,
        10000.0,
        0.02,
        {Probe{100, 100, 0.97, 1.0}, Probe{150, 100, 0.35, 0.65}, Probe{100, 150, 0.35, 0.65}}});
    result.push_back(Case{"horizontal blur effect",
                          composition(shape_layer(1, plain, effect(29, {"10", "2", "0"}))),
                          0.5,
                          10000.0,
                          0.02,
                          {Probe{150, 100, 0.35, 0.65}, Probe{100, 149.5, 0.97, 1.0}}});
    // A 60 square's shadow 40 to its right: their union.
    result.push_back(
        Case{"drop shadow effect",
             composition(shape_layer(1, rect(70, 100, 60, 60) + "," + white,
                                     effect(25, {"[1,1,1,1]", "255", "90", "40", "0"}))),
             0.5,
             6000.0,
             0.02,
             {Probe{120, 100, 0.97, 1.0}, Probe{140, 140, 0.0, 0.03}}});
    // An image layer: an 8 x 8 white PNG drawn 100 square, embedded and as a file beside it.
    const std::vector<std::uint8_t> png = white_png(root / "assets/lottie-tests/images/white.png");
    const std::string image_layer =
        R"({"ty":2,"ind":1,"refId":"image","ip":0,"op":60,"st":0,"ks":{"p":{"a":0,"k":[50,50]}}})";
    result.push_back(Case{
        "embedded image",
        composition(image_layer,
                    R"([{"id":"image","w":100,"h":100,"e":1,"u":"","p":"data:image/png;base64,)" +
                        base64(png) + R"("}])"),
        0.5, 10000.0, 0.02});
    result.push_back(
        Case{"image file",
             composition(image_layer,
                         R"([{"id":"image","w":100,"h":100,"e":0,"u":"images/","p":"white.png"}])"),
             0.5, 10000.0, 0.02});
    // A masked image: the disc of a 160 white image.
    result.push_back(Case{
        "masked image",
        composition(
            R"({"ty":2,"ind":1,"refId":"image","ip":0,"op":60,"st":0,"ks":{"p":{"a":0,"k":[20,20]}},"masksProperties":[)" +
                mask("a") + R"(]})",
            R"([{"id":"image","w":160,"h":160,"e":0,"u":"images/","p":"white.png"}])"),
        0.5, disc, 0.02});
    // Text from exported glyphs: an "A" that is a 50 square at size 100, twice.
    const std::string glyph_font =
        R"("fonts":{"list":[{"fName":"Box","fFamily":"Box","fStyle":"Regular","ascent":75}]},"chars":[{"ch":"A","size":100,"style":"Regular","w":60,"fFamily":"Box","data":{"shapes":[{"ty":"gr","it":[{"ty":"sh","ks":{"a":0,"k":{"c":true,"v":[[0,-50],[50,-50],[50,0],[0,0]],"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]]}}}]}]}}],)";
    const auto with_glyphs = [&](const std::string& layers) {
        std::string document = composition(layers);
        document.insert(1U, glyph_font);
        return document;
    };
    result.push_back(
        Case{"text from glyphs", with_glyphs(text_layer("AA", "Box", 100.0)), 0.5, 5000.0, 0.01});
    // A range selector over the first half of the letters with opacity 0: one letter is left.
    result.push_back(Case{
        "text animator",
        with_glyphs(text_layer(
            "AA", "Box", 100.0,
            R"([{"s":{"t":0,"xe":{"a":0,"k":0},"ne":{"a":0,"k":0},"a":{"a":0,"k":100},"b":1,"rn":0,"sh":1,"s":{"a":0,"k":0},"e":{"a":0,"k":50},"o":{"a":0,"k":0},"r":1},"a":{"o":{"a":0,"k":0}}}])")),
        0.5,
        2500.0,
        0.01,
        {Probe{45, 95, 0.0, 0.03}, Probe{105, 95, 0.97, 1.0}}});
    // Text in a system font: Arial's "IIII" covers some area; an animator hiding half of it
    // leaves half.
    const std::string arial =
        R"("fonts":{"list":[{"fName":"ArialMT","fFamily":"Arial","fStyle":"Regular","ascent":72}]},)";
    const auto with_arial = [&](const std::string& layers) {
        std::string document = composition(layers);
        document.insert(1U, arial);
        return document;
    };
    const int system_text = static_cast<int>(result.size());
    result.push_back(
        Case{"system font text", with_arial(text_layer("IIII", "ArialMT", 80.0)), 0.5, 0.0, 0.0});
    result.push_back(Case{
        "system font text animator",
        with_arial(text_layer(
            "IIII", "ArialMT", 80.0,
            R"([{"s":{"t":0,"a":{"a":0,"k":100},"b":1,"sh":1,"s":{"a":0,"k":0},"e":{"a":0,"k":50},"o":{"a":0,"k":0},"r":1},"a":{"o":{"a":0,"k":0}}}])")),
        0.5,
        0.5,
        0.03,
        {},
        system_text});
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
    const std::vector<Case> all = cases(root);
    const int rows = static_cast<int>((all.size() + columns - 1U) / columns);
    std::string module = "screen Frames {\n  root Panel(style: { background: null, border: null }, "
                         "layout: { kind: \"PANEL\", width: " +
                         std::to_string(cell * columns) +
                         ", height: " + std::to_string(cell * rows) + " }) {\n";
    for (std::size_t index = 0U; index < all.size(); ++index) {
        const std::string file = "assets/lottie-tests/case" + std::to_string(index) + ".json";
        write(root / file, all[index].json);
        module += "    Lottie(source: \"" + file +
                  "\", playing: false, progress: " + std::to_string(all[index].progress) +
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
    // Text in a system font is drawn once the system's font names are read, on a worker, as a
    // page's text waits for a web font: frame until it shows (paused, nothing else moves).
    const auto system_text = std::ranges::find(all, std::string_view("system font text"),
                                               [](const Case& entry) { return entry.name; });
    if (system_text != all.end()) {
        const auto index = static_cast<std::size_t>(system_text - all.begin());
        const double left = cell * static_cast<double>(index % columns);
        const double top = cell * static_cast<double>(index / columns);
        std::int64_t now = 16'666'667;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (covered(host, left, top, cell, cell) < 100.0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            host.frame(now += 16'666'667);
        }
    }
    std::string failures;
    std::vector<double> areas;
    for (std::size_t index = 0U; index < all.size(); ++index) {
        const Case& entry = all[index];
        const double left = cell * static_cast<double>(index % columns);
        const double top = cell * static_cast<double>(index / columns);
        const double area = covered(host, left, top, cell, cell);
        areas.push_back(area);
        if (entry.relative < 0 && entry.area == 0.0) {
            // Only drawn: something must show, and later cases measure against it.
            std::cout << "  " << backend << " " << entry.name << ": area " << area << "\n";
            if (area < 100.0)
                failures += "\n  " + backend + ": " + entry.name + " drew nothing";
            continue;
        }
        const double expected = entry.relative >= 0
                                    ? areas[static_cast<std::size_t>(entry.relative)] * entry.area
                                    : entry.area;
        const double error = std::abs(area - expected) / expected;
        std::cout << "  " << backend << " " << entry.name << ": area " << area << " of " << expected
                  << " (" << error * 100.0 << "%)\n";
        if (error > entry.tolerance) {
            failures += "\n  " + backend + ": " + entry.name + " covers " + std::to_string(area) +
                        " instead of " + std::to_string(expected);
        }
        for (const Probe& probe : entry.probes) {
            const double value = covered(host, left + probe.x, top + probe.y, 1.0, 1.0);
            if (value < probe.low || value > probe.high) {
                failures += "\n  " + backend + ": " + entry.name + " is " + std::to_string(value) +
                            " at (" + std::to_string(probe.x) + ", " + std::to_string(probe.y) +
                            "), not within [" + std::to_string(probe.low) + ", " +
                            std::to_string(probe.high) + "]";
            }
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
