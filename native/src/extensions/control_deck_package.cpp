#include <strata/extension.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
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

[[nodiscard]] Color color_at(const double hue, const double saturation, const double value,
                             const double alpha = 1.0) noexcept {
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

[[nodiscard]] Region region_at(const PickerGeometry& value, const double x,
                               const double y) noexcept {
    if (contains(value.plane, x, y))
        return Region::plane;
    if (contains(value.hue, x, y))
        return Region::hue;
    if (contains(value.alpha, x, y))
        return Region::alpha;
    return Region::none;
}

template <std::size_t Capacity, typename... Arguments>
[[nodiscard]] std::string_view formatted(std::array<char, Capacity>& buffer,
                                         const std::format_string<Arguments...> pattern,
                                         Arguments&&... arguments) {
    const auto result = std::format_to_n(buffer.data(), static_cast<std::ptrdiff_t>(buffer.size()),
                                         pattern, std::forward<Arguments>(arguments)...);
    return std::string_view(buffer.data(), static_cast<std::size_t>(result.out - buffer.data()));
}

template <std::size_t Columns, std::size_t Rows> consteval auto grid_indices() {
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
            const double saturation =
                static_cast<double>(x) / static_cast<double>(plane_resolution - 1U);
            const double value =
                1.0 - static_cast<double>(y) / static_cast<double>(plane_resolution - 1U);
            vertices[y * plane_resolution + x] = MeshVertex{
                saturation, 1.0 - value, 0.0,
                saturation, 1.0 - value, color_at(hue, saturation, value),
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
                fraction, static_cast<double>(y), 0.0,
                fraction, static_cast<double>(y), color_at(fraction, 1.0, 1.0),
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
        color.red, color.green, color.blue, color.alpha, hue * 360.0, saturation, value);
    static_cast<void>(input.invalidate(Invalidation::semantics));
    static_cast<void>(input.emit("control-deck.color.commit", json, "color-committed", json));
}

bool picker_pointer(Input& input, const Pointer& pointer) {
    if (pointer.button != 0)
        return false;
    if (pointer.kind == Pointer::Kind::press) {
        const Region region = region_at(geometry(input.bounds()), pointer.x, pointer.y);
        if (region == Region::none)
            return false;
        input.set(picker_active, static_cast<double>(region));
        static_cast<void>(input.claim_gesture());
        update_at(input, region, pointer.x, pointer.y);
        return true;
    }

    const Region active = static_cast<Region>(
        std::clamp(static_cast<int>(std::round(input.get(picker_active))), 0, 3));
    if (active == Region::none)
        return false;
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
        changed =
            input.set(picker_saturation, unit(input.get(picker_saturation) + direction * step));
    } else if (key.name == "up" || key.name == "down") {
        const double direction = key.name == "up" ? 1.0 : -1.0;
        changed = input.set(picker_value, unit(input.get(picker_value) + direction * step));
    } else if (key.name == "pageup" || key.name == "pagedown") {
        const double direction = key.name == "pageup" ? 1.0 : -1.0;
        changed = input.set(picker_alpha, unit(input.get(picker_alpha) + direction * step));
    } else {
        return false;
    }
    if (changed)
        emit_commit(input);
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
    semantics.value_text(formatted(description, "#{:02X}{:02X}{:02X}{:02X}, hue {:.0f} degrees",
                                   color.red, color.green, color.blue, color.alpha, hue * 360.0));
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
    present.text("SATURATION / VALUE",
                 Rect{layout.plane.x, bounds.y + 8.0, layout.plane.width, 24.0}, label,
                 TextAlignment::start, TextAlignment::center);

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
    present.mesh(layout.alpha, "deck.color.alpha-gradient", Mesh{alpha_gradient, quad_indices});
    present.border(layout.alpha, 4.0, stroke(1.0, line));

    const double plane_x = layout.plane.x + saturation * layout.plane.width;
    const double plane_y = layout.plane.y + (1.0 - value) * layout.plane.height;
    present.rounded_rect(Rect{plane_x - 7.0, plane_y - 7.0, 14.0, 14.0}, 7.0,
                         rgba(5U, 8U, 11U, 110U), stroke(2.0, rgba(255U, 255U, 255U)));
    const double hue_x = layout.hue.x + hue * layout.hue.width;
    present.rounded_rect(Rect{hue_x - 3.0, layout.hue.y - 4.0, 6.0, layout.hue.height + 8.0}, 3.0,
                         inset, stroke(1.0, rgba(255U, 255U, 255U)));
    const double alpha_x = layout.alpha.x + alpha * layout.alpha.width;
    present.rounded_rect(Rect{alpha_x - 3.0, layout.alpha.y - 4.0, 6.0, layout.alpha.height + 8.0},
                         3.0, inset, stroke(1.0, rgba(255U, 255U, 255U)));

    present.mesh(layout.preview, "deck.color.preview-checker", Mesh{checker, checker_indices});
    present.rounded_rect(layout.preview, 7.0, selected, stroke(1.0, line));

    std::array<char, 48U> hex{};
    present.text(formatted(hex, "#{:02X}{:02X}{:02X}{:02X}", selected.red, selected.green,
                           selected.blue, selected.alpha),
                 Rect{layout.preview.x, layout.preview.y + layout.preview.height + 8.0,
                      layout.preview.width, 20.0},
                 value_label, TextAlignment::start, TextAlignment::center);
    std::array<char, 48U> channels{};
    present.text(formatted(channels, "H {:3.0f}  A {:3.0f}%", hue * 360.0, alpha * 100.0),
                 Rect{layout.preview.x, layout.preview.y + layout.preview.height + 28.0,
                      layout.preview.width, 20.0},
                 label, TextAlignment::start, TextAlignment::center);
    present.text(
        "Arrows adjust · Ctrl adjusts saturation",
        Rect{layout.plane.x, layout.alpha.y + layout.alpha.height + 8.0, layout.plane.width, 24.0},
        label, TextAlignment::start, TextAlignment::center);

    if (present.focus_visible())
        present.border(bounds, 9.0, stroke(2.0, focus));
}

