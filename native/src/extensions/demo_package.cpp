#include "extensions/demo_package.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string_view>

namespace strata::extension {
namespace {

constexpr auto pulse_count = retained<number>("demo.pulse.count");
constexpr auto pulse_open = retained<boolean>("demo.pulse.popup-open");
constexpr auto disclosure_expanded = retained<boolean>("demo.disclosure.expanded");
constexpr auto pulse_step = parameter<number>("step", 1.0);

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
        buffer.size(),
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
    created->widget(std::move(pulse)).widget(std::move(expander)).behavior(std::move(picker));
    return created;
}

} // namespace strata::extension
