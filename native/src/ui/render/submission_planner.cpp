#include "ui/render/submission_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <strata/raster_job.hpp>

#include "font/raster.hpp"
#include "ui/render/path_geometry.hpp"
#include "ui/text.hpp"

namespace strata::ui::submission_detail {
namespace {

constexpr double transform_epsilon = 0.000001;
constexpr std::uint16_t coverage_subpixel_divisions = 4U;

[[nodiscard]] bool rounded(const CornerRadii radii) noexcept {
    return radii.top_left > 0.0 || radii.top_right > 0.0 ||
           radii.bottom_right > 0.0 || radii.bottom_left > 0.0;
}

[[nodiscard]] Rect intersect(const Rect first, const Rect second) noexcept {
    const double left = std::max(first.x, second.x);
    const double top = std::max(first.y, second.y);
    const double right = std::min(first.right(), second.right());
    const double bottom = std::min(first.bottom(), second.bottom());
    return Rect{left, top, std::max(0.0, right - left), std::max(0.0, bottom - top)};
}

[[nodiscard]] SubmissionScissor scissor(
    const Rect clip,
    const SubmissionContext& context
) noexcept {
    const auto clamp_i64 = [](const double value) noexcept -> std::int64_t {
        if (value <= static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
            return std::numeric_limits<std::int64_t>::min();
        }
        if (value >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return static_cast<std::int64_t>(value);
    };
    const std::int64_t left = std::clamp(
        clamp_i64(std::floor(clip.x * context.scale)), std::int64_t{0}, context.framebuffer_width
    );
    const std::int64_t top = std::clamp(
        clamp_i64(std::floor(clip.y * context.scale)), std::int64_t{0}, context.framebuffer_height
    );
    const std::int64_t right = std::clamp(
        clamp_i64(std::ceil(clip.right() * context.scale)), std::int64_t{0}, context.framebuffer_width
    );
    const std::int64_t bottom = std::clamp(
        clamp_i64(std::ceil(clip.bottom() * context.scale)), std::int64_t{0}, context.framebuffer_height
    );
    return SubmissionScissor{
        static_cast<std::uint32_t>(left),
        static_cast<std::uint32_t>(top),
        static_cast<std::uint32_t>(std::max(std::int64_t{0}, right - left)),
        static_cast<std::uint32_t>(std::max(std::int64_t{0}, bottom - top)),
    };
}

[[nodiscard]] Rect command_bounds(const PreparedCommand& command) {
    return std::visit([](const auto& value) -> Rect {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, PreparedTextPtr>) {
            if (value == nullptr || value->glyphs.empty()) return Rect{};
            double left = std::numeric_limits<double>::infinity();
            double top = std::numeric_limits<double>::infinity();
            double right = -std::numeric_limits<double>::infinity();
            double bottom = -std::numeric_limits<double>::infinity();
            for (const PreparedGlyph& glyph : value->glyphs) {
                left = std::min(left, value->origin.x + glyph.bounds.x);
                top = std::min(top, value->origin.y + glyph.bounds.y);
                right = std::max(right, value->origin.x + glyph.bounds.right());
                bottom = std::max(bottom, value->origin.y + glyph.bounds.bottom());
            }
            return Rect{left, top, right - left, bottom - top};
        } else if constexpr (std::is_same_v<Type, PreparedMaskPtr>) {
            return value != nullptr ? value->bounds : Rect{};
        } else if constexpr (std::is_same_v<Type, ShadowRenderCommand>) {
            const double outset =
                std::max(0.0, value.radius + std::max(value.spread, 0.0));
            return Rect{
                value.bounds.x - outset,
                value.bounds.y - outset,
                value.bounds.width + outset * 2.0,
                value.bounds.height + outset * 2.0,
            };
        } else {
            return value.bounds;
        }
    }, command);
}

/** A command's texture: none when it samples nothing, else its host id or an unknown image. */
struct CommandTexture final {
    std::optional<std::string> host_id;
    bool missing = false;
};

[[nodiscard]] CommandTexture command_texture(
    const PreparedCommand& command,
    const SubmissionContext& context
) {
    return std::visit([&context](const auto& value) -> CommandTexture {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, ImageRenderCommand> ||
                      std::is_same_v<Type, NinePatchRenderCommand> ||
                      std::is_same_v<Type, CustomMeshRenderCommand>) {
            const std::optional<std::string> logical_texture = [&]() {
                if constexpr (std::is_same_v<Type, CustomMeshRenderCommand>) {
                    return value.texture;
                } else {
                    return std::optional<std::string>{value.texture};
                }
            }();
            if (!logical_texture.has_value()) return {};
            const auto declared = std::ranges::find(
                context.textures,
                *logical_texture,
                &resource::TextureResourceDescriptor::logical_id
            );
            // An image the Surface cannot resolve (a runtime image not yet published or already
            // released) draws nothing rather than sampling a texture the host lacks.
            if (declared == context.textures.end()) return CommandTexture{std::nullopt, true};
            return CommandTexture{declared->host_id, false};
        } else if constexpr (std::is_same_v<Type, PreparedTextPtr> ||
                             std::is_same_v<Type, PreparedMaskPtr>) {
            return CommandTexture{
                value != nullptr ? std::optional<std::string>(value->texture) : std::nullopt,
                false,
            };
        } else {
            return {};
        }
    }, command);
}

[[nodiscard]] MaterialState merged_material(
    const PreparedCommand& command,
    const std::optional<MaterialState>& override
) {
    MaterialState base = default_material(command);
    const bool fill = std::holds_alternative<SolidRectRenderCommand>(command) ||
        std::holds_alternative<RoundedRectRenderCommand>(command);
    if (!override.has_value() || !fill) return base;
    MaterialState merged = *override;
    if (!base.parameters.empty()) {
        std::vector<MaterialParameter> parameters = base.parameters;
        for (const MaterialParameter& parameter : override->parameters) {
            const auto existing = std::find_if(
                parameters.begin(), parameters.end(), [&](const MaterialParameter& candidate) {
                    return candidate.name == parameter.name;
                }
            );
            if (existing == parameters.end()) parameters.push_back(parameter);
            else *existing = parameter;
        }
        merged.parameters = std::move(parameters);
    }
    return merged;
}

[[nodiscard]] font::GlyphRasterMode raster_mode(
    const FontRasterization rasterization
) noexcept {
    return rasterization == FontRasterization::msdf
        ? font::GlyphRasterMode::msdf
        : font::GlyphRasterMode::coverage;
}

/** The mode a run's glyphs are rasterized in: coverage runs drawn per subpixel where they can be. */
[[nodiscard]] font::GlyphRasterMode run_raster_mode(const TextRunRenderCommand& command,
                                                    const bool subpixel) noexcept {
    const font::GlyphRasterMode mode = raster_mode(command.font_rasterization);
    return subpixel && mode == font::GlyphRasterMode::coverage ? font::GlyphRasterMode::subpixel
                                                               : mode;
}

/** Whether glyphs in this mode sit at quantized subpixel positions, each texel on a pixel. */
[[nodiscard]] bool positioned(const font::GlyphRasterMode mode) noexcept {
    return mode == font::GlyphRasterMode::coverage || mode == font::GlyphRasterMode::subpixel;
}

[[nodiscard]] double quantized_position(
    const double logical,
    const double display_scale,
    const std::uint16_t divisions
) noexcept {
    return std::round(logical * display_scale * static_cast<double>(divisions)) /
           (display_scale * static_cast<double>(divisions));
}

/** How far beyond its glyphs' edges a run's effects read their distance (0: no such effect). */
[[nodiscard]] double effect_reach(const TextRunRenderCommand& command) noexcept {
    return command.effects.distance()
               ? std::max({command.effects.outline_width, command.effects.glow_radius,
                           command.effects.shadow_blur}) +
                     1.0
               : 0.0;
}

/**
 * The density a run rasterizes at beyond the display scale: a coverage run in a zoomed group takes
 * the group's raster scale, so it is rasterized at the size it shows rather than magnified.
 * Distance-field runs scale cleanly and keep their own.
 */
[[nodiscard]] double effective_raster_scale(const TextRunRenderCommand& command,
                                            const double group_scale) noexcept {
    return raster_mode(command.font_rasterization) == font::GlyphRasterMode::coverage
               ? std::clamp(group_scale, 0.125, 8.0)
               : 1.0;
}

[[nodiscard]] std::vector<PreparedText>
prepare_text(const TextRunRenderCommand& command, font::GlyphAtlas& atlas,
             const TextEngine& text_engine, const double display_scale, const double group_scale,
             const bool subpixel, const bool unhinted) {
    // Glyphs are requested at the zoomed size and placed back at the run's own: plane bounds
    // shrink by the same factor, and pen positions snap to the zoomed device grid.
    const double raster_scale = effective_raster_scale(command, group_scale);
    const double density = display_scale * raster_scale;
    struct Entry final {
        font::GlyphAtlasEntry atlas;
        PreparedGlyph glyph;
    };
    std::vector<Entry> glyphs;
    glyphs.reserve(command.glyphs.size());
    for (const LogicalGlyph& logical : command.glyphs) {
        if (logical.glyph_id > std::numeric_limits<std::uint16_t>::max()) {
            throw font::FontError("logical glyph id exceeds the native OpenType glyph range");
        }
        const double requested_x = logical.x + logical.x_placement;
        const font::SubpixelPhase phase =
            font::SubpixelPhase::quantize(requested_x * density, coverage_subpixel_divisions);
        const std::optional<font::GlyphAtlasEntry> atlas_entry = atlas.request(
            logical.font_id, text_engine.font(logical.font_id),
            static_cast<std::uint16_t>(logical.glyph_id), command.pixel_size * raster_scale, phase,
            logical.font_style_flags, run_raster_mode(command, subpixel),
            // Text effects read the distance this far beyond the glyph's edge.
            effect_reach(command) * raster_scale, unhinted);
        if (!atlas_entry.has_value()) continue;
        const double pen_x =
            positioned(atlas_entry->mode)
                ? quantized_position(requested_x, density, coverage_subpixel_divisions)
                : requested_x;
        font::RasterPlaneBounds plane = atlas_entry->plane_bounds_layout_pixels;
        plane.left /= raster_scale;
        plane.right /= raster_scale;
        plane.top /= raster_scale;
        plane.bottom /= raster_scale;
        glyphs.push_back(Entry{
            *atlas_entry,
            PreparedGlyph{
                Rect{
                    pen_x + plane.left,
                    logical.baseline - logical.y_placement - plane.top,
                    plane.right - plane.left,
                    plane.top - plane.bottom,
                },
                TextureRegion{
                    atlas_entry->u,
                    atlas_entry->v,
                    atlas_entry->uv_width,
                    atlas_entry->uv_height,
                },
                logical.baseline,
            },
        });
    }

    std::vector<PreparedText> groups;
    std::size_t begin = 0U;
    while (begin < glyphs.size()) {
        std::size_t end = begin + 1U;
        while (end < glyphs.size() &&
               glyphs[end].atlas.texture == glyphs[begin].atlas.texture &&
               glyphs[end].atlas.mode == glyphs[begin].atlas.mode) {
            ++end;
        }
        PreparedText text{
            command.origin,
            command.color,
            command.pixel_size,
            glyphs[begin].atlas.texture,
            glyphs[begin].atlas.mode,
            glyphs[begin].atlas.atlas_pixel_range,
            glyphs[begin].atlas.layout_pixel_range / raster_scale,
            {},
            command.effects,
            false,
            raster_scale,
        };
        text.glyphs.reserve(end - begin);
        for (std::size_t index = begin; index < end; ++index) {
            text.glyphs.push_back(std::move(glyphs[index].glyph));
        }
        groups.push_back(std::move(text));
        begin = end;
    }
    // A shadow is the run again under it: moved, in the shadow's colour, and softened.
    if (command.effects.shadowed() && !groups.empty()) {
        std::vector<PreparedText> shadowed;
        shadowed.reserve(groups.size() * 2U);
        for (const PreparedText& text : groups) {
            if (positioned(text.mode))
                continue;
            PreparedText shadow = text;
            shadow.origin.x += command.effects.shadow_x;
            shadow.origin.y += command.effects.shadow_y;
            shadow.color = command.effects.shadow_color;
            shadow.shadow = true;
            shadowed.push_back(std::move(shadow));
        }
        for (PreparedText& text : groups)
            shadowed.push_back(std::move(text));
        groups = std::move(shadowed);
    }
    return groups;
}

[[nodiscard]] bool same_text_content(
    const TextRunRenderCommand& left,
    const TextRunRenderCommand& right
) {
    return left.color == right.color && left.pixel_size == right.pixel_size &&
           left.font_rasterization == right.font_rasterization && left.glyphs == right.glyphs &&
           left.effects == right.effects;
}

void align_text_cache(
    const RenderCommandBuffer& commands,
    PreparationCache& cache
) {
    const std::size_t previous_size = cache.text.size();
    const std::size_t current_size = commands.size();
    const auto compatible = [&](const std::size_t previous, const std::size_t current) {
        const auto* text = std::get_if<TextRunRenderCommand>(
            &commands.commands()[current]
        );
        return text != nullptr
            ? cache.text[previous].has_value() &&
                same_text_content(cache.text[previous]->source, *text)
            : !cache.text[previous].has_value();
    };
    const std::size_t common = std::min(previous_size, current_size);
    std::size_t prefix = 0U;
    while (prefix < common && compatible(prefix, prefix)) ++prefix;
    std::size_t suffix = 0U;
    while (suffix < common - prefix &&
           compatible(previous_size - suffix - 1U, current_size - suffix - 1U)) {
        ++suffix;
    }
    if (previous_size == current_size && prefix + suffix == current_size) return;

    std::vector<std::optional<PreparedTextCacheEntry>> previous =
        std::move(cache.text);
    cache.text.resize(current_size);
    const auto move_slot = [&](const std::size_t previous_index,
                               const std::size_t current_index) {
        cache.text[current_index].swap(previous[previous_index]);
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
        const auto* text = std::get_if<TextRunRenderCommand>(
            &commands.commands()[index]
        );
        if (text == nullptr || cache.text[index].has_value()) continue;
        const auto found = std::ranges::find_if(
            cache.detached_text,
            [&](const PreparedTextCacheEntry& entry) {
                return same_text_content(entry.source, *text);
            }
        );
        if (found == cache.detached_text.end()) continue;
        const std::size_t detached_index =
            static_cast<std::size_t>(found - cache.detached_text.begin());
        cache.text[index].emplace(std::move(*found));
        if (detached_index + 1U != cache.detached_text.size()) {
            cache.detached_text[detached_index] =
                std::move(cache.detached_text.back());
        }
        cache.detached_text.pop_back();
    }
    for (std::optional<PreparedTextCacheEntry>& entry : previous) {
        if (entry.has_value()) {
            cache.detached_text.push_back(std::move(*entry));
        }
    }
    const std::size_t detached_limit = std::max(previous_size, current_size);
    if (cache.detached_text.size() > detached_limit) {
        cache.detached_text.erase(
            cache.detached_text.begin(),
            cache.detached_text.begin() +
                static_cast<std::ptrdiff_t>(
                    cache.detached_text.size() - detached_limit
                )
        );
    }
}

void append_warmup_requests(const TextRunRenderCommand& text, const TextEngine& text_engine,
                            const SubmissionContext& context, const double group_scale,
                            const bool subpixel, const bool unhinted,
                            std::vector<font::GlyphAtlasWarmupRequest>& requests) {
    const double raster_scale = effective_raster_scale(text, group_scale);
    requests.reserve(requests.size() + text.glyphs.size());
    for (const LogicalGlyph& logical : text.glyphs) {
        if (logical.glyph_id > std::numeric_limits<std::uint16_t>::max()) {
            throw font::FontError(
                "logical glyph id exceeds the native OpenType glyph range"
            );
        }
        const double requested_x = logical.x + logical.x_placement;
        requests.push_back(font::GlyphAtlasWarmupRequest{
            logical.font_id,
            &text_engine.font(logical.font_id),
            static_cast<std::uint16_t>(logical.glyph_id),
            text.pixel_size * raster_scale,
            font::SubpixelPhase::quantize(requested_x * context.scale * raster_scale,
                                          coverage_subpixel_divisions),
            logical.font_style_flags,
            run_raster_mode(text, subpixel),
            effect_reach(text) * raster_scale,
            unhinted,
        });
    }
}

/**
 * Each text command's group raster scale (one outside zoomed groups): the composed raster scale of
 * the innermost presentation group it is drawn in.
 */
[[nodiscard]] std::vector<double>
text_group_scales(const RenderCommandBuffer& commands,
                  const std::span<const GroupRasterScale> group_scales) {
    std::vector<double> result(commands.size(), 1.0);
    std::vector<std::uint32_t> groups;
    // A text run under a baked (static) scale rasterizes at that scale too, so `scale: 2` text is
    // sharp, not a magnified bitmap, and `scale: 1.05` text is not resampled.
    std::vector<double> baked{1.0};
    for (std::size_t index = 0U; index < commands.commands().size(); ++index) {
        const RenderCommand& source = commands.commands()[index];
        if (const auto* push = std::get_if<GroupPushRenderCommand>(&source); push != nullptr) {
            groups.push_back(push->group);
        } else if (std::holds_alternative<GroupPopRenderCommand>(source)) {
            if (!groups.empty())
                groups.pop_back();
        } else if (const auto* transform = std::get_if<TransformPushRenderCommand>(&source);
                   transform != nullptr) {
            const double scale = std::max(std::hypot(transform->m00, transform->m10),
                                          std::hypot(transform->m01, transform->m11));
            baked.push_back(baked.back() * (std::isfinite(scale) && scale > 0.0 ? scale : 1.0));
        } else if (std::holds_alternative<TransformPopRenderCommand>(source)) {
            if (baked.size() > 1U)
                baked.pop_back();
        } else if (std::holds_alternative<TextRunRenderCommand>(source)) {
            // A baked transform is static, so its scale is taken exactly and the run's texels land
            // one to one; a group's is already exact at rest and stepped while it moves.
            double scale = baked.back();
            if (!groups.empty()) {
                const auto found = std::ranges::lower_bound(group_scales, groups.back(), {},
                                                            &GroupRasterScale::group);
                if (found != group_scales.end() && found->group == groups.back())
                    scale *= found->text;
            }
            result[index] = std::clamp(scale, 0.125, 8.0);
        }
    }
    return result;
}

/**
 * Which text runs are in a group drawn scaled (a zoom): they rasterize without hinting, which would
 * move their stems and x-height a pixel at a time as the scale passes through sizes.
 */
[[nodiscard]] std::vector<bool>
zoomed_text_runs(const RenderCommandBuffer& commands,
                 const std::span<const GroupRasterScale> group_scales) {
    std::vector<bool> result(commands.size(), false);
    std::vector<std::uint32_t> groups;
    for (std::size_t index = 0U; index < commands.commands().size(); ++index) {
        const RenderCommand& source = commands.commands()[index];
        if (const auto* push = std::get_if<GroupPushRenderCommand>(&source)) {
            groups.push_back(push->group);
        } else if (std::holds_alternative<GroupPopRenderCommand>(source)) {
            if (!groups.empty())
                groups.pop_back();
        } else if (std::holds_alternative<TextRunRenderCommand>(source) && !groups.empty()) {
            const auto found = std::ranges::lower_bound(group_scales, groups.back(), {},
                                                        &GroupRasterScale::group);
            result[index] = found != group_scales.end() && found->group == groups.back() &&
                            found->text != 1.0;
        }
    }
    return result;
}

/**
 * Which text runs are drawn per subpixel: coverage runs drawn straight onto the surface, where
 * each colour channel can blend over what is already there. Text into a content effect's layer
 * (translucent until composited), under an authored material, under a transform or in a group
 * that turns, shears or scales it, or rasterized at a zoomed scale stays gray: its subpixels would
 * not land on the display's.
 */
[[nodiscard]] std::vector<bool>
subpixel_text_runs(const RenderCommandBuffer& commands, const std::span<const double> text_scales,
                   const std::span<const std::uint32_t> turned_groups,
                   const font::SubpixelOrder order) {
    std::vector<bool> result(commands.size(), false);
    if (order == font::SubpixelOrder::none)
        return result;
    std::size_t effects = 0U;
    std::size_t materials = 0U;
    std::vector<bool> transforms{false};
    std::vector<std::uint32_t> groups;
    for (std::size_t index = 0U; index < commands.commands().size(); ++index) {
        const RenderCommand& source = commands.commands()[index];
        if (std::holds_alternative<ContentEffectPushRenderCommand>(source)) {
            ++effects;
        } else if (std::holds_alternative<ContentEffectPopRenderCommand>(source)) {
            effects -= effects != 0U ? 1U : 0U;
        } else if (std::holds_alternative<MaterialPushRenderCommand>(source)) {
            ++materials;
        } else if (std::holds_alternative<MaterialPopRenderCommand>(source)) {
            materials -= materials != 0U ? 1U : 0U;
        } else if (const auto* push = std::get_if<GroupPushRenderCommand>(&source)) {
            groups.push_back(push->group);
        } else if (std::holds_alternative<GroupPopRenderCommand>(source)) {
            if (!groups.empty())
                groups.pop_back();
        } else if (const auto* transform = std::get_if<TransformPushRenderCommand>(&source)) {
            const bool moves_only = transform->m00 == 1.0 && transform->m11 == 1.0 &&
                                    transform->m01 == 0.0 && transform->m10 == 0.0;
            transforms.push_back(transforms.back() || !moves_only);
        } else if (std::holds_alternative<TransformPopRenderCommand>(source)) {
            if (transforms.size() > 1U)
                transforms.pop_back();
        } else if (const auto* text = std::get_if<TextRunRenderCommand>(&source)) {
            const bool turned =
                !groups.empty() && std::ranges::binary_search(turned_groups, groups.back());
            result[index] = effects == 0U && materials == 0U && !transforms.back() && !turned &&
                            text_scales[index] == 1.0 &&
                            raster_mode(text->font_rasterization) ==
                                font::GlyphRasterMode::coverage;
        }
    }
    return result;
}

[[nodiscard]] std::vector<bool> visible_text_commands(
    const RenderCommandBuffer& commands,
    const SubmissionContext& context
) {
    std::vector<bool> result(commands.size(), false);
    std::vector<Rect> clip_stack;
    std::vector<Transform> transform_stack;
    Rect clip{0.0, 0.0, context.logical_width, context.logical_height};
    Transform transform;
    // Grouped text moves on the GPU; its screen position is unknown here, so never cull it.
    std::size_t group_depth = 0U;
    for (std::size_t index = 0U; index < commands.commands().size(); ++index) {
        const RenderCommand& source = commands.commands()[index];
        if (std::holds_alternative<GroupPushRenderCommand>(source)) {
            ++group_depth;
            continue;
        }
        if (std::holds_alternative<GroupPopRenderCommand>(source)) {
            if (group_depth == 0U) throw std::logic_error("render group stack underflow");
            --group_depth;
            continue;
        }
        if (const auto* clip_push = std::get_if<ClipPushRenderCommand>(&source);
            clip_push != nullptr) {
            clip_stack.push_back(clip);
            clip = intersect(clip, transform.bounds(clip_push->rect));
        } else if (std::holds_alternative<ClipPopRenderCommand>(source)) {
            if (clip_stack.empty()) throw std::logic_error("render clip stack underflow");
            clip = clip_stack.back();
            clip_stack.pop_back();
        } else if (const auto* transform_push =
                       std::get_if<TransformPushRenderCommand>(&source);
                   transform_push != nullptr) {
            transform_stack.push_back(transform);
            transform = transform.concatenate(Transform{
                transform_push->m00,
                transform_push->m01,
                transform_push->m02,
                transform_push->m10,
                transform_push->m11,
                transform_push->m12,
            });
        } else if (std::holds_alternative<TransformPopRenderCommand>(source)) {
            if (transform_stack.empty()) {
                throw std::logic_error("render transform stack underflow");
            }
            transform = transform_stack.back();
            transform_stack.pop_back();
        } else if (const auto* text = std::get_if<TextRunRenderCommand>(&source);
                   text != nullptr) {
            result[index] = group_depth != 0U || !text->cull_bounds.has_value() ||
                !intersect(clip, transform.bounds(*text->cull_bounds)).empty();
        }
    }
    if (!clip_stack.empty() || !transform_stack.empty()) {
        throw std::logic_error("render command state stacks are unbalanced");
    }
    return result;
}

void append_draw(
    std::vector<PlannedItem>& output,
    PreparedCommand command,
    const std::uint32_t source_order,
    const Transform& transform,
    const Rect clip,
    const std::vector<SubmissionRoundedClip>& rounded_clips,
    const std::optional<MaterialState>& material_override,
    const double opacity,
    const std::uint32_t group,
    const SubmissionContext& context,
    std::size_t& skipped_draws
) {
    const Rect local_bounds = command_bounds(command);
    const Rect transformed_bounds = transform.bounds(local_bounds);
    const Rect visible = intersect(clip, transformed_bounds);
    const SubmissionScissor resolved_scissor = scissor(clip, context);
    const bool retained_empty = local_bounds.empty();
    // A grouped draw moves on the GPU, so only its (layout-space) clip can cull it here.
    if ((!retained_empty && group == 0U &&
         (transformed_bounds.empty() || visible.empty())) ||
        resolved_scissor.width == 0U || resolved_scissor.height == 0U) {
        ++skipped_draws;
        return;
    }
    CommandTexture texture = command_texture(command, context);
    if (texture.missing) {
        ++skipped_draws;
        return;
    }
    if (retained_empty) ++skipped_draws;
    MaterialState material = merged_material(command, material_override);
    // Scope opacity reaches the per-vertex material opacity, not the command's colours.
    material.opacity *= opacity;
    output.emplace_back(PreparedDraw{
        source_order,
        std::move(command),
        local_bounds,
        transform,
        std::move(material),
        std::move(texture.host_id),
        resolved_scissor,
        rounded_clips,
        group,
    });
}

/** FNV-1a over raw bytes: mask identities hash everything their pixels depend on. */
class MaskHash final {
  public:
    /** Adds a value's bytes eight at a time: a path of a thousand segments hashes each frame. */
    template <typename Value>
    void add(const Value& value) noexcept {
        static_assert(std::is_trivially_copyable_v<Value>);
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        std::size_t index = 0U;
        for (; index + 8U <= sizeof(Value); index += 8U) {
            std::uint64_t word;
            std::memcpy(&word, bytes + index, 8U);
            mix(word);
        }
        if constexpr (sizeof(Value) % 8U != 0U) {
            std::uint64_t word = 0U;
            std::memcpy(&word, bytes + index, sizeof(Value) - index);
            mix(word ^ (std::uint64_t{sizeof(Value) % 8U} << 56U));
        }
    }
    /** The state through a finalizer, so every input bit reaches every output bit. */
    [[nodiscard]] std::uint64_t value() const noexcept {
        std::uint64_t result = state_;
        result ^= result >> 33U;
        result *= 0xFF51AFD7ED558CCDULL;
        result ^= result >> 33U;
        result *= 0xC4CEB9FE1A85EC53ULL;
        result ^= result >> 33U;
        return result;
    }

