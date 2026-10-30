#include "software_renderer.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "gpu/effect_policy.hpp"
#include "gpu/effect_shaders.hpp"
#include "gpu/isolation.hpp"
#include "gpu/layer_shaders.hpp"
#include <strata/render_shapes.hpp>
#include <strata/vector_text.hpp>

namespace strata::headless {
namespace {

constexpr std::size_t vertex_size = host::packet_vertex_bytes;

struct Color final {
    float red = 1.0F;
    float green = 1.0F;
    float blue = 1.0F;
    float alpha = 1.0F;
};

struct Vertex final {
    float x = 0.0F;
    float y = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
    Color color;
    std::array<float, 16U> data{};
};

struct PixelRect final {
    std::uint32_t left = 0U;
    std::uint32_t top = 0U;
    std::uint32_t right = 0U;
    std::uint32_t bottom = 0U;
};

[[nodiscard]] PixelRect effect_region(const host::EffectBatch& effect, const std::uint32_t width,
                                      const std::uint32_t height, const double logical_width,
                                      const double logical_height) noexcept {
    const double scale_x = logical_width > 0.0 ? width / logical_width : 1.0;
    const double scale_y = logical_height > 0.0 ? height / logical_height : 1.0;
    const std::uint32_t effect_left = static_cast<std::uint32_t>(
        std::clamp(std::floor(effect.x * scale_x), 0.0, static_cast<double>(width)));
    const std::uint32_t effect_top = static_cast<std::uint32_t>(
        std::clamp(std::floor(effect.y * scale_y), 0.0, static_cast<double>(height)));
    const std::uint32_t effect_right = static_cast<std::uint32_t>(std::clamp(
        std::ceil((effect.x + effect.width) * scale_x), 0.0, static_cast<double>(width)));
    const std::uint32_t effect_bottom = static_cast<std::uint32_t>(std::clamp(
        std::ceil((effect.y + effect.height) * scale_y), 0.0, static_cast<double>(height)));
    const std::uint64_t clip_right = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(effect.scissor.x) + effect.scissor.width, width);
    const std::uint64_t clip_bottom = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(effect.scissor.y) + effect.scissor.height, height);
    return PixelRect{
        std::max(effect_left, std::min(effect.scissor.x, width)),
        std::max(effect_top, std::min(effect.scissor.y, height)),
        std::min(effect_right, static_cast<std::uint32_t>(clip_right)),
        std::min(effect_bottom, static_cast<std::uint32_t>(clip_bottom)),
    };
}

[[nodiscard]] float read_float(const std::span<const std::uint8_t> bytes,
                               const std::size_t offset) {
    if (offset > bytes.size() || sizeof(float) > bytes.size() - offset) {
        throw std::invalid_argument("render packet vertex is truncated");
    }
    float result = 0.0F;
    std::memcpy(&result, bytes.data() + offset, sizeof(result));
    if (!std::isfinite(result))
        throw std::invalid_argument("render packet vertex is non-finite");
    return result;
}

/** Mirrors the vertex stage: z selects the vertex's presentation group (zero is the identity). */
[[nodiscard]] Vertex read_vertex(const std::span<const std::uint8_t> bytes, const std::size_t index,
                                 const float scale_x, const float scale_y,
                                 const std::vector<host::PresentationGroup>& groups) {
    if (index >= bytes.size() / vertex_size) {
        throw std::invalid_argument("render packet vertex index is out of range");
    }
    const std::size_t base = index * vertex_size;
    const auto group_index = static_cast<std::size_t>(
        std::clamp(std::floor(read_float(bytes, base + 8U) + 0.5F), 0.0F,
                   static_cast<float>(host::maximum_presentation_group)));
    const host::PresentationGroup group =
        group_index < groups.size() ? groups[group_index] : host::PresentationGroup{};
    // Group translation lands on whole device pixels, as in the vertex stage.
    Vertex result;
    double x = read_float(bytes, base);
    double y = read_float(bytes, base + 4U);
    result.u = read_float(bytes, base + 12U);
    result.v = read_float(bytes, base + 16U);
    const int vertex_mode =
        static_cast<int>(std::floor(read_float(bytes, base + 24U + 14U * sizeof(float)) + 0.5F));
    if (vertex_mode == 13 || vertex_mode == 14) {
        // A vector glyph's quad (its outline's bounds, uv in font units) grows by two device
        // pixels each way at whatever scale its group shows it, as in the vertex stage: value 3
        // is its corner (bit 0 right, bit 1 top), 4-5 and 6-7 the logical pixels a font unit
        // spans along x and y.
        const auto value = [&](const std::size_t index) {
            return static_cast<double>(read_float(bytes, base + 24U + index * sizeof(float)));
        };
        const double corner = value(3U);
        const double outward_x = std::fmod(corner, 2.0) >= 0.5 ? 1.0 : -1.0;
        const double outward_y = corner >= 1.5 ? 1.0 : -1.0;
        const double along_x_x = value(4U);
        const double along_x_y = value(5U);
        const double along_y_x = value(6U);
        const double along_y_y = value(7U);
        const auto device_length = [&](const double dx, const double dy) {
            return std::hypot((group.scale_x * dx + group.shear_x * dy) * scale_x,
                              (group.shear_y * dx + group.scale_y * dy) * scale_y);
        };
        const double grow_x =
            outward_x * 2.0 / std::max(device_length(along_x_x, along_x_y), 1.0e-6);
        const double grow_y =
            outward_y * 2.0 / std::max(device_length(along_y_x, along_y_y), 1.0e-6);
        x += grow_x * along_x_x + grow_y * along_y_x;
        y += grow_x * along_x_y + grow_y * along_y_y;
        result.u = static_cast<float>(result.u + grow_x);
        result.v = static_cast<float>(result.v + grow_y);
    }
    // A group turned in depth divides by its w; the reference interpolates in screen space.
    const double w = group.place_w(x, y);
    result.x = static_cast<float>(((group.scale_x * x + group.shear_x * y) * scale_x +
                                   std::round(group.translate_x * scale_x)) /
                                  w);
    result.y = static_cast<float>(((group.shear_y * x + group.scale_y * y) * scale_y +
                                   std::round(group.translate_y * scale_y)) /
                                  w);
    result.color = Color{
        static_cast<float>(bytes[base + 20U]) / 255.0F,
        static_cast<float>(bytes[base + 21U]) / 255.0F,
        static_cast<float>(bytes[base + 22U]) / 255.0F,
        static_cast<float>(bytes[base + 23U]) / 255.0F,
    };
    for (std::size_t value = 0U; value < result.data.size(); ++value) {
        result.data[value] = read_float(bytes, base + 24U + value * sizeof(float));
    }
    result.data[15U] *= static_cast<float>(group.opacity);
    // Tone, as in the vertex stage: images (mode 1) and shadows (mode 6) are not ink, and a rounded
    // rect (mode 2) carries its border colour in values 8 to 10.
    const int mode = static_cast<int>(std::floor(result.data[14U] + 0.5F));
    // A colour mask (mode 11) tones its texels in the pixel stage, its tone in value 4.
    if (mode == 11)
        result.data[4U] = static_cast<float>(group.tone);
    if (group.tone > 0.0 && mode != 1 && mode != 6 && mode != 11) {
        const float tone = static_cast<float>(group.tone);
        host::tone_color(result.color.red, result.color.green, result.color.blue, tone);
        if (mode == 2 || (mode == host::draw_mode_coverage_mask && result.data[6U] > 0.5F))
            host::tone_color(result.data[8U], result.data[9U], result.data[10U], tone);
    }
    // The rasterizer sees positions on the 1/256 subpixel grid D3D11 hardware snaps to, so a
    // vertex a hair either side of a pixel centre covers the same pixels as on the GPU.
    result.x = std::round(result.x * 256.0F) / 256.0F;
    result.y = std::round(result.y * 256.0F) / 256.0F;
    return result;
}

/**
 * The edge function of `p` against a->b. With snapped positions every term is exact in double, so
 * the two triangles sharing an edge agree on which side each pixel centre lies and nothing cracks.
 */
[[nodiscard]] double edge_function(const Vertex& a, const Vertex& b, const double px,
                                   const double py) noexcept {
    return (static_cast<double>(b.x) - a.x) * (py - a.y) -
           (static_cast<double>(b.y) - a.y) * (px - a.x);
}

[[nodiscard]] bool top_left(const Vertex& a, const Vertex& b) noexcept {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    return dy < 0.0F || (dy == 0.0F && dx > 0.0F);
}

[[nodiscard]] bool covered(const double edge, const bool includes_edge) noexcept {
    return edge > 0.0 || (edge == 0.0 && includes_edge);
}

[[nodiscard]] float saturate(const float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] float smoothstep(const float minimum, const float maximum,
                               const float value) noexcept {
    if (maximum <= minimum)
        return value < minimum ? 0.0F : 1.0F;
    const float normalized = saturate((value - minimum) / (maximum - minimum));
    return normalized * normalized * (3.0F - 2.0F * normalized);
}

[[nodiscard]] float rounded_box_sdf(const float x, const float y, const float half_width,
                                    const float half_height,
                                    const std::array<float, 4U>& radii) noexcept {
    const bool trailing = x >= 0.0F;
    const bool bottom = y >= 0.0F;
    // Clamped for the same reason as the GPU path: an oversized radius otherwise pushes the field
    // outside the shape and the surface disappears instead of resolving to a pill.
    const float radius =
        std::min(std::max(0.0F, bottom ? (trailing ? radii[2U] : radii[3U])
                                       : (trailing ? radii[1U] : radii[0U])),
                 std::min(half_width, half_height));
    const float qx = std::abs(x) - half_width + radius;
    const float qy = std::abs(y) - half_height + radius;
    return std::hypot(std::max(qx, 0.0F), std::max(qy, 0.0F)) + std::min(std::max(qx, qy), 0.0F) -
           radius;
}

