#include "ui/render/submission_internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "runtime/value.hpp"
#include "ui/render/paint_geometry.hpp"
#include "ui/render/path_geometry.hpp"

namespace strata::ui::submission_detail {
namespace {

constexpr std::size_t draw_data_float_count = 16U;
constexpr std::size_t vertex_bytes = 24U + draw_data_float_count * sizeof(float);
constexpr TextureRegion full_uv{0.0, 0.0, 1.0, 1.0};

struct BatchKey final {
    std::string material;
    std::string blend_mode;
    std::optional<std::string> texture;
    SubmissionScissor scissor;
    std::vector<SubmissionRoundedClip> rounded_clips;
    bool texture_sampled = false;
};

[[nodiscard]] bool compatible(const BatchKey& left, const BatchKey& right) noexcept {
    if (left.material != right.material || left.blend_mode != right.blend_mode ||
        left.scissor != right.scissor || left.rounded_clips != right.rounded_clips) return false;
    return !(left.texture_sampled && right.texture_sampled && left.texture != right.texture);
}

void write_u32(std::uint8_t*& output, const std::uint32_t value) noexcept {
    *output++ = static_cast<std::uint8_t>(value);
    *output++ = static_cast<std::uint8_t>(value >> 8U);
    *output++ = static_cast<std::uint8_t>(value >> 16U);
    *output++ = static_cast<std::uint8_t>(value >> 24U);
}

void write_float(std::uint8_t*& output, const float value) noexcept {
    write_u32(output, std::bit_cast<std::uint32_t>(value));
}

void color_data(
    std::array<float, draw_data_float_count>& values,
    const std::size_t offset,
    const RenderColor color
) noexcept {
    if (offset + 3U >= values.size()) return;
    values[offset] = static_cast<float>(color.red) / 255.0F;
    values[offset + 1U] = static_cast<float>(color.green) / 255.0F;
    values[offset + 2U] = static_cast<float>(color.blue) / 255.0F;
    values[offset + 3U] = static_cast<float>(color.alpha) / 255.0F;
}

void radii_data(
    std::array<float, draw_data_float_count>& values,
    const CornerRadii radii
) noexcept {
    values[4] = static_cast<float>(radii.top_left);
    values[5] = static_cast<float>(radii.top_right);
    values[6] = static_cast<float>(radii.bottom_right);
    values[7] = static_cast<float>(radii.bottom_left);
}

void parameter_data(
    std::array<float, draw_data_float_count>& values,
    const MaterialParameter& parameter
) noexcept {
    const auto number_at = [&](const std::size_t offset) noexcept {
        if (const double* number = parameter.value.number(); number != nullptr && offset < values.size()) {
            values[offset] = static_cast<float>(*number);
        }
    };
    const auto vector_at = [&](const std::size_t offset, const std::size_t count) noexcept {
        const runtime::ValueList* list = parameter.value.list();
        if (list == nullptr || list->values.size() != count || offset + count > values.size()) return;
        for (std::size_t index = 0U; index < count; ++index) {
            const double* number = list->values[index].number();
            if (number == nullptr || !std::isfinite(*number)) return;
        }
        for (std::size_t index = 0U; index < count; ++index) {
            values[offset + index] = static_cast<float>(*list->values[index].number());
        }
    };
    if (parameter.name.starts_with('@')) {
        // An authored material's parameters were rewritten to their draw-data slots, so packing
        // them needs no material contract at geometry time.
        std::size_t slot = 0U;
        const std::string_view digits(parameter.name.begin() + 1, parameter.name.end());
        if (std::from_chars(digits.data(), digits.data() + digits.size(), slot).ec != std::errc{}) {
            return;
        }
        if (parameter.value.color() != nullptr) {
            const runtime::ColorValue color = *parameter.value.color();
            color_data(values, slot, RenderColor{color.red, color.green, color.blue, color.alpha});
            return;
        }
        if (const runtime::ValueList* list = parameter.value.list(); list != nullptr) {
            vector_at(slot, list->values.size());
            return;
        }
        number_at(slot);
        return;
    }
    if (parameter.name == "opacity") number_at(15U);
    else if (parameter.name == "softness") number_at(2U);
    else if (parameter.name == "borderWidth") number_at(3U);
    else if (parameter.name == "blurRadius") number_at(2U);
    else if (parameter.name == "spread") number_at(3U);
    else if (parameter.name == "msdfDistanceScale") number_at(0U);
    else if (parameter.name == "msdfLayoutPixelRange") number_at(1U);
    else if (parameter.name == "textPixelSize") number_at(2U);
    else if (parameter.name == "drawMode") number_at(14U);
    else if (parameter.name == "rectSize") vector_at(0U, 2U);
    else if (parameter.name == "cornerRadii") vector_at(4U, 4U);
    else if (parameter.name == "borderColor") {
        if (const runtime::ColorValue* color = parameter.value.color(); color != nullptr) {
            color_data(values, 8U, RenderColor{color->red, color->green, color->blue, color->alpha});
        }
    }
}

[[nodiscard]] std::array<float, draw_data_float_count> draw_data(const PreparedDraw& draw) {
    std::array<float, draw_data_float_count> values{};
    std::visit([&](const auto& command) {
        using Type = std::decay_t<decltype(command)>;
        if constexpr (std::is_same_v<Type, SolidRectRenderCommand>) {
            // A gradient fill is planned onto the rounded-rect material so its tessellation is
            // masked to the rectangle; that material reads the shape size from the draw data.
            values[0] = static_cast<float>(draw.local_bounds.width);
            values[1] = static_cast<float>(draw.local_bounds.height);
            values[2] = 0.5F;
        } else if constexpr (std::is_same_v<Type, RoundedRectRenderCommand>) {
            values[0] = static_cast<float>(draw.local_bounds.width);
            values[1] = static_cast<float>(draw.local_bounds.height);
            values[2] = static_cast<float>(command.softness);
            values[3] = command.border.has_value() ? static_cast<float>(command.border->width) : 0.0F;
            radii_data(values, command.radii);
            if (command.border.has_value()) color_data(values, 8U, command.border->color);
        } else if constexpr (std::is_same_v<Type, BorderRenderCommand>) {
            values[0] = static_cast<float>(draw.local_bounds.width);
            values[1] = static_cast<float>(draw.local_bounds.height);
            values[2] = static_cast<float>(command.border.width);
            values[3] = 1.0F;
            radii_data(values, command.radii);
            color_data(values, 8U, command.border.color);
        } else if constexpr (std::is_same_v<Type, ShadowRenderCommand>) {
            values[0] = static_cast<float>(command.bounds.width);
            values[1] = static_cast<float>(command.bounds.height);
            values[2] = static_cast<float>(command.radius);
            values[3] = static_cast<float>(command.spread);
            radii_data(values, command.radii);
            values[8] = static_cast<float>(draw.local_bounds.width);
            values[9] = static_cast<float>(draw.local_bounds.height);
        } else if constexpr (std::is_same_v<Type, PreparedTextPtr>) {
            if (command != nullptr) {
                values[0] = static_cast<float>(command->atlas_pixel_range);
                values[1] = static_cast<float>(command->layout_pixel_range);
                values[2] = static_cast<float>(command->pixel_size);
            }
        }
    }, draw.command);
    for (const MaterialParameter& parameter : draw.material.parameters) parameter_data(values, parameter);
    // An authored fill material replaces shading, not geometry semantics. Preserve the command's
    // native draw mode so rounded coverage and any fallback path still see the actual primitive.
    values[14] = unified_draw_mode(default_material(draw.command).id);
    values[15] = static_cast<float>(draw.material.opacity);
    return values;
}

void vertex(
    RenderSubmission& output,
    const Transform& transform,
    const double x,
    const double y,
    const double z,
    const RenderColor color,
    const double u,
    const double v,
    const std::array<float, draw_data_float_count>& data
) {
    const Point point = transform.apply(Point{x, y});
    std::array<std::uint8_t, vertex_bytes> encoded;
    std::uint8_t* destination = encoded.data();
    write_float(destination, static_cast<float>(point.x));
    write_float(destination, static_cast<float>(point.y));
    write_float(destination, static_cast<float>(z));
    write_float(destination, static_cast<float>(u));
    write_float(destination, static_cast<float>(v));
    *destination++ = color.red;
    *destination++ = color.green;
    *destination++ = color.blue;
    *destination++ = color.alpha;
    for (const float value : data) write_float(destination, value);
    output.vertex_bytes.insert(
        output.vertex_bytes.end(),
        encoded.begin(),
        encoded.end()
    );
}

void quad(
    RenderSubmission& output,
    const PreparedDraw& draw,
    const Rect bounds,
    const RenderColor color,
    const TextureRegion uv,
    const std::array<float, draw_data_float_count>& data,
    const std::uint32_t batch_base_vertex
) {
    const std::size_t global_base_size = output.vertex_bytes.size() / vertex_bytes;
    if (global_base_size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("render submission vertex count exceeds uint32");
    }
    const std::uint32_t global_base = static_cast<std::uint32_t>(global_base_size);
    const std::uint32_t local_base = global_base - batch_base_vertex;
    vertex(output, draw.transform, bounds.x, bounds.y, 0.0, color, uv.u, uv.v, data);
    vertex(output, draw.transform, bounds.x, bounds.bottom(), 0.0, color, uv.u, uv.v + uv.height, data);
    vertex(output, draw.transform, bounds.right(), bounds.bottom(), 0.0, color, uv.u + uv.width, uv.v + uv.height, data);
    vertex(output, draw.transform, bounds.right(), bounds.y, 0.0, color, uv.u + uv.width, uv.v, data);
    output.indices.insert(output.indices.end(), {
        local_base, local_base + 1U, local_base + 2U,
        local_base + 2U, local_base + 3U, local_base,
    });
}

void text_geometry(
    RenderSubmission& output,
    const PreparedDraw& draw,
    const PreparedText& text,
    const std::array<float, draw_data_float_count>& data,
    const std::uint32_t batch_base_vertex,
    const SubmissionContext& context
) {
    Point origin = text.origin;
    if (draw.transform.axis_aligned_translation() && !text.glyphs.empty()) {
        origin.x = std::round((origin.x + draw.transform.m02) * context.scale) /
            context.scale - draw.transform.m02;
        if (text.mode == font::GlyphRasterMode::coverage) {
            const double baseline = text.glyphs.front().baseline;
            origin.y = std::round((origin.y + baseline + draw.transform.m12) * context.scale) /
                context.scale - draw.transform.m12 - baseline;
        }
    }
    for (const PreparedGlyph& glyph : text.glyphs) {
        const Rect bounds{
            origin.x + glyph.bounds.x,
            origin.y + glyph.bounds.y,
            glyph.bounds.width,
            glyph.bounds.height,
        };
        quad(output, draw, bounds, text.color, glyph.uv, data, batch_base_vertex);
    }
}

[[nodiscard]] std::pair<double, double> fitted_insets(
    const double leading,
    const double trailing,
    const double extent
) noexcept {
    const double safe_leading = std::max(0.0, leading);
    const double safe_trailing = std::max(0.0, trailing);
    const double total = safe_leading + safe_trailing;
    if (total <= extent || total == 0.0) return {safe_leading, safe_trailing};
    const double scale = extent / total;
    return {safe_leading * scale, safe_trailing * scale};
}

[[nodiscard]] const resource::TextureResourceDescriptor& texture_resource(
    const SubmissionContext& context,
    const std::string_view id
) {
    const auto found = std::ranges::find(context.textures, id,
        &resource::TextureResourceDescriptor::logical_id);
    if (found == context.textures.end()) {
        throw std::invalid_argument(
            "nine-patch texture '" + std::string(id) + "' has no declared resource descriptor"
        );
    }
    return *found;
}

void nine_patch_geometry(
    RenderSubmission& output,
    const PreparedDraw& draw,
    const NinePatchRenderCommand& command,
    const std::array<float, draw_data_float_count>& data,
    const std::uint32_t batch_base_vertex,
    const SubmissionContext& context
) {
    const resource::TextureResourceDescriptor& texture = texture_resource(context, command.texture);
    if (command.source.width <= 0.0 || command.source.height <= 0.0) {
        throw std::invalid_argument("nine-patch source region must be positive");
    }
    const auto [source_left, source_right] = fitted_insets(
        command.source_insets.left,
        command.source_insets.right,
        command.source.width * static_cast<double>(texture.dimensions.width)
    );
    const auto [source_top, source_bottom] = fitted_insets(
        command.source_insets.top,
        command.source_insets.bottom,
        command.source.height * static_cast<double>(texture.dimensions.height)
    );
    const auto [destination_left, destination_right] = fitted_insets(
        command.destination_insets.left, command.destination_insets.right, command.bounds.width
    );
    const auto [destination_top, destination_bottom] = fitted_insets(
        command.destination_insets.top, command.destination_insets.bottom, command.bounds.height
    );
    const std::array<double, 4U> x{
        command.bounds.x,
        command.bounds.x + destination_left,
        command.bounds.right() - destination_right,
        command.bounds.right(),
    };
    const std::array<double, 4U> y{
        command.bounds.y,
        command.bounds.y + destination_top,
        command.bounds.bottom() - destination_bottom,
        command.bounds.bottom(),
    };
    const std::array<double, 4U> u{
        command.source.u,
        command.source.u + source_left / static_cast<double>(texture.dimensions.width),
        command.source.u + command.source.width -
            source_right / static_cast<double>(texture.dimensions.width),
        command.source.u + command.source.width,
    };
    const std::array<double, 4U> v{
        command.source.v,
        command.source.v + source_top / static_cast<double>(texture.dimensions.height),
        command.source.v + command.source.height -
            source_bottom / static_cast<double>(texture.dimensions.height),
        command.source.v + command.source.height,
    };
    for (std::size_t row = 0U; row < 3U; ++row) {
        for (std::size_t column = 0U; column < 3U; ++column) {
            const Rect bounds{x[column], y[row], x[column + 1U] - x[column], y[row + 1U] - y[row]};
            if (bounds.empty()) continue;
            quad(
                output,
                draw,
                bounds,
                command.tint,
                TextureRegion{
                    u[column], v[row], u[column + 1U] - u[column], v[row + 1U] - v[row],
                },
                data,
                batch_base_vertex
            );
        }
    }
}

void custom_mesh_geometry(
    RenderSubmission& output,
    const PreparedDraw& draw,
    const CustomMeshRenderCommand& command,
    const std::array<float, draw_data_float_count>& data,
    const std::uint32_t batch_base_vertex
) {
    const MeshGeometry& geometry = command.geometry;
    if (geometry.vertices.empty() && geometry.indices.empty()) return;
    if (geometry.vertices.empty() || geometry.indices.empty() ||
        geometry.indices.size() % 3U != 0U) {
        throw std::invalid_argument("custom mesh geometry requires indexed triangles");
    }
    const std::size_t global_base_size = output.vertex_bytes.size() / vertex_bytes;
    if (global_base_size > std::numeric_limits<std::uint32_t>::max() ||
        geometry.vertices.size() > std::numeric_limits<std::uint32_t>::max() - global_base_size) {
        throw std::length_error("custom mesh vertex count exceeds uint32");
    }
    const std::uint32_t global_base = static_cast<std::uint32_t>(global_base_size);
    const std::uint32_t local_base = global_base - batch_base_vertex;
    constexpr double maximum_float = static_cast<double>(std::numeric_limits<float>::max());
    for (const MeshVertex& value : geometry.vertices) {
        if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z) ||
            !std::isfinite(value.u) || !std::isfinite(value.v) || value.x < 0.0 || value.x > 1.0 ||
            value.y < 0.0 || value.y > 1.0 || std::abs(value.z) > maximum_float ||
            std::abs(value.u) > maximum_float || std::abs(value.v) > maximum_float) {
            throw std::invalid_argument("custom mesh vertex is outside the portable vertex domain");
        }
        vertex(
            output,
            draw.transform,
            command.bounds.x + value.x * command.bounds.width,
            command.bounds.y + value.y * command.bounds.height,
            value.z,
            value.color,
            value.u,
            value.v,
            data
        );
    }
    for (const std::uint32_t index : geometry.indices) {
        if (index >= geometry.vertices.size()) {
            throw std::invalid_argument("custom mesh index exceeds its vertex array");
        }
        output.indices.push_back(local_base + index);
    }
}