  private:
    void mix(const std::uint64_t word) noexcept {
        state_ ^= word;
        state_ *= 0x9E3779B97F4A7C15ULL;
        state_ ^= state_ >> 29U;
    }

    std::uint64_t state_ = 1469598103934665603ULL;
};

/**
 * A gradient as bands over a mask: the affine map from the draw's local space to the gradient
 * position, and one band per interval between stops, the outer ones open-ended so they hold the
 * end colours past the stops.
 */
[[nodiscard]] MaskGradient mask_gradient(const Gradient& gradient, const Rect bounds) {
    MaskGradient result;
    result.paint = static_cast<float>(1U + static_cast<std::uint32_t>(gradient.kind) +
                                      3U * static_cast<std::uint32_t>(gradient.extend));
    const double width = std::max(bounds.width, 1.0e-9);
    const double height = std::max(bounds.height, 1.0e-9);
    // The gradient's frame composed with the normalized position
    // n = ((x - bounds.x) / width, (y - bounds.y) / height).
    const std::array<double, 6U> frame = gradient.frame(Size{bounds.width, bounds.height});
    for (std::size_t row = 0U; row < 2U; ++row) {
        const double a = frame[row * 3U] / width;
        const double b = frame[row * 3U + 1U] / height;
        result.position[row * 3U] = a;
        result.position[row * 3U + 1U] = b;
        result.position[row * 3U + 2U] = frame[row * 3U + 2U] - a * bounds.x - b * bounds.y;
    }
    const std::vector<GradientStop>& stops = gradient.stops;
    if (stops.empty())
        return result;
    for (std::size_t index = 1U; index < stops.size(); ++index) {
        if (stops[index].offset > stops[index - 1U].offset) {
            result.bands.push_back(MaskGradientBand{
                static_cast<float>(stops[index - 1U].offset),
                static_cast<float>(stops[index].offset), 0.0F, stops[index - 1U].color,
                stops[index].color});
        }
    }
    if (result.bands.empty()) {
        // Every stop at one offset: a hard step from the first colour to the last.
        const auto offset = static_cast<float>(stops.front().offset);
        result.bands.push_back(
            MaskGradientBand{offset, offset, 0.0F, stops.front().color, stops.back().color});
    }
    result.bands.front().open += 1.0F;
    result.bands.back().open += 2.0F;
    return result;
}

/**
 * How far, in device pixels, a flattened curve may stray from the true one. Chords lie inside a
 * curve, so this is area lost along every curved edge: a thirtieth of a pixel keeps a masked
 * circle within a few hundredths of a percent of its area.
 */
constexpr double mask_flatness = 0.03;

/** The largest mask edge, in device pixels, kept below an atlas page with its gutter. */
constexpr double maximum_mask_extent = 1000.0;
/** Art larger than a mask rasterizes in tiles of this many device pixels a side. */
constexpr double art_tile = 960.0;
/**
 * Larger shapes rasterize in tiles of this many device pixels a side, each with a one-pixel apron
 * of real coverage so sampling across a seam reads its neighbour's pixels. Empty tiles take
 * nothing and full ones a single shared texel, so only the outline's own tiles use atlas space.
 */
constexpr std::uint32_t mask_tile = 256U;
/** Past this many tiles (or this extent), a shape draws as geometry instead. */
constexpr double maximum_mask_tiles = 1024.0;
constexpr double maximum_tiled_extent = 16384.0;
/** Tile classes kept before the record starts over. */
constexpr std::size_t maximum_tile_classes = 16384U;

/** Where a draw's masks are rasterized: the device grid and the shape's position on it. */
struct MaskGrid final {
    bool aligned = true;
    double scale_x = 1.0;
    double scale_y = 1.0;
    /** Device pixels per local pixel along the shape's own axes. */
    double device_x = 1.0;
    double device_y = 1.0;
    /** The shape's origin: its whole device pixel and the sub-pixel phase in sixteenths. */
    Point whole;
    Point phase;
};

/**
 * Device pixels per local pixel. An axis-aligned draw is rasterized on the device grid itself; a
 * rotated or skewed one in its own axes at device density, its quad turned with it. A group settled
 * at another scale (whose translation lands on whole device pixels) is rasterized at its density
 * too. Nothing for a degenerate transform.
 */
[[nodiscard]] std::optional<MaskGrid> mask_grid(const Transform& transform, const Rect bounds,
                                                const SubmissionContext& context,
                                                const std::uint32_t group,
                                                const std::span<const GroupRasterScale> group_scales) {
    double group_x = 1.0;
    double group_y = 1.0;
    if (const auto found = std::ranges::lower_bound(group_scales, group, {}, &GroupRasterScale::group);
        found != group_scales.end() && found->group == group) {
        group_x = found->x;
        group_y = found->y;
    }
    MaskGrid grid;
    grid.scale_x = context.scale * group_x;
    grid.scale_y = context.scale * group_y;
    grid.aligned = transform.axis_aligned();
    grid.device_x = grid.aligned ? transform.m00 * grid.scale_x
                                 : std::hypot(transform.m00, transform.m10) * grid.scale_x;
    grid.device_y = grid.aligned ? transform.m11 * grid.scale_y
                                 : std::hypot(transform.m01, transform.m11) * grid.scale_y;
    if (!(std::abs(grid.device_x) > 1.0e-6) || !(std::abs(grid.device_y) > 1.0e-6))
        return std::nullopt;
    const Point origin =
        grid.aligned ? Point{(bounds.x * transform.m00 + transform.m02) * grid.scale_x,
                             (bounds.y * transform.m11 + transform.m12) * grid.scale_y}
                     : Point{0.0, 0.0};
    // Sub-pixel position in sixteenths, so a moving shape reuses a handful of masks.
    grid.whole = Point{std::floor(origin.x), std::floor(origin.y)};
    grid.phase = Point{std::round((origin.x - grid.whole.x) * 16.0) / 16.0,
                       std::round((origin.y - grid.whole.y) * 16.0) / 16.0};
    return grid;
}

/**
 * A mask's device box back in the draw's local space, and its texels turned to match: through the
 * transform's own axes when it is axis-aligned, and in the shape's axes otherwise.
 */
[[nodiscard]] std::pair<Rect, TextureRegion> mask_quad(const MaskGrid& grid,
                                                       const Transform& transform,
                                                       const Rect bounds,
                                                       const font::RasterPlaneBounds& box,
                                                       TextureRegion uv) {
    Rect quad;
    if (grid.aligned) {
        const double left =
            ((grid.whole.x + box.left) / grid.scale_x - transform.m02) / transform.m00;
        const double right =
            ((grid.whole.x + box.right) / grid.scale_x - transform.m02) / transform.m00;
        const double top =
            ((grid.whole.y + box.top) / grid.scale_y - transform.m12) / transform.m11;
        const double bottom =
            ((grid.whole.y + box.bottom) / grid.scale_y - transform.m12) / transform.m11;
        quad = Rect{std::min(left, right), std::min(top, bottom), std::abs(right - left),
                    std::abs(bottom - top)};
    } else {
        quad = Rect{bounds.x + box.left / grid.device_x, bounds.y + box.top / grid.device_y,
                    (box.right - box.left) / grid.device_x, (box.bottom - box.top) / grid.device_y};
    }
    if (grid.aligned && transform.m00 < 0.0) {
        uv.u += uv.width;
        uv.width = -uv.width;
    }
    if (grid.aligned && transform.m11 < 0.0) {
        uv.v += uv.height;
        uv.height = -uv.height;
    }
    return {quad, uv};
}

/** Adds a paint to a mask identity: its colour, or everything its gradient is made of. */
void hash_paint(MaskHash& hash, const std::optional<Paint>& paint) {
    hash.add(paint.has_value());
    if (!paint.has_value())
        return;
    if (const runtime::ColorValue* color = paint->color(); color != nullptr) {
        hash.add(*color);
        return;
    }
    const Gradient& gradient = *paint->gradient();
    hash.add(gradient.kind);
    hash.add(gradient.extend);
    hash.add(gradient.start);
    hash.add(gradient.end);
    hash.add(gradient.angle_degrees.value_or(std::numeric_limits<double>::quiet_NaN()));
    hash.add(gradient.center);
    hash.add(gradient.radius_x);
    hash.add(gradient.radius_y);
    hash.add(gradient.rotation_degrees);
    for (const GradientStop& stop : gradient.stops) {
        hash.add(stop.offset);
        hash.add(stop.color);
    }
}

/** A closed outline on the device grid, in device pixels from the shape's whole pixel. */
using DevicePolygons = std::vector<std::vector<host::RasterPoint>>;

/** The device box of an outline; empty until something is included. */
struct DeviceBox final {
    double left = std::numeric_limits<double>::infinity();
    double top = std::numeric_limits<double>::infinity();
    double right = -std::numeric_limits<double>::infinity();
    double bottom = -std::numeric_limits<double>::infinity();