constexpr std::size_t gradient_capacity = 8U;

struct GradientStop final {
    std::uint32_t id = 0U;
    double position = 0.0;
    Color color{};
};

struct GradientState final {
    std::uint32_t version = 1U;
    std::uint32_t count = 0U;
    std::uint32_t selected_id = 0U;
    std::uint32_t next_id = 1U;
    std::uint32_t active_id = 0U;
    double drag_origin = 0.0;
    std::array<GradientStop, gradient_capacity> stops{};
};

[[nodiscard]] constexpr GradientState initial_gradient() noexcept {
    GradientState state;
    state.count = 4U;
    state.selected_id = 2U;
    state.next_id = 5U;
    state.stops[0U] = GradientStop{1U, 0.0, rgba(41U, 91U, 214U)};
    state.stops[1U] = GradientStop{2U, 0.34, rgba(89U, 214U, 191U)};
    state.stops[2U] = GradientStop{3U, 0.68, rgba(238U, 169U, 84U)};
    state.stops[3U] = GradientStop{4U, 1.0, rgba(220U, 73U, 126U)};
    return state;
}

constexpr auto gradient_state =
    retained<structured<GradientState>>("gradient.state", initial_gradient(), Invalidation::paint);
constexpr auto gradient_stops = parameter<any>("stops");
constexpr auto gradient_panel_color = parameter<color>("panelColor");
constexpr auto gradient_track_color = parameter<color>("trackColor");
constexpr auto gradient_outline_color = parameter<color>("outlineColor");
constexpr auto gradient_text_color = parameter<color>("textColor");

constexpr std::array<Color, 6U> gradient_palette{
    rgba(56U, 112U, 232U), rgba(73U, 203U, 178U),  rgba(239U, 181U, 83U),
    rgba(225U, 76U, 128U), rgba(153U, 104U, 230U), rgba(235U, 239U, 243U),
};

struct GradientGeometry final {
    Rect track{};
    Rect palette{};
    Rect remove{};
};

