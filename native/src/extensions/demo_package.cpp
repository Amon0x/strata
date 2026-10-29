#include "extensions/demo_package.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numbers>
#include <span>
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
constexpr auto particles_pointer_x = retained<number>("demo.particles.pointer-x");
constexpr auto particles_pointer_y = retained<number>("demo.particles.pointer-y");
constexpr auto particles_count = parameter<number>("particles", 110.0);

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
 * A hypotrochoid whose inner wheel slowly changes size: a different figure every frame, drawn as
 * art. The GPU generates its 1,400 points from the time (one cycle per nine seconds), so the
 * figure moves without the widget repainting: a soft wide stroke under a thin bright one.
 *
 * A wheel of any size leaves the figure open, its pen stopping short of where it started. So the
 * pen runs 13 turns of the ring and the wheel a whole number of its own, `turns`, and the figure
 * between two wheel sizes is the blend of their two, which closes because both do.
 */
constexpr std::string_view spirograph_art = R"hlsl(
float2 hypotrochoid(float t, float turns, float pen) {
    float ratio = 13.0 / (13.0 + turns);
    float wheel = turns / 13.0 * t;
    return float2((1.0 - ratio) * cos(t) + pen * ratio * cos(wheel),
                  (1.0 - ratio) * sin(t) - pen * ratio * sin(wheel));
}

float2 artPoint(uint index, uint count) {
    float phase = artTime() * artParameter(0);
    float turns = 8.0 + 3.5 * sin(phase * 6.28318531);
    float pen = 0.55 + 0.25 * cos(phase * 12.5663706);
    float t = float(index) / float(count) * 6.28318531 * 13.0;
    float below = floor(turns);
    float2 curve = lerp(hypotrochoid(t, below, pen), hypotrochoid(t, below + 1.0, pen),
                        smoothstep(0.0, 1.0, turns - below));
    return artCentred(curve, 0.92);
}
)hlsl";

void spirograph(Present& present, const Rect area) {
    const std::array<float, 1U> turns_per_second{1.0F / 9.0F};
    Stroke glow;
    glow.width = 6.0;
    glow.join = Join::round;
    present.art(area, spirograph_art, 1400U, turns_per_second, std::nullopt,
                hue(0.83, 1.0, 46U), glow);
    Stroke line;
    line.width = 1.25;
    line.join = Join::round;
    present.art(area, spirograph_art, 1400U, turns_per_second, std::nullopt, hue(0.91), line);
}

/**
 * A Maurer rose: the rose r = sin(6a) visited every `step` degrees and joined by straight lines,
 * a web that refolds as the step drifts, over the rose itself (parameter 0 at 1: a step of one
 * degree). A step between two whole degrees is the blend of their two webs, so the web closes;
 * smoothstep holds each whole web a moment before the next.
 */
constexpr std::string_view maurer_art = R"hlsl(
float2 roseAt(float index, float step, float spin) {
    float angle = index * step * 0.0174532925;
    float radius = sin(6.0 * angle);
    return radius * float2(cos(angle + spin), sin(angle + spin));
}

float2 artPoint(uint index, uint count) {
    float spin = artTime() * 0.08;
    if (artParameter(0) > 0.5)
        return artCentred(roseAt(float(index), 1.0, spin), 0.92);
    // Each whole step is a different web: a new one every three seconds, out from 30 to 70
    // degrees and back.
    float step = 30.0 + 40.0 * abs(frac(artTime() / 240.0) * 2.0 - 1.0);
    float below = floor(step);
    return artCentred(lerp(roseAt(float(index), below, spin), roseAt(float(index), below + 1.0, spin),
                           smoothstep(0.0, 1.0, step - below)),
                      0.92);
}
)hlsl";

/** A wire globe: a line winding round a sphere from pole to pole and back, turning in 3D. */
constexpr std::string_view globe_art = R"hlsl(
float2 artPoint(uint index, uint count) {
    float time = artTime();
    float along = float(index) / float(count);
    float latitude = 3.14159265 * (0.5 - 0.5 * cos(6.28318531 * along));
    float longitude = 6.28318531 * 21.0 * along;
    float3 p = float3(sin(latitude) * cos(longitude), cos(latitude), sin(latitude) * sin(longitude));
    float spin = time * 0.45;
    p = float3(p.x * cos(spin) + p.z * sin(spin), p.y, p.z * cos(spin) - p.x * sin(spin));
    float tilt = 0.55 + 0.3 * sin(time * 0.27);
    p = float3(p.x, p.y * cos(tilt) - p.z * sin(tilt), p.y * sin(tilt) + p.z * cos(tilt));
    return artCentred(p.xy * (3.4 / (3.4 + p.z)), 0.82);
}
)hlsl";