[[nodiscard]] float rounded_clip_coverage(
    const std::span<const host::RoundedClip> clips,
    const float logical_x,
    const float logical_y,
    const float logical_pixel_width,
    const float logical_pixel_height
) noexcept {
    const auto distance = [](const host::RoundedClip& clip, const float x, const float y) {
        const auto local = [&clip](const float logical_x_value, const float logical_y_value) {
            return std::array<float, 2U>{
                static_cast<float>(
                    clip.inverse_transform[0U] * logical_x_value +
                    clip.inverse_transform[1U] * logical_y_value +
                    clip.inverse_transform[2U]
                ),
                static_cast<float>(
                    clip.inverse_transform[3U] * logical_x_value +
                    clip.inverse_transform[4U] * logical_y_value +
                    clip.inverse_transform[5U]
                ),
            };
        };
        const std::array<float, 2U> point = local(x, y);
        const float half_width = static_cast<float>(clip.width * 0.5);
        const float half_height = static_cast<float>(clip.height * 0.5);
        const float px = point[0U] - static_cast<float>(clip.x) - half_width;
        const float py = point[1U] - static_cast<float>(clip.y) - half_height;
        const float rx = std::max(half_width, 0.0001F);
        const float ry = std::max(half_height, 0.0001F);
        if (clip.shape == 1U)
            return (std::hypot(px / rx, py / ry) - 1.0F) * std::min(rx, ry);
        if (clip.shape == 2U || clip.shape == 3U) {
            // As the shader's roundedClipPolygonSdf.
            const float qx = px / rx;
            const float qy = py / ry;
            const float n = std::max(static_cast<float>(clip.sides), 3.0F);
            const float sector = 6.28318531F / n;
            const float angle = std::atan2(qx, -qy) + sector * 0.5F;
            const float local_angle = angle - sector * std::floor(angle / sector + 0.5F);
            const float radius = std::hypot(qx, qy);
            float result = 0.0F;
            if (clip.shape == 3U) {
                const float inner = 0.5F;
                const float t = std::abs(local_angle) / (sector * 0.5F);
                const float ax = 0.0F, ay = 1.0F;
                const float bx = std::sin(sector * 0.5F) * inner,
                            by = std::cos(sector * 0.5F) * inner;
                const float pointx = std::sin(std::abs(local_angle)) * radius;
                const float pointy = std::cos(std::abs(local_angle)) * radius;
                const float ex = bx - ax, ey = by - ay;
                const float length = std::max(std::hypot(ex, ey), 1.0e-6F);
                const float nx = -ey / length, ny = ex / length;
                result = t > 1.0F ? radius - inner : (pointx - ax) * nx + (pointy - ay) * ny;
            } else {
                result = radius * std::cos(local_angle) - std::cos(sector * 0.5F);
            }
            return result * std::min(rx, ry);
        }
        std::array<float, 4U> radii{};
        const float radius_limit = std::max(0.0F, std::min(half_width, half_height));
        for (std::size_t index = 0U; index < radii.size(); ++index) {
            radii[index] = std::min(static_cast<float>(clip.radii[index]), radius_limit);
        }
        return rounded_box_sdf(
            point[0U] - static_cast<float>(clip.x) - half_width,
            point[1U] - static_cast<float>(clip.y) - half_height,
            half_width,
            half_height,
            radii
        );
    };
    float coverage = 1.0F;
    for (const host::RoundedClip& clip : clips) {
        const float center = distance(clip, logical_x, logical_y);
        const float derivative =
            std::abs(distance(clip, logical_x + logical_pixel_width, logical_y) - center) +
            std::abs(distance(clip, logical_x, logical_y + logical_pixel_height) - center);
        coverage *= 1.0F - smoothstep(
            -std::max(derivative, 0.0001F),
            std::max(derivative, 0.0001F),
            center
        );
        if (clip.fade[0U] > 0.0 || clip.fade[1U] > 0.0 || clip.fade[2U] > 0.0 ||
            clip.fade[3U] > 0.0) {
            const double x = clip.inverse_transform[0U] * logical_x +
                             clip.inverse_transform[1U] * logical_y + clip.inverse_transform[2U];
            const double y = clip.inverse_transform[3U] * logical_x +
                             clip.inverse_transform[4U] * logical_y + clip.inverse_transform[5U];
            const auto ramp = [](const double distance, const double fade) {
                return fade > 0.0 ? static_cast<float>(std::clamp(distance / fade, 0.0, 1.0))
                                  : 1.0F;
            };
            coverage *=
                ramp(y - clip.y, clip.fade[0U]) * ramp(clip.x + clip.width - x, clip.fade[1U]) *
                ramp(clip.y + clip.height - y, clip.fade[2U]) * ramp(x - clip.x, clip.fade[3U]);
        }
    }
    return coverage;
}

[[nodiscard]] std::uint8_t channel(const float value) noexcept {
    return static_cast<std::uint8_t>(std::lround(saturate(value) * 255.0F));
}

/**
 * The taps of one blur pass from the centre out, as the GPU blur shader weighs them: a Gaussian of
 * a third of the radius, cut at the radius. A radius under one half is the centre alone.
 */
[[nodiscard]] std::vector<double> blur_weights(const double radius) {
    if (radius < 0.5)
        return {1.0};
    const auto reach = static_cast<std::uint32_t>(std::ceil(radius));
    const double sigma = std::max(radius / 3.0, 0.5);
    const double inverse_two_sigma_squared = 0.5 / (sigma * sigma);
    std::vector<double> weights(reach + 1U);
    for (std::uint32_t offset = 0U; offset <= reach; ++offset) {
        const double distance = static_cast<double>(offset);
        const double coverage = std::clamp(radius + 0.5 - distance, 0.0, 1.0);
        weights[offset] = std::exp(-distance * distance * inverse_two_sigma_squared) * coverage;
    }
    return weights;
}

using Rgba = std::array<float, 4U>;

[[nodiscard]] Rgba straight(const Rgba color) noexcept {
    if (!(color[3] > 0.00001F))
        return Rgba{};
    return Rgba{color[0] / color[3], color[1] / color[3], color[2] / color[3], color[3]};
}

[[nodiscard]] float luminance(const float red, const float green, const float blue) noexcept {
    return 0.2126F * red + 0.7152F * green + 0.0722F * blue;
}

/** A layer filter's colour programs, as gpu::layer_filter_pixel runs them (premultiplied in/out).
 */
[[nodiscard]] Rgba filter_color(const std::uint32_t kind, const std::span<const float, 24U> values,
                                const Rgba premultiplied) noexcept {
    const Rgba color = straight(premultiplied);
    Rgba result = color;
    const auto value = [&values](const std::size_t index) { return values[index]; };
    if (kind == 2U) {
        for (std::size_t row = 0U; row < 4U; ++row) {
            result[row] = value(row * 5U) * color[0] + value(row * 5U + 1U) * color[1] +
                          value(row * 5U + 2U) * color[2] + value(row * 5U + 3U) * color[3] +
                          value(row * 5U + 4U);
        }
    } else if (kind == 3U) {
        const float level = saturate(luminance(color[0], color[1], color[2]));
        const auto count = static_cast<std::uint32_t>(std::clamp(value(1U), 0.0F, 5.0F));
        std::array<float, 3U> mapped{};
        if (count > 0U) {
            std::array<float, 3U> previous{value(3U), value(4U), value(5U)};
            float previous_offset = value(2U);
            mapped = previous;
            if (level > previous_offset) {
                bool found = false;
                for (std::uint32_t stop = 1U; stop < count && !found; ++stop) {
                    const std::size_t at = 2U + stop * 4U;
                    const float offset = value(at);
                    const std::array<float, 3U> stop_color{value(at + 1U), value(at + 2U),
                                                           value(at + 3U)};
                    if (level <= offset) {
                        const float span = offset - previous_offset;
                        const float t = span > 0.00001F ? (level - previous_offset) / span : 1.0F;
                        for (std::size_t channel_index = 0U; channel_index < 3U; ++channel_index)
                            mapped[channel_index] =
                                previous[channel_index] +
                                (stop_color[channel_index] - previous[channel_index]) * t;
                        found = true;
                    } else {
                        previous = stop_color;
                        previous_offset = offset;
                        mapped = previous;
                    }
                }
            }
        }
        const float mix = saturate(value(0U));
        for (std::size_t channel_index = 0U; channel_index < 3U; ++channel_index)
            result[channel_index] =
                color[channel_index] + (mapped[channel_index] - color[channel_index]) * mix;
    } else if (kind == 4U) {
        for (std::size_t channel_index = 0U; channel_index < 4U; ++channel_index) {
            const std::size_t first = channel_index * 5U;
            const float black = value(first);
            const float white = value(first + 1U);
            const float gamma = std::max(value(first + 2U), 0.00001F);
            const float span = white - black;
            float x = std::abs(span) > 0.00001F ? saturate((color[channel_index] - black) / span)
                                                : (color[channel_index] >= black ? 1.0F : 0.0F);
            x = std::pow(x, 1.0F / gamma);
            result[channel_index] = value(first + 3U) + (value(first + 4U) - value(first + 3U)) * x;
        }
    } else if (kind == 6U) {
        result = Rgba{value(0U), value(1U), value(2U), color[3] * value(3U)};
    }
    for (float& component : result)
        component = saturate(component);
    return Rgba{result[0] * result[3], result[1] * result[3], result[2] * result[3], result[3]};
}

[[nodiscard]] float hard_light(const float backdrop, const float source) noexcept {
    return source <= 0.5F ? backdrop * 2.0F * source
                          : backdrop + (2.0F * source - 1.0F) - backdrop * (2.0F * source - 1.0F);
}

[[nodiscard]] float soft_light(const float backdrop, const float source) noexcept {
    if (source <= 0.5F)
        return backdrop - (1.0F - 2.0F * source) * backdrop * (1.0F - backdrop);
    const float d = backdrop <= 0.25F ? ((16.0F * backdrop - 12.0F) * backdrop + 4.0F) * backdrop
                                      : std::sqrt(backdrop);
    return backdrop + (2.0F * source - 1.0F) * (d - backdrop);
}

[[nodiscard]] float color_dodge(const float backdrop, const float source) noexcept {
    if (backdrop <= 0.0F)
        return 0.0F;
    if (source >= 1.0F)
        return 1.0F;
    return std::min(1.0F, backdrop / (1.0F - source));
}

[[nodiscard]] float color_burn(const float backdrop, const float source) noexcept {
    if (backdrop >= 1.0F)
        return 1.0F;
    if (source <= 0.0F)
        return 0.0F;
    return 1.0F - std::min(1.0F, (1.0F - backdrop) / source);
}

using Rgb = std::array<float, 3U>;

[[nodiscard]] float lum(const Rgb& c) noexcept {
    return 0.3F * c[0] + 0.59F * c[1] + 0.11F * c[2];
}

[[nodiscard]] Rgb clip_color(Rgb c) noexcept {
    const float l = lum(c);
    const float n = std::min({c[0], c[1], c[2]});
    const float x = std::max({c[0], c[1], c[2]});
    if (n < 0.0F) {
        for (float& v : c)
            v = l + (v - l) * l / std::max(l - n, 0.00001F);
    }
    if (x > 1.0F) {
        for (float& v : c)
            v = l + (v - l) * (1.0F - l) / std::max(x - l, 0.00001F);
    }
    return c;
}

[[nodiscard]] Rgb set_lum(Rgb c, const float l) noexcept {
    const float d = l - lum(c);
    for (float& v : c)
        v += d;
    return clip_color(c);
}

[[nodiscard]] float sat(const Rgb& c) noexcept {
    return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]});
}

[[nodiscard]] Rgb set_sat(Rgb c, const float s) noexcept {
    const float high = std::max({c[0], c[1], c[2]});
    const float low = std::min({c[0], c[1], c[2]});
    const float span = high - low;
    if (span <= 0.00001F)
        return Rgb{};
    for (float& v : c)
        v = (v - low) * s / span;
    return c;
}

[[nodiscard]] Rgb blend(const Rgb& cb, const Rgb& cs, const std::uint32_t mode) noexcept {
    Rgb result{};
    const auto each = [&](const auto& function) {
        for (std::size_t index = 0U; index < 3U; ++index)
            result[index] = function(cb[index], cs[index]);
        return result;
    };
    switch (mode) {
    case 1U:
        return each([](const float b, const float s) { return b * s; });
    case 2U:
        return each([](const float b, const float s) { return b + s - b * s; });
    case 3U:
        return each([](const float b, const float s) { return hard_light(s, b); });
    case 4U:
        return each([](const float b, const float s) { return std::min(b, s); });
    case 5U:
        return each([](const float b, const float s) { return std::max(b, s); });
    case 6U:
        return each(color_dodge);
    case 7U:
        return each(color_burn);
    case 8U:
        return each(hard_light);
    case 9U:
        return each(soft_light);
    case 10U:
        return each([](const float b, const float s) { return std::abs(b - s); });
    case 11U:
        return each([](const float b, const float s) { return b + s - 2.0F * b * s; });
    case 12U:
        return set_lum(set_sat(cs, sat(cb)), lum(cb));
    case 13U:
        return set_lum(set_sat(cb, sat(cs)), lum(cb));
    case 14U:
        return set_lum(cs, lum(cb));
    default:
        return set_lum(cb, lum(cs));
    }
}

