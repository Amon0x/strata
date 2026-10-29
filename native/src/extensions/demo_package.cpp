#include "extensions/demo_package.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <numbers>
#include <string_view>
#include <utility>
#include <vector>

namespace strata::extension {
namespace {

constexpr auto pulse_count = retained<number>("demo.pulse.count");
constexpr auto pulse_open = retained<boolean>("demo.pulse.popup-open");
constexpr auto disclosure_expanded = retained<boolean>("demo.disclosure.expanded");
constexpr auto pulse_step = parameter<number>("step", 1.0);
constexpr auto canvas_kind = parameter<text>("kind");

constexpr Color chrome_fill = rgba(31U, 39U, 54U);
constexpr Color chrome_line = rgba(93U, 109U, 137U);
constexpr Color accent = rgba(65U, 151U, 130U);
constexpr Color label_text = rgba(232U, 237U, 245U);
constexpr Color focus_ring = rgba(116U, 194U, 255U);

/** Formats into caller storage so no presentation or input pass allocates. */
template <std::size_t Capacity, typename... Arguments>
[[nodiscard]] std::string_view formatted(
    std::array<char, Capacity>& buffer,
    const std::format_string<Arguments...> pattern,
    Arguments&&... arguments
) {
    const auto result = std::format_to_n(
        buffer.data(),
        static_cast<std::ptrdiff_t>(buffer.size()),
        pattern,
        std::forward<Arguments>(arguments)...
    );
    return std::string_view(buffer.data(), static_cast<std::size_t>(result.out - buffer.data()));
}

[[nodiscard]] Rect with_height(const Rect value, const double height) noexcept {
    return Rect{value.x, value.y, value.width, std::min(value.height, height)};
}

bool activate_pulse(Input& input) {
    const double count = input.get(pulse_count) + input.get(pulse_step);
    input.set(pulse_count, count);
    input.set(pulse_open, true);
    std::array<char, 64> payload{};
    input.emit("demo.custom.pulse", formatted(payload, R"({{"value":{:.0f}}})", count));
    return true;
}

void present_pulse(Present& present) {
    const Rect bounds = present.bounds();
    present.rounded_rect(bounds, 7.0, chrome_fill, stroke(1.0, chrome_line));
    const double hover = std::clamp(present.motion("extension.hover"), 0.0, 1.0);
    if (hover > 0.0) {
        present.rounded_rect(
            bounds,
            7.0,
            rgba(accent.red, accent.green, accent.blue, static_cast<unsigned char>(std::round(hover * 44.0)))
        );
    }
    const double count = present.get(pulse_count);
    std::array<char, 96> label{};
    present.text(
        formatted(label, "Native pulse  -  clicks {:.0f}", count),
        bounds.x + 12.0,
        bounds.y + 18.0,
        label_text
    );
    const Rect track{
        bounds.x + 10.0,
        bounds.y + bounds.height - 8.0,
        std::max(0.0, bounds.width - 20.0),
        3.0,
    };
    present.rounded_rect(track, 1.5, rgba(74U, 86U, 108U, 210U));
    if (count > 0.0) {
        const double step = std::fmod(std::max(0.0, count - 1.0), 10.0) + 1.0;
        Rect filled = track;
        filled.width *= step / 10.0;
        present.rounded_rect(filled, 1.5, accent);
    }
    if (present.focus_visible()) present.border(bounds, 7.0, stroke(2.0, focus_ring));
}

void present_pulse_popup(Present& present) {
    if (!present.get(pulse_open)) return;
    const Rect anchor = present.bounds();
    const Rect root = present.root_bounds();
    const double width = std::min(std::max(220.0, anchor.width), root.width);
    const double height = 38.0;
    const double x = std::clamp(anchor.x, root.x, std::max(root.x, root.x + root.width - width));
    double y = anchor.y + anchor.height + 5.0;
    if (y + height > root.y + root.height) y = std::max(root.y, anchor.y - height - 5.0);
    const Rect surface{x, y, width, height};
    present.rounded_rect(surface, 6.0, rgba(20U, 27U, 38U, 252U), stroke(1.0, accent));
    present.text(
        "Native popup - Escape or click outside",
        surface.x + 8.0,
        surface.y + 11.0,
        rgba(216U, 232U, 228U)
    );
}

void describe_pulse(Semantics& semantics) {
    std::array<char, 32> value{};
    semantics.value_text(formatted(value, "{:.0f}", semantics.get(pulse_count)));
}

bool activate_disclosure(Input& input) {
    const bool expanded = !input.get(disclosure_expanded);
    input.set(disclosure_expanded, expanded);
    input.emit_event("boolean-changed", expanded ? "true" : "false");
    return true;
}

void present_disclosure(Present& present) {
    const Rect bounds = present.bounds();
    const Rect header = with_height(bounds, 44.0);
    present.rounded_rect(bounds, 7.0, chrome_fill, stroke(1.0, chrome_line));
    present.text(
        present.get(disclosure_expanded)
            ? "v  Native extension disclosure"
            : ">  Native extension disclosure",
        header.x + 12.0,
        header.y + 14.0,
        label_text
    );
    if (bounds.height > 44.0) {
        present.rect(Rect{bounds.x, bounds.y + 43.0, bounds.width, 1.0}, chrome_line);
    }
    if (present.focus_visible()) present.border(header, 7.0, stroke(2.0, focus_ring));
}

void describe_disclosure(Semantics& semantics) {
    semantics.expanded(semantics.get(disclosure_expanded));
}

Rect disclosure_hit_bounds(Inspect& inspect) {
    return with_height(inspect.layout_bounds(), 44.0);
}

bool inspect_pointer(BehaviorInput& input, const Pointer& pointer) {
    if (pointer.button != 0 ||
        (pointer.kind != Pointer::Kind::press && pointer.kind != Pointer::Kind::release) ||
        (pointer.phase != Pointer::Phase::capture && !pointer.on_target)) {
        return false;
    }
    if (pointer.kind == Pointer::Kind::release) {
        std::array<char, 128> payload{};
        const std::string_view value = formatted(
            payload,
            R"({{"x":{:.17g},"y":{:.17g}}})",
            pointer.x,
            pointer.y
        );
        input.emit("demo.inspect.pointer", value, "activated", value);
    }
    return true;
}

/** A hue in turns, at full saturation and the given brightness, as a colour. */
[[nodiscard]] Color hue(const double turns, const double value = 1.0,
                        const unsigned char alpha = 255U) noexcept {
    const double h = (turns - std::floor(turns)) * 6.0;
    const double x = 1.0 - std::abs(std::fmod(h, 2.0) - 1.0);
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    if (h < 1.0) { r = 1.0; g = x; }
    else if (h < 2.0) { r = x; g = 1.0; }
    else if (h < 3.0) { g = 1.0; b = x; }
    else if (h < 4.0) { g = x; b = 1.0; }
    else if (h < 5.0) { r = x; b = 1.0; }
    else { r = 1.0; b = x; }
    const auto channel = [value](const double c) {
        return static_cast<unsigned char>(std::round((0.35 + 0.65 * c) * value * 255.0));
    };
    return rgba(channel(r), channel(g), channel(b), alpha);
}

/**
 * A hypotrochoid whose inner wheel slowly changes size: a different figure every frame, 1,400
 * points drawn as a soft wide stroke under a thin bright one.
 */
void spirograph(Present& present, const Rect area, const double phase) {
    const Point centre{area.x + area.width * 0.5, area.y + area.height * 0.5};
    const double scale = std::min(area.width, area.height) * 0.46;
    const double ratio = 0.62 + 0.18 * std::sin(phase * 2.0 * std::numbers::pi);
    const double pen = 0.55 + 0.25 * std::cos(phase * 4.0 * std::numbers::pi);
    std::vector<Point> points;
    points.reserve(1400U);
    for (int index = 0; index < 1400; ++index) {
        const double t = static_cast<double>(index) / 1399.0 * 2.0 * std::numbers::pi * 13.0;
        const double x = (1.0 - ratio) * std::cos(t) + pen * ratio * std::cos((1.0 - ratio) / ratio * t);
        const double y = (1.0 - ratio) * std::sin(t) - pen * ratio * std::sin((1.0 - ratio) / ratio * t);
        points.push_back(Point{centre.x + x * scale, centre.y + y * scale});
    }
    Stroke glow;
    glow.width = 6.0;
    glow.cap = Cap::round;
    glow.join = Join::round;
    present.polyline(points, hue(phase, 1.0, 46U), glow);
    Stroke line;
    line.width = 1.25;
    line.join = Join::round;
    present.polyline(points, hue(phase + 0.08), line);
}

/** Gauges sweeping on concentric tracks, planets orbiting them and a spinning pie at the hub. */
void orbits(Present& present, const Rect area, const double phase) {
    const Point centre{area.x + area.width * 0.5, area.y + area.height * 0.5};
    const double outer = std::min(area.width, area.height) * 0.46;
    const double turn = phase * 360.0;
    for (int ring = 0; ring < 4; ++ring) {
        const double radius = outer - ring * outer * 0.17;
        Stroke track;
        track.width = outer * 0.075;
        track.cap = Cap::round;
        present.arc(centre, radius, 0.0, 360.0, rgba(36U, 44U, 60U), track);
        const double sweep =
            150.0 + 120.0 * std::sin((phase * (ring + 1) + ring * 0.21) * 2.0 * std::numbers::pi);
        present.arc(centre, radius, turn * (ring % 2 == 0 ? 1.0 : -1.0) + ring * 40.0, sweep,
                    hue(0.08 + ring * 0.18), track);
        const double angle = (turn * (1.0 + ring * 0.5) + ring * 90.0) * std::numbers::pi / 180.0;
        present.circle(Point{centre.x + std::cos(-angle) * radius, centre.y + std::sin(-angle) * radius},
                       outer * 0.05, rgba(244U, 247U, 251U));
    }
    const double hub = outer * 0.34;
    for (int slice = 0; slice < 6; ++slice) {
        present.sector(centre, hub, 0.0, turn * 0.5 + slice * 60.0, 60.0,
                       hue(slice / 6.0 + phase, 0.95));
    }
}

/** Three translucent swells over each other, each with a bright crest, rolling across. */
void waves(Present& present, const Rect area, const double phase) {
    for (int layer = 0; layer < 3; ++layer) {
        std::vector<Point> crest;
        crest.reserve(161U);
        const double base = area.y + area.height * (0.42 + layer * 0.14);
        const double amplitude = area.height * (0.16 - layer * 0.035);
        for (int index = 0; index <= 160; ++index) {
            const double u = static_cast<double>(index) / 160.0;
            const double angle = (u * (1.6 + layer * 0.7) + phase * (layer % 2 == 0 ? 2.0 : -3.0)) *
                                 2.0 * std::numbers::pi;
            const double y = base + std::sin(angle) * amplitude +
                             std::sin(angle * 2.3 + layer) * amplitude * 0.35;
            crest.push_back(Point{area.x + u * area.width, y});
        }
        std::vector<Point> body = crest;
        body.push_back(Point{area.x + area.width, area.y + area.height});
        body.push_back(Point{area.x, area.y + area.height});
        present.polygon(body, hue(0.52 + layer * 0.09, 0.9, 96U));
        Stroke line;
        line.width = 2.0;
        line.cap = Cap::round;
        line.join = Join::round;
        present.polyline(crest, hue(0.52 + layer * 0.09), line);
    }
}

/**
 * Vector art drawn every frame through the painter, on the clock of a looping animation attached
 * to the node (`animation:`): its progress is the phase each figure is drawn at.
 */
void present_canvas(Present& present) {
    const Rect bounds = present.bounds();
    present.rounded_rect(bounds, 10.0, rgba(14U, 19U, 28U), stroke(1.0, rgba(52U, 63U, 84U)));
    const ClipScope clip = present.clip(bounds);
    const double phase = present.motion("strata.trigger.animate");
    const Rect area{bounds.x + 8.0, bounds.y + 8.0, std::max(0.0, bounds.width - 16.0),
                    std::max(0.0, bounds.height - 16.0)};
    const std::string kind = present.get(canvas_kind);
    if (kind == "orbits")
        orbits(present, area, phase);
    else if (kind == "waves")
        waves(present, area, phase);
    else
        spirograph(present, area, phase);
}

} // namespace

std::unique_ptr<Package> demo_package() {
    auto pulse = widget("DemoPulse")
        .parameter(pulse_step)
        .no_children()
        .focusable()
        .intrinsic_size(220.0, 54.0)
        .retained(pulse_count)
        .retained(pulse_open)
        .semantics_role("button")
        .semantics_actions({"activate"})
        .depends_on_motion()
        .emits(ActionContract{
            "demo.custom.pulse",
            "Report activation of the native pulse extension",
            "DemoPulseActivation",
            "optional",
            {ActionArgument{"value", "number"}},
        })
        .on_activate(&activate_pulse)
        .on_semantics(&describe_pulse)
        .present(&present_pulse)
        .detached_overlay(&present_pulse_popup, pulse_open);

    auto expander = widget("DemoDisclosure")
        .children()
        .focusable()
        .intrinsic_size(220.0, 44.0)
        .padding(Padding{44.0, 0.0, 0.0, 0.0})
        .clip()
        .retained(disclosure_expanded)
        .disclosure(Disclosure{disclosure_expanded, 44.0})
        .semantics_role("button")
        .semantics_actions({"activate"})
        .on_activate(&activate_disclosure)
        .on_semantics(&describe_disclosure)
        .present(&present_disclosure)
        .hit_bounds(&disclosure_hit_bounds);

    auto canvas = widget("DemoCanvas")
        .parameter(canvas_kind)
        .no_children()
        .intrinsic_size(260.0, 200.0)
        .depends_on_motion()
        .present(&present_canvas);

    auto picker = behavior("demo.inspector-pick")
        .on_pointer(&inspect_pointer)
        .emits(ActionContract{
            "demo.inspect.pointer",
            "Complete inspector picking at a pointer position",
            "LogicalPoint",
            "optional",
            {ActionArgument{"x", "number"}, ActionArgument{"y", "number"}},
        });

    auto created = package("strata.demo.v1");
    created->widget(std::move(pulse))
        .widget(std::move(expander))
        .widget(std::move(canvas))
        .behavior(std::move(picker));
    return created;
}

} // namespace strata::extension

STRATA_EXTENSION_PACKAGE(strata::extension::demo_package)