/**
 * A tesseract turning through the fourth dimension, in perspective twice: its 32 edges are one
 * closed walk (each corner has four, so a walk can cross every edge once and come home).
 */
constexpr std::string_view tesseract_art = R"hlsl(
static const uint tesseractWalk[32] = {0, 8, 10, 2, 6, 14, 10, 11, 3, 7, 15, 11, 9, 1, 5, 13,
                                       9, 8, 12, 14, 15, 13, 12, 4, 6, 7, 5, 4, 0, 2, 3, 1};

float2 artPoint(uint index, uint count) {
    float time = artTime();
    uint corner = tesseractWalk[index % 32];
    float4 p = float4(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1, (corner >> 3) & 1) * 2.0 - 1.0;
    float a = time * 0.45;
    float b = time * 0.31;
    p = float4(p.x * cos(a) - p.w * sin(a), p.y * cos(b) - p.z * sin(b),
               p.y * sin(b) + p.z * cos(b), p.x * sin(a) + p.w * cos(a));
    float3 q = p.xyz * (2.6 / (3.6 - p.w));
    float spin = time * 0.2;
    q = float3(q.x * cos(spin) + q.z * sin(spin), q.y, q.z * cos(spin) - q.x * sin(spin));
    q = float3(q.x, q.y * cos(0.5) - q.z * sin(0.5), q.y * sin(0.5) + q.z * cos(0.5));
    return artCentred(q.xy * (4.5 / (4.5 + q.z)), 0.38);
}
)hlsl";

/**
 * A harmonograph: two damped pendulums a hair out of tune, swinging out from the centre and back
 * into it, so neither end is left hanging; their phases drift, so the figure turns over.
 */
constexpr std::string_view harmonograph_art = R"hlsl(
float2 artPoint(uint index, uint count) {
    float drift = artTime() * 0.25;
    float t = (float(index) / float(count - 1) * 2.0 - 1.0) * 90.0;
    float decay = exp(-0.022 * abs(t));
    float x = sin(2.0 * t + drift) * 0.55 + sin(3.003 * t + 1.1 + drift * 0.6) * 0.45;
    float y = sin(3.0 * t + 0.4) * 0.55 + sin(2.002 * t + 2.3 - drift * 0.8) * 0.45;
    return artCentred(decay * float2(x, y), 0.82);
}
)hlsl";

/**
 * Gielis's superformula: with symmetry `m` (parameter 0), a shape that breathes between a star
 * and a round polygon. Parameters 1 to 3: its spin, size and phase. Its radius peaks at one.
 */
constexpr std::string_view supershape_art = R"hlsl(
float2 artPoint(uint index, uint count) {
    float time = artTime();
    float angle = float(index) / float(count) * 6.28318531;
    float m = artParameter(0);
    float offset = artParameter(3);
    float n = 0.95 + 0.7 * sin(time * 0.37 + offset * 2.1);
    float n1 = 0.75 + 0.4 * sin(time * 0.29 + offset * 1.3);
    float lobes = pow(abs(cos(m * angle / 4.0)), n) + pow(abs(sin(m * angle / 4.0)), n);
    float radius = pow(max(lobes, 1.0e-6), -1.0 / n1) * artParameter(2);
    float turned = angle + time * artParameter(1);
    return artCentred(radius * float2(cos(turned), sin(turned)), 0.92);
}
)hlsl";

/**
 * A hexagonal spiral that twists as it shrinks, flying in forever: it moves outward by one turn
 * each cycle, and a spiral one turn on is the same spiral, so the loop has no seam. Its outer end
 * lies past the canvas's corners.
 */