/** A layer and what is under it combined by `mode`, as gpu::layer_composite_pixel does. */
[[nodiscard]] Rgba layer_composite(const Rgba& s, const Rgba& d,
                                   const std::uint32_t mode) noexcept {
    Rgba result{};
    if (mode == 0U) {
        for (std::size_t index = 0U; index < 4U; ++index)
            result[index] = s[index] + d[index] * (1.0F - s[3]);
        return result;
    }
    if (mode <= 15U) {
        const Rgba straight_s = straight(s);
        const Rgba straight_d = straight(d);
        const Rgb cs{saturate(straight_s[0]), saturate(straight_s[1]), saturate(straight_s[2])};
        const Rgb cb{saturate(straight_d[0]), saturate(straight_d[1]), saturate(straight_d[2])};
        const Rgb mixed = blend(cb, cs, mode);
        for (std::size_t index = 0U; index < 3U; ++index) {
            result[index] = s[index] * (1.0F - d[3]) + d[index] * (1.0F - s[3]) +
                            s[3] * d[3] * saturate(mixed[index]);
        }
        result[3] = s[3] + d[3] - s[3] * d[3];
        return result;
    }
    const float level = saturate(luminance(s[0], s[1], s[2]));
    const float scale = mode == 16U   ? s[3]
                        : mode == 17U ? 1.0F - s[3]
                        : mode == 18U ? level
                                      : 1.0F - level;
    for (std::size_t index = 0U; index < 4U; ++index) {
        if (mode <= 19U)
            result[index] = d[index] * scale;
        else if (mode == 20U)
            result[index] = std::max(s[index], d[index]);
        else if (mode == 21U)
            result[index] = std::min(s[index], d[index]);
        else if (mode == 23U)
            result[index] = std::min(s[index] + d[index], 1.0F);
        else
            result[index] = std::abs(s[index] - d[index]);
    }
    return result;
}

[[nodiscard]] Color interpolate_color(const Vertex& a, const Vertex& b, const Vertex& c,
                                      const float wa, const float wb, const float wc) noexcept {
    return Color{
        a.color.red * wa + b.color.red * wb + c.color.red * wc,
        a.color.green * wa + b.color.green * wb + c.color.green * wc,
        a.color.blue * wa + b.color.blue * wb + c.color.blue * wc,
        a.color.alpha * wa + b.color.alpha * wb + c.color.alpha * wc,
    };
}

[[nodiscard]] std::array<float, 16U> interpolate_data(const Vertex& a, const Vertex& b,
                                                      const Vertex& c, const float wa,
                                                      const float wb, const float wc) noexcept {
    std::array<float, 16U> result{};
    for (std::size_t index = 0U; index < result.size(); ++index) {
        result[index] = a.data[index] * wa + b.data[index] * wb + c.data[index] * wc;
    }
    return result;
}

template <typename Source>
void composite_filtered_pixels(std::vector<std::uint8_t>& destination,
                               const host::EffectBatch& effect, const std::uint32_t width,
                               const std::uint32_t height, const double logical_width,
                               const double logical_height, Source&& source) {
    const double scale_x = width / logical_width;
    const double scale_y = height / logical_height;
    const double exact_left = effect.x * scale_x;
    const double exact_top = effect.y * scale_y;
    const double exact_right = (effect.x + effect.width) * scale_x;
    const double exact_bottom = (effect.y + effect.height) * scale_y;
    const PixelRect region = effect_region(effect, width, height, logical_width, logical_height);
    const float center_x = static_cast<float>((exact_left + exact_right) * 0.5);
    const float center_y = static_cast<float>((exact_top + exact_bottom) * 0.5);
    const float half_width = static_cast<float>((exact_right - exact_left) * 0.5);
    const float half_height = static_cast<float>((exact_bottom - exact_top) * 0.5);
    const float radius_scale = static_cast<float>(std::sqrt(std::abs(scale_x * scale_y)));
    const float radius_limit = std::max(0.0F, std::min(half_width, half_height));
    std::array<float, 4U> radii{};
    for (std::size_t index = 0U; index < radii.size(); ++index) {
        radii[index] =
            std::min(static_cast<float>(effect.radii[index] * radius_scale), radius_limit);
    }
    for (std::uint32_t y = region.top; y < region.bottom; ++y) {
        for (std::uint32_t x = region.left; x < region.right; ++x) {
            const float effect_mask =
                1.0F - smoothstep(-1.0F, 1.0F,
                                  rounded_box_sdf(static_cast<float>(x) + 0.5F - center_x,
                                                  static_cast<float>(y) + 0.5F - center_y,
                                                  half_width, half_height, radii));
            const float clip_mask = rounded_clip_coverage(
                effect.rounded_clips,
                static_cast<float>((static_cast<double>(x) + 0.5) / scale_x),
                static_cast<float>((static_cast<double>(y) + 0.5) / scale_y),
                static_cast<float>(1.0 / scale_x),
                static_cast<float>(1.0 / scale_y)
            );
            const std::size_t pixel = (static_cast<std::size_t>(y) * width + x) * 4U;
            if (effect.composite != host::LayerComposite::source_over) {
                Rgba layer{};
                Rgba under{};
                for (std::size_t index = 0U; index < 4U; ++index) {
                    layer[index] = static_cast<float>(source(x, y, index)) / 255.0F *
                                   static_cast<float>(effect.opacity);
                    under[index] = static_cast<float>(destination[pixel + index]) / 255.0F;
                }
                const Rgba combined =
                    layer_composite(layer, under, static_cast<std::uint32_t>(effect.composite));
                const float mixed = effect_mask * clip_mask;
                for (std::size_t index = 0U; index < 4U; ++index)
                    destination[pixel + index] =
                        channel(under[index] + (combined[index] - under[index]) * mixed);
                continue;
            }
            const float coverage =
                effect_mask * clip_mask * static_cast<float>(effect.opacity);
            const float alpha = static_cast<float>(source(x, y, 3U)) / 255.0F * coverage;
            for (std::size_t channel_index = 0U; channel_index < 3U; ++channel_index) {
                const float source_value = static_cast<float>(source(x, y, channel_index));
                const float destination_value =
                    static_cast<float>(destination[pixel + channel_index]);
                destination[pixel + channel_index] = static_cast<std::uint8_t>(std::clamp(
                    std::lround(source_value * coverage + destination_value * (1.0F - alpha)), 0L,
                    255L));
            }
            const float destination_alpha = static_cast<float>(destination[pixel + 3U]) / 255.0F;
            destination[pixel + 3U] = channel(alpha + destination_alpha * (1.0F - alpha));
        }
    }
}

} // namespace

SoftwareRenderer::SoftwareRenderer() = default;

void SoftwareRenderer::resize(const std::uint32_t framebuffer_width,
                              const std::uint32_t framebuffer_height, const double logical_width,
                              const double logical_height) {
    if (framebuffer_width == 0U || framebuffer_height == 0U || !std::isfinite(logical_width) ||
        !std::isfinite(logical_height) || logical_width <= 0.0 || logical_height <= 0.0) {
        throw std::invalid_argument("headless renderer requires positive finite dimensions");
    }
    const std::uint64_t byte_count =
        static_cast<std::uint64_t>(framebuffer_width) * framebuffer_height * 4U;
    if (byte_count > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("headless framebuffer exceeds host address space");
    }
    width_ = framebuffer_width;
    height_ = framebuffer_height;
    logical_width_ = logical_width;
    logical_height_ = logical_height;
    pixels_.resize(static_cast<std::size_t>(byte_count));
    cached_effects_.clear();
}

void SoftwareRenderer::set_clear_color(const std::array<std::uint8_t, 4U> rgba) noexcept {
    clear_ = rgba;
}

void SoftwareRenderer::declare_material(std::string_view, std::string_view) {}

void SoftwareRenderer::declare_effect_pass(const std::string_view effect_id,
                                           const std::uint32_t index, const std::uint32_t kind,
                                           const double radius, const std::uint32_t downsample,
                                           const std::uint32_t radius_parameter,
                                           const std::uint32_t downsample_parameter,
                                           std::string_view) {
    std::vector<EffectPass>& passes = effects_[std::string(effect_id)];
    if (passes.size() <= index)
        passes.resize(static_cast<std::size_t>(index) + 1U);
    passes[index] = EffectPass{
        kind, radius, std::clamp(downsample, 1U, 8U), radius_parameter, downsample_parameter,
    };
    cached_effects_.clear();
}

void SoftwareRenderer::apply(const host::ResourceOperation& operation) {
    if (operation.kind == 2U) {
        textures_.erase(operation.texture);
        vector_glyphs_.erase(operation.texture);
        return;
    }
    if (operation.kind == host::resource_vector_glyphs) {
        VectorGlyphs& glyphs = vector_glyphs_[operation.texture];
        const host::VectorGlyphBuffers& append = operation.vector_glyphs;
        // Each buffer takes its part where it starts; a part starting at zero replaces it.
        const auto write = [](auto& target, const std::uint32_t first, const std::size_t element,
                              const std::vector<std::uint8_t>& bytes) {
            using Value = typename std::decay_t<decltype(target)>::value_type;
            const std::size_t per = element / sizeof(Value);
            const std::size_t offset = static_cast<std::size_t>(first) * per;
            if (offset > target.size())
                throw std::invalid_argument("vector glyph append leaves a gap in its buffer");
            target.resize(offset + bytes.size() / sizeof(Value));
            if (!bytes.empty())
                std::memcpy(target.data() + offset, bytes.data(), bytes.size());
        };
        write(glyphs.records, append.first_record, host::vector_glyph_record_bytes,
              append.records);
        write(glyphs.segments, append.first_segment, host::vector_glyph_segment_bytes,
              append.segments);
        write(glyphs.bands, append.first_band, host::vector_glyph_band_bytes, append.bands);
        return;
    }
    if (operation.kind == 0U) {
        // Glyph pages (format 0) sample texel-exact; shape masks (2) and MSDF (1) bilinearly.
        const std::uint32_t channels = host::texture_format_rgba(operation.format) ? 4U : 1U;
        const std::uint64_t size =
            static_cast<std::uint64_t>(operation.width) * operation.height * channels;
        if (size > std::numeric_limits<std::size_t>::max()) {
            throw std::length_error("headless atlas exceeds host address space");
        }
        textures_.insert_or_assign(operation.texture,
                                   Texture{
                                       operation.width,
                                       operation.height,
                                       channels,
                                       host::texture_format_exact(operation.format) ? 0U : 1U,
                                       std::vector<std::uint8_t>(static_cast<std::size_t>(size)),
                                   });
        return;
    }
    if (operation.kind == host::resource_image) {
        // The core decoded the image: straight-alpha RGBA8 rows, sampled as stored.
        textures_.insert_or_assign(operation.texture, Texture{
                                                          operation.width,
                                                          operation.height,
                                                          4U,
                                                          operation.sampling,
                                                          operation.bytes,
                                                      });
        return;
    }
    if (operation.kind == host::resource_raster) {
        // The reference rasterizer, which the GPU backends reproduce texel for texel.
        auto found = textures_.find(operation.texture);
        if (found == textures_.end())
            throw std::invalid_argument("headless raster job has no live texture");
        Texture& texture = found->second;
        const std::uint32_t channels = host::texture_format_rgba(operation.format) ? 4U : 1U;
        if (texture.channels != channels ||
            static_cast<std::uint64_t>(operation.x) + operation.width > texture.width ||
            static_cast<std::uint64_t>(operation.y) + operation.height > texture.height) {
            throw std::invalid_argument("headless raster job differs from its texture descriptor");
        }
        const std::size_t row_bytes = static_cast<std::size_t>(texture.width) * channels;
        const std::size_t origin =
            static_cast<std::size_t>(operation.y) * row_bytes +
            static_cast<std::size_t>(operation.x) * channels;
        host::rasterize_job(operation.raster, operation.format, operation.width, operation.height,
                            std::span(texture.pixels).subspan(origin), row_bytes);
        return;
    }
    if (operation.kind != 1U) {
        throw std::invalid_argument("headless renderer received an unknown resource operation");
    }
    auto found = textures_.find(operation.texture);
    if (found == textures_.end()) {
        throw std::invalid_argument("headless atlas upload has no live texture");
    }
    Texture& texture = found->second;
    const std::uint32_t channels = host::texture_format_rgba(operation.format) ? 4U : 1U;
    const std::uint64_t right = static_cast<std::uint64_t>(operation.x) + operation.width;
    const std::uint64_t bottom = static_cast<std::uint64_t>(operation.y) + operation.height;
    const std::uint64_t expected =
        static_cast<std::uint64_t>(operation.width) * operation.height * channels;
    if (texture.channels != channels || right > texture.width || bottom > texture.height ||
        expected != operation.bytes.size()) {
        throw std::invalid_argument("headless atlas upload differs from its texture descriptor");
    }
    for (std::uint32_t row = 0U; row < operation.height; ++row) {
        const std::size_t source = static_cast<std::size_t>(row) * operation.width * channels;
        const std::size_t destination =
            (static_cast<std::size_t>(operation.y + row) * texture.width + operation.x) * channels;
        std::copy_n(operation.bytes.begin() + static_cast<std::ptrdiff_t>(source),
                    static_cast<std::size_t>(operation.width) * channels,
                    texture.pixels.begin() + static_cast<std::ptrdiff_t>(destination));
    }
}