    void include(const DevicePolygons& polygons) noexcept {
        for (const std::vector<host::RasterPoint>& polygon : polygons) {
            for (const host::RasterPoint point : polygon) {
                left = std::min(left, point.x);
                top = std::min(top, point.y);
                right = std::max(right, point.x);
                bottom = std::max(bottom, point.y);
            }
        }
    }
    [[nodiscard]] bool empty() const noexcept { return !(left <= right && top <= bottom); }
    [[nodiscard]] bool meets(const DeviceBox& other) const noexcept {
        return !empty() && left <= other.right && right >= other.left && top <= other.bottom &&
               bottom >= other.top;
    }
};

/**
 * Whether an edge of the outline passes through `box`, or within a quantum of it: when none does,
 * every pixel of the box is covered alike.
 */
[[nodiscard]] bool edges_cross(const DevicePolygons& polygons, const DeviceBox& box) noexcept {
    constexpr double margin = 1.0 / 64.0;
    const double left = box.left - margin;
    const double top = box.top - margin;
    const double right = box.right + margin;
    const double bottom = box.bottom + margin;
    for (const std::vector<host::RasterPoint>& polygon : polygons) {
        for (std::size_t index = 0U; index < polygon.size(); ++index) {
            const host::RasterPoint a = polygon[index];
            const host::RasterPoint b = polygon[(index + 1U) % polygon.size()];
            if (std::max(a.x, b.x) < left || std::min(a.x, b.x) > right ||
                std::max(a.y, b.y) < top || std::min(a.y, b.y) > bottom) {
                continue;
            }
            // The part of the edge inside the box, if any (Liang-Barsky).
            double enter = 0.0;
            double leave = 1.0;
            const auto clip = [&](const double p, const double q) {
                if (p == 0.0)
                    return q >= 0.0;
                const double r = q / p;
                if (p < 0.0) {
                    if (r > leave)
                        return false;
                    enter = std::max(enter, r);
                } else {
                    if (r < enter)
                        return false;
                    leave = std::min(leave, r);
                }
                return true;
            };
            const double dx = b.x - a.x;
            const double dy = b.y - a.y;
            if (clip(-dx, a.x - left) && clip(dx, right - a.x) && clip(-dy, a.y - top) &&
                clip(dy, bottom - a.y) && enter <= leave) {
                return true;
            }
        }
    }
    return false;
}

/** Whether the outline covers a point, under its fill rule. */
[[nodiscard]] bool covers(const DevicePolygons& polygons, const host::RasterFill fill,
                          const double x, const double y) noexcept {
    int winding = 0;
    for (const std::vector<host::RasterPoint>& polygon : polygons) {
        for (std::size_t index = 0U; index < polygon.size(); ++index) {
            const host::RasterPoint a = polygon[index];
            const host::RasterPoint b = polygon[(index + 1U) % polygon.size()];
            if ((a.y <= y) == (b.y <= y))
                continue;
            if (a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y) < x)
                winding += b.y > a.y ? 1 : -1;
        }
    }
    return fill == host::RasterFill::even_odd ? (winding & 1) != 0 : winding != 0;
}

/** A stroke's centreline runs on the device grid, which the backend expands into the stroke. */
using DeviceRuns = std::vector<host::RasterStrokeRun>;

/** The device box of a stroke: its runs grown by its reach. */
[[nodiscard]] DeviceBox stroke_box(const DeviceRuns& runs, const double reach) noexcept {
    DeviceBox box;
    for (const host::RasterStrokeRun& run : runs) {
        for (const host::RasterPoint point : run.points) {
            box.left = std::min(box.left, point.x - reach);
            box.top = std::min(box.top, point.y - reach);
            box.right = std::max(box.right, point.x + reach);
            box.bottom = std::max(box.bottom, point.y + reach);
        }
    }
    return box;
}

/** Whether any segment of a stroke, grown by its reach, meets `box`. */
[[nodiscard]] bool stroke_meets(const DeviceRuns& runs, const double reach,
                                const DeviceBox& box) noexcept {
    for (const host::RasterStrokeRun& run : runs) {
        const std::size_t count = run.points.size();
        const std::size_t segments = run.closed ? count : count - (count > 0U ? 1U : 0U);
        for (std::size_t index = 0U; index < segments; ++index) {
            const host::RasterPoint a = run.points[index];
            const host::RasterPoint b = run.points[(index + 1U) % count];
            if (std::max(a.x, b.x) + reach >= box.left && std::min(a.x, b.x) - reach <= box.right &&
                std::max(a.y, b.y) + reach >= box.top && std::min(a.y, b.y) - reach <= box.bottom) {
                return true;
            }
        }
    }
    return false;
}

/**
 * A stroke's style in device pixels for the backend to expand, or nothing when the device grid
 * scales the shape's axes differently: the backend's pen is round, so such a stroke (an elliptic
 * pen on screen) is outlined on the CPU instead.
 */
[[nodiscard]] std::optional<host::RasterStroke> device_stroke(const StrokeStyle& style,
                                                              const MaskGrid& grid) {
    const double x = std::abs(grid.device_x);
    const double y = std::abs(grid.device_y);
    if (std::abs(x - y) > 1.0e-3 * std::max(x, y))
        return std::nullopt;
    host::RasterStroke stroke;
    stroke.half_width = static_cast<float>(style.width * 0.5 * x);
    stroke.join = static_cast<host::RasterJoin>(style.join);
    stroke.cap = static_cast<host::RasterCap>(style.cap);
    stroke.miter_limit = static_cast<float>(std::max(style.miter_limit, 1.0));
    stroke.tolerance = static_cast<float>(mask_flatness);
    return stroke;
}

/** Centrelines (in the shape's pixels) dashed into runs and placed on the device grid. */
[[nodiscard]] DeviceRuns device_runs(const std::vector<PathContour>& centrelines,
                                     const StrokeStyle& style, const MaskGrid& grid) {
    DeviceRuns runs;
    for (StrokeRun& run : stroke_runs(centrelines, style)) {
        host::RasterStrokeRun placed;
        placed.closed = run.closed;
        placed.points.reserve(run.points.size());
        for (const Point point : run.points)
            placed.points.push_back(host::RasterPoint{grid.phase.x + point.x * grid.device_x,
                                                      grid.phase.y + point.y * grid.device_y});
        runs.push_back(std::move(placed));
    }
    return runs;
}

/** A colour as a raster job packs it: straight RGBA8, red in the low byte. */
[[nodiscard]] std::uint32_t packed_color(const runtime::ColorValue color) noexcept {
    return std::uint32_t{color.red} | (std::uint32_t{color.green} << 8U) |
           (std::uint32_t{color.blue} << 16U) | (std::uint32_t{color.alpha} << 24U);
}

/**
 * A paint as a raster layer whose content's top-left is `origin` on the device grid: its colour,
 * or its gradient's parameter as a function of the content pixel and its colours as a table.
 */
[[nodiscard]] host::RasterLayer raster_paint(const Paint& paint, const MaskGrid& grid,
                                             const Rect bounds, const host::RasterPoint origin) {
    host::RasterLayer layer;
    const Gradient* gradient = paint.gradient();
    if (gradient == nullptr) {
        layer.paint = host::RasterPaint::solid;
        layer.color = packed_color(*paint.color());
        return layer;
    }
    // The shape's normalized position at content pixel x: x * scale + offset, per axis.
    const double scale_x = 1.0 / (grid.device_x * bounds.width);
    const double scale_y = 1.0 / (grid.device_y * bounds.height);
    const double offset_x = (origin.x - grid.phase.x) * scale_x;
    const double offset_y = (origin.y - grid.phase.y) * scale_y;
    switch (gradient->kind) {
    case GradientKind::linear:
        layer.paint = host::RasterPaint::linear;
        break;
    case GradientKind::radial:
        layer.paint = host::RasterPaint::radial;
        break;
    case GradientKind::conic:
        layer.paint = host::RasterPaint::conic;
        break;
    }
    // The gradient's frame composed with the content pixel's normalized position.
    const std::array<double, 6U> frame = gradient->frame(Size{bounds.width, bounds.height});
    std::array<double, 6U> parameters{};
    for (std::size_t row = 0U; row < 2U; ++row) {
        parameters[row * 3U] = frame[row * 3U] * scale_x;
        parameters[row * 3U + 1U] = frame[row * 3U + 1U] * scale_y;
        parameters[row * 3U + 2U] =
            frame[row * 3U] * offset_x + frame[row * 3U + 1U] * offset_y + frame[row * 3U + 2U];
    }
    for (std::size_t index = 0U; index < parameters.size(); ++index)
        layer.parameters[index] = static_cast<float>(parameters[index]);
    layer.extend = static_cast<host::RasterExtend>(gradient->extend);
    // The table spans offsets 0 to 1; the job applies the extend before reading it.
    Gradient clamped = *gradient;
    clamped.extend = GradientExtend::clamp;
    layer.lut.resize(host::raster_lut_size);
    for (std::uint32_t index = 0U; index < host::raster_lut_size; ++index) {
        layer.lut[index] = packed_color(clamped.sample(
            static_cast<double>(index) / static_cast<double>(host::raster_lut_size - 1U)));
    }
    return layer;
}

/** The corners of a `width` x `height` rectangle at the origin, as one polygon. */
[[nodiscard]] DevicePolygons rectangle(const double width, const double height) {
    return {{host::RasterPoint{0.0, 0.0}, host::RasterPoint{width, 0.0},
             host::RasterPoint{width, height}, host::RasterPoint{0.0, height}}};
}

/**
 * A 4 x 4 texel of one colour (or of full coverage), which tiles nothing crosses draw from: the
 * colour packed as `rgba` (red highest) for a picture, or nothing for coverage.
 */
[[nodiscard]] std::optional<font::GlyphAtlasEntry> solid_texel(font::GlyphAtlas& atlas,
                                                              const std::optional<std::uint32_t> rgba) {
    char key[24];
    if (rgba.has_value())
        std::snprintf(key, sizeof(key), "solid-%08x", *rgba);
    else
        std::snprintf(key, sizeof(key), "solid");
    return atlas.request_raster(
        key, rgba.has_value() ? font::GlyphRasterMode::msdf : font::GlyphRasterMode::coverage,
        [&](const std::uint32_t gutter) -> std::optional<font::RasterRequest> {
            host::RasterJobBuilder builder(4U, 4U);
            host::RasterLayer layer;
            if (rgba.has_value()) {
                layer.paint = host::RasterPaint::solid;
                layer.color = ((*rgba >> 24U) & 0xFFU) | (((*rgba >> 16U) & 0xFFU) << 8U) |
                              (((*rgba >> 8U) & 0xFFU) << 16U) | ((*rgba & 0xFFU) << 24U);
            }
            const DevicePolygons square = rectangle(4.0, 4.0);
            builder.add_layer(host::RasterFill::nonzero, layer, square, host::RasterPoint{});
            return font::RasterRequest{builder.encode(gutter, gutter), 4U, 4U,
                                       font::RasterPlaneBounds{0.0, 4.0, 4.0, 0.0}};
        });
}

/**
 * Draws joined shapes (PathRenderCommand::joined) as one picture: every part of every shape is
 * rasterized to exact coverage on one device grid, then composited front to back by area into one
 * colour mask. Each shape takes what is left of a pixel, so shapes meeting along an edge split
 * its pixels between them and nothing behind shows through, while a shape over another covers it
 * as it would drawn alone (a translucent one lets the one below show through by its alpha). Paints
 * are evaluated per pixel. The backend rasterizes the picture from its outlines (a raster job). A
 * picture larger than a page draws in tiles, as a single shape's mask does. Returns false, drawing
 * nothing, when there is no shape atlas or the picture is too large even for tiles; the shapes
 * then draw one by one.
 */