constexpr std::string_view tunnel_art = R"hlsl(
float2 artPoint(uint index, uint count) {
    float time = artTime();
    float twist = 0.09 + 0.04 * sin(time * 0.31);
    float along = float(index) + (1.0 - frac(time * 0.4)) * 6.0;
    float angle = along * (1.04719755 + twist);
    float radius = 2.6 * exp(-along * 0.017);
    return artCentred(radius * float2(cos(angle), sin(angle)), 0.92);
}
)hlsl";

/** One art figure twice: a soft wide stroke under a thin bright one. */
void glowing(Present& present, const Rect area, const std::string_view program,
             const std::uint32_t count, const std::span<const float> parameters, const double turns,
             const double glow_width, const double line_width,
             const ArtPath path = ArtPath::closed) {
    Stroke glow;
    glow.width = glow_width;
    glow.join = Join::round;
    glow.cap = Cap::round;
    present.art(area, program, count, parameters, std::nullopt, hue(turns, 1.0, 54U), glow, path);
    Stroke line;
    line.width = line_width;
    line.join = Join::round;
    line.cap = Cap::round;
    present.art(area, program, count, parameters, std::nullopt, hue(turns), line, path);
}

/** The GPU art gallery: each figure generated entirely on the GPU, every frame. */
void gallery(Present& present, const Rect area, const std::string_view kind) {
    if (kind == "rose") {
        const std::array<float, 1U> web{0.0F};
        const std::array<float, 1U> rose{1.0F};
        Stroke thread;
        thread.width = 0.7;
        thread.join = Join::round;
        present.art(area, maurer_art, 360U, web, std::nullopt, hue(0.5, 1.0, 150U), thread);
        glowing(present, area, maurer_art, 360U, rose, 0.47, 5.0, 1.4);
    } else if (kind == "globe") {
        glowing(present, area, globe_art, 1200U, {}, 0.36, 4.0, 1.0);
    } else if (kind == "tesseract") {
        glowing(present, area, tesseract_art, 32U, {}, 0.09, 7.0, 1.6);
    } else if (kind == "harmonograph") {
        glowing(present, area, harmonograph_art, 4000U, {}, 0.63, 3.0, 0.6, ArtPath::open);
    } else if (kind == "supershape") {
        // Largest first, each over the last: a translucent body under a bright edge.
        constexpr std::array<std::array<float, 4U>, 3U> layers{{
            {6.0F, 0.15F, 1.0F, 0.0F},
            {10.0F, -0.22F, 0.7F, 1.0F},
            {4.0F, 0.31F, 0.4F, 2.0F},
        }};
        for (std::size_t layer = 0U; layer < layers.size(); ++layer) {
            Stroke edge;
            edge.width = 1.0;
            edge.join = Join::round;
            const double turns = 0.78 + 0.08 * static_cast<double>(layer);
            present.art(area, supershape_art, 720U, layers[layer], hue(turns, 1.0, 72U), hue(turns),
                        edge);
        }
    } else {
        glowing(present, area, tunnel_art, 340U, {}, 0.52, 3.0, 0.8, ArtPath::open);
    }
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
    else if (kind == "spirograph")
        spirograph(present, area);
    else
        gallery(present, area, kind);
}

/**
 * A particle network: particles drifting and bouncing off the canvas's edges, a line between
 * every two closer than parameter 4 pixels that fades as they part, and lines from the pointer
 * (parameters 0 and 1, the canvas's 0-1 space; parameter 2, whether it is there) to every
 * particle within parameter 5 pixels, which it draws gently toward it. Parameter 3 is the
 * particle count, and parameter 6 picks the layer: 0 the lines, 1 the particles. All of it is
 * the GPU's: the CPU only says where the pointer is.
 */
constexpr std::string_view particles_art = R"hlsl(
float particleHash(float n) {
    return frac(sin(n * 91.3458) * 47453.5453);
}

/** Back and forth across 0 to 1 at an even pace: a particle bouncing between two walls. */
float bounce(float x) {
    return 1.0 - abs(2.0 * frac(x * 0.5) - 1.0);
}

