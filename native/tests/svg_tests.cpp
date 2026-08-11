#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include <strata/svg.hpp>

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

template <typename Function>
void check_throws(Function&& operation, const std::string_view message) {
    try {
        operation();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

[[nodiscard]] bool near(const double left, const double right, const double tolerance = 1.0e-9) {
    return std::abs(left - right) <= tolerance;
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("could not read SVG fixture: " + path.string());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

[[nodiscard]] strata::svg::Document parse_fixture(const std::filesystem::path& fixtures,
                                                  const std::string_view name) {
    return strata::svg::parse(read_file(fixtures / std::string(name)));
}

void test_lucide_parsing(const std::filesystem::path& fixtures) {
    const strata::svg::Document activity = parse_fixture(fixtures, "lucide-activity.svg");
    check(activity.width == 24.0 && activity.height == 24.0, "Lucide viewport size changed");
    check(activity.view_box == strata::svg::ViewBox{0.0, 0.0, 24.0, 24.0},
          "Lucide viewBox changed");
    check(activity.commands.size() == 1U, "Lucide Activity draw count changed");
    const strata::svg::PaintStyle& paint = activity.commands.front().paint;
    check(!paint.has_fill && paint.has_stroke, "Lucide fill/stroke inheritance failed");
    check(paint.stroke_width == 2.0, "Lucide stroke width inheritance failed");
    check(paint.line_cap == strata::svg::LineCap::round, "Lucide round cap was lost");
    check(paint.line_join == strata::svg::LineJoin::round, "Lucide round join was lost");

    const strata::svg::Document heart = parse_fixture(fixtures, "lucide-heart.svg");
    check(heart.commands.size() == 1U, "Lucide Heart draw count changed");
    check(std::ranges::any_of(
              heart.commands.front().path.segments,
              [](const auto& segment) { return segment.verb == strata::svg::PathVerb::cubic; }),
          "relative Lucide cubic commands were not normalized");
    check(heart.commands.front().path.segments.back().verb == strata::svg::PathVerb::close,
          "Lucide close command was not retained");

    const strata::svg::Document check_icon = parse_fixture(fixtures, "lucide-circle-check-big.svg");
    check(check_icon.commands.size() == 2U, "Lucide Circle Check draw count changed");
    const std::size_t cubic_count = static_cast<std::size_t>(
        std::ranges::count_if(check_icon.commands.front().path.segments, [](const auto& segment) {
            return segment.verb == strata::svg::PathVerb::cubic;
        }));
    check(cubic_count >= 4U, "Lucide elliptical arc was not converted to cubic curves");
}

void test_shapes_transforms_and_paint() {
    strata::svg::ParseOptions options;
    options.current_color = strata::svg::Color{12U, 34U, 56U, 255U};
    const strata::svg::Document document = strata::svg::parse(R"SVG(
<svg width="100" height="100" viewBox="0 0 100 100" fill="none" stroke="currentColor">
  <g transform="translate(1 2) scale(2)" stroke-width="3" stroke-linecap="square">
    <rect x="1" y="2" width="20" height="10" rx="2"/>
    <circle cx="10" cy="10" r="4"/>
    <ellipse cx="10" cy="10" rx="5" ry="2"/>
    <line x1="0" y1="0" x2="4" y2="5"/>
    <polyline points="0,0 1,2 3,4"/>
    <polygon points="0,0 4,0 2,3" fill="#12345678"/>
    <path d="M1 1 Q2 3 4 1 T8 1 C9 1 9 2 10 2 S11 3 12 2 A3 2 20 0 1 18 4 Z"/>
  </g>
</svg>)SVG",
                                                              options);
    check(document.commands.size() == 7U, "static SVG shape coverage changed");
    const strata::svg::Point transformed = document.commands.front().transform.apply({1.0, 1.0});
    check(near(transformed.x, 3.0) && near(transformed.y, 4.0), "SVG transform list order changed");
    check(document.commands.front().paint.stroke == options.current_color &&
              document.commands.front().paint.stroke_uses_current_color,
          "currentColor did not resolve statically or retain its semantic marker");
    check(document.commands.front().paint.stroke_width == 3.0,
          "group stroke width did not inherit");
    check(document.commands[5].paint.has_fill &&
              document.commands[5].paint.fill == strata::svg::Color{18U, 52U, 86U, 120U},
          "eight-digit static hex paint changed");
}

void test_view_box_mapping_and_fill_rules() {
    const auto centered =
        strata::svg::rasterize(strata::svg::parse(R"SVG(
<svg width="20" height="10" viewBox="0 0 10 10"><rect width="10" height="10"/></svg>)SVG"),
                               strata::svg::RasterOptions{20U, 10U, 4U, {0U, 0U, 0U, 0U}});
    check(centered.pixel(2U, 5U)[3] == 0U && centered.pixel(10U, 5U)[3] == 255U,
          "default xMidYMid meet mapping changed");

    const auto stretched =
        strata::svg::rasterize(strata::svg::parse(R"SVG(
<svg width="20" height="10" viewBox="0 0 10 10" preserveAspectRatio="none">
  <rect width="10" height="10"/>
</svg>)SVG"),
                               strata::svg::RasterOptions{20U, 10U, 4U, {0U, 0U, 0U, 0U}});
    check(stretched.pixel(2U, 5U)[3] == 255U && stretched.pixel(18U, 5U)[3] == 255U,
          "preserveAspectRatio none mapping changed");

    const auto evenodd =
        strata::svg::rasterize(strata::svg::parse(R"SVG(
<svg width="10" height="10"><path fill-rule="evenodd"
  d="M1 1h8v8H1z M3 3h4v4H3z"/></svg>)SVG"),
                               strata::svg::RasterOptions{10U, 10U, 4U, {0U, 0U, 0U, 0U}});
    check(evenodd.pixel(2U, 2U)[3] == 255U && evenodd.pixel(5U, 5U)[3] == 0U,
          "evenodd fill rule changed");

    const auto nonzero =
        strata::svg::rasterize(strata::svg::parse(R"SVG(
<svg width="10" height="10"><path fill-rule="nonzero"
  d="M1 1h8v8H1z M3 3h4v4H3z"/></svg>)SVG"),
                               strata::svg::RasterOptions{10U, 10U, 4U, {0U, 0U, 0U, 0U}});
    check(nonzero.pixel(5U, 5U)[3] == 255U, "nonzero fill rule changed");
}

void test_compositing_and_encoding() {
    const strata::svg::Document document = strata::svg::parse(R"SVG(
<svg width="8" height="8">
  <rect x="1" y="1" width="6" height="6" fill="#ff000080"/>
  <rect x="3" y="3" width="4" height="4" fill="#0000ff80"/>
</svg>)SVG");
    const strata::svg::RasterOptions options{8U, 8U, 4U, {0U, 0U, 0U, 0U}};
    const strata::svg::Image first = strata::svg::rasterize(document, options);
    const strata::svg::Image second = strata::svg::rasterize(document, options);
    check(first.rgba == second.rgba, "SVG raster output is not deterministic");
    const auto red = first.pixel(2U, 2U);
    check(red[0] == 255U && red[2] == 0U && red[3] == 128U, "straight-alpha SVG color changed");
    const auto overlap = first.pixel(4U, 4U);
    check(overlap[0] > 70U && overlap[2] > 160U && overlap[3] > 190U,
          "SVG source-over compositing changed");
    const std::vector<std::uint8_t> pam = strata::svg::encode_pam(first);
    const std::string_view prefix(reinterpret_cast<const char*>(pam.data()),
                                  std::min<std::size_t>(pam.size(), 80U));
    check(prefix.starts_with("P7\nWIDTH 8\nHEIGHT 8\nDEPTH 4\n"), "SVG PAM header changed");
}

struct AlphaBounds final {
    std::uint32_t left = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t top = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t right = 0U;
    std::uint32_t bottom = 0U;
    std::uint64_t sum = 0U;
};

[[nodiscard]] AlphaBounds alpha_bounds(const strata::svg::Image& image) {
    AlphaBounds result;
    for (std::uint32_t y = 0U; y < image.height; ++y) {
        for (std::uint32_t x = 0U; x < image.width; ++x) {
            const std::uint8_t alpha = image.pixel(x, y)[3];
            result.sum += alpha;
            if (alpha == 0U)
                continue;
            result.left = std::min(result.left, x);
            result.top = std::min(result.top, y);
            result.right = std::max(result.right, x + 1U);
            result.bottom = std::max(result.bottom, y + 1U);
        }
    }
    return result;
}

void test_lucide_reference_coverage(const std::filesystem::path& fixtures) {
    struct Reference final {
        std::string_view file;
        std::uint64_t resvg_alpha_sum;
        std::array<std::uint32_t, 4U> bounds;
    };
    // Independent reference values were produced by resvg 2.6.2 at 96x96.
    constexpr std::array references{
        Reference{"lucide-activity.svg", 372041U, {4U, 8U, 92U, 88U}},
        Reference{"lucide-heart.svg", 486224U, {4U, 8U, 92U, 88U}},
        Reference{"lucide-circle-check-big.svg", 616670U, {4U, 4U, 92U, 92U}},
    };
    for (const Reference& reference : references) {
        const strata::svg::Document document = parse_fixture(fixtures, reference.file);
        const strata::svg::Image image = strata::svg::rasterize(
            document, strata::svg::RasterOptions{96U, 96U, 4U, {0U, 0U, 0U, 0U}});
        const AlphaBounds actual = alpha_bounds(image);
        check(std::array{actual.left, actual.top, actual.right, actual.bottom} == reference.bounds,
              "Lucide raster bounds differ from the independent renderer");
        const double ratio =
            static_cast<double>(actual.sum) / static_cast<double>(reference.resvg_alpha_sum);
        check(ratio > 0.98 && ratio < 1.02,
              "Lucide raster coverage differs from the independent renderer");
    }
}

void test_active_and_ambiguous_content_is_rejected() {
    constexpr std::array forbidden{
        std::string_view{"<!DOCTYPE svg [<!ENTITY x SYSTEM 'file:///secret'>]><svg>&x;</svg>"},
        std::string_view{"<svg width='1' height='1'><script/></svg>"},
        std::string_view{"<?xml-stylesheet href='theme.css'?><svg width='1' height='1'/>"},
        std::string_view{"<svg width='1' height='1'><style>path{fill:red}</style></svg>"},
        std::string_view{"<svg width='1' height='1'><title><script/></title></svg>"},
        std::string_view{"<svg width='1' height='1' onload='run()'/>"},
        std::string_view{"<svg width='1' height='1'><path d='M0 0' style='fill:red'/></svg>"},
        std::string_view{"<svg width='1' height='1'><path d='M0 0' class='red'/></svg>"},
        std::string_view{"<svg width='1' height='1'><image href='https://example.test/x'/></svg>"},
        std::string_view{"<svg width='1' height='1'><use href='#x'/></svg>"},
        std::string_view{"<svg width='1' height='1'><animate/></svg>"},
        std::string_view{"<svg width='1' height='1'><foreignObject/></svg>"},
        std::string_view{
            "<svg width='1' height='1'><g opacity='.5'><rect width='1' height='1'/></g></svg>"},
        std::string_view{"<svg width='1' height='1'><path d='M0 0L1 0L1 1Z' fill='red' "
                         "stroke='blue' opacity='.5'/></svg>"},
        std::string_view{"<svg width='1' height='1'><path d='M0 0' fill='url(#paint)'/></svg>"},
        std::string_view{"<svg width='1' width='2' height='1'/>"},
        std::string_view{"<svg width='1' height='1'><g></svg>"},
        std::string_view{"<svg width='1' height='1'><path d='M0 0A1 1 0 2 0 1 1'/></svg>"},
        std::string_view{"<svg width='1e999' height='1'/>"},
        std::string_view{"<svg width='1' height='1'><path transform='scale(1e308) "
                         "scale(1e308)' d='M0 0L1 1'/></svg>"},
        std::string_view{
            "<svg width='1' height='1'><path d='M1e308 0 A1 1 0 0 0 -1e308 0'/></svg>"},
        std::string_view{"<svg width='1' height='1'><path d='M1e308 0 l1e308 0'/></svg>"},
        std::string_view{"<svg width='1' height='1'><path d='M-1e308 0 L1e308 0'/></svg>"},
        std::string_view{
            "<svg width='1' height='1' viewBox='1e308 0 1e308 1'><path d='M0 0L1 1'/></svg>"},
        std::string_view{"<svg width='1' height='1'><path d='M0 0L1 1' fill='none' "
                         "stroke='red' stroke-width='1e308' "
                         "stroke-miterlimit='1e308'/></svg>"},
    };
    for (const std::string_view source : forbidden) {
        check_throws([&] { static_cast<void>(strata::svg::parse(source)); },
                     "active, ambiguous, or non-finite SVG content was accepted");
    }
    check_throws(
        [] {
            static_cast<void>(
                strata::svg::parse("<svg width='1' height='1'><path d='L1 1'/></svg>"));
        },
        "SVG path without an initial move was accepted");
}

void test_limits() {
    strata::svg::ParseOptions bytes;
    bytes.limits.maximum_input_bytes = 8U;
    check_throws(
        [&] { static_cast<void>(strata::svg::parse("<svg width='1' height='1'/>", bytes)); },
        "SVG byte limit was ignored");

    strata::svg::ParseOptions elements;
    elements.limits.maximum_elements = 1U;
    check_throws(
        [&] {
            static_cast<void>(
                strata::svg::parse("<svg width='1' height='1'><path d='M0 0'/></svg>", elements));
        },
        "SVG element limit was ignored");

    strata::svg::ParseOptions segments;
    segments.limits.maximum_path_segments = 2U;
    check_throws(
        [&] {
            static_cast<void>(strata::svg::parse(
                "<svg width='4' height='4'><path d='M0 0L1 1L2 2'/></svg>", segments));
        },
        "SVG path segment limit was ignored");

    check_throws(
        [] {
            const auto document =
                strata::svg::parse("<svg width='1' height='1'><rect width='1' height='1'/></svg>");
            static_cast<void>(
                strata::svg::rasterize(document, strata::svg::RasterOptions{9000U, 1U, 1U, {}}));
        },
        "SVG raster dimension limit was ignored");

    check_throws(
        [] {
            strata::svg::Document document;
            document.width = 128.0;
            document.height = 128.0;
            document.view_box = {0.0, 0.0, 128.0, 128.0};
            strata::svg::DrawCommand command;
            command.paint.has_fill = false;
            command.paint.has_stroke = true;
            command.path.segments.push_back({strata::svg::PathVerb::move, {}, {}, {0.0, 0.0}});
            for (std::size_t index = 0U; index < 2000U; ++index) {
                command.path.segments.push_back({strata::svg::PathVerb::line,
                                                 {},
                                                 {},
                                                 {static_cast<double>(index % 128U),
                                                  static_cast<double>((index * 67U) % 128U)}});
            }
            document.commands.push_back(std::move(command));
            static_cast<void>(
                strata::svg::rasterize(document, strata::svg::RasterOptions{128U, 128U, 4U, {}}));
        },
        "SVG geometric work limit was ignored");

    check_throws(
        [] {
            strata::svg::Document document;
            document.width = 1.0;
            document.height = 1.0;
            document.view_box = {0.0, 0.0, 1.0, 1.0};
            strata::svg::DrawCommand command;
            command.paint.has_fill = true;
            command.path.segments.push_back(
                {strata::svg::PathVerb::move, {}, {}, {0.0, 0.0}});
            for (std::size_t point = 0U; point < 1'100'000U; ++point) {
                command.path.segments.push_back({
                    strata::svg::PathVerb::line,
                    {},
                    {},
                    {static_cast<double>(point % 2U),
                     static_cast<double>((point / 2U) % 2U)},
                });
            }
            document.commands.push_back(std::move(command));
            static_cast<void>(
                strata::svg::rasterize(document, strata::svg::RasterOptions{1U, 1U, 1U, {}}));
        },
        "aggregate SVG flattened-point limit was ignored");

    check_throws(
        [] {
            const strata::svg::Image malformed{1U, 1U, {}};
            static_cast<void>(malformed.pixel(0U, 0U));
        },
        "malformed SVG image storage was accessed");
}

void test_truncated_and_arbitrary_inputs(const std::filesystem::path& fixtures) {
    check_throws(
        [] {
            const std::string invalid = "<svg width='1' height='1'><title>" +
                                        std::string("\xC0\xAF", 2U) + "</title></svg>";
            static_cast<void>(strata::svg::parse(invalid));
        },
        "invalid UTF-8 XML text was accepted");
    check_throws(
        [] {
            static_cast<void>(
                strata::svg::parse(std::string("<svg width='1' height='1'><metadata>") + '\x01' +
                                   "</metadata></svg>"));
        },
        "forbidden XML control text was accepted");
    for (const std::string_view file :
         {"lucide-activity.svg", "lucide-heart.svg", "lucide-circle-check-big.svg"}) {
        const std::string source = read_file(fixtures / std::string(file));
        const std::size_t complete_length = source.rfind("</svg>") + 6U;
        for (std::size_t length = 0U; length < complete_length; ++length) {
            check_throws(
                [&] {
                    static_cast<void>(
                        strata::svg::parse(std::string_view(source).substr(0U, length)));
                },
                "truncated SVG fixture was accepted");
        }
    }

    std::uint32_t state = 0x6D2B79F5U;
    for (std::size_t iteration = 0U; iteration < 1000U; ++iteration) {
        state = state * 1664525U + 1013904223U;
        std::string source(state % 128U, '\0');
        for (char& value : source) {
            state = state * 1664525U + 1013904223U;
            value = static_cast<char>(state & 0x7FU);
        }
        try {
            static_cast<void>(strata::svg::parse(source));
        } catch (const std::exception&) {
            // Rejection is expected; a coincidentally valid tiny document is also harmless.
        }
    }
}

} // namespace

int strata_test_svg(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count != 2) {
            throw std::runtime_error("usage: strata_svg_tests FIXTURE_DIRECTORY");
        }
        const std::filesystem::path fixtures(arguments[1]);
        test_lucide_parsing(fixtures);
        test_shapes_transforms_and_paint();
        test_view_box_mapping_and_fill_rules();
        test_compositing_and_encoding();
        test_lucide_reference_coverage(fixtures);
        test_active_and_ambiguous_content_is_rejected();
        test_limits();
        test_truncated_and_arbitrary_inputs(fixtures);
        std::cout << "strata_svg_tests: OK\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "strata_svg_tests: " << exception.what() << '\n';
        return 1;
    }
}