[[nodiscard]] GradientGeometry gradient_geometry(const Rect bounds) noexcept {
    constexpr double margin = 22.0;
    const double width = std::max(120.0, bounds.width - margin * 2.0);
    return GradientGeometry{
        Rect{bounds.x + margin, bounds.y + 42.0, width, 68.0},
        Rect{bounds.x + margin, bounds.y + 140.0, 6.0 * 28.0 + 5.0 * 8.0, 24.0},
        Rect{bounds.x + bounds.width - margin - 82.0, bounds.y + 136.0, 82.0, 30.0},
    };
}

void sort_gradient(GradientState& state) noexcept {
    for (std::size_t index = 1U; index < state.count; ++index) {
        GradientStop moved = state.stops[index];
        std::size_t insertion = index;
        while (insertion > 0U && state.stops[insertion - 1U].position > moved.position) {
            state.stops[insertion] = state.stops[insertion - 1U];
            --insertion;
        }
        state.stops[insertion] = moved;
    }
}

[[nodiscard]] std::size_t stop_index(const GradientState& state, const std::uint32_t id) noexcept {
    for (std::size_t index = 0U; index < state.count; ++index) {
        if (state.stops[index].id == id)
            return index;
    }
    return state.count;
}

[[nodiscard]] Color mix_color(const Color left, const Color right, const double amount) noexcept {
    const auto mixed = [amount](const unsigned char a, const unsigned char b) {
        return static_cast<unsigned char>(
            std::round(static_cast<double>(a) +
                       (static_cast<double>(b) - static_cast<double>(a)) * unit(amount)));
    };
    return rgba(mixed(left.red, right.red), mixed(left.green, right.green),
                mixed(left.blue, right.blue), mixed(left.alpha, right.alpha));
}

[[nodiscard]] Color gradient_color_at(const GradientState& state, const double position) noexcept {
    if (state.count == 0U)
        return rgba(255U, 255U, 255U);
    if (position <= state.stops[0U].position)
        return state.stops[0U].color;
    for (std::size_t index = 1U; index < state.count; ++index) {
        if (position <= state.stops[index].position) {
            const GradientStop& left = state.stops[index - 1U];
            const GradientStop& right = state.stops[index];
            const double span = std::max(0.000001, right.position - left.position);
            return mix_color(left.color, right.color, (position - left.position) / span);
        }
    }
    return state.stops[state.count - 1U].color;
}

[[nodiscard]] Color value_color(const ValueView value, const Color fallback) noexcept {
    if (const std::optional<Color> direct = value.color(); direct.has_value())
        return *direct;
    if (value.kind() != ValueView::Kind::object)
        return fallback;
    const auto component = [value](const std::string_view name, const unsigned char base) {
        return static_cast<unsigned char>(std::clamp(
            std::round(value.field(name).number(static_cast<double>(base))), 0.0, 255.0));
    };
    return rgba(component("red", fallback.red), component("green", fallback.green),
                component("blue", fallback.blue), component("alpha", fallback.alpha));
}

[[nodiscard]] GradientState controlled_gradient(const ValueView value,
                                                GradientState fallback) noexcept {
    if (value.kind() != ValueView::Kind::list || value.size() < 2U)
        return fallback;
    GradientState result{};
    result.version = 1U;
    result.next_id = 1U;
    result.selected_id = fallback.selected_id;
    const std::size_t count = std::min(value.size(), gradient_capacity);
    for (std::size_t index = 0U; index < count; ++index) {
        const ValueView entry = value.at(index);
        if (entry.kind() != ValueView::Kind::object)
            continue;
        const double raw_id = entry.field("id").number(static_cast<double>(result.next_id));
        const auto id = static_cast<std::uint32_t>(
            std::clamp(std::round(raw_id), 1.0, static_cast<double>(UINT32_MAX)));
        const double position = unit(
            entry.field("position")
                .number(count > 1U ? static_cast<double>(index) / static_cast<double>(count - 1U)
                                   : 0.0));
        const Color base = gradient_color_at(fallback, position);
        result.stops[result.count++] = GradientStop{
            id,
            position,
            value_color(entry.field("color"), base),
        };
        result.next_id = std::max(result.next_id, id + 1U);
    }
    if (result.count < 2U)
        return fallback;
    sort_gradient(result);
    if (stop_index(result, result.selected_id) == result.count) {
        result.selected_id = result.stops[0U].id;
    }
    return result;
}