void SoftwareRenderer::consume_resources(const host::RenderPacket& packet) {
    for (const host::ResourceOperation& operation : packet.resources)
        apply(operation);
}

void SoftwareRenderer::draw(const host::DrawBatch& batch, const host::RenderPacket& packet) {
    if (batch.index_count == 0U)
        return;
    // The core already adapted blend and clip coverage to the rounded clips; the pixel loop
    // below only branches on the two enums, exactly as the GPU pipelines are configured.
    const host::DrawBlend blend = batch.blend;
    const host::ClipCoverage coverage_mode = batch.clip_coverage;
    const std::string& material = packet.materials[batch.material];
    if (material != "strata:unified_ui" &&
        std::ranges::find(material_fallbacks_, material) == material_fallbacks_.end()) {
        material_fallbacks_.push_back(material);
    }
    const Texture* texture = nullptr;
    const VectorGlyphs* vector_glyphs = nullptr;
    if (batch.texture.has_value()) {
        const auto found = textures_.find(*batch.texture);
        if (found != textures_.end())
            texture = &found->second;
        if (const auto vectors = vector_glyphs_.find(*batch.texture); vectors != vector_glyphs_.end())
            vector_glyphs = &vectors->second;
    }
    // One glyph of the batch's vector glyph buffers, as the GPU reads it; empty if out of range.
    const auto vector_glyph = [vector_glyphs](const std::uint32_t index) {
        host::VectorGlyphView view;
        if (vector_glyphs == nullptr ||
            (static_cast<std::size_t>(index) + 1U) * host::vector_glyph_record_bytes >
                vector_glyphs->records.size()) {
            return view;
        }
        std::array<std::uint32_t, 8U> fields{};
        std::memcpy(fields.data(),
                    vector_glyphs->records.data() + index * host::vector_glyph_record_bytes,
                    host::vector_glyph_record_bytes);
        view.left = std::bit_cast<float>(fields[0U]);
        view.bottom = std::bit_cast<float>(fields[1U]);
        view.right = std::bit_cast<float>(fields[2U]);
        view.top = std::bit_cast<float>(fields[3U]);
        const std::size_t segment_base = fields[4U];
        const std::size_t band_base = fields[5U];
        const std::uint32_t band_count = fields[6U];
        const std::size_t segment_count = fields[7U];
        const std::span<const float> segments(vector_glyphs->segments);
        const std::span<const std::uint32_t> bands(vector_glyphs->bands);
        if (band_count == 0U || band_base + band_count >= bands.size())
            return view;
        const std::size_t band_entries = band_count + 1U + bands[band_base + band_count];
        if ((segment_base + segment_count) * 4U > segments.size() ||
            band_base + band_entries > bands.size()) {
            return view;
        }
        view.band_count = band_count;
        view.segments = segments.subspan(segment_base * 4U, segment_count * 4U);
        view.bands = bands.subspan(band_base, band_entries);
        return view;
    };
    // `filtered` blends texels bilinearly even on a texel-exact page (glyphs drawn off one to one).
    const auto texture_sample = [texture](const float u, const float v,
                                          const bool filtered = false) noexcept {
        if (texture == nullptr || texture->pixels.empty())
            return Color{};
        const auto texel = [texture](const std::int64_t x, const std::int64_t y) noexcept {
            const std::uint32_t clipped_x = static_cast<std::uint32_t>(
                std::clamp<std::int64_t>(x, 0, static_cast<std::int64_t>(texture->width) - 1));
            const std::uint32_t clipped_y = static_cast<std::uint32_t>(
                std::clamp<std::int64_t>(y, 0, static_cast<std::int64_t>(texture->height) - 1));
            const std::size_t offset =
                (static_cast<std::size_t>(clipped_y) * texture->width + clipped_x) *
                texture->channels;
            if (texture->channels == 1U) {
                const float value = static_cast<float>(texture->pixels[offset]) / 255.0F;
                return Color{value, 0.0F, 0.0F, 1.0F};
            }
            return Color{
                static_cast<float>(texture->pixels[offset]) / 255.0F,
                static_cast<float>(texture->pixels[offset + 1U]) / 255.0F,
                static_cast<float>(texture->pixels[offset + 2U]) / 255.0F,
                static_cast<float>(texture->pixels[offset + 3U]) / 255.0F,
            };
        };
        if (texture->sampling == 0U && !filtered) {
            return texel(
                static_cast<std::int64_t>(std::floor(u * static_cast<float>(texture->width))),
                static_cast<std::int64_t>(std::floor(v * static_cast<float>(texture->height))));
        }
        const float x = u * static_cast<float>(texture->width) - 0.5F;
        const float y = v * static_cast<float>(texture->height) - 0.5F;
        const std::int64_t x0 = static_cast<std::int64_t>(std::floor(x));
        const std::int64_t y0 = static_cast<std::int64_t>(std::floor(y));
        const float tx = x - static_cast<float>(x0);
        const float ty = y - static_cast<float>(y0);
        const Color top_left_value = texel(x0, y0);
        const Color top_right_value = texel(x0 + 1, y0);
        const Color bottom_left_value = texel(x0, y0 + 1);
        const Color bottom_right_value = texel(x0 + 1, y0 + 1);
        const auto mix = [tx, ty](const float top_left_channel, const float top_right_channel,
                                  const float bottom_left_channel,
                                  const float bottom_right_channel) noexcept {
            const float top = top_left_channel + (top_right_channel - top_left_channel) * tx;
            const float bottom =
                bottom_left_channel + (bottom_right_channel - bottom_left_channel) * tx;
            return top + (bottom - top) * ty;
        };
        return Color{
            mix(top_left_value.red, top_right_value.red, bottom_left_value.red,
                bottom_right_value.red),
            mix(top_left_value.green, top_right_value.green, bottom_left_value.green,
                bottom_right_value.green),
            mix(top_left_value.blue, top_right_value.blue, bottom_left_value.blue,
                bottom_right_value.blue),
            mix(top_left_value.alpha, top_right_value.alpha, bottom_left_value.alpha,
                bottom_right_value.alpha),
        };
    };

    const float scale_x = static_cast<float>(static_cast<double>(width_) / logical_width_);
    const float scale_y = static_cast<float>(static_cast<double>(height_) / logical_height_);
    const std::uint64_t scissor_right_wide =
        static_cast<std::uint64_t>(batch.scissor.x) + batch.scissor.width;
    const std::uint64_t scissor_bottom_wide =
        static_cast<std::uint64_t>(batch.scissor.y) + batch.scissor.height;
    const std::uint32_t scissor_left = std::min(batch.scissor.x, width_);
    const std::uint32_t scissor_top = std::min(batch.scissor.y, height_);
    const std::uint32_t scissor_right =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(scissor_right_wide, width_));
    const std::uint32_t scissor_bottom =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(scissor_bottom_wide, height_));
    if (scissor_left >= scissor_right || scissor_top >= scissor_bottom)
        return;

    const std::size_t first = batch.first_index;
    const std::size_t end = first + batch.index_count;
    for (std::size_t index = first; index + 2U < end; index += 3U) {
        const std::size_t ia = static_cast<std::size_t>(batch.base_vertex) + packet.indices[index];
        const std::size_t ib =
            static_cast<std::size_t>(batch.base_vertex) + packet.indices[index + 1U];
        const std::size_t ic =
            static_cast<std::size_t>(batch.base_vertex) + packet.indices[index + 2U];
        Vertex a = read_vertex(packet.vertices, ia, scale_x, scale_y, packet.groups);
        Vertex b = read_vertex(packet.vertices, ib, scale_x, scale_y, packet.groups);
        Vertex c = read_vertex(packet.vertices, ic, scale_x, scale_y, packet.groups);
        double exact_area = edge_function(a, b, c.x, c.y);
        if (exact_area == 0.0)
            continue;
        if (exact_area < 0.0) {
            std::swap(b, c);
            exact_area = -exact_area;
        }
        const auto area = static_cast<float>(exact_area);
        const std::int64_t minimum_x = std::max<std::int64_t>(
            scissor_left, static_cast<std::int64_t>(std::floor(std::min({a.x, b.x, c.x}))));
        const std::int64_t minimum_y = std::max<std::int64_t>(
            scissor_top, static_cast<std::int64_t>(std::floor(std::min({a.y, b.y, c.y}))));
        const std::int64_t maximum_x = std::min<std::int64_t>(
            scissor_right, static_cast<std::int64_t>(std::ceil(std::max({a.x, b.x, c.x}))));
        const std::int64_t maximum_y = std::min<std::int64_t>(
            scissor_bottom, static_cast<std::int64_t>(std::ceil(std::max({a.y, b.y, c.y}))));
        if (minimum_x >= maximum_x || minimum_y >= maximum_y)
            continue;

        const float inverse_area = 1.0F / area;
        const float du1 = b.u - a.u;
        const float du2 = c.u - a.u;
        const float dv1 = b.v - a.v;
        const float dv2 = c.v - a.v;
        const float dx1 = b.x - a.x;
        const float dx2 = c.x - a.x;
        const float dy1 = b.y - a.y;
        const float dy2 = c.y - a.y;
        const float du_dx = (du1 * dy2 - du2 * dy1) * inverse_area;
        const float du_dy = (dx1 * du2 - dx2 * du1) * inverse_area;
        const float dv_dx = (dv1 * dy2 - dv2 * dy1) * inverse_area;
        const float dv_dy = (dx1 * dv2 - dx2 * dv1) * inverse_area;
        const float fwidth_u = std::abs(du_dx) + std::abs(du_dy);
        const float fwidth_v = std::abs(dv_dx) + std::abs(dv_dy);
        // Whether a glyph's texels land one to one on pixels (strataGlyphTexel): exactly sampled
        // then, blended bilinearly otherwise.
        const bool glyph_one_to_one =
            texture == nullptr ||
            (std::abs(fwidth_u * static_cast<float>(texture->width) - 1.0F) < 0.01F &&
             std::abs(fwidth_v * static_cast<float>(texture->height) - 1.0F) < 0.01F);
        const bool include_ab = top_left(a, b);
        const bool include_bc = top_left(b, c);
        const bool include_ca = top_left(c, a);

        for (std::int64_t y = minimum_y; y < maximum_y; ++y) {
            for (std::int64_t x = minimum_x; x < maximum_x; ++x) {
                const double sample_x = static_cast<double>(x) + 0.5;
                const double sample_y = static_cast<double>(y) + 0.5;
                const double edge_ab = edge_function(a, b, sample_x, sample_y);
                const double edge_bc = edge_function(b, c, sample_x, sample_y);
                const double edge_ca = edge_function(c, a, sample_x, sample_y);
                if (!covered(edge_ab, include_ab) || !covered(edge_bc, include_bc) ||
                    !covered(edge_ca, include_ca)) {
                    continue;
                }
                const float wa = static_cast<float>(edge_bc) * inverse_area;
                const float wb = static_cast<float>(edge_ca) * inverse_area;
                const float wc = static_cast<float>(edge_ab) * inverse_area;
                const float u = a.u * wa + b.u * wb + c.u * wc;
                const float v = a.v * wa + b.v * wb + c.v * wc;
                Color source = interpolate_color(a, b, c, wa, wb, wc);
                const std::array<float, 16U> data = interpolate_data(a, b, c, wa, wb, wc);
                const int mode = static_cast<int>(std::floor(data[14U] + 0.5F));
                // Subpixel text's coverage per colour channel (mode 12); white otherwise.
                std::array<float, 3U> subpixel{1.0F, 1.0F, 1.0F};
                if (mode == 1 || mode == 11) {
                    Color sampled = texture_sample(u, v);
                    if (mode == 11 && data[4U] > 0.0F)
                        host::tone_color(sampled.red, sampled.green, sampled.blue, data[4U]);
                    source.red *= sampled.red;
                    source.green *= sampled.green;
                    source.blue *= sampled.blue;
                    source.alpha *= sampled.alpha;
                } else if (mode == 2 || mode == 3 || mode == 6) {
                    const float shape_width = std::max(data[0U], 1.0F);
                    const float shape_height = std::max(data[1U], 1.0F);
                    std::array<float, 4U> radii{
                        data[4U],
                        data[5U],
                        data[6U],
                        data[7U],
                    };
                    const float px = (u - 0.5F) * shape_width;
                    const float py = (v - 0.5F) * shape_height;
                    for (float& radius : radii)
                        radius = std::max(radius, 0.0F);
                    // The footprint a device pixel spans across a rounded box's edge, as the
                    // pixel stage measures it from the local position's derivatives.
                    const auto footprint = [&](const host::ShapeDistance& box, const float width,
                                               const float height) {
                        return host::pixel_footprint(box.normal_x, box.normal_y, du_dx * width,
                                                     dv_dx * height, du_dy * width,
                                                     dv_dy * height);
                    };
                    if (mode == 2) {
                        const float softness = data[2U];
                        const float border_width = std::max(data[3U], 0.0F);
                        const host::ShapeDistance box = host::rounded_box_distance(
                            px, py, shape_width * 0.5F, shape_height * 0.5F, radii.data());
                        const float distance = box.distance;
                        const float pixel = footprint(box, shape_width, shape_height);
                        const float alpha = host::rounded_edge_coverage(distance, pixel, softness);
                        const float border_mix = saturate(
                            (1.0F - host::rounded_edge_coverage(distance + border_width, pixel,
                                                                softness)) *
                            (border_width >= 0.001F ? 1.0F : 0.0F));
                        source.red += (data[8U] - source.red) * border_mix;
                        source.green += (data[9U] - source.green) * border_mix;
                        source.blue += (data[10U] - source.blue) * border_mix;
                        source.alpha += (data[11U] - source.alpha) * border_mix;
                        source.alpha *= alpha;
                    } else if (mode == 3) {
                        const float line_width = std::max(data[2U], 0.0F);
                        const float softness = data[3U];
                        const host::ShapeDistance box = host::rounded_box_distance(
                            px, py, shape_width * 0.5F, shape_height * 0.5F, radii.data());
                        const float pixel = footprint(box, shape_width, shape_height);
                        const float outer =
                            host::rounded_edge_coverage(box.distance, pixel, softness);
                        const float inner =
                            host::rounded_edge_coverage(box.distance + line_width, pixel, softness);
                        source.alpha *= saturate(outer - inner);
                    } else {
                        const float radius = std::max(data[2U], 0.5F);
                        const float spread = data[3U];
                        const float quad_width = std::max(data[8U], shape_width);
                        const float quad_height = std::max(data[9U], shape_height);
                        const float shadow_x = (u - 0.5F) * quad_width;
                        const float shadow_y = (v - 0.5F) * quad_height;
                        const float distance = rounded_box_sdf(
                            shadow_x,
                            shadow_y,
                            shape_width * 0.5F,
                            shape_height * 0.5F,
                            radii
                        );
                        // Only the source silhouette, `offset` behind the shadow, stays uncovered.
                        const float source_distance = rounded_box_sdf(
                            shadow_x + data[10U],
                            shadow_y + data[11U],
                            shape_width * 0.5F,
                            shape_height * 0.5F,
                            radii
                        );
                        const host::ShapeDistance source_box = host::rounded_box_distance(
                            shadow_x + data[10U], shadow_y + data[11U], shape_width * 0.5F,
                            shape_height * 0.5F, radii.data());
                        const float outside =
                            1.0F - host::rounded_edge_coverage(
                                       source_distance,
                                       footprint(source_box, quad_width, quad_height), 1.0F);
                        const float falloff = 1.0F - smoothstep(
                            0.0F,
                            radius,
                            std::max(distance - spread, 0.0F)
                        );
                        source.alpha *= outside * falloff;
                    }
                } else if (mode == 4) {
                    source.alpha *= host::text_coverage(texture_sample(u, v, !glyph_one_to_one).red,
                                                        source.red, source.green, source.blue);
                } else if (mode == 12) {
                    const Color sampled = texture_sample(u, v, !glyph_one_to_one);
                    subpixel = {
                        host::text_coverage(sampled.red, source.red, source.green, source.blue),
                        host::text_coverage(sampled.green, source.red, source.green, source.blue),
                        host::text_coverage(sampled.blue, source.red, source.green, source.blue),
                    };
                    source.alpha *= std::max({subpixel[0U], subpixel[1U], subpixel[2U]});
                } else if (mode == 13 || mode == 14) {
                    // Vector text, as vectorTextCoverage measures it: where the glyph shows
                    // upright at the size it was laid out for, its variant fitted to the pixel
                    // grid (value 1, when it has one), else its outline (value 0).
                    const double laid_out = std::hypot(data[6U], data[7U]) * scale_y;
                    const double shown = dv_dy != 0.0F ? 1.0 / std::abs(dv_dy) : 0.0;
                    const bool native = std::abs(du_dy) < 1.0e-6F && std::abs(dv_dx) < 1.0e-6F &&
                                        std::abs(shown - laid_out) <= laid_out * 1.0e-3;
                    const float record = native && data[1U] >= 0.0F ? data[1U] : data[0U];
                    const std::uint32_t order =
                        mode == 14 ? static_cast<std::uint32_t>(std::floor(data[2U] + 0.5F)) : 0U;
                    const std::array<double, 4U> measured = host::vector_text_coverage(
                        vector_glyph(static_cast<std::uint32_t>(std::floor(record + 0.5F))),
                        host::VectorSample{sample_x, sample_y, u, v, du_dx, dv_dx, du_dy, dv_dy},
                        order);
                    if (order == 0U) {
                        source.alpha *= host::text_coverage(static_cast<float>(measured[3U]),
                                                            source.red, source.green, source.blue);
                    } else {
                        subpixel = {
                            host::text_coverage(static_cast<float>(measured[0U]), source.red,
                                                source.green, source.blue),
                            host::text_coverage(static_cast<float>(measured[1U]), source.red,
                                                source.green, source.blue),
                            host::text_coverage(static_cast<float>(measured[2U]), source.red,
                                                source.green, source.blue),
                        };
                        source.alpha *= std::max({subpixel[0U], subpixel[1U], subpixel[2U]});
                    }
                } else if (mode == 5) {
                    const Color sampled = texture_sample(u, v);
                    const float median =
                        std::max(std::min(sampled.red, sampled.green),
                                 std::min(std::max(sampled.red, sampled.green), sampled.blue));
                    const float signed_distance = median - 0.5F;
                    const float texture_width =
                        texture != nullptr ? static_cast<float>(texture->width) : 1.0F;
                    const float texture_height =
                        texture != nullptr ? static_cast<float>(texture->height) : 1.0F;
                    const float unit_x =
                        std::max(data[0U], 0.0001F) / std::max(texture_width, 1.0F);
                    const float unit_y =
                        std::max(data[0U], 0.0001F) / std::max(texture_height, 1.0F);
                    const float screen_u = 1.0F / std::max(fwidth_u, 0.000001F);
                    const float screen_v = 1.0F / std::max(fwidth_v, 0.000001F);
                    const float range =
                        std::max(0.5F * (unit_x * screen_u + unit_y * screen_v), 1.0F);
                    source.alpha *= saturate(signed_distance * range + 0.5F);
                } else if (mode == host::draw_mode_coverage_mask) {
                    float coverage = texture_sample(u, v).red;
                    if (data[6U] > 0.5F) {
                        const float band = host::gradient_band_factor(data.data());
                        if (band < 0.0F)
                            coverage = 0.0F;
                        const float factor = std::max(band, 0.0F);
                        source.red += (data[8U] - source.red) * factor;
                        source.green += (data[9U] - source.green) * factor;
                        source.blue += (data[10U] - source.blue) * factor;
                        source.alpha += (data[11U] - source.alpha) * factor;
                    }
                    source.alpha *= coverage;
                } else if (mode >= host::draw_mode_ellipse && mode <= host::draw_mode_segment) {
                    source.alpha *= host::analytic_shape_coverage(
                        mode, u, v, du_dx, dv_dx, du_dy, dv_dy, data.data());
                }
                source.alpha *= data[15U];
                if (source.alpha <= 0.000001F)
                    continue;
                const float clip_mask = rounded_clip_coverage(
                    batch.rounded_clips,
                    (static_cast<float>(x) + 0.5F) / scale_x,
                    (static_cast<float>(y) + 0.5F) / scale_y,
                    1.0F / scale_x,
                    1.0F / scale_y
                );
                switch (coverage_mode) {
                case host::ClipCoverage::multiply:
                    source.red *= clip_mask;
                    source.green *= clip_mask;
                    source.blue *= clip_mask;
                    source.alpha = clip_mask;
                    break;
                case host::ClipCoverage::hard:
                    if (clip_mask < 0.5F)
                        continue;
                    break;
                case host::ClipCoverage::premultiplied_alpha:
                    source.red *= clip_mask;
                    source.green *= clip_mask;
                    source.blue *= clip_mask;
                    source.alpha *= clip_mask;
                    break;
                case host::ClipCoverage::straight_alpha:
                    source.alpha *= clip_mask;
                    break;
                }
                if (source.alpha <= 0.000001F)
                    continue;

                const std::size_t pixel =
                    (static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)) * 4U;
                const Color destination{
                    static_cast<float>(pixels_[pixel]) / 255.0F,
                    static_cast<float>(pixels_[pixel + 1U]) / 255.0F,
                    static_cast<float>(pixels_[pixel + 2U]) / 255.0F,
                    static_cast<float>(pixels_[pixel + 3U]) / 255.0F,
                };
                Color output;
                if (blend == host::DrawBlend::opaque) {
                    output = source;
                } else if (blend == host::DrawBlend::straight_alpha) {
                    const float inverse = 1.0F - source.alpha;
                    output = Color{
                        source.red * source.alpha + destination.red * inverse,
                        source.green * source.alpha + destination.green * inverse,
                        source.blue * source.alpha + destination.blue * inverse,
                        source.alpha + destination.alpha * inverse,
                    };
                } else if (blend == host::DrawBlend::subpixel_text) {
                    // Each colour channel by its own coverage: the alpha scaled from the
                    // strongest channel to that one.
                    const float strongest =
                        std::max({subpixel[0U], subpixel[1U], subpixel[2U], 0.000001F});
                    const float red = source.alpha * subpixel[0U] / strongest;
                    const float green = source.alpha * subpixel[1U] / strongest;
                    const float blue = source.alpha * subpixel[2U] / strongest;
                    output = Color{
                        source.red * red + destination.red * (1.0F - red),
                        source.green * green + destination.green * (1.0F - green),
                        source.blue * blue + destination.blue * (1.0F - blue),
                        source.alpha + destination.alpha * (1.0F - source.alpha),
                    };
                } else if (blend == host::DrawBlend::premultiplied_alpha) {
                    const float inverse = 1.0F - source.alpha;
                    output = Color{
                        source.red + destination.red * inverse,
                        source.green + destination.green * inverse,
                        source.blue + destination.blue * inverse,
                        source.alpha + destination.alpha * inverse,
                    };
                } else if (blend == host::DrawBlend::additive) {
                    output = Color{
                        source.red * source.alpha + destination.red,
                        source.green * source.alpha + destination.green,
                        source.blue * source.alpha + destination.blue,
                        source.alpha + destination.alpha * (1.0F - source.alpha),
                    };
                } else if (blend == host::DrawBlend::multiply) {
                    output = Color{
                        source.red * destination.red,
                        source.green * destination.green,
                        source.blue * destination.blue,
                        source.alpha + destination.alpha * (1.0F - source.alpha),
                    };
                } else {
                    output = Color{
                        source.red * destination.red +
                            destination.red * (1.0F - source.alpha),
                        source.green * destination.green +
                            destination.green * (1.0F - source.alpha),
                        source.blue * destination.blue +
                            destination.blue * (1.0F - source.alpha),
                        source.alpha + destination.alpha * (1.0F - source.alpha),
                    };
                }
                pixels_[pixel] = channel(output.red);
                pixels_[pixel + 1U] = channel(output.green);
                pixels_[pixel + 2U] = channel(output.blue);
                pixels_[pixel + 3U] = channel(output.alpha);
            }
        }
    }
}