[[nodiscard]] bool append_joined_masks(
    std::vector<PlannedItem>& output,
    const PathRenderCommand& command,
    const std::uint32_t source_order,
    const Transform& transform,
    const Rect clip,
    const std::vector<SubmissionRoundedClip>& rounded_clips,
    const double opacity,
    const std::uint32_t group,
    const SubmissionContext& context,
    std::size_t& skipped_draws,
    font::GlyphAtlas* const atlas,
    std::unordered_map<std::uint64_t, std::uint64_t>& tile_classes,
    const std::span<const GroupRasterScale> group_scales
) {
    const Rect bounds = command.bounds;
    if (atlas == nullptr || bounds.width <= 0.0 || bounds.height <= 0.0)
        return false;
    const std::optional<MaskGrid> grid = mask_grid(transform, bounds, context, group, group_scales);
    if (!grid.has_value())
        return false;
    std::vector<const PathShape*> shapes{&command.shape};
    for (const PathShape& shape : command.joined)
        shapes.push_back(&shape);
    const double tolerance = mask_flatness / std::max(std::min(std::abs(grid->device_x),
                                                               std::abs(grid->device_y)),
                                                      1.0e-3);

    MaskHash hash;
    hash.add(std::uint32_t{0x4A4F494EU});
    // The control points bound every outline, so they give the picture's device box unflattened.
    double control_left = std::numeric_limits<double>::infinity();
    double control_top = std::numeric_limits<double>::infinity();
    double control_right = -std::numeric_limits<double>::infinity();
    double control_bottom = -std::numeric_limits<double>::infinity();
    for (const PathShape* shape : shapes) {
        hash.add(shape->path.segments().size());
        const StrokeStyle style = shape->stroke_style.value_or(StrokeStyle{});
        double reach = 0.0;
        if (shape->stroke.has_value() && style.width > 0.0) {
            double factor = 1.0;
            if (style.join == PathJoin::miter)
                factor = std::max(factor, style.miter_limit);
            if (style.cap == PathCap::square)
                factor = std::max(factor, std::numbers::sqrt2);
            reach = style.width * 0.5 * factor;
        }
        const auto include = [&](const Point point) {
            control_left = std::min(control_left, point.x * bounds.width - reach);
            control_top = std::min(control_top, point.y * bounds.height - reach);
            control_right = std::max(control_right, point.x * bounds.width + reach);
            control_bottom = std::max(control_bottom, point.y * bounds.height + reach);
        };
        for (const PathSegment& segment : shape->path.segments()) {
            hash.add(segment);
            if (segment.verb == PathVerb::close)
                continue;
            include(segment.to);
            if (segment.verb == PathVerb::quadratic || segment.verb == PathVerb::cubic)
                include(segment.control_a);
            if (segment.verb == PathVerb::cubic)
                include(segment.control_b);
        }
        hash.add(shape->fill_rule);
        hash_paint(hash, shape->fill);
        hash_paint(hash, shape->stroke);
        hash.add(style.width);
        hash.add(style.cap);
        hash.add(style.join);
        hash.add(style.miter_limit);
        hash.add(style.dash_offset);
        for (const double dash : style.dash)
            hash.add(dash);
    }
    if (!std::isfinite(control_left))
        return true;
    hash.add(bounds.width);
    hash.add(bounds.height);
    hash.add(grid->device_x);
    hash.add(grid->device_y);
    hash.add(grid->phase);
    const double x_a = grid->phase.x + control_left * grid->device_x;
    const double x_b = grid->phase.x + control_right * grid->device_x;
    const double y_a = grid->phase.y + control_top * grid->device_y;
    const double y_b = grid->phase.y + control_bottom * grid->device_y;
    const double box_left = std::min(x_a, x_b) - 1.0;
    const double box_right = std::max(x_a, x_b) + 1.0;
    const double box_top = std::min(y_a, y_b) - 1.0;
    const double box_bottom = std::max(y_a, y_b) + 1.0;

    // Every part's outline on the device grid with the paint it carries, front to back; made once,
    // when a picture is missing.
    struct Layer final {
        DevicePolygons polygons;
        host::RasterFill fill = host::RasterFill::nonzero;
        const Paint* paint = nullptr;
        DeviceBox box;
        /** A stroke the backend expands: its runs and style, instead of polygons. */
        DeviceRuns runs;
        std::optional<host::RasterStroke> pen;
        /** Whether any of its outline or stroke passes through `region`. */
        [[nodiscard]] bool crosses(const DeviceBox& region) const {
            if (!box.meets(region))
                return false;
            return pen.has_value() ? stroke_meets(runs, host::raster_stroke_reach(*pen), region)
                                   : edges_cross(polygons, region);
        }
    };
    std::optional<std::vector<Layer>> made;
    const auto layers = [&]() -> const std::vector<Layer>& {
        if (made.has_value())
            return *made;
        made.emplace();
        const auto device = [&](const std::vector<PathContour>& outline) {
            DevicePolygons polygons;
            polygons.reserve(outline.size());
            for (const PathContour& contour : outline) {
                std::vector<host::RasterPoint> polygon;
                polygon.reserve(contour.points.size());
                for (const Point point : contour.points)
                    polygon.push_back(host::RasterPoint{grid->phase.x + point.x * grid->device_x,
                                                        grid->phase.y + point.y * grid->device_y});
                polygons.push_back(std::move(polygon));
            }
            return polygons;
        };
        for (const PathShape* shape : shapes) {
            std::vector<PathContour> centrelines =
                flatten_path(shape->path, Size{bounds.width, bounds.height}, tolerance);
            for (PathContour& contour : centrelines) {
                for (Point& point : contour.points) {
                    point.x *= bounds.width;
                    point.y *= bounds.height;
                }
            }
            if (shape->fill.has_value() && !shape->fill->transparent()) {
                std::vector<PathContour> outline;
                for (const PathContour& contour : centrelines) {
                    if (contour.points.size() >= 3U)
                        outline.push_back(PathContour{contour.points, true});
                }
                made->push_back(Layer{device(outline),
                                      shape->fill_rule == PathFillRule::evenodd
                                          ? host::RasterFill::even_odd
                                          : host::RasterFill::nonzero,
                                      &*shape->fill, {}, {}, std::nullopt});
            }
            const StrokeStyle style = shape->stroke_style.value_or(StrokeStyle{});
            if (shape->stroke.has_value() && !shape->stroke->transparent() && style.width > 0.0) {
                if (const std::optional<host::RasterStroke> pen = device_stroke(style, *grid)) {
                    made->push_back(Layer{{}, host::RasterFill::nonzero, &*shape->stroke, {},
                                          device_runs(centrelines, style, *grid), pen});
                } else {
                    made->push_back(Layer{device(stroke_outline(centrelines, style, tolerance)),
                                          host::RasterFill::nonzero, &*shape->stroke, {}, {},
                                          std::nullopt});
                }
            }
        }
        std::ranges::reverse(*made);
        for (Layer& layer : *made) {
            if (layer.pen.has_value())
                layer.box = stroke_box(layer.runs, host::raster_stroke_reach(*layer.pen));
            else
                layer.box.include(layer.polygons);
        }
        return *made;
    };

    // The picture over a `width` x `height` region whose top-left is `origin`, as a raster job.
    const auto picture = [&](const std::uint32_t width, const std::uint32_t height,
                             const host::RasterPoint origin, const std::uint32_t gutter) {
        host::RasterJobBuilder builder(width, height);
        const DeviceBox region{origin.x, origin.y, origin.x + width, origin.y + height};
        bool any = false;
        for (const Layer& layer : layers()) {
            if (!layer.box.meets(region))
                continue;
            if (layer.pen.has_value()) {
                builder.add_stroke_layer(raster_paint(*layer.paint, *grid, bounds, origin),
                                         *layer.pen, layer.runs, origin);
            } else {
                builder.add_layer(layer.fill, raster_paint(*layer.paint, *grid, bounds, origin),
                                  layer.polygons, origin);
            }
            any = true;
        }
        if (!any) {
            host::RasterLayer clear;
            clear.paint = host::RasterPaint::solid;
            clear.color = 0U;
            builder.add_layer(host::RasterFill::nonzero, clear, {}, origin);
        }
        return builder.encode(gutter, gutter);
    };

    const auto draw = [&](const font::GlyphAtlasEntry& entry, const font::RasterPlaneBounds& box,
                          const TextureRegion texels) {
        const auto [quad, uv] = mask_quad(*grid, transform, bounds, box, texels);
        append_draw(output,
                    PreparedCommand{std::make_shared<const PreparedMask>(PreparedMask{
                        quad, uv, RenderColor{255U, 255U, 255U, 255U}, entry.texture,
                        std::nullopt, true})},
                    source_order, transform, clip, rounded_clips, std::nullopt, opacity, group,
                    context, skipped_draws);
    };

    if (box_right - box_left <= maximum_mask_extent && box_bottom - box_top <= maximum_mask_extent) {
        char key[40];
        std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(hash.value()));
        const std::optional<font::GlyphAtlasEntry> entry = atlas->request_raster(
            key, font::GlyphRasterMode::msdf,
            [&](const std::uint32_t gutter) -> std::optional<font::RasterRequest> {
                const double x0 = std::floor(box_left);
                const double y0 = std::floor(box_top);
                const auto width = static_cast<std::uint32_t>(std::ceil(box_right) - x0);
                const auto height = static_cast<std::uint32_t>(std::ceil(box_bottom) - y0);
                return font::RasterRequest{picture(width, height, host::RasterPoint{x0, y0}, gutter),
                                           width, height,
                                           font::RasterPlaneBounds{x0, y0 + height, x0 + width, y0}};
            });
        if (!entry.has_value())
            return false;
        draw(*entry, entry->plane_bounds_layout_pixels,
             TextureRegion{entry->u, entry->v, entry->uv_width, entry->uv_height});
        return true;
    }

    // Tiles over the device box, each with a one-pixel apron of the picture around its core. A
    // tile no edge crosses is empty or the front-most covering shape's colour throughout.
    const double tile = static_cast<double>(mask_tile);
    const auto first_x = static_cast<std::int64_t>(std::floor(box_left / tile));
    const auto last_x = static_cast<std::int64_t>(std::floor(box_right / tile));
    const auto first_y = static_cast<std::int64_t>(std::floor(box_top / tile));
    const auto last_y = static_cast<std::int64_t>(std::floor(box_bottom / tile));
    if (box_right - box_left > maximum_tiled_extent || box_bottom - box_top > maximum_tiled_extent ||
        static_cast<double>(last_x - first_x + 1) * static_cast<double>(last_y - first_y + 1) >
            maximum_mask_tiles) {
        return false;
    }
    if (tile_classes.size() > maximum_tile_classes)
        tile_classes.clear();
    constexpr std::uint32_t side = mask_tile + 2U;
    // Every tile is found before any draws, so a picture that cannot fit draws whole the other way.
    struct Placed final {
        font::GlyphAtlasEntry entry;
        font::RasterPlaneBounds core;
        TextureRegion uv;
    };
    std::vector<Placed> placed;
    for (std::int64_t tile_y = first_y; tile_y <= last_y; ++tile_y) {
        for (std::int64_t tile_x = first_x; tile_x <= last_x; ++tile_x) {
            MaskHash tile_hash = hash;
            tile_hash.add(tile_x);
            tile_hash.add(tile_y);
            const std::uint64_t identity = tile_hash.value();
            const font::RasterPlaneBounds core{
                static_cast<double>(tile_x) * tile, static_cast<double>(tile_y + 1) * tile,
                static_cast<double>(tile_x + 1) * tile, static_cast<double>(tile_y) * tile};
            std::optional<std::uint32_t> uniform;
            if (const auto known = tile_classes.find(identity); known != tile_classes.end()) {
                if (known->second == 0U)
                    continue;
                if ((known->second & 0xFFU) == 2U)
                    uniform = static_cast<std::uint32_t>(known->second >> 8U);
            }
            bool empty = false;
            std::optional<font::GlyphAtlasEntry> entry;
            if (!uniform.has_value()) {
                char key[48];
                std::snprintf(key, sizeof(key), "%016llx",
                              static_cast<unsigned long long>(identity));
                entry = atlas->request_raster(
                    key, font::GlyphRasterMode::msdf,
                    [&](const std::uint32_t gutter) -> std::optional<font::RasterRequest> {
                        const host::RasterPoint origin{core.left - 1.0, core.top - 1.0};
                        const DeviceBox region{origin.x, origin.y, origin.x + side, origin.y + side};
                        const bool crossed = std::ranges::any_of(
                            layers(), [&](const Layer& layer) { return layer.crosses(region); });
                        if (!crossed) {
                            const double x = origin.x + side * 0.5;
                            const double y = origin.y + side * 0.5;
                            const Layer* front = nullptr;
                            for (const Layer& layer : layers()) {
                                // A stroke that reaches none of the tile covers none of it.
                                if (!layer.pen.has_value() && layer.box.meets(region) &&
                                    covers(layer.polygons, layer.fill, x, y)) {
                                    front = &layer;
                                    break;
                                }
                            }
                            if (front == nullptr) {
                                empty = true;
                                return std::nullopt;
                            }
                            if (const runtime::ColorValue* color = front->paint->color();
                                color != nullptr && color->alpha == 255U) {
                                uniform = (std::uint32_t{color->red} << 24U) |
                                          (std::uint32_t{color->green} << 16U) |
                                          (std::uint32_t{color->blue} << 8U) | color->alpha;
                                return std::nullopt;
                            }
                        }
                        return font::RasterRequest{
                            picture(side, side, origin, gutter), side, side,
                            font::RasterPlaneBounds{core.left - 1.0, core.bottom + 1.0,
                                                    core.right + 1.0, core.top - 1.0}};
                    });
                if (empty) {
                    tile_classes[identity] = 0U;
                    continue;
                }
                if (uniform.has_value())
                    tile_classes[identity] = (std::uint64_t{*uniform} << 8U) | 2U;
            }
            if (uniform.has_value()) {
                const std::optional<font::GlyphAtlasEntry> texel = solid_texel(*atlas, *uniform);
                if (!texel.has_value())
                    return false;
                placed.push_back(Placed{*texel, core,
                                        TextureRegion{texel->u + texel->uv_width * 0.5,
                                                      texel->v + texel->uv_height * 0.5, 0.0, 0.0}});
                continue;
            }
            if (!entry.has_value())
                return false;
            // The core's texels: the apron stays outside the quad.
            const double inset_u = entry->uv_width / static_cast<double>(side);
            const double inset_v = entry->uv_height / static_cast<double>(side);
            placed.push_back(Placed{*entry, core,
                                    TextureRegion{entry->u + inset_u, entry->v + inset_v,
                                                  entry->uv_width - 2.0 * inset_u,
                                                  entry->uv_height - 2.0 * inset_v}});
        }
    }
    for (const Placed& tile_draw : placed)
        draw(tile_draw.entry, tile_draw.core, tile_draw.uv);
    return true;
}

/**
 * Draws art (PathRenderCommand::art): each part (fill, stroke) is a coverage mask over the
 * canvas whose raster job carries the art's program instead of edges, drawn with the part's paint
 * as a shape's mask is. The mask is cached by everything it depends on but the time, so the CPU
 * plans it once while the backend draws it again every frame. A stroke under a scale that differs
 * by axis is left out (the backend's pen is round). A canvas larger than a page rasterizes in
 * tiles. Returns false when there is no shape atlas or the canvas is too large even for tiles.
 */