template <typename Context>
[[nodiscard]] GradientState resolved_gradient(Context& context) noexcept {
    GradientState state = context.get(gradient_state);
    if (state.version != 1U || state.count < 2U || state.count > gradient_capacity) {
        state = initial_gradient();
    }
    if (state.active_id == 0U)
        state = controlled_gradient(context.get(gradient_stops), state);
    return state;
}

[[nodiscard]] bool add_gradient_stop(GradientState& state, const double position) noexcept {
    if (state.count >= gradient_capacity)
        return false;
    const std::uint32_t id = state.next_id++;
    state.stops[state.count++] = GradientStop{
        id,
        unit(position),
        gradient_color_at(state, position),
    };
    state.selected_id = id;
    sort_gradient(state);
    return true;
}

[[nodiscard]] bool remove_selected_stop(GradientState& state) noexcept {
    if (state.count <= 2U)
        return false;
    const std::size_t index = stop_index(state, state.selected_id);
    if (index == state.count)
        return false;
    for (std::size_t current = index; current + 1U < state.count; ++current) {
        state.stops[current] = state.stops[current + 1U];
    }
    --state.count;
    state.selected_id = state.stops[std::min(index, static_cast<std::size_t>(state.count - 1U))].id;
    state.active_id = 0U;
    return true;
}

[[nodiscard]] std::string gradient_payload(const GradientState& state) {
    std::string result = R"({"stops":[)";
    for (std::size_t index = 0U; index < state.count; ++index) {
        if (index != 0U)
            result.push_back(',');
        const GradientStop& stop = state.stops[index];
        result += std::format(
            R"({{"id":{},"position":{:.8g},"color":{{"red":{},"green":{},"blue":{},"alpha":{}}}}})",
            stop.id, stop.position, stop.color.red, stop.color.green, stop.color.blue,
            stop.color.alpha);
    }
    result += "]}";
    return result;
}

void gradient_commit(Input& input, const GradientState& state) {
    static_cast<void>(input.invalidate(Invalidation::input));
    static_cast<void>(input.invalidate(Invalidation::semantics));
    const std::string payload = gradient_payload(state);
    static_cast<void>(
        input.emit("control-deck.gradient.commit", payload, "gradient-committed", payload));
}

[[nodiscard]] bool gradient_pointer(Input& input, const Pointer& pointer) {
    if (pointer.phase != Pointer::Phase::target || pointer.button != 0 ||
        !pointer.has_local_position) {
        return false;
    }
    GradientState state = resolved_gradient(input);
    const Rect local_bounds{0.0, 0.0, input.bounds().width, input.bounds().height};
    const GradientGeometry layout = gradient_geometry(local_bounds);

    if (pointer.kind == Pointer::Kind::press) {
        if (pointer.subtarget_id.starts_with("palette/")) {
            const std::size_t palette_index = pointer.subtarget_index >= 1'000U
                                                  ? pointer.subtarget_index - 1'000U
                                                  : gradient_palette.size();
            const std::size_t selected = stop_index(state, state.selected_id);
            if (palette_index < gradient_palette.size() && selected < state.count) {
                state.stops[selected].color = gradient_palette[palette_index];
                static_cast<void>(input.set(gradient_state, state));
                gradient_commit(input, state);
                return true;
            }
            return false;
        }
        if (pointer.subtarget_id == "remove") {
            if (remove_selected_stop(state)) {
                static_cast<void>(input.set(gradient_state, state));
                gradient_commit(input, state);
            }
            return true;
        }

        std::uint32_t selected_id = 0U;
        if (pointer.subtarget_id.starts_with("stop/") && pointer.subtarget_index <= UINT32_MAX) {
            selected_id = static_cast<std::uint32_t>(pointer.subtarget_index);
        } else if (contains(layout.track, pointer.local_x, pointer.local_y)) {
            if (!add_gradient_stop(state,
                                   (pointer.local_x - layout.track.x) / layout.track.width)) {
                return true;
            }
            selected_id = state.selected_id;
        } else {
            return false;
        }
        const std::size_t selected = stop_index(state, selected_id);
        if (selected == state.count)
            return false;
        state.selected_id = selected_id;
        state.active_id = selected_id;
        state.drag_origin = state.stops[selected].position;
        static_cast<void>(input.set(gradient_state, state));
        static_cast<void>(input.claim_gesture());
        return true;
    }

    if (state.active_id == 0U)
        return false;
    const std::size_t active = stop_index(state, state.active_id);
    if (active == state.count)
        return false;
    if (pointer.kind == Pointer::Kind::move || pointer.kind == Pointer::Kind::release) {
        state.stops[active].position =
            unit((pointer.local_x - layout.track.x) / layout.track.width);
        sort_gradient(state);
        if (pointer.kind == Pointer::Kind::release) {
            state.active_id = 0U;
            static_cast<void>(input.set(gradient_state, state));
            gradient_commit(input, state);
        } else {
            static_cast<void>(input.set(gradient_state, state));
        }
        return true;
    }

    const std::size_t cancelled = stop_index(state, state.active_id);
    if (cancelled < state.count)
        state.stops[cancelled].position = state.drag_origin;
    state.active_id = 0U;
    sort_gradient(state);
    static_cast<void>(input.set(gradient_state, state));
    static_cast<void>(input.cancel_gesture());
    return true;
}