void paint_mesh_geometry(
    RenderSubmission& output,
    const PreparedDraw& draw,
    const Rect bounds,
    const PaintMesh& mesh,
    const std::array<float, draw_data_float_count>& data,
    const std::uint32_t batch_base_vertex
) {
    if (mesh.vertices.empty() || mesh.indices.empty()) return;
    const std::size_t global_base_size = output.vertex_bytes.size() / vertex_bytes;
    if (global_base_size > std::numeric_limits<std::uint32_t>::max() ||
        mesh.vertices.size() > std::numeric_limits<std::uint32_t>::max() - global_base_size) {
        throw std::length_error("tessellated paint vertex count exceeds uint32");
    }
    const std::uint32_t local_base =
        static_cast<std::uint32_t>(global_base_size) - batch_base_vertex;
    for (const PaintVertex& value : mesh.vertices) {
        // The shape-local coordinate doubles as the texture coordinate, which is what the
        // signed-distance materials read, so a tessellated fill keeps the shape's own mask.
        vertex(
            output,
            draw.transform,
            bounds.x + value.normalized.x * bounds.width,
            bounds.y + value.normalized.y * bounds.height,
            0.0,
            value.color,
            value.normalized.x,
            value.normalized.y,
            data
        );
    }
    for (const std::uint32_t index : mesh.indices) output.indices.push_back(local_base + index);
}