void SoftwareRenderer::blur(const host::BlurBatch& batch, const double radius_x,
                            const double radius_y) {
    if (batch.radius <= 0.0 || batch.width <= 0.0 || batch.height <= 0.0)
        return;
    const double scale_x = static_cast<double>(width_) / logical_width_;
    const double scale_y = static_cast<double>(height_) / logical_height_;
    const auto leading = [](const double value, const std::uint32_t maximum) {
        return static_cast<std::uint32_t>(
            std::clamp(std::floor(value), 0.0, static_cast<double>(maximum)));
    };
    const auto trailing = [](const double value, const std::uint32_t maximum) {
        return static_cast<std::uint32_t>(
            std::clamp(std::ceil(value), 0.0, static_cast<double>(maximum)));
    };
    std::uint32_t left = leading(batch.x * scale_x, width_);
    std::uint32_t top = leading(batch.y * scale_y, height_);
    std::uint32_t right = trailing((batch.x + batch.width) * scale_x, width_);
    std::uint32_t bottom = trailing((batch.y + batch.height) * scale_y, height_);
    const std::uint64_t clip_right = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(batch.scissor.x) + batch.scissor.width, width_);
    const std::uint64_t clip_bottom = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(batch.scissor.y) + batch.scissor.height, height_);
    left = std::max(left, std::min(batch.scissor.x, width_));
    top = std::max(top, std::min(batch.scissor.y, height_));
    right = std::min(right, static_cast<std::uint32_t>(clip_right));
    bottom = std::min(bottom, static_cast<std::uint32_t>(clip_bottom));
    if (left >= right || top >= bottom)
        return;
    std::vector<std::uint8_t> clipped_original;
    const std::uint32_t clipped_width = right - left;
    if (!batch.rounded_clips.empty()) {
        clipped_original.resize(
            static_cast<std::size_t>(clipped_width) * (bottom - top) * 4U
        );
        for (std::uint32_t row = top; row < bottom; ++row) {
            const std::size_t source =
                (static_cast<std::size_t>(row) * width_ + left) * 4U;
            const std::size_t destination =
                static_cast<std::size_t>(row - top) * clipped_width * 4U;
            std::copy_n(
                pixels_.begin() + static_cast<std::ptrdiff_t>(source),
                static_cast<std::size_t>(clipped_width) * 4U,
                clipped_original.begin() + static_cast<std::ptrdiff_t>(destination)
            );
        }
    }
    const std::uint32_t downsample = std::clamp(batch.downsample, 1U, 8U);
    if (downsample > 1U) {
        blur_downsampled(batch, left, top, right, bottom, downsample, clipped_original, radius_x,
                         radius_y);
        return;
    }
    const double physical_radius = std::clamp(batch.radius * std::max(scale_x, scale_y), 0.5, 32.0);
    const std::uint32_t radius = static_cast<std::uint32_t>(std::ceil(physical_radius));
    const std::uint32_t source_left = left > radius ? left - radius : 0U;
    const std::uint32_t source_top = top > radius ? top - radius : 0U;
    const std::uint32_t source_right = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(right) + radius, width_));
    const std::uint32_t source_bottom = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(bottom) + radius, height_));
    const std::uint32_t source_width = source_right - source_left;
    const std::uint32_t source_height = source_bottom - source_top;
    // Each way may blur by its own radius (a layer's blur); a radius of zero leaves that way be,
    // as a GPU pass under one half samples only its centre.
    const auto directional = [&](const double logical) {
        return logical < 0.0
                   ? physical_radius
                   : (logical <= 0.0 ? 0.0
                                     : std::clamp(logical * std::max(scale_x, scale_y), 0.5, 32.0));
    };
    const std::vector<double> weights_x = blur_weights(directional(radius_x));
    const std::vector<double> weights_y = blur_weights(directional(radius_y));
    const auto reach_x = static_cast<std::int64_t>(weights_x.size()) - 1;
    const auto reach_y = static_cast<std::int64_t>(weights_y.size()) - 1;
    std::vector<std::uint8_t> horizontal(static_cast<std::size_t>(source_width) * source_height *
                                         4U);
    for (std::uint32_t row = 0U; row < source_height; ++row) {
        for (std::uint32_t column = 0U; column < source_width; ++column) {
            const std::size_t target = (static_cast<std::size_t>(row) * source_width + column) * 4U;
            std::array<double, 4U> sums{};
            double total_weight = 0.0;
            for (std::int64_t offset = -reach_x; offset <= reach_x; ++offset) {
                const std::uint32_t sample_column = static_cast<std::uint32_t>(
                    std::clamp<std::int64_t>(static_cast<std::int64_t>(column) + offset, 0,
                                             static_cast<std::int64_t>(source_width) - 1));
                const double weight = weights_x[static_cast<std::size_t>(std::abs(offset))];
                const std::size_t source = (static_cast<std::size_t>(source_top + row) * width_ +
                                            source_left + sample_column) *
                                           4U;
                for (std::size_t component = 0U; component < 4U; ++component) {
                    sums[component] += static_cast<double>(pixels_[source + component]) * weight;
                }
                total_weight += weight;
            }
            for (std::size_t component = 0U; component < 4U; ++component) {
                horizontal[target + component] = static_cast<std::uint8_t>(
                    std::clamp(std::lround(sums[component] / total_weight), 0L, 255L));
            }
        }
    }
    for (std::uint32_t column = left; column < right; ++column) {
        const std::uint32_t local_x = column - source_left;
        for (std::uint32_t row = top; row < bottom; ++row) {
            const std::uint32_t local_y = row - source_top;
            const std::size_t target = (static_cast<std::size_t>(row) * width_ + column) * 4U;
            std::array<double, 4U> sums{};
            double total_weight = 0.0;
            for (std::int64_t offset = -reach_y; offset <= reach_y; ++offset) {
                const std::uint32_t sample_row = static_cast<std::uint32_t>(
                    std::clamp<std::int64_t>(static_cast<std::int64_t>(local_y) + offset, 0,
                                             static_cast<std::int64_t>(source_height) - 1));
                const double weight = weights_y[static_cast<std::size_t>(std::abs(offset))];
                const std::size_t source =
                    (static_cast<std::size_t>(sample_row) * source_width + local_x) * 4U;
                for (std::size_t component = 0U; component < 4U; ++component) {
                    sums[component] += static_cast<double>(horizontal[source + component]) * weight;
                }
                total_weight += weight;
            }
            const float clip_coverage = clipped_original.empty()
                ? 1.0F
                : rounded_clip_coverage(
                      batch.rounded_clips,
                      static_cast<float>(
                          (static_cast<double>(column) + 0.5) / scale_x
                      ),
                      static_cast<float>((static_cast<double>(row) + 0.5) / scale_y),
                      static_cast<float>(1.0 / scale_x),
                      static_cast<float>(1.0 / scale_y)
                  );
            for (std::size_t component = 0U; component < 4U; ++component) {
                const double blurred = sums[component] / total_weight;
                if (clipped_original.empty()) {
                    pixels_[target + component] = static_cast<std::uint8_t>(
                        std::clamp(std::lround(blurred), 0L, 255L)
                    );
                    continue;
                }
                const std::size_t original =
                    (static_cast<std::size_t>(row - top) * clipped_width +
                     (column - left)) * 4U + component;
                const double composed =
                    static_cast<double>(clipped_original[original]) * (1.0 - clip_coverage) +
                    blurred * clip_coverage;
                pixels_[target + component] = static_cast<std::uint8_t>(
                    std::clamp(std::lround(composed), 0L, 255L)
                );
            }
        }
    }
}

