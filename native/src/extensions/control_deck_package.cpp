#include <strata/extension.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

namespace strata::extension {
namespace {

constexpr auto picker_hue = retained<number>("picker.hue", 0.58, Invalidation::paint);
constexpr auto picker_saturation = retained<number>("picker.saturation", 0.72, Invalidation::paint);
constexpr auto picker_value = retained<number>("picker.value", 0.92, Invalidation::paint);
constexpr auto picker_alpha = retained<number>("picker.alpha", 0.82, Invalidation::paint);
constexpr auto picker_active = retained<number>("picker.active", 0.0, Invalidation::input);

constexpr Color chrome = rgba(17U, 22U, 28U);
constexpr Color inset = rgba(9U, 12U, 16U);
constexpr Color line = rgba(108U, 125U, 137U, 120U);
constexpr Color label = rgba(166U, 179U, 188U);
constexpr Color value_label = rgba(230U, 236U, 240U);
constexpr Color focus = rgba(190U, 216U, 232U);

struct Rgb final {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
};

struct PickerGeometry final {
    Rect plane{};
    Rect hue{};
    Rect alpha{};
    Rect preview{};
};

enum class Region : int { none = 0, plane = 1, hue = 2, alpha = 3 };

[[nodiscard]] double unit(const double value) noexcept {
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] double wrap_unit(const double value) noexcept {
    const double wrapped = std::fmod(value, 1.0);
    return wrapped < 0.0 ? wrapped + 1.0 : wrapped;
}

[[nodiscard]] Rgb hsv(const double hue, const double saturation, const double value) noexcept {
    const double h = wrap_unit(hue) * 6.0;
    const double chroma = unit(value) * unit(saturation);
    const double x = chroma * (1.0 - std::abs(std::fmod(h, 2.0) - 1.0));
    const double offset = unit(value) - chroma;
    Rgb rgb;
    if (h < 1.0) {
        rgb = Rgb{chroma, x, 0.0};
    } else if (h < 2.0) {
        rgb = Rgb{x, chroma, 0.0};
    } else if (h < 3.0) {
        rgb = Rgb{0.0, chroma, x};
    } else if (h < 4.0) {
        rgb = Rgb{0.0, x, chroma};
    } else if (h < 5.0) {
        rgb = Rgb{x, 0.0, chroma};
    } else {
        rgb = Rgb{chroma, 0.0, x};
    }
    return Rgb{rgb.red + offset, rgb.green + offset, rgb.blue + offset};
}

[[nodiscard]] unsigned char channel(const double value) noexcept {
    return static_cast<unsigned char>(std::round(unit(value) * 255.0));
}

[[nodiscard]] Color color_at(
    const double hue,
    const double saturation,
    const double value,
    const double alpha = 1.0
) noexcept {
    const Rgb rgb = hsv(hue, saturation, value);
    return rgba(channel(rgb.red), channel(rgb.green), channel(rgb.blue), channel(alpha));
}

[[nodiscard]] PickerGeometry geometry(const Rect bounds) noexcept {
    constexpr double margin = 18.0;
    constexpr double label_height = 24.0;
    constexpr double inspector_width = 126.0;
    constexpr double gap = 18.0;
    constexpr double rail_height = 16.0;
    const double content_width = std::max(1.0, bounds.width - margin * 2.0);
    const double plane_width = std::max(190.0, content_width - inspector_width - gap);
    const double plane_height = std::clamp(bounds.height - 126.0, 120.0, 194.0);
    const double x = bounds.x + margin;
    const double y = bounds.y + margin + label_height;
    return PickerGeometry{
        Rect{x, y, plane_width, plane_height},
        Rect{x, y + plane_height + 20.0, plane_width, rail_height},
        Rect{x, y + plane_height + 54.0, plane_width, rail_height},
        Rect{x + plane_width + gap, y, inspector_width, inspector_width},
    };
}

[[nodiscard]] bool contains(const Rect bounds, const double x, const double y) noexcept {
    return x >= bounds.x && y >= bounds.y && x <= bounds.x + bounds.width &&
        y <= bounds.y + bounds.height;
}

[[nodiscard]] Region region_at(
    const PickerGeometry& value,
    const double x,
    const double y
) noexcept {
    if (contains(value.plane, x, y)) return Region::plane;
    if (contains(value.hue, x, y)) return Region::hue;
    if (contains(value.alpha, x, y)) return Region::alpha;
    return Region::none;
}

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

template <std::size_t Columns, std::size_t Rows>
consteval auto grid_indices() {
    std::array<std::uint32_t, (Columns - 1U) * (Rows - 1U) * 6U> result{};
    std::size_t output = 0U;
    for (std::size_t y = 0U; y + 1U < Rows; ++y) {
        for (std::size_t x = 0U; x + 1U < Columns; ++x) {
            const auto top_left = static_cast<std::uint32_t>(y * Columns + x);
            const auto top_right = top_left + 1U;
            const auto bottom_left = top_left + static_cast<std::uint32_t>(Columns);
            const auto bottom_right = bottom_left + 1U;
            result[output++] = top_left;
            result[output++] = top_right;
            result[output++] = bottom_right;
            result[output++] = top_left;
            result[output++] = bottom_right;
            result[output++] = bottom_left;
        }
    }
    return result;
}

constexpr std::size_t plane_resolution = 9U;
constexpr auto plane_indices = grid_indices<plane_resolution, plane_resolution>();
constexpr auto strip_indices = grid_indices<7U, 2U>();

[[nodiscard]] auto plane_vertices(const double hue) {
    std::array<MeshVertex, plane_resolution * plane_resolution> vertices{};
    for (std::size_t y = 0U; y < plane_resolution; ++y) {
        for (std::size_t x = 0U; x < plane_resolution; ++x) {
            const double saturation = static_cast<double>(x) /
                static_cast<double>(plane_resolution - 1U);
            const double value = 1.0 - static_cast<double>(y) /
                static_cast<double>(plane_resolution - 1U);
            vertices[y * plane_resolution + x] = MeshVertex{
                saturation,
                1.0 - value,
                0.0,
                saturation,
                1.0 - value,
                color_at(hue, saturation, value),
            };
        }
    }
    return vertices;
}

[[nodiscard]] auto hue_vertices() {
    std::array<MeshVertex, 14U> vertices{};
    for (std::size_t y = 0U; y < 2U; ++y) {
        for (std::size_t x = 0U; x < 7U; ++x) {
            const double fraction = static_cast<double>(x) / 6.0;
            vertices[y * 7U + x] = MeshVertex{
                fraction,
                static_cast<double>(y),
                0.0,
                fraction,
                static_cast<double>(y),
                color_at(fraction, 1.0, 1.0),
            };
        }
    }
    return vertices;
}

constexpr std::size_t checker_columns = 12U;
constexpr std::size_t checker_rows = 2U;
constexpr auto checker_indices = grid_indices<checker_columns + 1U, checker_rows + 1U>();

[[nodiscard]] auto checker_vertices() {
    std::array<MeshVertex, (checker_columns + 1U) * (checker_rows + 1U)> vertices{};
    for (std::size_t y = 0U; y <= checker_rows; ++y) {
        for (std::size_t x = 0U; x <= checker_columns; ++x) {
            const bool light = ((x + y) % 2U) == 0U;
            vertices[y * (checker_columns + 1U) + x] = MeshVertex{
                static_cast<double>(x) / static_cast<double>(checker_columns),
                static_cast<double>(y) / static_cast<double>(checker_rows),
                0.0,
                0.0,
                0.0,
                light ? rgba(182U, 190U, 196U) : rgba(91U, 100U, 108U),
            };
        }
    }
    return vertices;
}

constexpr std::array<std::uint32_t, 6U> quad_indices{0U, 1U, 2U, 0U, 2U, 3U};

[[nodiscard]] std::array<MeshVertex, 4U> alpha_vertices(Color color) {
    Color transparent = color;
    transparent.alpha = 0U;
    color.alpha = 255U;
    return std::array{
        MeshVertex{0.0, 0.0, 0.0, 0.0, 0.0, transparent},
        MeshVertex{1.0, 0.0, 0.0, 1.0, 0.0, color},
        MeshVertex{1.0, 1.0, 0.0, 1.0, 1.0, color},
        MeshVertex{0.0, 1.0, 0.0, 0.0, 1.0, transparent},
    };
}

void update_at(Input& input, const Region region, const double x, const double y) {
    const PickerGeometry value = geometry(input.bounds());
    if (region == Region::plane) {
        input.set(picker_saturation, unit((x - value.plane.x) / value.plane.width));
        input.set(picker_value, unit(1.0 - (y - value.plane.y) / value.plane.height));
    } else if (region == Region::hue) {
        input.set(picker_hue, unit((x - value.hue.x) / value.hue.width));
    } else if (region == Region::alpha) {
        input.set(picker_alpha, unit((x - value.alpha.x) / value.alpha.width));
    }
}

void emit_commit(Input& input) {
    const double hue = wrap_unit(input.get(picker_hue));
    const double saturation = unit(input.get(picker_saturation));
    const double value = unit(input.get(picker_value));
    const double alpha = unit(input.get(picker_alpha));
    const Color color = color_at(hue, saturation, value, alpha);
    std::array<char, 256U> payload{};
    const std::string_view json = formatted(
        payload,
        R"({{"red":{},"green":{},"blue":{},"alpha":{},"hue":{:.6g},"saturation":{:.6g},"value":{:.6g}}})",
        color.red,
        color.green,
        color.blue,
        color.alpha,
        hue * 360.0,
        saturation,
        value
    );
    static_cast<void>(input.invalidate(Invalidation::semantics));
    static_cast<void>(input.emit("control-deck.color.commit", json, "color-committed", json));
}

bool picker_pointer(Input& input, const Pointer& pointer) {
    if (pointer.button != 0) return false;
    if (pointer.kind == Pointer::Kind::press) {
        const Region region = region_at(geometry(input.bounds()), pointer.x, pointer.y);
        if (region == Region::none) return false;
        input.set(picker_active, static_cast<double>(region));
        static_cast<void>(input.claim_gesture());
        update_at(input, region, pointer.x, pointer.y);
        return true;
    }

    const Region active = static_cast<Region>(
        std::clamp(static_cast<int>(std::round(input.get(picker_active))), 0, 3)
    );
    if (active == Region::none) return false;
    if (pointer.kind == Pointer::Kind::move) {
        update_at(input, active, pointer.x, pointer.y);
        return true;
    }
    if (pointer.kind == Pointer::Kind::release) {
        update_at(input, active, pointer.x, pointer.y);
        input.set(picker_active, 0.0);
        emit_commit(input);
        return true;
    }
    input.set(picker_active, 0.0);
    static_cast<void>(input.cancel_gesture());
    return true;
}

bool picker_key(Input& input, const Key& key) {
    const double step = key.shift ? 0.05 : 0.01;
    bool changed = false;
    if (key.name == "left" || key.name == "right") {
        const double direction = key.name == "right" ? 1.0 : -1.0;
        changed = input.set(picker_hue, wrap_unit(input.get(picker_hue) + direction * step));
    } else if ((key.name == "up" || key.name == "down") && key.control) {
        const double direction = key.name == "up" ? 1.0 : -1.0;
        changed = input.set(
            picker_saturation,
            unit(input.get(picker_saturation) + direction * step)
        );
    } else if (key.name == "up" || key.name == "down") {
        const double direction = key.name == "up" ? 1.0 : -1.0;
        changed = input.set(picker_value, unit(input.get(picker_value) + direction * step));
    } else if (key.name == "pageup" || key.name == "pagedown") {
        const double direction = key.name == "pageup" ? 1.0 : -1.0;
        changed = input.set(picker_alpha, unit(input.get(picker_alpha) + direction * step));
    } else {
        return false;
    }
    if (changed) emit_commit(input);
    return true;
}

void picker_semantics(Semantics& semantics) {
    const double hue = wrap_unit(semantics.get(picker_hue));
    const double saturation = unit(semantics.get(picker_saturation));
    const double value = unit(semantics.get(picker_value));
    const double alpha = unit(semantics.get(picker_alpha));
    const Color color = color_at(hue, saturation, value, alpha);
    std::array<char, 96U> description{};
    semantics.name("Workspace accent color");
    semantics.value_text(formatted(
        description,
        "#{:02X}{:02X}{:02X}{:02X}, hue {:.0f} degrees",
        color.red,
        color.green,
        color.blue,
        color.alpha,
        hue * 360.0
    ));
    semantics.value_range(hue * 360.0, 0.0, 360.0);
    semantics.add_action("decrement");
    semantics.add_action("focus");
    semantics.add_action("increment");
}

void picker_present(Present& present) {
    const Rect bounds = present.bounds();
    const PickerGeometry layout = geometry(bounds);
    const double hue = wrap_unit(present.get(picker_hue));
    const double saturation = unit(present.get(picker_saturation));
    const double value = unit(present.get(picker_value));
    const double alpha = unit(present.get(picker_alpha));
    const Color opaque = color_at(hue, saturation, value);
    const Color selected = color_at(hue, saturation, value, alpha);

    present.rounded_rect(bounds, 9.0, chrome, stroke(1.0, line));
    present.text("SATURATION / VALUE", layout.plane.x, bounds.y + 20.0, label);

    const auto plane = plane_vertices(hue);
    {
        const ClipScope clip = present.clip(layout.plane);
        present.mesh(layout.plane, "deck.color.plane", Mesh{plane, plane_indices});
    }
    present.border(layout.plane, 5.0, stroke(1.0, line));

    const auto hues = hue_vertices();
    present.mesh(layout.hue, "deck.color.hue", Mesh{hues, strip_indices});
    present.border(layout.hue, 4.0, stroke(1.0, line));

    const auto checker = checker_vertices();
    present.mesh(layout.alpha, "deck.color.alpha-checker", Mesh{checker, checker_indices});
    const auto alpha_gradient = alpha_vertices(opaque);
    present.mesh(
        layout.alpha,
        "deck.color.alpha-gradient",
        Mesh{alpha_gradient, quad_indices}
    );
    present.border(layout.alpha, 4.0, stroke(1.0, line));

    const double plane_x = layout.plane.x + saturation * layout.plane.width;
    const double plane_y = layout.plane.y + (1.0 - value) * layout.plane.height;
    present.rounded_rect(
        Rect{plane_x - 7.0, plane_y - 7.0, 14.0, 14.0},
        7.0,
        rgba(5U, 8U, 11U, 110U),
        stroke(2.0, rgba(255U, 255U, 255U))
    );
    const double hue_x = layout.hue.x + hue * layout.hue.width;
    present.rounded_rect(
        Rect{hue_x - 3.0, layout.hue.y - 4.0, 6.0, layout.hue.height + 8.0},
        3.0,
        inset,
        stroke(1.0, rgba(255U, 255U, 255U))
    );
    const double alpha_x = layout.alpha.x + alpha * layout.alpha.width;
    present.rounded_rect(
        Rect{alpha_x - 3.0, layout.alpha.y - 4.0, 6.0, layout.alpha.height + 8.0},
        3.0,
        inset,
        stroke(1.0, rgba(255U, 255U, 255U))
    );

    present.mesh(layout.preview, "deck.color.preview-checker", Mesh{checker, checker_indices});
    present.rounded_rect(layout.preview, 7.0, selected, stroke(1.0, line));

    std::array<char, 48U> hex{};
    present.text(
        formatted(hex, "#{:02X}{:02X}{:02X}{:02X}", selected.red, selected.green, selected.blue, selected.alpha),
        layout.preview.x,
        layout.preview.y + layout.preview.height + 24.0,
        value_label
    );
    std::array<char, 48U> channels{};
    present.text(
        formatted(channels, "H {:3.0f}  A {:3.0f}%", hue * 360.0, alpha * 100.0),
        layout.preview.x,
        layout.preview.y + layout.preview.height + 44.0,
        label
    );
    present.text(
        "Arrows adjust · Ctrl adjusts saturation",
        layout.plane.x,
        layout.alpha.y + layout.alpha.height + 24.0,
        label
    );

    if (present.focus_visible()) present.border(bounds, 9.0, stroke(2.0, focus));
}

[[nodiscard]] std::unique_ptr<Package> control_deck_package() {
    auto picker = widget("DeckColorPicker")
        .no_children()
        .focusable()
        .intrinsic_size(560.0, 330.0)
        .retained(picker_hue)
        .retained(picker_saturation)
        .retained(picker_value)
        .retained(picker_alpha)
        .retained(picker_active)
        .semantics_role("slider")
        .depends_on_status()
        .emits(ActionContract{
            "control-deck.color.commit",
            "Commit the color selected by the Control Deck picker",
            "CommittedColor",
            "optional",
            {
                ActionArgument{"red", "number"},
                ActionArgument{"green", "number"},
                ActionArgument{"blue", "number"},
                ActionArgument{"alpha", "number"},
                ActionArgument{"hue", "number"},
                ActionArgument{"saturation", "number"},
                ActionArgument{"value", "number"},
            },
        })
        .on_pointer(&picker_pointer)
        .on_key(&picker_key)
        .on_semantics(&picker_semantics)
        .present(&picker_present);

    auto created = package("strata.control-deck.v1");
    created->widget(std::move(picker));
    return created;
}

} // namespace
} // namespace strata::extension

STRATA_EXTENSION_PACKAGE(strata::extension::control_deck_package)