void geometry(
    RenderSubmission& output,
    const PreparedDraw& draw,
    const std::uint32_t batch_base_vertex,
    const SubmissionContext& context
) {
    const auto data = draw_data(draw);
    std::visit([&](const auto& command) {
        using Type = std::decay_t<decltype(command)>;
        if constexpr (std::is_same_v<Type, SolidRectRenderCommand> ||
                      std::is_same_v<Type, RoundedRectRenderCommand>) {
            if (const Gradient* gradient = command.fill.gradient(); gradient != nullptr) {
                paint_mesh_geometry(
                    output,
                    draw,
                    command.bounds,
                    tessellate_gradient(
                        *gradient,
                        Size{command.bounds.width, command.bounds.height},
                        context.scale
                    ),
                    data,
                    batch_base_vertex
                );
            } else {
                quad(
                    output, draw, command.bounds, command.fill.representative(), full_uv, data,
                    batch_base_vertex
                );
            }
        } else if constexpr (std::is_same_v<Type, BorderRenderCommand>) {
            quad(output, draw, command.bounds, command.border.color, full_uv, data, batch_base_vertex);
        } else if constexpr (std::is_same_v<Type, ImageRenderCommand>) {
            quad(output, draw, command.bounds, command.tint, command.source, data, batch_base_vertex);
        } else if constexpr (std::is_same_v<Type, NinePatchRenderCommand>) {
            nine_patch_geometry(output, draw, command, data, batch_base_vertex, context);
        } else if constexpr (std::is_same_v<Type, PreparedTextPtr>) {
            if (command != nullptr) {
                text_geometry(output, draw, *command, data, batch_base_vertex, context);
            }
        } else if constexpr (std::is_same_v<Type, PathRenderCommand>) {
            const double transform_scale = std::max(
                std::hypot(draw.transform.m00, draw.transform.m10),
                std::hypot(draw.transform.m01, draw.transform.m11)
            );
            paint_mesh_geometry(
                output,
                draw,
                command.bounds,
                tessellate_shape(
                    command.shape,
                    Size{command.bounds.width, command.bounds.height},
                    context.scale * std::max(transform_scale, 0.1)
                ),
                data,
                batch_base_vertex
            );
        } else if constexpr (std::is_same_v<Type, CustomMeshRenderCommand>) {
            custom_mesh_geometry(output, draw, command, data, batch_base_vertex);
        } else if constexpr (std::is_same_v<Type, ShadowRenderCommand>) {
            quad(output, draw, draw.local_bounds, command.color, full_uv, data, batch_base_vertex);
        }
    }, draw.command);
}