[[nodiscard]] bool
append_art_masks(std::vector<PlannedItem>& output, const PathRenderCommand& command,
                 const std::uint32_t source_order, const Transform& transform, const Rect clip,
                 const std::vector<SubmissionRoundedClip>& rounded_clips, const double opacity,
                 const std::uint32_t group, const SubmissionContext& context,
                 std::size_t& skipped_draws, font::GlyphAtlas* const atlas,
                 const std::span<const GroupRasterScale> group_scales,
                 PreparationCache::VolatileShape* const animated) {
    const ArtOutline& outline = *command.art;
    const PathShape& shape = command.shape;
    const Rect bounds = command.bounds;
    if (atlas == nullptr || bounds.width <= 0.0 || bounds.height <= 0.0)
        return false;
    const std::optional<MaskGrid> grid = mask_grid(transform, bounds, context, group, group_scales);
    if (!grid.has_value())
        return false;
    const StrokeStyle style = shape.stroke_style.value_or(StrokeStyle{});
    struct Part final {
        bool stroke = false;
        const Paint* paint = nullptr;
        std::optional<host::RasterStroke> pen;
    };
    std::vector<Part> parts;
    if (shape.fill.has_value() && !shape.fill->transparent())
        parts.push_back(Part{false, &*shape.fill, std::nullopt});
    if (shape.stroke.has_value() && !shape.stroke->transparent() && style.width > 0.0) {
        if (const std::optional<host::RasterStroke> pen = device_stroke(style, *grid))
            parts.push_back(Part{true, &*shape.stroke, pen});
    }
    // The canvas on the device grid, from the shape's whole pixel.
    const double canvas_x = grid->phase.x;
    const double canvas_y = grid->phase.y;
    const double canvas_width = bounds.width * grid->device_x;
    const double canvas_height = bounds.height * grid->device_y;
    MaskHash program;
    program.add(std::uint32_t{0x41525421U});
    program.add(std::hash<std::string>{}(outline.source));
    program.add(outline.source.size());
    program.add(outline.count);
    program.add(outline.closed);
    program.add(outline.segments);
    program.add(outline.parameters);
    program.add(shape.fill_rule);
    program.add(bounds.width);
    program.add(bounds.height);
    program.add(grid->device_x);
    program.add(grid->device_y);
    program.add(grid->phase);
    struct Piece final {
        const Part* part = nullptr;
        font::GlyphAtlasEntry entry;
        /** The device pixels the piece draws, and their texels. */
        font::RasterPlaneBounds core;
        TextureRegion uv;
    };
    std::vector<Piece> pieces;
    std::size_t slot_index = 0U;
    for (std::size_t part_index = 0U; part_index < parts.size(); ++part_index) {
        const Part& part = parts[part_index];
        const double reach = part.pen.has_value() ? host::raster_stroke_reach(*part.pen) : 0.0;
        const double left = std::floor(std::min(canvas_x, canvas_x + canvas_width) - reach - 1.0);
        const double right = std::ceil(std::max(canvas_x, canvas_x + canvas_width) + reach + 1.0);
        const double top = std::floor(std::min(canvas_y, canvas_y + canvas_height) - reach - 1.0);
        const double bottom = std::ceil(std::max(canvas_y, canvas_y + canvas_height) + reach + 1.0);
        if (right - left > maximum_tiled_extent || bottom - top > maximum_tiled_extent)
            return false;
        MaskHash hash = program;
        hash.add(part.stroke);
        if (part.stroke) {
            hash.add(style.width);
            hash.add(style.cap);
            hash.add(style.join);
            hash.add(style.miter_limit);
        }
        // A canvas larger than a page (a whole page's background, say) rasterizes in tiles, each
        // the same program over its own part of the canvas, with a one-pixel apron of real
        // coverage around its core so sampling across a seam reads its neighbour's pixels.
        const bool whole =
            right - left <= maximum_mask_extent && bottom - top <= maximum_mask_extent;
        const double step = whole ? std::max(right - left, bottom - top) : art_tile;
        const double apron = whole ? 0.0 : 1.0;
        for (double tile_top = top; tile_top < bottom; tile_top += step) {
            for (double tile_left = left; tile_left < right; tile_left += step) {
                const double core_right = std::min(tile_left + step, right);
                const double core_bottom = std::min(tile_top + step, bottom);
                const double x0 = tile_left - apron;
                const double y0 = tile_top - apron;
                const auto width = static_cast<std::uint32_t>(core_right + apron - x0);
                const auto height = static_cast<std::uint32_t>(core_bottom + apron - y0);
                MaskHash tile_hash = hash;
                tile_hash.add(tile_left - left);
                tile_hash.add(tile_top - top);
                char key[40];
                std::snprintf(key, sizeof(key), "%016llx",
                              static_cast<unsigned long long>(tile_hash.value()));
                const auto request =
                    [&](const std::uint32_t gutter) -> std::optional<font::RasterRequest> {
                    host::RasterArt art;
                    art.source = outline.source;
                    art.count = outline.count;
                    art.closed = outline.closed;
                    art.segments = outline.segments && part.stroke;
                    art.stroked = part.stroke;
                    art.x = static_cast<float>(canvas_x - x0);
                    art.y = static_cast<float>(canvas_y - y0);
                    art.width = static_cast<float>(canvas_width);
                    art.height = static_cast<float>(canvas_height);
                    art.parameters = outline.parameters;
                    host::RasterJobBuilder builder(width, height);
                    builder.add_art_layer(shape.fill_rule == PathFillRule::evenodd
                                              ? host::RasterFill::even_odd
                                              : host::RasterFill::nonzero,
                                          host::RasterLayer{},
                                          part.pen.value_or(host::RasterStroke{}), std::move(art));
                    return font::RasterRequest{
                        builder.encode(gutter, gutter), width, height,
                        font::RasterPlaneBounds{x0, y0 + height, x0 + width, y0}};
                };
                std::optional<font::GlyphAtlasEntry> entry;
                if (animated != nullptr) {
                    // Parameters that change every update (a pointer's place, say): the art
                    // rewrites its own slots, rather than making new art each time.
                    std::optional<font::RasterRequest> next = request(atlas->gutter());
                    if (!next.has_value())
                        return false;
                    if (animated->slots.size() <= slot_index)
                        animated->slots.resize(slot_index + 1U);
                    std::optional<font::RasterSlot>& slot = animated->slots[slot_index];
                    if (!slot.has_value() ||
                        !atlas->raster_slot_fits(*slot, next->width, next->height)) {
                        slot = atlas->reserve_raster_slot(next->width, next->height,
                                                          font::GlyphRasterMode::coverage);
                        if (!slot.has_value() ||
                            !atlas->raster_slot_fits(*slot, next->width, next->height)) {
                            return false;
                        }
                    }
                    entry = atlas->write_raster_slot(*slot, std::move(*next));
                } else {
                    entry = atlas->request_raster(key, font::GlyphRasterMode::coverage, request);
                }
                ++slot_index;
                if (!entry.has_value())
                    return false;
                // The core's texels: the apron stays outside the quad.
                const double inset_u = entry->uv_width * apron / static_cast<double>(width);
                const double inset_v = entry->uv_height * apron / static_cast<double>(height);
                pieces.push_back(
                    Piece{&part, *entry,
                          font::RasterPlaneBounds{tile_left, core_bottom, core_right, tile_top},
                          TextureRegion{entry->u + inset_u, entry->v + inset_v,
                                        entry->uv_width - 2.0 * inset_u,
                                        entry->uv_height - 2.0 * inset_v}});
            }
        }
    }
    for (const Piece& piece : pieces) {
        const Part& part = *piece.part;
        std::optional<MaskGradient> gradient;
        if (const Gradient* paint_gradient = part.paint->gradient(); paint_gradient != nullptr)
            gradient = mask_gradient(*paint_gradient, bounds);
        const auto [quad, uv] = mask_quad(*grid, transform, bounds, piece.core, piece.uv);
        append_draw(output,
                    PreparedCommand{std::make_shared<const PreparedMask>(
                        PreparedMask{quad, uv, part.paint->representative(), piece.entry.texture,
                                     gradient})},
                    source_order, transform, clip, rounded_clips, std::nullopt, opacity, group,
                    context, skipped_draws);
    }
    return true;
}

/**
 * Draws a path's fill and stroke from exact coverage masks in the shape atlas, as text draws its
 * glyphs: the outline is flattened and (for a stroke) expanded at device resolution, and the
 * backend rasterizes it by area (a raster job) into a mask placed on the device pixel grid at the
 * shape's own sub-pixel position, drawn with the paint: its colour, or its gradient evaluated per
 * pixel. A shape larger than a page is drawn in tiles. Returns false, drawing nothing, when the
 * shape draws analytically or as geometry instead: no shape atlas, full pages, or a shape too
 * large even for tiles.
 */