/**
 * A blur with a downsample, as the GPU renderers run one: the padded source is reduced by
 * averaging each downsample x downsample block, blurred there with the radius scaled down to match
 * (clamped as D3D11 clamps it), and filtered back up bilinearly onto the effect region.
 */
void SoftwareRenderer::blur_downsampled(const host::BlurBatch& batch, const std::uint32_t left,
                                        const std::uint32_t top, const std::uint32_t right,
                                        const std::uint32_t bottom, const std::uint32_t downsample,
                                        const std::vector<std::uint8_t>& clipped_original,
                                        const double radius_x, const double radius_y) {
    const double scale_x = static_cast<double>(width_) / logical_width_;
    const double scale_y = static_cast<double>(height_) / logical_height_;
    const double physical_radius = batch.radius * std::max(scale_x, scale_y);
    const std::uint32_t padding =
        static_cast<std::uint32_t>(std::clamp(std::ceil(physical_radius), 1.0, 256.0));
    const std::uint32_t source_left = left > padding ? left - padding : 0U;
    const std::uint32_t source_top = top > padding ? top - padding : 0U;
    const std::uint32_t source_right = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(right) + padding, width_));
    const std::uint32_t source_bottom = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(bottom) + padding, height_));
    const std::uint32_t source_width = source_right - source_left;
    const std::uint32_t source_height = source_bottom - source_top;
    const auto reduced = [downsample](const std::uint32_t extent) {
        return std::max(1U, extent / downsample + (extent % downsample == 0U ? 0U : 1U));
    };
    const std::uint32_t reduced_width = reduced(source_width);
    const std::uint32_t reduced_height = reduced(source_height);

    // Bilinear sampling with clamped edges, at a position in texel units, as the GPU sampler reads.
    const auto bilinear = [](const auto& read, const std::uint32_t texture_width,
                             const std::uint32_t texture_height, const double x, const double y,
                             const std::size_t component) {
        const double cx = std::clamp(x, 0.0, static_cast<double>(texture_width - 1U));
        const double cy = std::clamp(y, 0.0, static_cast<double>(texture_height - 1U));
        const auto x0 = static_cast<std::uint32_t>(cx);
        const auto y0 = static_cast<std::uint32_t>(cy);
        const std::uint32_t x1 = std::min(x0 + 1U, texture_width - 1U);
        const std::uint32_t y1 = std::min(y0 + 1U, texture_height - 1U);
        const double fx = cx - x0;
        const double fy = cy - y0;
        return (read(x0, y0, component) * (1.0 - fx) + read(x1, y0, component) * fx) * (1.0 - fy) +
               (read(x0, y1, component) * (1.0 - fx) + read(x1, y1, component) * fx) * fy;
    };
    const auto source_texel = [this, source_left, source_top](const std::uint32_t x,
                                                              const std::uint32_t y,
                                                              const std::size_t component) {
        return static_cast<double>(
            pixels_[(static_cast<std::size_t>(source_top + y) * width_ + source_left + x) * 4U +
                    component]);
    };
    // One sample per reduced texel at its centre, mapped across the whole source region.
    std::vector<double> small(static_cast<std::size_t>(reduced_width) * reduced_height * 4U);
    for (std::uint32_t y = 0U; y < reduced_height; ++y) {
        const double sample_y =
            (static_cast<double>(y) + 0.5) * source_height / reduced_height - 0.5;
        for (std::uint32_t x = 0U; x < reduced_width; ++x) {
            const double sample_x =
                (static_cast<double>(x) + 0.5) * source_width / reduced_width - 0.5;
            const std::size_t target = (static_cast<std::size_t>(y) * reduced_width + x) * 4U;
            for (std::size_t component = 0U; component < 4U; ++component) {
                small[target + component] = bilinear(source_texel, source_width, source_height,
                                                     sample_x, sample_y, component);
            }
        }
    }

    const auto reduced_radius = [&](const double logical) {
        if (logical < 0.0)
            return std::clamp(physical_radius / downsample, 0.5, 32.0);
        const double physical = logical * std::max(scale_x, scale_y);
        return physical <= 0.0 ? 0.0 : std::clamp(physical / downsample, 0.5, 32.0);
    };
    const std::vector<double> weights_x = blur_weights(reduced_radius(radius_x));
    const std::vector<double> weights_y = blur_weights(reduced_radius(radius_y));
    const auto pass = [&](const std::vector<double>& input, const bool horizontal) {
        const std::vector<double>& weights = horizontal ? weights_x : weights_y;
        const auto radius = static_cast<std::uint32_t>(weights.size() - 1U);
        std::vector<double> output(input.size());
        for (std::uint32_t y = 0U; y < reduced_height; ++y) {
            for (std::uint32_t x = 0U; x < reduced_width; ++x) {
                std::array<double, 4U> sums{};
                double total_weight = 0.0;
                for (std::int64_t offset = -static_cast<std::int64_t>(radius);
                     offset <= static_cast<std::int64_t>(radius); ++offset) {
                    const std::int64_t along = (horizontal ? x : y) + offset;
                    const std::int64_t limit = (horizontal ? reduced_width : reduced_height) - 1;
                    const auto sample = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                        along, 0, limit));
                    const std::size_t source =
                        (static_cast<std::size_t>(horizontal ? y : sample) * reduced_width +
                         (horizontal ? sample : x)) * 4U;
                    const double weight = weights[static_cast<std::size_t>(std::abs(offset))];
                    for (std::size_t component = 0U; component < 4U; ++component)
                        sums[component] += input[source + component] * weight;
                    total_weight += weight;
                }
                const std::size_t target = (static_cast<std::size_t>(y) * reduced_width + x) * 4U;
                for (std::size_t component = 0U; component < 4U; ++component)
                    output[target + component] = sums[component] / total_weight;
            }
        }
        return output;
    };
    const std::vector<double> blurred = pass(pass(small, true), false);

    // The effect region reads the reduced result through the window D3D11 uses: it starts at the
    // effect's integer-divided offset and spans its rounded-up reduced size.
    const auto blurred_texel = [&blurred, reduced_width](const std::uint32_t x,
                                                         const std::uint32_t y,
                                                         const std::size_t component) {
        return blurred[(static_cast<std::size_t>(y) * reduced_width + x) * 4U + component];
    };
    const std::uint32_t clipped_width = right - left;
    const std::uint32_t effect_height = bottom - top;
    const double window_x = static_cast<double>((left - source_left) / downsample);
    const double window_y = static_cast<double>((top - source_top) / downsample);
    const double window_width = reduced(clipped_width);
    const double window_height = reduced(effect_height);
    for (std::uint32_t row = top; row < bottom; ++row) {
        const double sample_y = window_y +
                                (static_cast<double>(row - top) + 0.5) / effect_height *
                                    window_height -
                                0.5;
        for (std::uint32_t column = left; column < right; ++column) {
            const double sample_x = window_x +
                                    (static_cast<double>(column - left) + 0.5) / clipped_width *
                                        window_width -
                                    0.5;
            const float clip_coverage =
                clipped_original.empty()
                    ? 1.0F
                    : rounded_clip_coverage(
                          batch.rounded_clips,
                          static_cast<float>((static_cast<double>(column) + 0.5) / scale_x),
                          static_cast<float>((static_cast<double>(row) + 0.5) / scale_y),
                          static_cast<float>(1.0 / scale_x), static_cast<float>(1.0 / scale_y));
            const std::size_t target = (static_cast<std::size_t>(row) * width_ + column) * 4U;
            for (std::size_t component = 0U; component < 4U; ++component) {
                const double value = bilinear(blurred_texel, reduced_width, reduced_height,
                                              sample_x, sample_y, component);
                double composed = value;
                if (!clipped_original.empty()) {
                    const std::size_t original =
                        (static_cast<std::size_t>(row - top) * clipped_width + (column - left)) *
                            4U +
                        component;
                    composed = static_cast<double>(clipped_original[original]) *
                                   (1.0 - clip_coverage) +
                               value * clip_coverage;
                }
                pixels_[target + component] =
                    static_cast<std::uint8_t>(std::clamp(std::lround(composed), 0L, 255L));
            }
        }
    }
}