[[nodiscard]] BatchKey key(const PreparedDraw& draw) {
    const bool sampled = samples_texture(draw.material.id);
    return BatchKey{
        unified_material(draw.material.id) ? "strata:unified_ui" : draw.material.id,
        draw.material.blend_mode,
        sampled ? draw.texture : std::nullopt,
        draw.scissor,
        draw.rounded_clips,
        sampled,
    };
}

[[nodiscard]] std::optional<Point> translation_from_cached_geometry(
    const PreparedDraw& retained,
    const PreparedDraw& current,
    const SubmissionContext& context
) {
    if (retained.command != current.command || retained.local_bounds != current.local_bounds ||
        retained.material != current.material || retained.transform.m00 != current.transform.m00 ||
        retained.transform.m01 != current.transform.m01 ||
        retained.transform.m10 != current.transform.m10 ||
        retained.transform.m11 != current.transform.m11) {
        return std::nullopt;
    }
    Point delta{
        current.transform.m02 - retained.transform.m02,
        current.transform.m12 - retained.transform.m12,
    };
    const auto* text = std::get_if<PreparedTextPtr>(&retained.command);
    if (text == nullptr || *text == nullptr ||
        !retained.transform.axis_aligned_translation() ||
        !current.transform.axis_aligned_translation()) {
        return delta;
    }
    const PreparedText& run = **text;
    const auto snapped = [&context](const double value) {
        return std::round(value * context.scale) / context.scale;
    };
    delta.x = snapped(run.origin.x + current.transform.m02) -
        snapped(run.origin.x + retained.transform.m02);
    if (run.mode == font::GlyphRasterMode::coverage && !run.glyphs.empty()) {
        const double baseline = run.glyphs.front().baseline;
        delta.y = snapped(run.origin.y + baseline + current.transform.m12) -
            snapped(run.origin.y + baseline + retained.transform.m12);
    }
    return delta;
}