[[nodiscard]] bool append_shape_masks(
    std::vector<PlannedItem>& output,
    const PathRenderCommand& command,
    const std::uint32_t source_order,
    const Transform& transform,
    const Rect clip,
    const std::vector<SubmissionRoundedClip>& rounded_clips,
    const double opacity,
    const std::uint32_t group,
    const SubmissionContext& context,
    std::size_t& skipped_draws,
    font::GlyphAtlas* const atlas,
    std::unordered_map<std::uint64_t, std::uint64_t>& tile_classes,
    const std::span<const GroupRasterScale> group_scales,
    PreparationCache::VolatileShape* const animated
) {
    if (command.art.has_value()) {
        return append_art_masks(output, command, source_order, transform, clip, rounded_clips,
                                opacity, group, context, skipped_draws, atlas, group_scales,
                                animated);
    }
    if (!command.joined.empty()) {
        return append_joined_masks(output, command, source_order, transform, clip, rounded_clips,
                                   opacity, group, context, skipped_draws, atlas, tile_classes,
                                   group_scales);
    }
    const PathShape& shape = command.shape;
    const Rect bounds = command.bounds;
    if (atlas == nullptr || bounds.width <= 0.0 || bounds.height <= 0.0 ||
        analytic_shape(shape, Size{bounds.width, bounds.height})) {
        return false;
    }
    const std::optional<MaskGrid> grid = mask_grid(transform, bounds, context, group, group_scales);
    if (!grid.has_value())
        return false;
    const double device_x = grid->device_x;
    const double device_y = grid->device_y;
    const Point phase = grid->phase;
    const StrokeStyle style = shape.stroke_style.value_or(StrokeStyle{});
    const double tolerance =
        mask_flatness / std::max(std::min(std::abs(device_x), std::abs(device_y)), 1.0e-3);

    std::optional<std::vector<PathContour>> centrelines;
    const auto local_contours = [&]() -> const std::vector<PathContour>& {
        if (!centrelines.has_value()) {
            std::vector<PathContour> flattened = flatten_path(
                shape.path, Size{bounds.width, bounds.height}, tolerance);
            for (PathContour& contour : flattened) {
                for (Point& point : contour.points) {
                    point.x *= bounds.width;
                    point.y *= bounds.height;
                }
            }
            centrelines = std::move(flattened);
        }
        return *centrelines;
    };

    // The path's control points bound it, so they give each part's device box without flattening.
    double control_left = std::numeric_limits<double>::infinity();
    double control_top = std::numeric_limits<double>::infinity();
    double control_right = -std::numeric_limits<double>::infinity();
    double control_bottom = -std::numeric_limits<double>::infinity();
    const auto include = [&](const Point point) {
        control_left = std::min(control_left, point.x);
        control_top = std::min(control_top, point.y);
        control_right = std::max(control_right, point.x);
        control_bottom = std::max(control_bottom, point.y);
    };
    for (const PathSegment& segment : shape.path.segments()) {
        if (segment.verb == PathVerb::close)
            continue;
        include(segment.to);
        if (segment.verb == PathVerb::quadratic || segment.verb == PathVerb::cubic)
            include(segment.control_a);
        if (segment.verb == PathVerb::cubic)
            include(segment.control_b);
    }
    if (!std::isfinite(control_left))
        return true;

    struct Part final {
        bool stroke = false;
        const Paint* paint = nullptr;
    };
    /** One quad to draw: its device box relative to the whole-pixel origin and its texels. */
    struct Piece final {
        const Part* part = nullptr;
        font::RasterPlaneBounds box;
        TextureRegion uv;
        std::string texture;
    };
    std::vector<Part> parts;
    if (shape.fill.has_value() && !shape.fill->transparent())
        parts.push_back(Part{false, &*shape.fill});
    if (shape.stroke.has_value() && !shape.stroke->transparent() && style.width > 0.0)
        parts.push_back(Part{true, &*shape.stroke});
    std::vector<Piece> pieces;
    std::optional<font::GlyphAtlasEntry> solid;
    // The path is hashed once, and only for parts drawn from the cache: an animated shape's
    // parts draw from slots of their own.
    std::optional<MaskHash> path_hash;
    for (std::size_t part_index = 0U; part_index < parts.size(); ++part_index) {
        const Part& part = parts[part_index];
        const auto part_hash = [&] {
            if (!path_hash.has_value()) {
                path_hash.emplace();
                for (const PathSegment& segment : shape.path.segments())
                    path_hash->add(segment);
            }
            MaskHash hash = *path_hash;
            hash.add(part.stroke);
            hash.add(shape.fill_rule);
            if (part.stroke) {
                hash.add(style.width);
                hash.add(style.cap);
                hash.add(style.join);
                hash.add(style.miter_limit);
                hash.add(style.dash_offset);
                for (const double dash : style.dash)
                    hash.add(dash);
            }
            hash.add(bounds.width);
            hash.add(bounds.height);
            hash.add(device_x);
            hash.add(device_y);
            hash.add(phase);
            return hash;
        };

        // The part's outline on the device grid, made once.
        std::optional<DevicePolygons> device_outline;
        const auto outline_polygons = [&]() -> const DevicePolygons& {
            if (!device_outline.has_value()) {
                const std::vector<PathContour>& contours = local_contours();
                std::vector<PathContour> outline;
                if (part.stroke) {
                    outline = stroke_outline(contours, style, tolerance);
                } else {
                    for (const PathContour& contour : contours) {
                        if (contour.points.size() >= 3U)
                            outline.push_back(PathContour{contour.points, true});
                    }
                }
                DevicePolygons polygons;
                polygons.reserve(outline.size());
                for (const PathContour& contour : outline) {
                    std::vector<host::RasterPoint> polygon;
                    polygon.reserve(contour.points.size());
                    for (const Point point : contour.points)
                        polygon.push_back(host::RasterPoint{phase.x + point.x * device_x,
                                                            phase.y + point.y * device_y});
                    polygons.push_back(std::move(polygon));
                }
                device_outline = std::move(polygons);
            }
            return *device_outline;
        };
        const host::RasterFill fill = !part.stroke && shape.fill_rule == PathFillRule::evenodd
                                          ? host::RasterFill::even_odd
                                          : host::RasterFill::nonzero;
        // A stroke the backend expands from its centreline runs, made once.
        const std::optional<host::RasterStroke> pen =
            part.stroke ? device_stroke(style, *grid) : std::nullopt;
        const double pen_reach = pen.has_value() ? host::raster_stroke_reach(*pen) : 0.0;
        std::optional<DeviceRuns> runs;
        const auto stroke_runs_on_device = [&]() -> const DeviceRuns& {
            if (!runs.has_value())
                runs = device_runs(local_contours(), style, *grid);
            return *runs;
        };
        // The part's coverage over a `width` x `height` region whose top-left is `origin`.
        const auto mask = [&](const std::uint32_t width, const std::uint32_t height,
                              const host::RasterPoint origin, const std::uint32_t gutter) {
            host::RasterJobBuilder builder(width, height);
            if (pen.has_value())
                builder.add_stroke_layer(host::RasterLayer{}, *pen, stroke_runs_on_device(), origin);
            else
                builder.add_layer(fill, host::RasterLayer{}, outline_polygons(), origin);
            return builder.encode(gutter, gutter);
        };

        // A conservative device box: the control points, a stroke's reach past them, a pixel.
        double reach = 0.0;
        if (part.stroke) {
            double factor = 1.0;
            if (style.join == PathJoin::miter)
                factor = std::max(factor, style.miter_limit);
            if (style.cap == PathCap::square)
                factor = std::max(factor, std::numbers::sqrt2);
            reach = style.width * 0.5 * factor;
        }
        const double x_a = phase.x + (control_left * bounds.width - reach) * device_x;
        const double x_b = phase.x + (control_right * bounds.width + reach) * device_x;
        const double y_a = phase.y + (control_top * bounds.height - reach) * device_y;
        const double y_b = phase.y + (control_bottom * bounds.height + reach) * device_y;
        const double left = std::min(x_a, x_b) - 1.0;
        const double right = std::max(x_a, x_b) + 1.0;
        const double top = std::min(y_a, y_b) - 1.0;
        const double bottom = std::max(y_a, y_b) + 1.0;

        if (right - left <= maximum_mask_extent && bottom - top <= maximum_mask_extent) {
            const auto request = [&](const std::uint32_t gutter) -> std::optional<font::RasterRequest> {
                    DeviceBox outline;
                    if (pen.has_value())
                        outline = stroke_box(stroke_runs_on_device(), pen_reach);
                    else
                        outline.include(outline_polygons());
                    if (outline.empty() || outline.right - outline.left > maximum_mask_extent ||
                        outline.bottom - outline.top > maximum_mask_extent) {
                        return std::nullopt;
                    }
                    // One clear pixel around the outline, so linear sampling never reaches a
                    // neighbour in the atlas.
                    const double x0 = std::floor(outline.left) - 1.0;
                    const double y0 = std::floor(outline.top) - 1.0;
                    const auto width =
                        static_cast<std::uint32_t>(std::ceil(outline.right) + 1.0 - x0);
                    const auto height =
                        static_cast<std::uint32_t>(std::ceil(outline.bottom) + 1.0 - y0);
                    // The mask's device-pixel box relative to the whole-pixel origin.
                    return font::RasterRequest{
                        mask(width, height, host::RasterPoint{x0, y0}, gutter), width, height,
                        font::RasterPlaneBounds{x0, y0 + height, x0 + width, y0}};
            };
            std::optional<font::GlyphAtlasEntry> entry;
            if (animated != nullptr) {
                // Rasterized into the part's own slot: the same texture and place each frame.
                std::optional<font::RasterRequest> next = request(atlas->gutter());
                if (!next.has_value())
                    return false;
                if (animated->slots.size() <= part_index)
                    animated->slots.resize(part_index + 1U);
                std::optional<font::RasterSlot>& slot = animated->slots[part_index];
                if (!slot.has_value() ||
                    !atlas->raster_slot_fits(*slot, next->width, next->height)) {
                    // Room to grow, so a shape that swells a little keeps its slot.
                    const auto room = [](const std::uint32_t size) { return size + size / 4U + 8U; };
                    slot = atlas->reserve_raster_slot(room(next->width), room(next->height),
                                                      font::GlyphRasterMode::coverage);
                    if (!slot.has_value()) {
                        slot = atlas->reserve_raster_slot(next->width, next->height,
                                                          font::GlyphRasterMode::coverage);
                    }
                    if (!slot.has_value() ||
                        !atlas->raster_slot_fits(*slot, next->width, next->height)) {
                        return false;
                    }
                }
                entry = atlas->write_raster_slot(*slot, std::move(*next));
            } else {
                char key[40];
                std::snprintf(key, sizeof(key), "%016llx",
                              static_cast<unsigned long long>(part_hash().value()));
                entry = atlas->request_raster(key, font::GlyphRasterMode::coverage, request);
            }
            if (!entry.has_value())
                return false;
            pieces.push_back(Piece{&part, entry->plane_bounds_layout_pixels,
                                   TextureRegion{entry->u, entry->v, entry->uv_width,
                                                 entry->uv_height},
                                   entry->texture});
            continue;
        }

        // Tiles over the device box. A tile no edge crosses is empty or full throughout.
        const double tile = static_cast<double>(mask_tile);
        const auto first_x = static_cast<std::int64_t>(std::floor(left / tile));
        const auto last_x = static_cast<std::int64_t>(std::floor(right / tile));
        const auto first_y = static_cast<std::int64_t>(std::floor(top / tile));
        const auto last_y = static_cast<std::int64_t>(std::floor(bottom / tile));
        if (right - left > maximum_tiled_extent || bottom - top > maximum_tiled_extent ||
            static_cast<double>(last_x - first_x + 1) * static_cast<double>(last_y - first_y + 1) >
                maximum_mask_tiles) {
            return false;
        }
        if (tile_classes.size() > maximum_tile_classes)
            tile_classes.clear();
        constexpr std::uint32_t side = mask_tile + 2U;
        const MaskHash hash = part_hash();
        for (std::int64_t tile_y = first_y; tile_y <= last_y; ++tile_y) {
            for (std::int64_t tile_x = first_x; tile_x <= last_x; ++tile_x) {
                MaskHash tile_hash = hash;
                tile_hash.add(tile_x);
                tile_hash.add(tile_y);
                const std::uint64_t identity = tile_hash.value();
                const font::RasterPlaneBounds core{
                    static_cast<double>(tile_x) * tile, static_cast<double>(tile_y + 1) * tile,
                    static_cast<double>(tile_x + 1) * tile, static_cast<double>(tile_y) * tile};
                std::uint8_t tile_class = 2U;
                if (const auto known = tile_classes.find(identity); known != tile_classes.end())
                    tile_class = static_cast<std::uint8_t>(known->second);
                if (tile_class == 0U)
                    continue;
                if (tile_class == 2U) {
                    char key[48];
                    std::snprintf(key, sizeof(key), "%016llx",
                                  static_cast<unsigned long long>(identity));
                    std::optional<std::uint8_t> found_class;
                    const std::optional<font::GlyphAtlasEntry> entry = atlas->request_raster(
                        key, font::GlyphRasterMode::coverage,
                        [&](const std::uint32_t gutter) -> std::optional<font::RasterRequest> {
                            const host::RasterPoint origin{core.left - 1.0, core.top - 1.0};
                            const DeviceBox region{origin.x, origin.y, origin.x + side,
                                                   origin.y + side};
                            if (pen.has_value()) {
                                // A stroke reaching none of the tile is none of it.
                                if (!stroke_meets(stroke_runs_on_device(), pen_reach, region)) {
                                    found_class = std::uint8_t{0U};
                                    return std::nullopt;
                                }
                            } else if (!edges_cross(outline_polygons(), region)) {
                                found_class = covers(outline_polygons(), fill,
                                                     origin.x + side * 0.5, origin.y + side * 0.5)
                                                  ? std::uint8_t{1U}
                                                  : std::uint8_t{0U};
                                return std::nullopt;
                            }
                            return font::RasterRequest{
                                mask(side, side, origin, gutter), side, side,
                                font::RasterPlaneBounds{core.left - 1.0, core.bottom + 1.0,
                                                        core.right + 1.0, core.top - 1.0}};
                        });
                    if (found_class.has_value()) {
                        tile_classes[identity] = *found_class;
                        tile_class = *found_class;
                        if (tile_class == 0U)
                            continue;
                    } else if (!entry.has_value()) {
                        return false;
                    } else {
                        // The core's texels: the apron stays outside the quad.
                        const double inset_u = entry->uv_width / static_cast<double>(side);
                        const double inset_v = entry->uv_height / static_cast<double>(side);
                        pieces.push_back(Piece{
                            &part, core,
                            TextureRegion{entry->u + inset_u, entry->v + inset_v,
                                          entry->uv_width - 2.0 * inset_u,
                                          entry->uv_height - 2.0 * inset_v},
                            entry->texture});
                        continue;
                    }
                }
                // A full tile samples one solid texel.
                if (!solid.has_value()) {
                    solid = solid_texel(*atlas, std::nullopt);
                    if (!solid.has_value())
                        return false;
                }
                pieces.push_back(Piece{
                    &part, core,
                    TextureRegion{solid->u + solid->uv_width * 0.5, solid->v + solid->uv_height * 0.5,
                                  0.0, 0.0},
                    solid->texture});
            }
        }
    }
    std::optional<MaskGradient> gradients[2];
    for (const Piece& piece : pieces) {
        const Part& part = *piece.part;
        std::optional<MaskGradient>& gradient = gradients[part.stroke ? 1 : 0];
        if (const Gradient* paint_gradient = part.paint->gradient();
            paint_gradient != nullptr && !gradient.has_value()) {
            gradient = mask_gradient(*paint_gradient, bounds);
        }
        const auto [quad, uv] = mask_quad(*grid, transform, bounds, piece.box, piece.uv);
        append_draw(output,
                    PreparedCommand{std::make_shared<const PreparedMask>(
                        PreparedMask{quad, uv, part.paint->representative(), piece.texture,
                                     gradient})},
                    source_order, transform, clip, rounded_clips, std::nullopt, opacity, group,
                    context, skipped_draws);
    }
    return true;
}

/**
 * The planner's state machine over a command stream: clip, transform, material, opacity, group
 * and content effect scopes, and the items each command makes. It starts from a recorded state
 * to plan a balanced range of commands again.
 */
class Planner final {
  public:
    Planner(const SubmissionContext& context, const PreparationCache& cache,
            const std::vector<bool>& visible_text, std::vector<PlannedItem>& output,
            std::size_t& skipped_draws, const PlannerState& initial)
        : context_(context), cache_(cache), visible_text_(visible_text), output_(output),
          skipped_draws_(skipped_draws), clip_(initial.clip), transform_(initial.transform),
          opacity_(initial.opacity), group_(initial.group), initially_in_group_(initial.in_group),
          content_effect_depth_(initial.content_effect_depth),
          initial_content_effect_depth_(initial.content_effect_depth),
          rounded_clips_snapshot_(initial.rounded_clips),
          material_snapshot_(initial.material_override) {
        if (initial.rounded_clips != nullptr) active_rounded_clips_ = *initial.rounded_clips;
        if (initial.material_override != nullptr) material_override_ = *initial.material_override;
        if (initial.content_effect_depth != 0U)
            content_clip_baselines_.push_back(initial.rounded_clip_baseline);
    }

    [[nodiscard]] PlannerState state() {
        if (rounded_clips_snapshot_ == nullptr && !active_rounded_clips_.empty()) {
            rounded_clips_snapshot_ =
                std::make_shared<const std::vector<SubmissionRoundedClip>>(active_rounded_clips_);
        }
        if (material_snapshot_ == nullptr && material_override_.has_value())
            material_snapshot_ = std::make_shared<const MaterialState>(*material_override_);
        return PlannerState{
            transform_,
            clip_,
            rounded_clips_snapshot_,
            content_clip_baselines_.empty() ? 0U : content_clip_baselines_.back(),
            content_effect_depth_,
            material_snapshot_,
            opacity_,
            group_,
            initially_in_group_ || !group_stack_.empty(),
        };
    }

    /** Whether every scope opened since the planner started has closed. */
    [[nodiscard]] bool closed() const noexcept {
        return clip_stack_.empty() && rounded_clip_stack_.empty() && transform_stack_.empty() &&
               material_stack_.empty() && opacity_stack_.empty() && group_stack_.empty() &&
               content_effect_depth_ == initial_content_effect_depth_ &&
               content_clip_baselines_.size() == (initial_content_effect_depth_ != 0U ? 1U : 0U);
    }