const std::vector<SoftwareRenderer::EffectPass>*
SoftwareRenderer::program_of(const host::EffectBatch& effect) const noexcept {
    return effect.effect < layer_effects_.size() ? layer_effects_[effect.effect] : nullptr;
}

void SoftwareRenderer::apply_effect(const host::EffectBatch& effect) {
    const std::vector<EffectPass>* const program = program_of(effect);
    if (program == nullptr)
        return;
    for (const EffectPass& pass : *program) {
        if (pass.kind != 0U)
            continue;
        const double radius = pass.radius_parameter < effect.parameter_count
                                  ? effect.parameters[pass.radius_parameter]
                                  : pass.radius;
        const double requested_downsample = pass.downsample_parameter < effect.parameter_count
                                                ? effect.parameters[pass.downsample_parameter]
                                                : static_cast<double>(pass.downsample);
        blur(host::BlurBatch{
            effect.source_order,
            effect.scissor,
            effect.x,
            effect.y,
            effect.width,
            effect.height,
            std::max(0.0, radius),
            gpu::blur_downsample(requested_downsample),
        });
    }
}

bool SoftwareRenderer::effect_refresh_due(const host::EffectBatch& effect, const bool content,
                                          const std::int64_t time_nanoseconds) const {
    const std::uint64_t key = static_cast<std::uint64_t>(effect.source_order) |
                              (static_cast<std::uint64_t>(content) << 32U);
    const auto cached = cached_effects_.find(key);
    // Whether anything the sample read changed is the decoder's revision; only the refresh
    // interval is this renderer's to check.
    return cached == cached_effects_.end() ||
           cached->second.revision != effect.sample_revision ||
           gpu::effect_refresh_due(effect.refresh_rate,
                                   static_cast<double>(cached->second.sampled_nanoseconds) * 1.0e-9,
                                   static_cast<double>(time_nanoseconds) * 1.0e-9);
}

void SoftwareRenderer::capture_effect(const host::EffectBatch& effect, const bool content,
                                      const std::int64_t time_nanoseconds) {
    const PixelRect region =
        effect_region(effect, width_, height_, logical_width_, logical_height_);
    CachedEffect sample;
    sample.left = region.left;
    sample.top = region.top;
    sample.width = region.right >= region.left ? region.right - region.left : 0U;
    sample.height = region.bottom >= region.top ? region.bottom - region.top : 0U;
    sample.sampled_nanoseconds = time_nanoseconds;
    sample.revision = effect.sample_revision;
    sample.pixels.resize(static_cast<std::size_t>(sample.width) * sample.height * 4U);
    for (std::uint32_t row = 0U; row < sample.height; ++row) {
        const std::size_t source =
            (static_cast<std::size_t>(sample.top + row) * width_ + sample.left) * 4U;
        const std::size_t destination = static_cast<std::size_t>(row) * sample.width * 4U;
        std::copy_n(pixels_.begin() + static_cast<std::ptrdiff_t>(source),
                    static_cast<std::size_t>(sample.width) * 4U,
                    sample.pixels.begin() + static_cast<std::ptrdiff_t>(destination));
    }
    const std::uint64_t key = static_cast<std::uint64_t>(effect.source_order) |
                              (static_cast<std::uint64_t>(content) << 32U);
    cached_effects_.insert_or_assign(key, std::move(sample));
}

void SoftwareRenderer::composite_cached_effect(const host::EffectBatch& effect,
                                               const bool content) {
    const std::uint64_t key = static_cast<std::uint64_t>(effect.source_order) |
                              (static_cast<std::uint64_t>(content) << 32U);
    const auto cached = cached_effects_.find(key);
    if (cached == cached_effects_.end()) {
        throw std::logic_error("software effect cache is unavailable");
    }
    const CachedEffect& sample = cached->second;
    composite_filtered_pixels(
        pixels_, effect, width_, height_, logical_width_, logical_height_,
        [&sample](const std::uint32_t x, const std::uint32_t y, const std::size_t channel_index) {
            const std::size_t pixel =
                (static_cast<std::size_t>(y - sample.top) * sample.width + (x - sample.left)) * 4U;
            return sample.pixels[pixel + channel_index];
        });
}

void SoftwareRenderer::apply_layer_filters(const host::EffectBatch& effect) {
    if (effect.filters.empty())
        return;
    const double scale_x = static_cast<double>(width_) / logical_width_;
    const double scale_y = static_cast<double>(height_) / logical_height_;
    const double scale = std::max(scale_x, scale_y);
    const double reach = effect.filter_reach();
    // As the GPU renderers: filters work over the bounds grown by the sample reach, or by twice
    // the filters' own reach when that is further.
    const double margin = std::ceil(std::max(
        gpu::effect_sample_reach * scale, std::clamp(std::ceil(reach * 2.0 * scale), 1.0, 512.0)));
    const auto edge = [](const double value, const std::uint32_t limit) {
        return static_cast<std::uint32_t>(std::clamp(value, 0.0, static_cast<double>(limit)));
    };
    const std::uint32_t left = edge(std::floor(effect.x * scale_x) - margin, width_);
    const std::uint32_t top = edge(std::floor(effect.y * scale_y) - margin, height_);
    const std::uint32_t right =
        edge(std::ceil((effect.x + effect.width) * scale_x) + margin, width_);
    const std::uint32_t bottom =
        edge(std::ceil((effect.y + effect.height) * scale_y) + margin, height_);
    const auto blur_layer = [&](const double radius_x, const double radius_y) {
        if (!(radius_x > 0.0) && !(radius_y > 0.0))
            return;
        const double widest = std::max(radius_x, radius_y);
        blur(host::BlurBatch{effect.source_order, host::Scissor{0U, 0U, width_, height_},
                             effect.x - reach, effect.y - reach, effect.width + reach * 2.0,
                             effect.height + reach * 2.0, widest,
                             gpu::layer_blur_downsample(widest * scale)},
             std::max(0.0, radius_x), std::max(0.0, radius_y));
    };
    const auto pixel_of = [this](const std::vector<std::uint8_t>& image, const std::uint32_t x,
                                 const std::uint32_t y) {
        const std::size_t at = (static_cast<std::size_t>(y) * width_ + x) * 4U;
        return Rgba{image[at] / 255.0F, image[at + 1U] / 255.0F, image[at + 2U] / 255.0F,
                    image[at + 3U] / 255.0F};
    };
    const auto store = [this](std::vector<std::uint8_t>& image, const std::uint32_t x,
                              const std::uint32_t y, const Rgba& color) {
        const std::size_t at = (static_cast<std::size_t>(y) * width_ + x) * 4U;
        for (std::size_t index = 0U; index < 4U; ++index)
            image[at + index] = channel(color[index]);
    };
    for (const host::LayerFilter& filter : effect.filters) {
        std::array<float, 24U> values{};
        for (std::size_t index = 0U; index < filter.values.size() && index < values.size(); ++index)
            values[index] = static_cast<float>(filter.values[index]);
        if (filter.kind == host::LayerFilterKind::blur) {
            blur_layer(filter.value(0U), filter.value(1U));
        } else if (filter.kind == host::LayerFilterKind::drop_shadow) {
            const std::vector<std::uint8_t> layer = pixels_;
            std::vector<std::uint8_t> shadow(pixels_.size(), 0U);
            for (std::uint32_t y = top; y < bottom; ++y) {
                for (std::uint32_t x = left; x < right; ++x)
                    store(shadow, x, y, filter_color(6U, values, pixel_of(layer, x, y)));
            }
            pixels_ = std::move(shadow);
            const double radius = std::max(0.0, filter.value(6U));
            blur_layer(radius, radius);
            shadow = std::move(pixels_);
            pixels_ = layer;
            const double offset_x = filter.value(4U) * scale_x;
            const double offset_y = filter.value(5U) * scale_y;
            const bool alone = filter.value(7U) > 0.5;
            // Bilinear, clamped to the image, as the GPU sampler reads the shifted shadow.
            const auto shifted = [&](const std::uint32_t x, const std::uint32_t y) {
                const double sx = std::clamp(static_cast<double>(x) - offset_x, 0.0,
                                             static_cast<double>(width_ - 1U));
                const double sy = std::clamp(static_cast<double>(y) - offset_y, 0.0,
                                             static_cast<double>(height_ - 1U));
                const auto x0 = static_cast<std::uint32_t>(sx);
                const auto y0 = static_cast<std::uint32_t>(sy);
                const std::uint32_t x1 = std::min(x0 + 1U, width_ - 1U);
                const std::uint32_t y1 = std::min(y0 + 1U, height_ - 1U);
                const auto fx = static_cast<float>(sx - x0);
                const auto fy = static_cast<float>(sy - y0);
                const Rgba a = pixel_of(shadow, x0, y0);
                const Rgba b = pixel_of(shadow, x1, y0);
                const Rgba c = pixel_of(shadow, x0, y1);
                const Rgba d = pixel_of(shadow, x1, y1);
                Rgba result{};
                for (std::size_t index = 0U; index < 4U; ++index) {
                    result[index] = (a[index] * (1.0F - fx) + b[index] * fx) * (1.0F - fy) +
                                    (c[index] * (1.0F - fx) + d[index] * fx) * fy;
                }
                return result;
            };
            for (std::uint32_t y = top; y < bottom; ++y) {
                for (std::uint32_t x = left; x < right; ++x) {
                    const Rgba under = shifted(x, y);
                    const Rgba over = pixel_of(layer, x, y);
                    Rgba result = under;
                    if (!alone) {
                        for (std::size_t index = 0U; index < 4U; ++index)
                            result[index] = over[index] + under[index] * (1.0F - over[3]);
                    }
                    store(pixels_, x, y, result);
                }
            }
        } else {
            const auto kind = static_cast<std::uint32_t>(filter.kind);
            for (std::uint32_t y = top; y < bottom; ++y) {
                for (std::uint32_t x = left; x < right; ++x)
                    store(pixels_, x, y, filter_color(kind, values, pixel_of(pixels_, x, y)));
            }
        }
    }
}