[[nodiscard]] bool gradient_key(Input& input, const Key& key) {
    GradientState state = resolved_gradient(input);
    std::size_t selected = stop_index(state, state.selected_id);
    if (selected == state.count) {
        state.selected_id = state.stops[0U].id;
        selected = 0U;
    }

    bool committed = false;
    bool selection_only = false;
    if (key.name == "left" || key.name == "right" || key.name == "home" || key.name == "end") {
        const double step = key.control ? 0.001 : key.shift ? 0.05 : 0.01;
        const double next = key.name == "home"  ? 0.0
                            : key.name == "end" ? 1.0
                                                : state.stops[selected].position +
                                                      (key.name == "right" ? step : -step);
        state.stops[selected].position = unit(next);
        sort_gradient(state);
        committed = true;
    } else if (key.name == "up" || key.name == "down") {
        const std::size_t next = key.name == "up"
                                     ? (selected == 0U ? state.count - 1U : selected - 1U)
                                     : (selected + 1U) % state.count;
        state.selected_id = state.stops[next].id;
        selection_only = true;
    } else if (key.name == "delete" || key.name == "backspace") {
        committed = remove_selected_stop(state);
    } else if (key.name == "insert") {
        const double next_position =
            selected + 1U < state.count
                ? (state.stops[selected].position + state.stops[selected + 1U].position) * 0.5
                : std::max(0.0, state.stops[selected].position - 0.1);
        committed = add_gradient_stop(state, next_position);
    } else if (key.name == "pageup" || key.name == "pagedown") {
        const Color current = state.stops[selected].color;
        std::size_t palette_index = 0U;
        for (std::size_t index = 0U; index < gradient_palette.size(); ++index) {
            if (gradient_palette[index].red == current.red &&
                gradient_palette[index].green == current.green &&
                gradient_palette[index].blue == current.blue) {
                palette_index = index;
                break;
            }
        }
        palette_index = key.name == "pageup" ? (palette_index + 1U) % gradient_palette.size()
                                             : (palette_index + gradient_palette.size() - 1U) %
                                                   gradient_palette.size();
        state.stops[selected].color = gradient_palette[palette_index];
        committed = true;
    } else if (key.name == "enter") {
        gradient_commit(input, state);
        return true;
    } else {
        return false;
    }

    static_cast<void>(input.set(gradient_state, state));
    if (committed)
        gradient_commit(input, state);
    else if (selection_only)
        static_cast<void>(input.invalidate(Invalidation::semantics));
    return true;
}