    void step(const std::size_t index, const RenderCommand& source) {
        if (index > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("render source order exceeds the submission packet range");
        }
        const std::uint32_t source_order = static_cast<std::uint32_t>(index);
        std::visit([&](const auto& value) {
            using Type = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Type, ClipPushRenderCommand>) {
                clip_stack_.push_back(clip_);
                const Rect presented = transform_.bounds(value.rect);
                clip_ = intersect(clip_, presented);
                const CornerRadii safe_radii{
                    std::max(0.0, value.radii.top_left),
                    std::max(0.0, value.radii.top_right),
                    std::max(0.0, value.radii.bottom_right),
                    std::max(0.0, value.radii.bottom_left),
                };
                // A rotated rectangle is not its scissor box: it clips through the same
                // per-pixel mask a rounded corner does, with square corners.
                const bool has_rounded_geometry =
                    (rounded(safe_radii) || !transform_.axis_aligned() || !value.shape.plain()) &&
                    !clip_.empty();
                rounded_clip_stack_.push_back(has_rounded_geometry);
                if (has_rounded_geometry) {
                    if (active_rounded_clips_.size() == maximum_rounded_clip_depth) {
                        throw std::length_error(
                            "render rounded clip nesting exceeds the packet limit"
                        );
                    }
                    const std::optional<Transform> inverse = transform_.inverse();
                    if (!inverse.has_value()) {
                        clip_ = Rect{};
                        rounded_clip_stack_.back() = false;
                    } else {
                        active_rounded_clips_.push_back(SubmissionRoundedClip{
                            value.rect,
                            safe_radii,
                            {
                                inverse->m00,
                                inverse->m01,
                                inverse->m02,
                                inverse->m10,
                                inverse->m11,
                                inverse->m12,
                            },
                            value.shape,
                        });
                        rounded_clips_snapshot_.reset();
                    }
                }
            } else if constexpr (std::is_same_v<Type, ClipPopRenderCommand>) {
                if (clip_stack_.empty() || rounded_clip_stack_.empty()) {
                    throw std::logic_error("render clip stack underflow");
                }
                if (rounded_clip_stack_.back()) {
                    if (active_rounded_clips_.empty()) {
                        throw std::logic_error("render rounded clip stack is invalid");
                    }
                    if (!content_clip_baselines_.empty() &&
                        active_rounded_clips_.size() <= content_clip_baselines_.back()) {
                        throw std::logic_error(
                            "render rounded clip crossed a content effect boundary"
                        );
                    }
                    active_rounded_clips_.pop_back();
                    rounded_clips_snapshot_.reset();
                }
                rounded_clip_stack_.pop_back();
                clip_ = clip_stack_.back();
                clip_stack_.pop_back();
            } else if constexpr (std::is_same_v<Type, TransformPushRenderCommand>) {
                transform_stack_.push_back(transform_);
                transform_ = transform_.concatenate(Transform{
                    value.m00, value.m01, value.m02, value.m10, value.m11, value.m12,
                });
            } else if constexpr (std::is_same_v<Type, TransformPopRenderCommand>) {
                if (transform_stack_.empty())
                    throw std::logic_error("render transform stack underflow");
                transform_ = transform_stack_.back();
                transform_stack_.pop_back();
            } else if constexpr (std::is_same_v<Type, MaterialPushRenderCommand>) {
                material_stack_.push_back(material_override_);
                material_override_ = value.material;
                material_snapshot_.reset();
            } else if constexpr (std::is_same_v<Type, MaterialPopRenderCommand>) {
                if (material_stack_.empty())
                    throw std::logic_error("render material stack underflow");
                material_override_ = material_stack_.back();
                material_stack_.pop_back();
                material_snapshot_.reset();
            } else if constexpr (std::is_same_v<Type, OpacityPushRenderCommand>) {
                opacity_stack_.push_back(opacity_);
                opacity_ *= std::clamp(value.opacity, 0.0, 1.0);
            } else if constexpr (std::is_same_v<Type, OpacityPopRenderCommand>) {
                if (opacity_stack_.empty())
                    throw std::logic_error("render opacity stack underflow");
                opacity_ = opacity_stack_.back();
                opacity_stack_.pop_back();
            } else if constexpr (std::is_same_v<Type, GroupPushRenderCommand>) {
                group_stack_.push_back(GroupScope{transform_, opacity_, group_});
                transform_ = Transform{};
                opacity_ = 1.0;
                group_ = value.group;
            } else if constexpr (std::is_same_v<Type, GroupPopRenderCommand>) {
                if (group_stack_.empty()) throw std::logic_error("render group stack underflow");
                transform_ = group_stack_.back().transform;
                opacity_ = group_stack_.back().opacity;
                group_ = group_stack_.back().group;
                group_stack_.pop_back();
            } else if constexpr (std::is_same_v<Type, BlurRegionRenderCommand>) {
                const Rect visible = intersect(clip_, transform_.bounds(value.bounds));
                if (group_ == 0U && visible.empty()) {
                    ++skipped_draws_;
                    return;
                }
                output_.emplace_back(SubmissionBatch{
                    SubmissionBatchKind::blur,
                    "strata:blur_region",
                    SubmissionBlend::straight_alpha,
                    std::nullopt,
                    scissor(clip_, context_),
                    0U, 0U, 0U, source_order,
                    transform_.bounds(value.bounds),
                    value.radius,
                    static_cast<std::uint32_t>(std::min<std::size_t>(
                        value.downsample, std::numeric_limits<std::uint32_t>::max()
                    )),
                    {},
                    std::nullopt,
                    batch_rounded_clips(),
                });
                std::get<SubmissionBatch>(output_.back().value).group = group_;
            } else if constexpr (std::is_same_v<Type, BackdropEffectRenderCommand>) {
                const Rect visible = intersect(clip_, transform_.bounds(value.bounds));
                if (group_ == 0U && visible.empty()) {
                    ++skipped_draws_;
                    return;
                }
                const double scale = std::sqrt(std::abs(
                    transform_.m00 * transform_.m11 - transform_.m01 * transform_.m10
                ));
                output_.emplace_back(SubmissionBatch{
                    .kind = SubmissionBatchKind::backdrop_effect,
                    .material = value.effect.id,
                    .scissor = scissor(clip_, context_),
                    .source_order = source_order,
                    .effect_bounds = transform_.bounds(value.bounds),
                    .effect_radii = CornerRadii{
                        value.radii.top_left * scale,
                        value.radii.top_right * scale,
                        value.radii.bottom_right * scale,
                        value.radii.bottom_left * scale,
                    },
                    .effect = scoped_effect(value.effect),
                    .rounded_clips = batch_rounded_clips(),
                    .group = group_,
                });
            } else if constexpr (std::is_same_v<Type, ContentEffectPushRenderCommand>) {
                if (content_effect_depth_ == maximum_content_effect_depth) {
                    throw std::length_error(
                        "render content effect nesting exceeds the packet limit"
                    );
                }
                ++content_effect_depth_;
                const std::vector<SubmissionRoundedClip> composite_clips =
                    batch_rounded_clips();
                const double scale = std::sqrt(std::abs(
                    transform_.m00 * transform_.m11 - transform_.m01 * transform_.m10
                ));
                output_.emplace_back(SubmissionBatch{
                    .kind = SubmissionBatchKind::content_effect_begin,
                    .material = value.effect.id,
                    .scissor = scissor(clip_, context_),
                    .source_order = source_order,
                    .effect_bounds = transform_.bounds(value.bounds),
                    .effect_radii = CornerRadii{
                        value.radii.top_left * scale,
                        value.radii.top_right * scale,
                        value.radii.bottom_right * scale,
                        value.radii.bottom_left * scale,
                    },
                    .effect = scoped_effect(value.effect),
                    .rounded_clips = composite_clips,
                    .group = group_,
                });
                content_clip_baselines_.push_back(active_rounded_clips_.size());
            } else if constexpr (std::is_same_v<Type, ContentEffectPopRenderCommand>) {
                if (content_effect_depth_ == 0U || content_clip_baselines_.empty()) {
                    throw std::logic_error("render content effect stack underflow");
                }
                --content_effect_depth_;
                content_clip_baselines_.pop_back();
                output_.emplace_back(SubmissionBatch{
                    .kind = SubmissionBatchKind::content_effect_end,
                    .scissor = scissor(clip_, context_),
                    .source_order = source_order,
                    .rounded_clips = batch_rounded_clips(),
                    .group = group_,
                });
            } else if constexpr (std::is_same_v<Type, TextRunRenderCommand>) {
                if (!visible_text_[index]) {
                    ++skipped_draws_;
                    return;
                }
                const PreparedTextCacheEntry& retained = *cache_.text[index];
                const std::vector<PreparedTextPtr>& runs = retained.groups;
                if (runs.empty()) {
                    ++skipped_draws_;
                    return;
                }
                const Transform positioned = transform_.concatenate(Transform{
                    1.0,
                    0.0,
                    value.origin.x - retained.source.origin.x,
                    0.0,
                    1.0,
                    value.origin.y - retained.source.origin.y,
                });
                for (const PreparedTextPtr& run : runs) {
                    append_draw(
                        output_, PreparedCommand{run}, source_order, positioned,
                        clip_, batch_rounded_clips(), material_override_, opacity_, group_,
                        context_, skipped_draws_
                    );
                }
            } else {
                if constexpr (std::is_same_v<Type, PathRenderCommand>) {
                    if (append_shape_masks(output_, value, source_order, transform_, clip_,
                                           batch_rounded_clips(), opacity_, group_, context_,
                                           skipped_draws_, cache_.shape_atlas,
                                           cache_.shape_tile_classes,
                                           cache_.group_raster_scales, animated(source_order))) {
                        return;
                    }
                }
                if constexpr (std::is_same_v<Type, RoundedRectRenderCommand>) {
                    if (value.border.has_value() && value.border->paint.is_gradient()) {
                        // The rounded-rect shading carries one solid border colour, so a gradient
                        // border draws as a border of its own over the fill. The fill keeps a
                        // border of its own colour at no opacity, which clears the ring exactly
                        // where the border will cover it: under a translucent gradient the fill
                        // does not show through, just as under a translucent solid border.
                        RenderColor clear = value.fill.representative();
                        clear.alpha = 0U;
                        RoundedRectRenderCommand fill = value;
                        fill.border =
                            RenderBorder{value.border->width, clear, value.border->inside};
                        append_draw(output_, PreparedCommand{std::move(fill)}, source_order,
                                    transform_, clip_, batch_rounded_clips(), material_override_,
                                    opacity_, group_, context_, skipped_draws_);
                        append_draw(output_,
                                    PreparedCommand{BorderRenderCommand{value.bounds, *value.border,
                                                                        value.radii}},
                                    source_order, transform_, clip_, batch_rounded_clips(),
                                    material_override_, opacity_, group_, context_, skipped_draws_);
                        return;
                    }
                }
                append_draw(
                    output_, PreparedCommand{value}, source_order, transform_, clip_,
                    batch_rounded_clips(), material_override_, opacity_, group_, context_,
                    skipped_draws_
                );
            }
        }, source);
    }

  private:
    /** The draw's animated-shape record, when it changed in this update and the one before. */
    [[nodiscard]] PreparationCache::VolatileShape* animated(const std::uint32_t source_order) const {
        const auto found = cache_.volatile_shapes.find(source_order);
        if (found == cache_.volatile_shapes.end() ||
            found->second.changed != cache_.update_serial ||
            found->second.previously_changed + 1U != cache_.update_serial) {
            return nullptr;
        }
        return &found->second;
    }
    // A group restarts transform and opacity: its content is encoded relative to the group.
    struct GroupScope {
        Transform transform;
        double opacity = 1.0;
        std::uint32_t group = 0U;
    };

    [[nodiscard]] EffectState scoped_effect(EffectState effect) const {
        effect.opacity *= opacity_;
        if (effect.composite != LayerComposite::source_over)
            effect.refresh_rate = 0.0;
        // A layer's filter lengths are in the space it is drawn in: carried into presented
        // logical pixels as its bounds are.
        const double along_x = std::hypot(transform_.m00, transform_.m10);
        const double along_y = std::hypot(transform_.m01, transform_.m11);
        const double area =
            std::sqrt(std::abs(transform_.m00 * transform_.m11 - transform_.m01 * transform_.m10));
        for (LayerFilter& filter : effect.filters) {
            std::vector<double>& values = filter.values;
            if (filter.kind == LayerFilterKind::blur && values.size() >= 2U) {
                values[0] *= along_x;
                values[1] *= along_y;
            } else if (filter.kind == LayerFilterKind::drop_shadow && values.size() >= 7U) {
                const double x = values[4];
                const double y = values[5];
                values[4] = transform_.m00 * x + transform_.m01 * y;
                values[5] = transform_.m10 * x + transform_.m11 * y;
                values[6] *= area;
            }
        }
        return effect;
    }
    [[nodiscard]] std::vector<SubmissionRoundedClip> batch_rounded_clips() const {
        const std::size_t begin =
            content_clip_baselines_.empty() ? 0U : content_clip_baselines_.back();
        if (begin > active_rounded_clips_.size()) {
            throw std::logic_error("render rounded clip crossed a content effect boundary");
        }
        return std::vector<SubmissionRoundedClip>(
            active_rounded_clips_.begin() + static_cast<std::ptrdiff_t>(begin),
            active_rounded_clips_.end()
        );
    }

    const SubmissionContext& context_;
    const PreparationCache& cache_;
    const std::vector<bool>& visible_text_;
    std::vector<PlannedItem>& output_;
    std::size_t& skipped_draws_;
    std::vector<Rect> clip_stack_;
    std::vector<bool> rounded_clip_stack_;
    std::vector<SubmissionRoundedClip> active_rounded_clips_;
    std::vector<Transform> transform_stack_;
    std::vector<std::optional<MaterialState>> material_stack_;
    std::vector<double> opacity_stack_;
    std::vector<GroupScope> group_stack_;
    std::vector<std::size_t> content_clip_baselines_;
    Rect clip_;
    Transform transform_;
    std::optional<MaterialState> material_override_;
    double opacity_ = 1.0;
    std::uint32_t group_ = 0U;
    bool initially_in_group_ = false;
    std::size_t content_effect_depth_ = 0U;
    std::size_t initial_content_effect_depth_ = 0U;
    // Snapshots of the current clip list and material, shared by the states that see them.
    std::shared_ptr<const std::vector<SubmissionRoundedClip>> rounded_clips_snapshot_;
    std::shared_ptr<const MaterialState> material_snapshot_;
};

} // namespace

Point Transform::apply(const Point point) const noexcept {
    return Point{m00 * point.x + m01 * point.y + m02, m10 * point.x + m11 * point.y + m12};
}

Rect Transform::bounds(const Rect rect) const noexcept {
    const Point top_left = apply(Point{rect.x, rect.y});
    const Point top_right = apply(Point{rect.right(), rect.y});
    const Point bottom_right = apply(Point{rect.right(), rect.bottom()});
    const Point bottom_left = apply(Point{rect.x, rect.bottom()});
    const double left = std::min({top_left.x, top_right.x, bottom_right.x, bottom_left.x});
    const double top = std::min({top_left.y, top_right.y, bottom_right.y, bottom_left.y});
    const double right = std::max({top_left.x, top_right.x, bottom_right.x, bottom_left.x});
    const double bottom = std::max({top_left.y, top_right.y, bottom_right.y, bottom_left.y});
    return Rect{left, top, std::max(0.0, right - left), std::max(0.0, bottom - top)};
}

Transform Transform::concatenate(const Transform& next) const noexcept {
    return Transform{
        m00 * next.m00 + m01 * next.m10,
        m00 * next.m01 + m01 * next.m11,
        m00 * next.m02 + m01 * next.m12 + m02,
        m10 * next.m00 + m11 * next.m10,
        m10 * next.m01 + m11 * next.m11,
        m10 * next.m02 + m11 * next.m12 + m12,
    };
}

std::optional<Transform> Transform::inverse() const noexcept {
    const double determinant = m00 * m11 - m01 * m10;
    if (!std::isfinite(determinant) ||
        std::abs(determinant) <= std::numeric_limits<double>::epsilon()) {
        return std::nullopt;
    }
    const double inverse_determinant = 1.0 / determinant;
    Transform result{
        m11 * inverse_determinant,
        -m01 * inverse_determinant,
        0.0,
        -m10 * inverse_determinant,
        m00 * inverse_determinant,
        0.0,
    };
    result.m02 = -(result.m00 * m02 + result.m01 * m12);
    result.m12 = -(result.m10 * m02 + result.m11 * m12);
    return result;
}

bool Transform::axis_aligned() const noexcept {
    return std::abs(m01) <= transform_epsilon && std::abs(m10) <= transform_epsilon;
}

bool Transform::axis_aligned_translation() const noexcept {
    return std::abs(m00 - 1.0) <= transform_epsilon &&
           std::abs(m11 - 1.0) <= transform_epsilon &&
           std::abs(m01) <= transform_epsilon && std::abs(m10) <= transform_epsilon;
}

std::string_view default_material_id(const PreparedCommand& command) {
    return std::visit([](const auto& value) -> std::string_view {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, SolidRectRenderCommand>) {
            // A gradient is tessellated geometry, and the rounded-rect material is what masks that
            // geometry to the shape. A plain rectangle keeps the cheaper solid material.
            return value.fill.is_gradient() ? "strata:rounded_rect" : "strata:solid";
        } else if constexpr (std::is_same_v<Type, RoundedRectRenderCommand>) {
            return "strata:rounded_rect";
        } else if constexpr (std::is_same_v<Type, BorderRenderCommand>) {
            return "strata:border";
        } else if constexpr (std::is_same_v<Type, ImageRenderCommand>) {
            return "strata:textured";
        } else if constexpr (std::is_same_v<Type, NinePatchRenderCommand>) {
            return "strata:nine_patch";
        } else if constexpr (std::is_same_v<Type, PreparedTextPtr>) {
            // A colour glyph's texels are its colours: it draws as an image.
            if (value != nullptr && value->mode == font::GlyphRasterMode::color)
                return "strata:textured";
            if (value != nullptr && value->mode == font::GlyphRasterMode::subpixel)
                return "strata:subpixel_text";
            return value != nullptr && value->mode == font::GlyphRasterMode::coverage
                ? "strata:coverage_text" : "strata:msdf_text";
        } else if constexpr (std::is_same_v<Type, PathRenderCommand>) {
            // Shape geometry is its own silhouette, so it needs no signed-distance mask.
            return "strata:solid";
        } else if constexpr (std::is_same_v<Type, CustomMeshRenderCommand>) {
            return value.material.has_value() ? std::string_view(value.material->id)
                                              : std::string_view("strata:custom_mesh");
        } else if constexpr (std::is_same_v<Type, PreparedMaskPtr>) {
            return value != nullptr && value->colored ? "strata:color_mask"
                                                      : "strata:coverage_mask";
        } else {
            static_assert(std::is_same_v<Type, ShadowRenderCommand>);
            return "strata:shadow";
        }
    }, command);
}

MaterialState default_material(const PreparedCommand& command) {
    if (const auto* mesh = std::get_if<CustomMeshRenderCommand>(&command)) {
        MaterialState material = mesh->material.value_or(MaterialState{"strata:custom_mesh"});
        material.opacity *= mesh->opacity;
        return material;
    }
    MaterialState material{std::string(default_material_id(command))};
    // Subpixel text blends each colour channel by its own coverage.
    if (material.id == "strata:subpixel_text")
        material.blend_mode = "subpixel_text";
    return material;
}

bool unified_material(const std::string_view material) noexcept {
    return material == "strata:solid" || material == "strata:textured" ||
           material == "strata:rounded_rect" || material == "strata:border" ||
           material == "strata:coverage_text" || material == "strata:msdf_text" ||
           material == "strata:subpixel_text" ||
           material == "strata:custom_mesh" || material == "strata:shadow" ||
           material == "strata:coverage_mask" || material == "strata:color_mask";
}

float unified_draw_mode(const std::string_view material) noexcept {
    if (material == "strata:textured" || material == "strata:custom_mesh") return 1.0F;
    if (material == "strata:rounded_rect") return 2.0F;
    if (material == "strata:border") return 3.0F;
    if (material == "strata:coverage_text") return 4.0F;
    if (material == "strata:msdf_text") return 5.0F;
    if (material == "strata:shadow") return 6.0F;
    if (material == "strata:coverage_mask") return 10.0F;
    if (material == "strata:color_mask") return 11.0F;
    if (material == "strata:subpixel_text") return 12.0F;
    return 0.0F;
}

bool samples_texture(const std::string_view material) noexcept {
    return material == "strata:textured" || material == "strata:nine_patch" ||
           material == "strata:coverage_text" || material == "strata:msdf_text" ||
           material == "strata:subpixel_text" ||
           material == "strata:custom_mesh" || material == "strata:coverage_mask" ||
           material == "strata:color_mask";
}