[[nodiscard]] bool align_geometry_cache(
    const std::vector<PlannedItem>& items,
    const SubmissionContext& context,
    PreparationCache& cache
) {
    const std::size_t previous_size = cache.geometry.size();
    const std::size_t current_size = items.size();
    if (previous_size == current_size &&
        cache.placements.size() == current_size) {
        return true;
    }
    const auto compatible = [&](const std::size_t previous, const std::size_t current) {
        const auto* draw = std::get_if<PreparedDraw>(&items[current].value);
        if (draw == nullptr) return !cache.geometry[previous].has_value();
        return cache.geometry[previous].has_value() &&
            translation_from_cached_geometry(
                cache.geometry[previous]->source,
                *draw,
                context
            ).has_value();
    };
    const std::size_t common = std::min(previous_size, current_size);
    std::size_t prefix = 0U;
    while (prefix < common && compatible(prefix, prefix)) ++prefix;
    std::size_t suffix = 0U;
    while (suffix < common - prefix &&
           compatible(previous_size - suffix - 1U, current_size - suffix - 1U)) {
        ++suffix;
    }
    std::vector<std::optional<EncodedDrawCacheEntry>> previous_geometry =
        std::move(cache.geometry);
    std::vector<std::optional<EncodedDrawPlacement>> previous_placements =
        std::move(cache.placements);
    cache.geometry.resize(current_size);
    cache.placements.resize(current_size);
    const auto move_slot = [&](const std::size_t previous_index,
                               const std::size_t current_index) {
        cache.geometry[current_index].swap(
            previous_geometry[previous_index]
        );
        if (previous_index < previous_placements.size()) {
            cache.placements[current_index].swap(
                previous_placements[previous_index]
            );
        }
    };
    for (std::size_t index = 0U; index < prefix; ++index) {
        move_slot(index, index);
    }
    for (std::size_t index = 0U; index < suffix; ++index) {
        move_slot(
            previous_size - index - 1U,
            current_size - index - 1U
        );
    }
    for (std::size_t index = prefix; index < current_size - suffix; ++index) {
        const auto* draw = std::get_if<PreparedDraw>(&items[index].value);
        if (draw == nullptr || cache.geometry[index].has_value()) continue;
        const auto found = std::ranges::find_if(
            cache.detached_geometry,
            [&](const EncodedDrawCacheEntry& entry) {
                return translation_from_cached_geometry(
                    entry.source,
                    *draw,
                    context
                ).has_value();
            }
        );
        if (found == cache.detached_geometry.end()) continue;
        const std::size_t detached_index =
            static_cast<std::size_t>(
                found - cache.detached_geometry.begin()
            );
        cache.geometry[index].emplace(std::move(*found));
        if (detached_index + 1U != cache.detached_geometry.size()) {
            cache.detached_geometry[detached_index] =
                std::move(cache.detached_geometry.back());
        }
        cache.detached_geometry.pop_back();
    }
    for (std::optional<EncodedDrawCacheEntry>& entry : previous_geometry) {
        if (entry.has_value()) {
            cache.detached_geometry.push_back(std::move(*entry));
        }
    }
    const std::size_t detached_limit = std::max(previous_size, current_size);
    if (cache.detached_geometry.size() > detached_limit) {
        cache.detached_geometry.erase(
            cache.detached_geometry.begin(),
            cache.detached_geometry.begin() +
                static_cast<std::ptrdiff_t>(
                    cache.detached_geometry.size() - detached_limit
                )
        );
    }
    return false;
}

void translate_vertex_positions(
    std::vector<std::uint8_t>& vertices,
    const std::size_t begin,
    const Point delta
) {
    if (delta.x == 0.0 && delta.y == 0.0) return;
    const float x_delta = static_cast<float>(delta.x);
    const float y_delta = static_cast<float>(delta.y);
    for (std::size_t offset = begin; offset < vertices.size(); offset += vertex_bytes) {
        float x = 0.0F;
        float y = 0.0F;
        std::memcpy(&x, vertices.data() + offset, sizeof(x));
        std::memcpy(&y, vertices.data() + offset + sizeof(x), sizeof(y));
        x += x_delta;
        y += y_delta;
        std::memcpy(vertices.data() + offset, &x, sizeof(x));
        std::memcpy(vertices.data() + offset + sizeof(x), &y, sizeof(y));
    }
}

[[nodiscard]] std::size_t retained_vertex_capacity(const std::size_t required_bytes) {
    if (required_bytes == 0U) return 0U;
    if (required_bytes % vertex_bytes != 0U) {
        throw std::logic_error("encoded draw vertices are not stride-aligned");
    }
    const std::size_t required_vertices = required_bytes / vertex_bytes;
    std::size_t capacity = 1U;
    while (capacity < required_vertices) {
        if (capacity > std::numeric_limits<std::size_t>::max() / 2U) {
            throw std::length_error("retained vertex slot exceeds size_t");
        }
        capacity *= 2U;
    }
    if (capacity > std::numeric_limits<std::size_t>::max() / vertex_bytes) {
        throw std::length_error("retained vertex slot exceeds size_t");
    }
    return capacity * vertex_bytes;
}

[[nodiscard]] std::size_t retained_index_capacity(const std::size_t required_indices) {
    if (required_indices == 0U) return 0U;
    if (required_indices % 3U != 0U) {
        throw std::logic_error("encoded draw indices are not triangle-aligned");
    }
    const std::size_t required_triangles = required_indices / 3U;
    std::size_t capacity = 1U;
    while (capacity < required_triangles) {
        if (capacity > std::numeric_limits<std::size_t>::max() / 2U) {
            throw std::length_error("retained index slot exceeds size_t");
        }
        capacity *= 2U;
    }
    if (capacity > std::numeric_limits<std::size_t>::max() / 3U) {
        throw std::length_error("retained index slot exceeds size_t");
    }
    return capacity * 3U;
}

} // namespace