void SoftwareRenderer::composite_effect(std::vector<std::uint8_t> foreground,
                                        std::vector<std::uint8_t> backdrop,
                                        const host::EffectBatch& effect) {
    pixels_ = std::move(backdrop);
    composite_filtered_pixels(pixels_, effect, width_, height_, logical_width_, logical_height_,
                              [&foreground, this](const std::uint32_t x, const std::uint32_t y,
                                                  const std::size_t channel_index) {
                                  const std::size_t pixel =
                                      (static_cast<std::size_t>(y) * width_ + x) * 4U;
                                  return foreground[pixel + channel_index];
                              });
}

void SoftwareRenderer::render(const host::RenderPacket& packet,
                              const std::int64_t time_nanoseconds) {
    if (width_ == 0U || height_ == 0U) {
        throw std::logic_error("headless renderer must be sized before rendering");
    }
    consume_resources(packet);
    layer_effects_.clear();
    layer_effects_.reserve(packet.effects.size());
    for (const std::string& id : packet.effects) {
        const auto found = effects_.find(id);
        layer_effects_.push_back(found != effects_.end() ? &found->second : nullptr);
    }
    pixels_.resize(static_cast<std::size_t>(width_) * height_ * 4U);
    for (std::size_t pixel = 0U; pixel < pixels_.size(); pixel += 4U) {
        std::copy(clear_.begin(), clear_.end(),
                  pixels_.begin() + static_cast<std::ptrdiff_t>(pixel));
    }
    const bool capture_surface_backdrop = std::ranges::any_of(
        packet.batches,
        [](const host::SubmissionBatch& batch) {
            const auto* effect = std::get_if<host::EffectBatch>(&batch);
            return effect != nullptr &&
                effect->kind == host::EffectBatchKind::backdrop &&
                effect->backdrop_source == host::EffectBackdropSource::surface;
        }
    );
    const std::vector<std::uint8_t> surface_backdrop =
        capture_surface_backdrop ? pixels_ : std::vector<std::uint8_t>{};
    // An isolated layer keeps only the pixels later passes read back from it while the layers
    // nested in it draw (gpu/isolation.hpp), as the GPU renderers park theirs: only the Surface
    // and the innermost layer are whole images, however deep layers nest. The read bounds use the
    // GPU renderers' sample region, which holds everything these passes read.
    const double scale = std::max(static_cast<double>(width_) / logical_width_,
                                  static_cast<double>(height_) / logical_height_);
    const std::vector<gpu::PixelBounds> reads = gpu::content_read_bounds(
        packet.batches, width_, height_, logical_width_, logical_height_,
        [&](const host::EffectBatch& effect) {
            double widest = 0.0;
            if (const std::vector<EffectPass>* const program = program_of(effect)) {
                for (const EffectPass& pass : *program) {
                    if (pass.kind == 0U)
                        widest = std::max(widest, pass.radius_parameter < effect.parameter_count
                                                      ? effect.parameters[pass.radius_parameter]
                                                      : pass.radius);
                }
            }
            const double reach = std::ceil(std::max(
                gpu::effect_sample_reach * scale,
                std::clamp(std::ceil(std::max(widest, effect.filter_reach() * 2.0) * scale), 1.0,
                           512.0)));
            const auto edge = [](const double value, const std::uint32_t limit) {
                return static_cast<std::uint32_t>(
                    std::clamp(value, 0.0, static_cast<double>(limit)));
            };
            const double scale_x = static_cast<double>(width_) / logical_width_;
            const double scale_y = static_cast<double>(height_) / logical_height_;
            return gpu::PixelBounds{
                edge(std::floor(effect.x * scale_x) - reach, width_),
                edge(std::floor(effect.y * scale_y) - reach, height_),
                edge(std::ceil((effect.x + effect.width) * scale_x) + reach, width_),
                edge(std::ceil((effect.y + effect.height) * scale_y) + reach, height_),
            };
        });
    std::size_t next_read = 0U;
    // The innermost open layer's read bounds, and whether anything has been drawn into it.
    gpu::PixelBounds active_read;
    bool active_drawn = false;
    const std::size_t row_bytes = static_cast<std::size_t>(width_) * 4U;
    std::vector<ContentEffect> content_effects;
    for (const host::SubmissionBatch& batch : packet.batches) {
        if (const auto* draw_batch = std::get_if<host::DrawBatch>(&batch); draw_batch != nullptr) {
            draw(*draw_batch, packet);
            active_drawn = true;
        } else if (const auto* blur_batch = std::get_if<host::BlurBatch>(&batch);
                   blur_batch != nullptr) {
            blur(*blur_batch);
            active_drawn = true;
        } else if (const auto* backdrop_effect = std::get_if<host::EffectBatch>(&batch);
                   backdrop_effect != nullptr &&
                   backdrop_effect->kind == host::EffectBatchKind::backdrop) {
            if (effect_refresh_due(*backdrop_effect, false, time_nanoseconds)) {
                std::vector<std::uint8_t> backdrop = pixels_;
                if (backdrop_effect->backdrop_source ==
                    host::EffectBackdropSource::surface) {
                    if (surface_backdrop.empty()) {
                        throw std::logic_error(
                            "software SURFACE backdrop effect has no captured Surface input"
                        );
                    }
                    pixels_ = surface_backdrop;
                }
                apply_effect(*backdrop_effect);
                if (backdrop_effect->refresh_rate > 0.0) {
                    capture_effect(*backdrop_effect, false, time_nanoseconds);
                }
                composite_effect(std::move(pixels_), std::move(backdrop), *backdrop_effect);
            } else {
                composite_cached_effect(*backdrop_effect, false);
            }
            active_drawn = true;
        } else if (const auto* content_effect = std::get_if<host::EffectBatch>(&batch);
                   content_effect != nullptr &&
                   content_effect->kind == host::EffectBatchKind::content_begin) {
            if (content_effects.size() == host::maximum_content_effect_depth) {
                throw std::length_error("software content effect nesting exceeds the packet limit");
            }
            ContentEffect saved{*content_effect, {}, active_read, !content_effects.empty()};
            if (!saved.parent_is_layer) {
                saved.parent = std::move(pixels_);
            } else if (active_drawn && !active_read.empty()) {
                const std::size_t kept_row = static_cast<std::size_t>(active_read.width()) * 4U;
                saved.parent.resize(kept_row * active_read.height());
                for (std::uint32_t row = 0U; row < active_read.height(); ++row) {
                    std::copy_n(pixels_.begin() + static_cast<std::ptrdiff_t>(
                                                      (active_read.top + row) * row_bytes +
                                                      active_read.left * 4U),
                                kept_row,
                                saved.parent.begin() +
                                    static_cast<std::ptrdiff_t>(row * kept_row));
                }
            }
            content_effects.push_back(std::move(saved));
            active_read = next_read < reads.size() ? reads[next_read]
                                                   : gpu::PixelBounds{0U, 0U, width_, height_};
            ++next_read;
            active_drawn = false;
            pixels_.assign(static_cast<std::size_t>(width_) * height_ * 4U, std::uint8_t{0U});
        } else {
            if (!std::holds_alternative<host::ContentEffectEndBatch>(batch) ||
                content_effects.empty()) {
                throw std::logic_error("software content effect stack is invalid");
            }
            ContentEffect content = std::move(content_effects.back());
            content_effects.pop_back();
            // The parent whole again: a layer's kept read bounds back in place.
            std::vector<std::uint8_t> backdrop = std::move(content.parent);
            if (content.parent_is_layer) {
                std::vector<std::uint8_t> whole(static_cast<std::size_t>(height_) * row_bytes,
                                                std::uint8_t{0U});
                const gpu::PixelBounds kept = content.parent_read;
                const std::size_t kept_row = static_cast<std::size_t>(kept.width()) * 4U;
                for (std::uint32_t row = 0U; !backdrop.empty() && row < kept.height(); ++row) {
                    std::copy_n(backdrop.begin() + static_cast<std::ptrdiff_t>(row * kept_row),
                                kept_row,
                                whole.begin() + static_cast<std::ptrdiff_t>(
                                                    (kept.top + row) * row_bytes + kept.left * 4U));
                }
                backdrop = std::move(whole);
            }
            active_read = content.parent_read;
            active_drawn = true;
            if (effect_refresh_due(content.effect, true, time_nanoseconds)) {
                apply_effect(content.effect);
                apply_layer_filters(content.effect);
                if (content.effect.refresh_rate > 0.0) {
                    capture_effect(content.effect, true, time_nanoseconds);
                }
                composite_effect(std::move(pixels_), std::move(backdrop), content.effect);
            } else {
                pixels_ = std::move(backdrop);
                composite_cached_effect(content.effect, true);
            }
        }
    }
    if (!content_effects.empty()) {
        throw std::logic_error("software content effect stack is unbalanced");
    }
}

std::string_view SoftwareRenderer::backend() const noexcept {
    return "reference";
}

std::uint32_t SoftwareRenderer::width() const noexcept {
    return width_;
}
std::uint32_t SoftwareRenderer::height() const noexcept {
    return height_;
}
std::span<const std::uint8_t> SoftwareRenderer::pixels() const noexcept {
    return pixels_;
}
const std::vector<std::string>& SoftwareRenderer::material_fallbacks() const noexcept {
    return material_fallbacks_;
}

} // namespace strata::headless