std::vector<PlannedItem> plan(
    const RenderCommandBuffer& commands,
    font::GlyphAtlas& glyph_atlas,
    const TextEngine* const text_engine,
    const SubmissionContext& context,
    RenderSubmission& telemetry,
    PreparationCache& cache
) {
    std::size_t& skipped_draws = telemetry.skipped_draws;
    bool text_required = false;
    for (const RenderCommand& source : commands.commands()) {
        if (std::holds_alternative<TextRunRenderCommand>(source)) {
            text_required = true;
            break;
        }
    }
    if (text_required && text_engine == nullptr) {
        throw std::invalid_argument(
            "render submission contains text commands but has no TextEngine"
        );
    }
    // Text-backed surfaces retain their atlas scale/release lifecycle even on a frame whose
    // current command buffer contains no text. Truly no-font submissions do not touch the atlas.
    if (text_engine != nullptr) glyph_atlas.adopt_display_scale(context.scale);
    align_text_cache(commands, cache);
    const std::vector<bool> visible_text = visible_text_commands(commands, context);
    const std::vector<double> text_scales = text_group_scales(commands, cache.group_raster_scales);
    const std::vector<bool> subpixel_runs =
        subpixel_text_runs(commands, text_scales, cache.turned_groups, context.text_subpixel);
    const std::vector<bool> zoomed_runs = zoomed_text_runs(commands, cache.group_raster_scales);
    const auto collect_warmup_requests =
        [&](const std::uint64_t atlas_generation, const bool all) {
        std::vector<font::GlyphAtlasWarmupRequest> requests;
        for (std::size_t index = 0U; index < commands.commands().size(); ++index) {
            if (!visible_text[index]) continue;
            const auto* text = std::get_if<TextRunRenderCommand>(
                &commands.commands()[index]
            );
            if (text == nullptr) continue;
            const auto& retained = cache.text[index];
            if (!all && retained.has_value() && same_text_content(retained->source, *text) &&
                retained->text_engine == text_engine &&
                retained->atlas_generation == atlas_generation &&
                retained->display_scale == context.scale &&
                retained->raster_scale == text_scales[index] &&
                retained->subpixel == subpixel_runs[index] &&
                retained->unhinted == zoomed_runs[index]) {
                continue;
            }
            append_warmup_requests(*text, *text_engine, context, text_scales[index],
                                   subpixel_runs[index], zoomed_runs[index], requests);
        }
        return requests;
    };
    // Populate only changed text runs. Capacity reclamation is generation-wide, so a recycle
    // during warmup invalidates older prepared UVs and repeats until every retained run belongs
    // to one stable generation.
    std::uint64_t warm_generation = glyph_atlas.generation();
    if (text_required) {
        bool warm_all = false;
        do {
            warm_generation = glyph_atlas.generation();
            const std::vector<font::GlyphAtlasWarmupRequest> warmup_requests =
                collect_warmup_requests(warm_generation, warm_all);
            const auto warmup_started = std::chrono::steady_clock::now();
            const bool warmed = glyph_atlas.warm(warmup_requests);
            telemetry.atlas_warmup_nanos +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - warmup_started
                ).count();
            if (!warmed) {
                warm_all = true;
                continue;
            }
            const auto preparation_started = std::chrono::steady_clock::now();
            for (std::size_t index = 0U; index < commands.commands().size(); ++index) {
                const auto* text = std::get_if<TextRunRenderCommand>(
                    &commands.commands()[index]
                );
                if (text == nullptr || !visible_text[index]) {
                    cache.text[index].reset();
                    continue;
                }
                const auto& retained = cache.text[index];
                if (retained.has_value() && same_text_content(retained->source, *text) &&
                    retained->text_engine == text_engine &&
                    retained->atlas_generation == warm_generation &&
                    retained->display_scale == context.scale &&
                    retained->raster_scale == text_scales[index] &&
                retained->subpixel == subpixel_runs[index] &&
                retained->unhinted == zoomed_runs[index]) {
                    continue;
                }
                std::vector<PreparedText> prepared =
                    prepare_text(*text, glyph_atlas, *text_engine, context.scale,
                                 text_scales[index], subpixel_runs[index], zoomed_runs[index]);
                std::vector<PreparedTextPtr> groups;
                groups.reserve(prepared.size());
                for (PreparedText& group : prepared) {
                    groups.push_back(std::make_shared<const PreparedText>(std::move(group)));
                }
                cache.text[index] = PreparedTextCacheEntry{
                    *text,         text_engine,          glyph_atlas.generation(),
                    context.scale, text_scales[index],   subpixel_runs[index],
                    zoomed_runs[index],  std::move(groups),
                };
            }
            telemetry.text_preparation_nanos +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - preparation_started
                ).count();
            warm_all = glyph_atlas.generation() != warm_generation;
        } while (warm_all);
    }
    std::vector<PlannedItem> output;
    output.reserve(commands.size());
    cache.command_plans.clear();
    cache.command_plans.resize(commands.size());
    Planner planner(context, cache, visible_text, output, skipped_draws, PlannerState{
        Transform{},
        Rect{0.0, 0.0, context.logical_width, context.logical_height},
    });
    for (std::size_t index = 0U; index < commands.commands().size(); ++index) {
        CommandPlan& record = cache.command_plans[index];
        record.state = planner.state();
        record.first_item = output.size();
        const std::size_t skipped_before = skipped_draws;
        planner.step(index, commands.commands()[index]);
        record.item_count = output.size() - record.first_item;
        record.skipped_draws = skipped_draws - skipped_before;
    }
    if (!planner.closed()) throw std::logic_error("render command state stacks are unbalanced");
    return output;
}

bool draw_command(const RenderCommand& command) noexcept {
    return std::visit([](const auto& value) {
        using Type = std::decay_t<decltype(value)>;
        return !(std::is_same_v<Type, ClipPushRenderCommand> ||
                 std::is_same_v<Type, ClipPopRenderCommand> ||
                 std::is_same_v<Type, TransformPushRenderCommand> ||
                 std::is_same_v<Type, TransformPopRenderCommand> ||
                 std::is_same_v<Type, MaterialPushRenderCommand> ||
                 std::is_same_v<Type, MaterialPopRenderCommand> ||
                 std::is_same_v<Type, OpacityPushRenderCommand> ||
                 std::is_same_v<Type, OpacityPopRenderCommand> ||
                 std::is_same_v<Type, GroupPushRenderCommand> ||
                 std::is_same_v<Type, GroupPopRenderCommand> ||
                 std::is_same_v<Type, BlurRegionRenderCommand> ||
                 std::is_same_v<Type, BackdropEffectRenderCommand> ||
                 std::is_same_v<Type, ContentEffectPushRenderCommand> ||
                 std::is_same_v<Type, ContentEffectPopRenderCommand>);
    }, command);
}

int scope_step(const RenderCommand& command) noexcept {
    return std::visit([](const auto& value) {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, ClipPushRenderCommand> ||
                      std::is_same_v<Type, TransformPushRenderCommand> ||
                      std::is_same_v<Type, MaterialPushRenderCommand> ||
                      std::is_same_v<Type, OpacityPushRenderCommand> ||
                      std::is_same_v<Type, GroupPushRenderCommand> ||
                      std::is_same_v<Type, ContentEffectPushRenderCommand>) {
            return 1;
        } else if constexpr (std::is_same_v<Type, ClipPopRenderCommand> ||
                             std::is_same_v<Type, TransformPopRenderCommand> ||
                             std::is_same_v<Type, MaterialPopRenderCommand> ||
                             std::is_same_v<Type, OpacityPopRenderCommand> ||
                             std::is_same_v<Type, GroupPopRenderCommand> ||
                             std::is_same_v<Type, ContentEffectPopRenderCommand>) {
            return -1;
        } else {
            return 0;
        }
    }, command);
}

std::optional<std::vector<std::size_t>> replan_ranges(
    const RenderCommandBuffer& commands,
    const std::span<const CommandRange> ranges,
    font::GlyphAtlas& glyph_atlas,
    const TextEngine* const text_engine,
    const SubmissionContext& context,
    RenderSubmission& telemetry,
    PreparationCache& cache,
    bool& resized
) {
    resized = false;
    const std::vector<RenderCommand>& stream = commands.commands();
    const std::vector<bool> visible_text = visible_text_commands(commands, context);
    const std::vector<double> text_scales = text_group_scales(commands, cache.group_raster_scales);
    const std::vector<bool> subpixel_runs =
        subpixel_text_runs(commands, text_scales, cache.turned_groups, context.text_subpixel);
    const std::vector<bool> zoomed_runs = zoomed_text_runs(commands, cache.group_raster_scales);
    // Text in the ranges is prepared as the full plan would: a run keeps its prepared glyphs
    // while its content does, and a hidden run drops them. Entries are exact for their content,
    // so they stand even when this update gives way to a full plan.
    for (const CommandRange& range : ranges) {
        for (std::size_t index = range.first; index <= range.last; ++index) {
            const auto* text = std::get_if<TextRunRenderCommand>(&stream[index]);
            if (text == nullptr) continue;
            if (!visible_text[index]) {
                cache.text[index].reset();
                continue;
            }
            if (text_engine == nullptr) return std::nullopt;
            const std::optional<PreparedTextCacheEntry>& retained = cache.text[index];
            if (retained.has_value() && same_text_content(retained->source, *text) &&
                retained->text_engine == text_engine &&
                retained->atlas_generation == glyph_atlas.generation() &&
                retained->display_scale == context.scale &&
                retained->raster_scale == text_scales[index] &&
                retained->subpixel == subpixel_runs[index] &&
                retained->unhinted == zoomed_runs[index]) {
                continue;
            }
            glyph_atlas.adopt_display_scale(context.scale);
            std::vector<font::GlyphAtlasWarmupRequest> requests;
            append_warmup_requests(*text, *text_engine, context, text_scales[index],
                                   subpixel_runs[index], zoomed_runs[index], requests);
            const std::uint64_t generation = glyph_atlas.generation();
            const auto warmup_started = std::chrono::steady_clock::now();
            const bool warmed = glyph_atlas.warm(requests);
            telemetry.atlas_warmup_nanos +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - warmup_started
                ).count();
            // A recycle changes every prepared run's atlas coordinates: plan them all.
            if (!warmed || glyph_atlas.generation() != generation) return std::nullopt;
            const auto preparation_started = std::chrono::steady_clock::now();
            std::vector<PreparedText> prepared =
                prepare_text(*text, glyph_atlas, *text_engine, context.scale, text_scales[index],
                             subpixel_runs[index], zoomed_runs[index]);
            std::vector<PreparedTextPtr> groups;
            groups.reserve(prepared.size());
            for (PreparedText& group : prepared) {
                groups.push_back(std::make_shared<const PreparedText>(std::move(group)));
            }
            cache.text[index] = PreparedTextCacheEntry{
                *text,         text_engine,        generation,
                context.scale, text_scales[index], subpixel_runs[index],
                zoomed_runs[index], std::move(groups),
            };
            telemetry.text_preparation_nanos +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - preparation_started
                ).count();
        }
    }

    struct Replanned final {
        std::vector<PlannedItem> items;
        std::vector<CommandPlan> plans;
        /** A lone draw whose items changed in number. */
        bool resized = false;
    };
    std::vector<Replanned> replanned;
    replanned.reserve(ranges.size());
    std::size_t skipped_delta_added = 0U;
    std::size_t skipped_delta_removed = 0U;
    for (const CommandRange& range : ranges) {
        Replanned next;
        std::size_t skipped_draws = 0U;
        Planner planner(context, cache, visible_text, next.items, skipped_draws,
                        cache.command_plans[range.first].state);
        for (std::size_t index = range.first; index <= range.last; ++index) {
            const CommandPlan& previous = cache.command_plans[index];
            CommandPlan plan;
            plan.state = planner.state();
            plan.first_item = previous.first_item;
            const std::size_t first_item = next.items.size();
            const std::size_t skipped_before = skipped_draws;
            planner.step(index, stream[index]);
            plan.item_count = next.items.size() - first_item;
            plan.skipped_draws = skipped_draws - skipped_before;
            if (plan.item_count != previous.item_count) {
                // Only a lone draw may: its items are all draws, and none of a scope's batches
                // move under it.
                if (range.first != range.last || !draw_command(stream[index]))
                    return std::nullopt;
                next.resized = true;
            }
            skipped_delta_added += plan.skipped_draws;
            skipped_delta_removed += previous.skipped_draws;
            next.plans.push_back(std::move(plan));
        }
        if (!planner.closed()) return std::nullopt;
        replanned.push_back(std::move(next));
    }

    // Draws may change; effect batches may not, or batching would change around them.
    std::vector<std::size_t> changed_items;
    for (std::size_t range_index = 0U; range_index < ranges.size(); ++range_index) {
        const Replanned& next = replanned[range_index];
        if (next.resized) {
            if (!std::ranges::all_of(next.items, [](const PlannedItem& item) {
                    return std::holds_alternative<PreparedDraw>(item.value);
                })) {
                return std::nullopt;
            }
            resized = true;
            continue;
        }
        const std::size_t first_item = cache.command_plans[ranges[range_index].first].first_item;
        for (std::size_t offset = 0U; offset < next.items.size(); ++offset) {
            const PlannedItem& previous = cache.planned_items[first_item + offset];
            const auto* previous_draw = std::get_if<PreparedDraw>(&previous.value);
            const auto* draw = std::get_if<PreparedDraw>(&next.items[offset].value);
            if ((previous_draw == nullptr) != (draw == nullptr)) return std::nullopt;
            if (draw == nullptr) {
                if (std::get<SubmissionBatch>(previous.value) !=
                    std::get<SubmissionBatch>(next.items[offset].value)) {
                    return std::nullopt;
                }
                continue;
            }
            if (*draw != *previous_draw) changed_items.push_back(first_item + offset);
        }
    }
    if (resized) {
        // Splice: the items between the ranges stay, each range's items replace its old ones,
        // and every command's first item follows from the counts before it.
        std::vector<PlannedItem> items;
        items.reserve(cache.planned_items.size());
        std::size_t cursor = 0U;
        for (std::size_t range_index = 0U; range_index < ranges.size(); ++range_index) {
            Replanned& next = replanned[range_index];
            const CommandRange& range = ranges[range_index];
            const std::size_t first_item = cache.command_plans[range.first].first_item;
            std::size_t old_count = 0U;
            for (std::size_t index = range.first; index <= range.last; ++index)
                old_count += cache.command_plans[index].item_count;
            std::move(cache.planned_items.begin() + static_cast<std::ptrdiff_t>(cursor),
                      cache.planned_items.begin() + static_cast<std::ptrdiff_t>(first_item),
                      std::back_inserter(items));
            std::move(next.items.begin(), next.items.end(), std::back_inserter(items));
            cursor = first_item + old_count;
            for (std::size_t offset = 0U; offset < next.plans.size(); ++offset)
                cache.command_plans[range.first + offset] = std::move(next.plans[offset]);
        }
        std::move(cache.planned_items.begin() + static_cast<std::ptrdiff_t>(cursor),
                  cache.planned_items.end(), std::back_inserter(items));
        cache.planned_items = std::move(items);
        std::size_t running = 0U;
        for (CommandPlan& plan : cache.command_plans) {
            plan.first_item = running;
            running += plan.item_count;
        }
        changed_items.clear();
    } else {
        for (std::size_t range_index = 0U; range_index < ranges.size(); ++range_index) {
            Replanned& next = replanned[range_index];
            const CommandRange& range = ranges[range_index];
            const std::size_t first_item = cache.command_plans[range.first].first_item;
            for (std::size_t offset = 0U; offset < next.items.size(); ++offset) {
                cache.planned_items[first_item + offset] = std::move(next.items[offset]);
            }
            for (std::size_t offset = 0U; offset < next.plans.size(); ++offset) {
                cache.command_plans[range.first + offset] = std::move(next.plans[offset]);
            }
        }
    }
    telemetry.skipped_draws = telemetry.skipped_draws + skipped_delta_added -
                              skipped_delta_removed;
    return changed_items;
}

} // namespace strata::ui::submission_detail