void encode(
    const std::vector<PlannedItem>& items,
    const SubmissionContext& context,
    RenderSubmission& output,
    PreparationCache& cache
) {
    output.previous_item_count = cache.geometry.size();
    output.item_count = items.size();
    bool topology_reused = align_geometry_cache(items, context, cache);
    const auto change_topology = [&output, &topology_reused](
                                     const SubmissionTopologyChange reason,
                                     const std::size_t item
                                 ) {
        if (output.topology_change == SubmissionTopologyChange::none) {
            output.topology_change = reason;
            output.topology_change_item = item;
        }
        topology_reused = false;
    };
    if (!topology_reused) {
        change_topology(SubmissionTopologyChange::item_count, 0U);
    }
    std::vector<std::optional<EncodedDrawCacheEntry>> geometry_updates(items.size());
    std::vector<std::optional<EncodedDrawPlacement>> next_placements(items.size());
    std::vector<bool> changed(items.size(), false);
    std::vector<SubmissionBatch> next_batches;
    next_batches.reserve(output.batches.capacity());
    std::size_t vertex_byte_count = 0U;
    std::size_t index_count = 0U;
    std::optional<BatchKey> open_key;
    std::size_t open_batch_index = 0U;
    std::uint32_t batch_base_vertex = 0U;
    const auto close_batch = [&]() {
        if (!open_key.has_value()) return;
        SubmissionBatch& batch = next_batches[open_batch_index];
        const std::size_t count = index_count - batch.first_index;
        if (count > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("render submission batch index count exceeds uint32");
        }
        batch.index_count = static_cast<std::uint32_t>(count);
        open_key.reset();
    };

    for (std::size_t item_index = 0U; item_index < items.size(); ++item_index) {
        const PlannedItem& item = items[item_index];
        if (const auto* effect = std::get_if<SubmissionBatch>(&item.value); effect != nullptr) {
            if (cache.geometry[item_index].has_value() ||
                (topology_reused && cache.placements[item_index].has_value())) {
                change_topology(
                    SubmissionTopologyChange::effect_placement,
                    item_index
                );
            }
            if (open_key.has_value()) ++output.effect_batch_breaks;
            close_batch();
            next_batches.push_back(*effect);
            continue;
        }
        const PreparedDraw& draw = std::get<PreparedDraw>(item.value);
        BatchKey next = key(draw);
        if (!open_key.has_value() || !compatible(*open_key, next)) {
            if (open_key.has_value()) {
                if (open_key->material != next.material ||
                    open_key->blend_mode != next.blend_mode) {
                    ++output.material_batch_breaks;
                }
                if (open_key->scissor != next.scissor ||
                    open_key->rounded_clips != next.rounded_clips) {
                    ++output.clip_batch_breaks;
                }
                if (open_key->texture_sampled && next.texture_sampled &&
                    open_key->texture != next.texture) {
                    ++output.texture_batch_breaks;
                }
            }
            close_batch();
            const std::size_t vertex_count = vertex_byte_count / vertex_bytes;
            if (vertex_count > std::numeric_limits<std::uint32_t>::max() ||
                index_count > std::numeric_limits<std::uint32_t>::max()) {
                throw std::length_error("render submission geometry exceeds uint32");
            }
            batch_base_vertex = static_cast<std::uint32_t>(vertex_count);
            next_batches.push_back(SubmissionBatch{
                SubmissionBatchKind::draw,
                next.material,
                next.blend_mode,
                next.texture,
                next.scissor,
                batch_base_vertex,
                static_cast<std::uint32_t>(index_count),
                0U,
                draw.source_order,
                {}, 0.0, 1U,
                {}, std::nullopt,
                next.rounded_clips,
            });
            open_batch_index = next_batches.size() - 1U;
            open_key = std::move(next);
        } else if (!open_key->texture_sampled && next.texture_sampled) {
            open_key->texture_sampled = true;
            open_key->texture = next.texture;
            next_batches[open_batch_index].texture = next.texture;
        }
        std::optional<EncodedDrawCacheEntry>& retained = cache.geometry[item_index];
        if (!retained.has_value() || retained->source != draw) {
            changed[item_index] = true;
            std::optional<Point> translated;
            if (retained.has_value()) {
                translated = translation_from_cached_geometry(retained->source, draw, context);
            }
            if (translated.has_value()) {
                EncodedDrawCacheEntry updated = *retained;
                updated.source = draw;
                translate_vertex_positions(updated.vertex_bytes, 0U, *translated);
                geometry_updates[item_index] = std::move(updated);
            } else {
                RenderSubmission fragment;
                geometry(fragment, draw, 0U, context);
                geometry_updates[item_index] = EncodedDrawCacheEntry{
                    draw,
                    std::move(fragment.vertex_bytes),
                    std::move(fragment.indices),
                };
            }
        }
        const EncodedDrawCacheEntry& geometry =
            geometry_updates[item_index].has_value()
                ? *geometry_updates[item_index]
                : *retained;
        const std::size_t global_vertex_count = vertex_byte_count / vertex_bytes;
        if (global_vertex_count < batch_base_vertex ||
            global_vertex_count - batch_base_vertex > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("render submission batch-local vertex offset exceeds uint32");
        }
        const std::uint32_t batch_local_offset = static_cast<std::uint32_t>(
            global_vertex_count - batch_base_vertex
        );
        const EncodedDrawPlacement* const previous_placement =
            item_index < cache.placements.size() &&
                    cache.placements[item_index].has_value()
                ? &*cache.placements[item_index]
                : nullptr;
        // Text is the ordinary topology-changing retained primitive (for example "9" -> "10"
        // while dragging a slider). Give only text a bounded geometric slot; fixed primitives
        // retain exact public geometry counts and do not pay padding for capacity they cannot use.
        const bool elastic_slot =
            std::holds_alternative<PreparedTextPtr>(draw.command);
        const std::size_t vertex_capacity =
            previous_placement != nullptr &&
                    geometry.vertex_bytes.size() <=
                        previous_placement->vertex_byte_capacity
                ? previous_placement->vertex_byte_capacity
                : elastic_slot
                    ? retained_vertex_capacity(geometry.vertex_bytes.size())
                    : geometry.vertex_bytes.size();
        const std::size_t index_capacity =
            previous_placement != nullptr &&
                    geometry.indices.size() <= previous_placement->index_capacity
                ? previous_placement->index_capacity
                : elastic_slot
                    ? retained_index_capacity(geometry.indices.size())
                    : geometry.indices.size();
        const EncodedDrawPlacement placement{
            vertex_byte_count,
            index_count,
            batch_local_offset,
            geometry.vertex_bytes.size(),
            geometry.indices.size(),
            vertex_capacity,
            index_capacity,
        };
        if (topology_reused) {
            if (previous_placement == nullptr) {
                change_topology(
                    SubmissionTopologyChange::missing_placement,
                    item_index
                );
            } else if (
                previous_placement->vertex_byte_offset !=
                placement.vertex_byte_offset
            ) {
                change_topology(
                    SubmissionTopologyChange::vertex_offset,
                    item_index
                );
            } else if (
                previous_placement->index_offset != placement.index_offset
            ) {
                change_topology(
                    SubmissionTopologyChange::index_offset,
                    item_index
                );
            } else if (
                previous_placement->batch_local_vertex !=
                placement.batch_local_vertex
            ) {
                change_topology(
                    SubmissionTopologyChange::batch_local_vertex,
                    item_index
                );
            } else if (
                previous_placement->vertex_byte_capacity !=
                placement.vertex_byte_capacity
            ) {
                change_topology(
                    SubmissionTopologyChange::vertex_capacity,
                    item_index
                );
            } else if (
                previous_placement->index_capacity != placement.index_capacity
            ) {
                change_topology(
                    SubmissionTopologyChange::index_capacity,
                    item_index
                );
            }
        }
        next_placements[item_index] = placement;
        if (vertex_capacity >
                std::numeric_limits<std::size_t>::max() - vertex_byte_count ||
            index_capacity >
                std::numeric_limits<std::size_t>::max() - index_count) {
            throw std::length_error("retained render submission geometry exceeds size_t");
        }
        vertex_byte_count += vertex_capacity;
        index_count += index_capacity;
        ++output.planned_draws;
    }
    close_batch();
    next_batches.erase(
        std::remove_if(next_batches.begin(), next_batches.end(), [](const SubmissionBatch& batch) {
            return batch.kind == SubmissionBatchKind::draw && batch.index_count == 0U;
        }),
        next_batches.end()
    );
    if (topology_reused &&
        (output.used_vertex_bytes != vertex_byte_count ||
         output.used_indices != index_count)) {
        change_topology(SubmissionTopologyChange::buffer_size, items.size());
    }
    output.geometry_topology_reused = topology_reused;
    output.full_geometry_bytes =
        vertex_byte_count + index_count * sizeof(std::uint32_t);

    const auto append_patch = [](
                                  std::vector<SubmissionGeometryPatch>& patches,
                                  const std::size_t offset,
                                  const std::span<const std::uint8_t> bytes
                              ) {
        if (bytes.empty()) return;
        if (offset > std::numeric_limits<std::uint32_t>::max() ||
            bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("retained geometry patch exceeds uint32");
        }
        if (!patches.empty() &&
            static_cast<std::size_t>(patches.back().offset) +
                    patches.back().bytes.size() == offset) {
            patches.back().bytes.insert(
                patches.back().bytes.end(),
                bytes.begin(),
                bytes.end()
            );
            return;
        }
        patches.push_back(SubmissionGeometryPatch{
            static_cast<std::uint32_t>(offset),
            std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
        });
    };
    const auto changed_ranges = [&append_patch](
                                    const std::span<const std::uint8_t> previous,
                                    const std::span<const std::uint8_t> current,
                                    const std::size_t stride
                                ) {
        std::vector<SubmissionGeometryPatch> patches;
        if (previous.size() != current.size() ||
            stride == 0U ||
            current.size() % stride != 0U) {
            return patches;
        }
        std::size_t begin = current.size();
        for (std::size_t offset = 0U; offset < current.size(); offset += stride) {
            const bool changed =
                std::memcmp(
                    previous.data() + offset,
                    current.data() + offset,
                    stride
                ) != 0;
            if (changed && begin == current.size()) {
                begin = offset;
            } else if (!changed && begin != current.size()) {
                append_patch(
                    patches,
                    begin,
                    current.subspan(begin, offset - begin)
                );
                begin = current.size();
            }
        }
        if (begin != current.size()) {
            append_patch(
                patches,
                begin,
                current.subspan(begin)
            );
        }
        return patches;
    };

    const auto geometry_at = [&cache, &geometry_updates](
                                 const std::size_t index
                             ) -> const EncodedDrawCacheEntry& {
        return geometry_updates[index].has_value()
            ? *geometry_updates[index]
            : *cache.geometry[index];
    };
    std::vector<SubmissionGeometryPatch> next_vertex_patches;
    std::vector<SubmissionGeometryPatch> next_index_patches;
    if (topology_reused) {
        for (std::size_t item_index = 0U; item_index < items.size(); ++item_index) {
            if (!changed[item_index] || !next_placements[item_index].has_value()) continue;
            const EncodedDrawPlacement& placement = *next_placements[item_index];
            const EncodedDrawCacheEntry& retained = geometry_at(item_index);
            const std::uint8_t* const vertex_destination =
                output.vertex_bytes.data() + placement.vertex_byte_offset;
            if (placement.vertex_byte_count != 0U && std::memcmp(
                    vertex_destination,
                    retained.vertex_bytes.data(),
                    placement.vertex_byte_count
                ) != 0) {
                append_patch(
                    next_vertex_patches,
                    placement.vertex_byte_offset,
                    retained.vertex_bytes
                );
            }
            std::vector<std::uint32_t> next_indices;
            next_indices.assign(
                placement.index_capacity,
                placement.batch_local_vertex
            );
            for (std::size_t index = 0U; index < placement.index_count; ++index) {
                const std::uint32_t local = retained.indices[index];
                if (local > std::numeric_limits<std::uint32_t>::max() -
                                placement.batch_local_vertex) {
                    throw std::length_error("render submission cached index exceeds uint32");
                }
                const std::uint32_t next = placement.batch_local_vertex + local;
                next_indices[index] = next;
            }
            if (!std::equal(
                    next_indices.begin(),
                    next_indices.end(),
                    output.indices.begin() +
                        static_cast<std::ptrdiff_t>(placement.index_offset)
                )) {
                const std::span<const std::uint32_t> indices(
                    next_indices
                );
                const std::span<const std::byte> bytes = std::as_bytes(indices);
                append_patch(
                    next_index_patches,
                    placement.index_offset * sizeof(std::uint32_t),
                    std::span<const std::uint8_t>(
                        reinterpret_cast<const std::uint8_t*>(bytes.data()),
                        bytes.size()
                    )
                );
            }
        }
        std::size_t patch_bytes = 0U;
        for (const SubmissionGeometryPatch& patch : next_vertex_patches) {
            patch_bytes += patch.bytes.size() + 8U;
        }
        for (const SubmissionGeometryPatch& patch : next_index_patches) {
            patch_bytes += patch.bytes.size() + 8U;
        }
        const std::size_t full_bytes =
            output.vertex_bytes.size() + output.indices.size() * sizeof(std::uint32_t);
        output.candidate_geometry_patch_bytes = patch_bytes;
        output.full_geometry_bytes = full_bytes;
        output.patch_from_previous = full_bytes == 0U || patch_bytes < full_bytes;
        for (const SubmissionGeometryPatch& patch : next_vertex_patches) {
            std::memcpy(
                output.vertex_bytes.data() + patch.offset,
                patch.bytes.data(),
                patch.bytes.size()
            );
        }
        std::uint8_t* const output_index_bytes =
            reinterpret_cast<std::uint8_t*>(output.indices.data());
        for (const SubmissionGeometryPatch& patch : next_index_patches) {
            std::memcpy(
                output_index_bytes + patch.offset,
                patch.bytes.data(),
                patch.bytes.size()
            );
        }
        if (output.patch_from_previous) {
            output.vertex_patches = std::move(next_vertex_patches);
            output.index_patches = std::move(next_index_patches);
        }
    } else {
        std::vector<std::uint8_t> next_vertices;
        std::vector<std::uint32_t> next_indices;
        next_vertices.reserve(vertex_byte_count);
        next_indices.reserve(index_count);
        for (std::size_t item_index = 0U; item_index < items.size(); ++item_index) {
            if (!next_placements[item_index].has_value()) continue;
            const EncodedDrawPlacement& placement = *next_placements[item_index];
            const EncodedDrawCacheEntry& retained = geometry_at(item_index);
            next_vertices.insert(
                next_vertices.end(),
                retained.vertex_bytes.begin(),
                retained.vertex_bytes.end()
            );
            next_vertices.resize(
                next_vertices.size() +
                    placement.vertex_byte_capacity -
                    placement.vertex_byte_count,
                0U
            );
            const std::size_t index_slot_begin = next_indices.size();
            next_indices.resize(
                index_slot_begin + placement.index_capacity,
                placement.batch_local_vertex
            );
            for (std::size_t local_index = 0U;
                 local_index < retained.indices.size();
                 ++local_index) {
                const std::uint32_t index = retained.indices[local_index];
                if (index > std::numeric_limits<std::uint32_t>::max() -
                                placement.batch_local_vertex) {
                    throw std::length_error("render submission cached index exceeds uint32");
                }
                next_indices[index_slot_begin + local_index] =
                    placement.batch_local_vertex + index;
            }
        }
        const bool arena_fits =
            next_vertices.size() <= output.vertex_bytes.size() &&
            next_indices.size() <= output.indices.size();
        if (arena_fits) {
            // Only the live prefix is reachable from the new batches. Preserve unused arena
            // capacity in place instead of allocating, zero-filling, and comparing the whole
            // reserved buffer on every structural transition.
            next_vertex_patches = changed_ranges(
                std::span<const std::uint8_t>(
                    output.vertex_bytes.data(),
                    next_vertices.size()
                ),
                next_vertices,
                vertex_bytes
            );
            const std::span<const std::byte> previous_index_bytes =
                std::as_bytes(std::span<const std::uint32_t>(
                    output.indices.data(),
                    next_indices.size()
                ));
            const std::span<const std::byte> next_index_bytes =
                std::as_bytes(std::span<const std::uint32_t>(next_indices));
            next_index_patches = changed_ranges(
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(
                        previous_index_bytes.data()
                    ),
                    previous_index_bytes.size()
                ),
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(
                        next_index_bytes.data()
                    ),
                    next_index_bytes.size()
                ),
                sizeof(std::uint32_t)
            );
            std::size_t patch_bytes = 0U;
            for (const SubmissionGeometryPatch& patch : next_vertex_patches) {
                patch_bytes += patch.bytes.size() + 8U;
            }
            for (const SubmissionGeometryPatch& patch : next_index_patches) {
                patch_bytes += patch.bytes.size() + 8U;
            }
            output.candidate_geometry_patch_bytes = patch_bytes;
            const std::size_t full_bytes =
                output.vertex_bytes.size() +
                output.indices.size() * sizeof(std::uint32_t);
            output.full_geometry_bytes = full_bytes;
            output.patch_from_previous =
                full_bytes == 0U || patch_bytes < full_bytes;
            for (const SubmissionGeometryPatch& patch : next_vertex_patches) {
                std::memcpy(
                    output.vertex_bytes.data() + patch.offset,
                    patch.bytes.data(),
                    patch.bytes.size()
                );
            }
            std::uint8_t* const output_index_bytes =
                reinterpret_cast<std::uint8_t*>(output.indices.data());
            for (const SubmissionGeometryPatch& patch : next_index_patches) {
                std::memcpy(
                    output_index_bytes + patch.offset,
                    patch.bytes.data(),
                    patch.bytes.size()
                );
            }
            if (output.patch_from_previous) {
                output.vertex_patches = std::move(next_vertex_patches);
                output.index_patches = std::move(next_index_patches);
            }
        } else {
            next_vertices.resize(
                retained_vertex_capacity(next_vertices.size()),
                0U
            );
            next_indices.resize(
                retained_index_capacity(next_indices.size()),
                0U
            );
            output.full_geometry_bytes =
                next_vertices.size() +
                next_indices.size() * sizeof(std::uint32_t);
            output.vertex_bytes.swap(next_vertices);
            output.indices.swap(next_indices);
        }
    }
    output.used_vertex_bytes = vertex_byte_count;
    output.used_indices = index_count;
    for (std::size_t item_index = 0U; item_index < items.size(); ++item_index) {
        if (std::holds_alternative<SubmissionBatch>(items[item_index].value)) {
            cache.geometry[item_index].reset();
        } else if (geometry_updates[item_index].has_value()) {
            cache.geometry[item_index] = std::move(geometry_updates[item_index]);
        }
    }
    output.batches = std::move(next_batches);
    cache.placements = std::move(next_placements);
}

static_assert(vertex_bytes == 88U);

} // namespace strata::ui::submission_detail