void gradient_subtargets(Subtargets& subtargets) {
    const GradientState state = resolved_gradient(subtargets);
    const Rect bounds = subtargets.bounds();
    const GradientGeometry layout = gradient_geometry(Rect{0.0, 0.0, bounds.width, bounds.height});
    for (std::size_t index = 0U; index < state.count; ++index) {
        const GradientStop& stop = state.stops[index];
        std::array<char, 32U> id{};
        const std::string_view name = formatted(id, "stop/{}", stop.id);
        const double x = layout.track.x + stop.position * layout.track.width;
        static_cast<void>(subtargets.add(Subtarget{
            name,
            stop.id,
            Rect{x - 12.0, layout.track.y - 9.0, 24.0, layout.track.height + 30.0},
            stop.id == state.selected_id ? 20 : 10,
            true,
            true,
        }));
    }
    for (std::size_t index = 0U; index < gradient_palette.size(); ++index) {
        std::array<char, 32U> id{};
        const std::string_view name = formatted(id, "palette/{}", index);
        static_cast<void>(subtargets.add(Subtarget{
            name,
            1'000U + index,
            Rect{layout.palette.x + static_cast<double>(index) * 36.0, layout.palette.y, 28.0,
                 layout.palette.height},
            5,
            true,
        }));
    }
    static_cast<void>(subtargets.add(Subtarget{
        "remove",
        SIZE_MAX,
        layout.remove,
        5,
        state.count > 2U,
    }));
}

void gradient_semantics(Semantics& semantics) {
    const GradientState state = resolved_gradient(semantics);
    std::array<char, 64U> summary{};
    semantics.name("Gradient editor");
    semantics.value_text(formatted(summary, "{} color stops", state.count));
    semantics.add_action("focus");
    for (std::size_t index = 0U; index < state.count; ++index) {
        const GradientStop& stop = state.stops[index];
        std::array<char, 64U> name{};
        std::array<char, 64U> value{};
        static_cast<void>(semantics.child(SemanticChild{
            stop.id,
            "slider",
            formatted(name, "Gradient stop {}", index + 1U),
            formatted(value, "{:.0f}%, #{:02X}{:02X}{:02X}{:02X}", stop.position * 100.0,
                      stop.color.red, stop.color.green, stop.color.blue, stop.color.alpha),
            stop.position,
            0.0,
            1.0,
            stop.id == state.selected_id,
            false,
        }));
    }
}

template <std::size_t Capacity> struct GradientMesh final {
    std::array<MeshVertex, Capacity * 2U> vertices{};
    std::array<std::uint32_t, (Capacity - 1U) * 6U> indices{};
    std::size_t columns = 0U;
};

[[nodiscard]] GradientMesh<gradient_capacity + 2U>
gradient_mesh(const GradientState& state) noexcept {
    GradientMesh<gradient_capacity + 2U> result;
    const auto append = [&result](const double position, const Color color) {
        const std::size_t column = result.columns++;
        result.vertices[column * 2U] = MeshVertex{
            position, 0.0, 0.0, position, 0.0, color,
        };
        result.vertices[column * 2U + 1U] = MeshVertex{
            position, 1.0, 0.0, position, 1.0, color,
        };
    };
    if (state.stops[0U].position > 0.0)
        append(0.0, state.stops[0U].color);
    for (std::size_t index = 0U; index < state.count; ++index) {
        append(state.stops[index].position, state.stops[index].color);
    }
    if (state.stops[state.count - 1U].position < 1.0) {
        append(1.0, state.stops[state.count - 1U].color);
    }
    std::size_t output = 0U;
    for (std::size_t column = 0U; column + 1U < result.columns; ++column) {
        const auto left_top = static_cast<std::uint32_t>(column * 2U);
        const auto left_bottom = left_top + 1U;
        const auto right_top = left_top + 2U;
        const auto right_bottom = left_top + 3U;
        result.indices[output++] = left_top;
        result.indices[output++] = right_top;
        result.indices[output++] = right_bottom;
        result.indices[output++] = left_top;
        result.indices[output++] = right_bottom;
        result.indices[output++] = left_bottom;
    }
    return result;
}

