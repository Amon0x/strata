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

namespace strata::headless {
namespace {

constexpr std::size_t vertex_size = 88U;

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

[[nodiscard]] Vertex read_vertex(const std::span<const std::uint8_t> bytes, const std::size_t index,
                                 const float scale_x, const float scale_y) {
    if (index >= bytes.size() / vertex_size) {
        throw std::invalid_argument("render packet vertex index is out of range");
    }
    const std::size_t base = index * vertex_size;
    Vertex result;
    result.x = read_float(bytes, base) * scale_x;
    result.y = read_float(bytes, base + 4U) * scale_y;
    result.u = read_float(bytes, base + 12U);
    result.v = read_float(bytes, base + 16U);
    result.color = Color{
        static_cast<float>(bytes[base + 20U]) / 255.0F,
        static_cast<float>(bytes[base + 21U]) / 255.0F,
        static_cast<float>(bytes[base + 22U]) / 255.0F,
        static_cast<float>(bytes[base + 23U]) / 255.0F,
    };
    for (std::size_t value = 0U; value < result.data.size(); ++value) {
        result.data[value] = read_float(bytes, base + 24U + value * sizeof(float));
    }
    return result;
}

[[nodiscard]] float cross(const float ax, const float ay, const float bx, const float by) noexcept {
    return ax * by - ay * bx;
}

[[nodiscard]] bool top_left(const Vertex& a, const Vertex& b) noexcept {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    return dy < 0.0F || (dy == 0.0F && dx > 0.0F);
}

[[nodiscard]] bool covered(const float edge, const bool includes_edge) noexcept {
    return edge > 0.0F || (edge == 0.0F && includes_edge);
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
    const float radius = std::max(0.0F, bottom ? (trailing ? radii[2U] : radii[3U])
                                               : (trailing ? radii[1U] : radii[0U]));
    const float qx = std::abs(x) - half_width + radius;
    const float qy = std::abs(y) - half_height + radius;
    return std::hypot(std::max(qx, 0.0F), std::max(qy, 0.0F)) + std::min(std::max(qx, qy), 0.0F) -
           radius;
}

[[nodiscard]] std::uint8_t channel(const float value) noexcept {
    return static_cast<std::uint8_t>(std::lround(saturate(value) * 255.0F));
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

} // namespace

SoftwareRenderer::SoftwareRenderer(const ImageCodec& image_codec) : image_codec_(image_codec) {}

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
}

void SoftwareRenderer::set_clear_color(const std::array<std::uint8_t, 4U> rgba) noexcept {
    clear_ = rgba;
}

void SoftwareRenderer::declare_material(std::string_view, std::string_view) {}

void SoftwareRenderer::apply(const host::ResourceOperation& operation) {
    if (operation.kind == 2U) {
        textures_.erase(operation.texture);
        return;
    }
    if (operation.kind == 0U) {
        const std::uint32_t channels = operation.format == 0U ? 1U : 4U;
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
                                       operation.format == 0U ? 0U : 1U,
                                       std::vector<std::uint8_t>(static_cast<std::size_t>(size)),
                                   });
        return;
    }
    if (operation.kind == 3U) {
        DecodedImage image =
            image_codec_.decode_png(operation.bytes, operation.width, operation.height);
        textures_.insert_or_assign(operation.texture, Texture{
                                                          image.width,
                                                          image.height,
                                                          4U,
                                                          operation.sampling,
                                                          std::move(image.rgba),
                                                      });
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
    const std::uint32_t channels = operation.format == 0U ? 1U : 4U;
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
    if (batch.material != "strata:unified_ui" &&
        std::ranges::find(material_fallbacks_, batch.material) == material_fallbacks_.end()) {
        material_fallbacks_.push_back(batch.material);
    }
    const Texture* texture = nullptr;
    if (batch.texture.has_value()) {
        const auto found = textures_.find(*batch.texture);
        if (found != textures_.end())
            texture = &found->second;
    }
    const auto texture_sample = [texture](const float u, const float v) noexcept {
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
        if (texture->sampling == 0U) {
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
        Vertex a = read_vertex(packet.vertices, ia, scale_x, scale_y);
        Vertex b = read_vertex(packet.vertices, ib, scale_x, scale_y);
        Vertex c = read_vertex(packet.vertices, ic, scale_x, scale_y);
        float area = cross(b.x - a.x, b.y - a.y, c.x - a.x, c.y - a.y);
        if (area == 0.0F)
            continue;
        if (area < 0.0F) {
            std::swap(b, c);
            area = -area;
        }
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
        const bool include_ab = top_left(a, b);
        const bool include_bc = top_left(b, c);
        const bool include_ca = top_left(c, a);

        for (std::int64_t y = minimum_y; y < maximum_y; ++y) {
            for (std::int64_t x = minimum_x; x < maximum_x; ++x) {
                const float sample_x = static_cast<float>(x) + 0.5F;
                const float sample_y = static_cast<float>(y) + 0.5F;
                const float edge_ab = cross(b.x - a.x, b.y - a.y, sample_x - a.x, sample_y - a.y);
                const float edge_bc = cross(c.x - b.x, c.y - b.y, sample_x - b.x, sample_y - b.y);
                const float edge_ca = cross(a.x - c.x, a.y - c.y, sample_x - c.x, sample_y - c.y);
                if (!covered(edge_ab, include_ab) || !covered(edge_bc, include_bc) ||
                    !covered(edge_ca, include_ca)) {
                    continue;
                }
                const float wa = edge_bc * inverse_area;
                const float wb = edge_ca * inverse_area;
                const float wc = edge_ab * inverse_area;
                const float u = a.u * wa + b.u * wb + c.u * wc;
                const float v = a.v * wa + b.v * wb + c.v * wc;
                Color source = interpolate_color(a, b, c, wa, wb, wc);
                const std::array<float, 16U> data = interpolate_data(a, b, c, wa, wb, wc);
                const int mode = static_cast<int>(std::floor(data[14U] + 0.5F));
                if (mode == 1) {
                    const Color sampled = texture_sample(u, v);
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
                    if (mode == 2) {
                        const float softness = std::max(data[2U], 0.5F);
                        const float border_width = std::max(data[3U], 0.0F);
                        const float distance =
                            rounded_box_sdf(px, py, shape_width * 0.5F, shape_height * 0.5F, radii);
                        const float alpha = 1.0F - smoothstep(-softness, softness, distance);
                        const float border_mix =
                            saturate((1.0F - smoothstep(border_width - softness,
                                                        border_width + softness, -distance)) *
                                     (border_width >= 0.001F ? 1.0F : 0.0F));
                        source.red += (data[8U] - source.red) * border_mix;
                        source.green += (data[9U] - source.green) * border_mix;
                        source.blue += (data[10U] - source.blue) * border_mix;
                        source.alpha += (data[11U] - source.alpha) * border_mix;
                        source.alpha *= alpha;
                    } else if (mode == 3) {
                        const float line_width = std::max(data[2U], 0.0F);
                        const float softness = std::max(data[3U], 0.5F);
                        const float distance =
                            rounded_box_sdf(px, py, shape_width * 0.5F, shape_height * 0.5F, radii);
                        const float outer = 1.0F - smoothstep(-softness, softness, distance);
                        const float inner =
                            1.0F - smoothstep(-softness, softness, distance + line_width);
                        source.alpha *= saturate(outer - inner);
                    } else {
                        const float radius = std::max(data[2U], 0.5F);
                        const float spread = std::max(data[3U], 0.0F);
                        for (float& value : radii)
                            value = std::max(value + spread, 0.0F);
                        const float distance = rounded_box_sdf(px, py, shape_width * 0.5F - radius,
                                                               shape_height * 0.5F - radius, radii);
                        source.alpha *= 1.0F - smoothstep(-radius, radius, distance);
                    }
                } else if (mode == 4) {
                    source.alpha *= texture_sample(u, v).red;
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
                }
                source.alpha *= data[15U];
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
                if (batch.blend_mode == "opaque") {
                    output = source;
                } else if (batch.blend_mode == "straight_alpha") {
                    const float inverse = 1.0F - source.alpha;
                    output = Color{
                        source.red * source.alpha + destination.red * inverse,
                        source.green * source.alpha + destination.green * inverse,
                        source.blue * source.alpha + destination.blue * inverse,
                        source.alpha + destination.alpha * inverse,
                    };
                } else if (batch.blend_mode == "premultiplied_alpha") {
                    const float inverse = 1.0F - source.alpha;
                    output = Color{
                        source.red + destination.red * inverse,
                        source.green + destination.green * inverse,
                        source.blue + destination.blue * inverse,
                        source.alpha + destination.alpha * inverse,
                    };
                } else if (batch.blend_mode == "additive") {
                    output = Color{
                        source.red * source.alpha + destination.red,
                        source.green * source.alpha + destination.green,
                        source.blue * source.alpha + destination.blue,
                        source.alpha + destination.alpha * (1.0F - source.alpha),
                    };
                } else if (batch.blend_mode == "multiply") {
                    output = Color{
                        source.red * destination.red,
                        source.green * destination.green,
                        source.blue * destination.blue,
                        source.alpha + destination.alpha * (1.0F - source.alpha),
                    };
                } else {
                    throw std::invalid_argument("headless renderer received an unknown blend mode");
                }
                pixels_[pixel] = channel(output.red);
                pixels_[pixel + 1U] = channel(output.green);
                pixels_[pixel + 2U] = channel(output.blue);
                pixels_[pixel + 3U] = channel(output.alpha);
            }
        }
    }
}

void SoftwareRenderer::blur(const host::BlurBatch& batch) {
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
    const std::uint32_t radius = static_cast<std::uint32_t>(
        std::clamp(std::ceil(batch.radius * std::max(scale_x, scale_y)), 1.0, 32.0));
    const std::uint32_t source_left = left > radius ? left - radius : 0U;
    const std::uint32_t source_top = top > radius ? top - radius : 0U;
    const std::uint32_t source_right = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(right) + radius, width_));
    const std::uint32_t source_bottom = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(bottom) + radius, height_));
    const std::uint32_t source_width = source_right - source_left;
    const std::uint32_t source_height = source_bottom - source_top;
    std::vector<std::uint8_t> horizontal(static_cast<std::size_t>(source_width) * source_height *
                                         4U);
    std::vector<std::uint64_t> prefix(static_cast<std::size_t>(source_width + 1U) * 4U);
    for (std::uint32_t row = 0U; row < source_height; ++row) {
        std::fill(prefix.begin(), prefix.end(), 0U);
        for (std::uint32_t column = 0U; column < source_width; ++column) {
            const std::size_t source =
                (static_cast<std::size_t>(source_top + row) * width_ + source_left + column) * 4U;
            for (std::size_t component = 0U; component < 4U; ++component) {
                prefix[(static_cast<std::size_t>(column) + 1U) * 4U + component] =
                    prefix[static_cast<std::size_t>(column) * 4U + component] +
                    pixels_[source + component];
            }
        }
        for (std::uint32_t column = 0U; column < source_width; ++column) {
            const std::uint32_t begin = column > radius ? column - radius : 0U;
            const std::uint32_t end = std::min(source_width, column + radius + 1U);
            const std::uint64_t count = end - begin;
            const std::size_t target = (static_cast<std::size_t>(row) * source_width + column) * 4U;
            for (std::size_t component = 0U; component < 4U; ++component) {
                const std::uint64_t sum = prefix[static_cast<std::size_t>(end) * 4U + component] -
                                          prefix[static_cast<std::size_t>(begin) * 4U + component];
                horizontal[target + component] = static_cast<std::uint8_t>(sum / count);
            }
        }
    }
    prefix.resize(static_cast<std::size_t>(source_height + 1U) * 4U);
    for (std::uint32_t column = left; column < right; ++column) {
        std::fill(prefix.begin(), prefix.end(), 0U);
        const std::uint32_t local_x = column - source_left;
        for (std::uint32_t row = 0U; row < source_height; ++row) {
            const std::size_t source =
                (static_cast<std::size_t>(row) * source_width + local_x) * 4U;
            for (std::size_t component = 0U; component < 4U; ++component) {
                prefix[(static_cast<std::size_t>(row) + 1U) * 4U + component] =
                    prefix[static_cast<std::size_t>(row) * 4U + component] +
                    horizontal[source + component];
            }
        }
        for (std::uint32_t row = top; row < bottom; ++row) {
            const std::uint32_t local_y = row - source_top;
            const std::uint32_t begin = local_y > radius ? local_y - radius : 0U;
            const std::uint32_t end = std::min(source_height, local_y + radius + 1U);
            const std::uint64_t count = end - begin;
            const std::size_t target = (static_cast<std::size_t>(row) * width_ + column) * 4U;
            for (std::size_t component = 0U; component < 4U; ++component) {
                const std::uint64_t sum = prefix[static_cast<std::size_t>(end) * 4U + component] -
                                          prefix[static_cast<std::size_t>(begin) * 4U + component];
                pixels_[target + component] = static_cast<std::uint8_t>(sum / count);
            }
        }
    }
}

void SoftwareRenderer::render(const host::RenderPacket& packet, std::int64_t) {
    if (width_ == 0U || height_ == 0U) {
        throw std::logic_error("headless renderer must be sized before rendering");
    }
    consume_resources(packet);
    for (std::size_t pixel = 0U; pixel < pixels_.size(); pixel += 4U) {
        std::copy(clear_.begin(), clear_.end(),
                  pixels_.begin() + static_cast<std::ptrdiff_t>(pixel));
    }
    for (const host::SubmissionBatch& batch : packet.batches) {
        if (const auto* draw_batch = std::get_if<host::DrawBatch>(&batch); draw_batch != nullptr) {
            draw(*draw_batch, packet);
        } else {
            blur(std::get<host::BlurBatch>(batch));
        }
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