/** Particle `index`'s place, in canvas pixels. */
float2 particle(uint index) {
    float2 size = artSize();
    float n = float(index);
    float heading = particleHash(n * 1.7 + 0.3) * 6.28318531;
    float speed = 12.0 + 26.0 * particleHash(n * 3.1 + 1.9);
    float2 travel = float2(cos(heading), sin(heading)) * speed * artTime() / size;
    float2 start = float2(particleHash(n * 5.3 + 2.7), particleHash(n * 7.7 + 4.1)) * 2.0;
    float2 place = float2(bounce(start.x + travel.x), bounce(start.y + travel.y)) * size;
    // Drawn toward the pointer, the more the closer it is.
    float2 toward = float2(artParameter(0), artParameter(1)) * size - place;
    float pull = artParameter(2) * saturate(1.0 - length(toward) / artParameter(5));
    return place + toward * 0.22 * pull * pull;
}

float2 artPoint(uint index, uint count) {
    float2 size = artSize();
    uint n = uint(artParameter(3));
    uint segment = index / 2;
    bool second = (index & 1) != 0;
    float2 pointer = float2(artParameter(0), artParameter(1)) * size;
    float grab = artParameter(5);
    if (artParameter(6) > 0.5) {
        // A particle: a line that does not move, so a dot; brighter near the pointer.
        float2 place = particle(segment);
        artSetWeight(0.55 + 0.45 * artParameter(2) * saturate(1.0 - length(pointer - place) / grab));
        return place / size;
    }
    uint pairs = n * (n - 1) / 2;
    float2 from;
    float2 to;
    float reach;
    float strength;
    if (segment < pairs) {
        // The segment-th pair (i, j), i < j, counted row by row.
        float pair = float(segment);
        float total = float(n);
        uint i = uint(total - 2.0 - floor(sqrt(-8.0 * pair + 4.0 * total * (total - 1.0) - 7.0) * 0.5 - 0.5));
        uint j = uint(int(segment) + int(i) + 1 - int(pairs) + int((n - i) * (n - i - 1) / 2));
        from = particle(i);
        to = particle(j);
        reach = artParameter(4);
        strength = 0.75;
    } else {
        from = particle(segment - pairs);
        to = pointer;
        reach = grab;
        strength = artParameter(2);
    }
    float fade = saturate(1.0 - length(to - from) / reach);
    artSetWeight(strength * fade);
    return (second ? to : from) / size;
}
)hlsl";

/** A particle network container: the pointer's place over it, kept for its art. */
bool track_particles_pointer(Input& input, const Pointer& pointer) {
    if (pointer.kind == Pointer::Kind::move && pointer.has_local_position) {
        input.set(particles_pointer_x, pointer.local_x);
        input.set(particles_pointer_y, pointer.local_y);
    }
    // The pointer still reaches whatever it is over.
    return false;
}

void present_particles(Present& present) {
    const Rect bounds = present.bounds();
    present.rect(bounds, rgba(5U, 8U, 17U));
    if (bounds.width <= 0.0 || bounds.height <= 0.0)
        return;
    const auto count =
        static_cast<std::uint32_t>(std::clamp(present.get(particles_count), 2.0, 160.0));
    const bool here = present.hovered();
    std::array<float, 7U> values{
        static_cast<float>(present.get(particles_pointer_x) / bounds.width),
        static_cast<float>(present.get(particles_pointer_y) / bounds.height),
        here ? 1.0F : 0.0F,
        static_cast<float>(count),
        150.0F,
        210.0F,
        0.0F,
    };
    const std::uint32_t pairs = count * (count - 1U) / 2U;
    Stroke line;
    line.width = 1.1;
    line.cap = Cap::round;
    present.art(bounds, particles_art, (pairs + count) * 2U, values, std::nullopt,
                rgba(132U, 186U, 255U), line, ArtPath::segments);
    values[6] = 1.0F;
    Stroke dot;
    dot.width = 3.6;
    dot.cap = Cap::round;
    present.art(bounds, particles_art, count * 2U, values, std::nullopt, rgba(226U, 236U, 255U),
                dot, ArtPath::segments);
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

    auto particles = widget("DemoParticles")
                         .parameter(particles_count)
                         .children()
                         .clip()
                         .intrinsic_size(640.0, 400.0)
                         .retained(particles_pointer_x)
                         .retained(particles_pointer_y)
                         .depends_on_status()
                         .on_pointer(&track_particles_pointer)
                         .present(&present_particles);

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
        .widget(std::move(particles))
        .behavior(std::move(picker));
    return created;
}

} // namespace strata::extension

STRATA_EXTENSION_PACKAGE(strata::extension::demo_package)