void gradient_present(Present& present) {
    const GradientState state = resolved_gradient(present);
    const Rect bounds = present.bounds();
    const GradientGeometry layout = gradient_geometry(bounds);
    const Color panel = present.get(gradient_panel_color).value_or(rgba(15U, 20U, 27U, 242U));
    const Color track = present.get(gradient_track_color).value_or(rgba(8U, 11U, 16U));
    const Color outline = present.get(gradient_outline_color).value_or(line);
    const Color text_color = present.get(gradient_text_color).value_or(value_label);

    present.rounded_rect(bounds, 10.0, panel, stroke(1.0, outline));
    present.text("MULTI-STOP GRADIENT",
                 Rect{layout.track.x, bounds.y + 10.0, layout.track.width, 24.0}, text_color,
                 TextAlignment::start, TextAlignment::center);
    present.rounded_rect(layout.track, 7.0, track);
    const auto checker = checker_vertices();
    present.mesh(layout.track, "deck.gradient.checker", Mesh{checker, checker_indices});
    const auto gradient = gradient_mesh(state);
    const std::span<const MeshVertex> vertices(gradient.vertices.data(), gradient.columns * 2U);
    const std::span<const std::uint32_t> indices(gradient.indices.data(),
                                                 (gradient.columns - 1U) * 6U);
    present.mesh(layout.track, "deck.gradient.fill", Mesh{vertices, indices});
    present.border(layout.track, 7.0, stroke(1.0, outline));

    for (std::size_t index = 0U; index < state.count; ++index) {
        const GradientStop& stop = state.stops[index];
        const double x = layout.track.x + stop.position * layout.track.width;
        const bool selected = stop.id == state.selected_id;
        present.shadow(Rect{x - 9.0, layout.track.y + layout.track.height - 4.0, 18.0, 24.0},
                       corners(5.0), rgba(0U, 0U, 0U, 150U), 8.0, 1.0);
        present.rounded_rect(
            Rect{x - 8.0, layout.track.y + layout.track.height - 5.0, 16.0, 22.0}, 4.0, stop.color,
            stroke(selected ? 3.0 : 1.0, selected ? focus : rgba(239U, 243U, 246U)));
    }

    for (std::size_t index = 0U; index < gradient_palette.size(); ++index) {
        const Rect swatch{
            layout.palette.x + static_cast<double>(index) * 36.0,
            layout.palette.y,
            28.0,
            layout.palette.height,
        };
        present.rounded_rect(swatch, 5.0, gradient_palette[index], stroke(1.0, outline));
    }
    present.rounded_rect(layout.remove, 5.0,
                         state.count > 2U ? rgba(73U, 34U, 47U) : rgba(36U, 39U, 43U),
                         stroke(1.0, outline));
    present.text("REMOVE", layout.remove, text_color, TextAlignment::center, TextAlignment::center);
    present.text("Click track to add · drag stops · ↑↓ select · Del remove · PgUp/PgDn color",
                 Rect{layout.track.x, bounds.y + bounds.height - 31.0, layout.track.width, 24.0},
                 label, TextAlignment::start, TextAlignment::center);
    if (present.focus_visible())
        present.border(bounds, 10.0, stroke(2.0, focus));
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
    auto gradient = widget("DeckGradientEditor")
                        .no_children()
                        .focusable()
                        .intrinsic_size(600.0, 210.0)
                        .parameter(gradient_stops)
                        .parameter(gradient_panel_color)
                        .parameter(gradient_track_color)
                        .parameter(gradient_outline_color)
                        .parameter(gradient_text_color)
                        .retained(gradient_state)
                        .semantics_role("group")
                        .depends_on_status()
                        .emits(ActionContract{
                            "control-deck.gradient.commit",
                            "Commit the ordered stops from the Control Deck gradient editor",
                            "GradientStops",
                            "optional",
                            {
                                ActionArgument{"stops", "any"},
                            },
                        })
                        .on_pointer(&gradient_pointer)
                        .on_key(&gradient_key)
                        .on_semantics(&gradient_semantics)
                        .subtargets(&gradient_subtargets)
                        .present(&gradient_present);

    auto created = package("strata.control-deck.v1");
    created->widget(std::move(picker));
    created->widget(std::move(gradient));
    return created;
}

} // namespace
} // namespace strata::extension

STRATA_EXTENSION_PACKAGE(strata::extension::control_deck_package)
