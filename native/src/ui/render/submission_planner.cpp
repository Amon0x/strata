#include "ui/render/submission_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "font/raster.hpp"
#include "ui/render/path_geometry.hpp"
#include "ui/text.hpp"

namespace strata::ui::submission_detail {
namespace {

constexpr double transform_epsilon = 0.000001;
constexpr std::uint16_t coverage_subpixel_divisions = 4U;
constexpr std::size_t maximum_content_effect_depth = 4U;

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

[[nodiscard]] double quantized_position(
    const double logical,
    const double display_scale,
    const std::uint16_t divisions
) noexcept {
    return std::round(logical * display_scale * static_cast<double>(divisions)) /
           (display_scale * static_cast<double>(divisions));
}

[[nodiscard]] std::vector<PreparedText> prepare_text(
    const TextRunRenderCommand& command,
    font::GlyphAtlas& atlas,
    const TextEngine& text_engine,
    const double display_scale
) {
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
        const font::SubpixelPhase phase = font::SubpixelPhase::quantize(
            requested_x * display_scale, coverage_subpixel_divisions
        );
        const std::optional<font::GlyphAtlasEntry> atlas_entry = atlas.request(
            logical.font_id,
            text_engine.font(logical.font_id),
            static_cast<std::uint16_t>(logical.glyph_id),
            command.pixel_size,
            phase,
            logical.font_style_flags,
            raster_mode(command.font_rasterization)
        );
        if (!atlas_entry.has_value()) continue;
        const double pen_x = atlas_entry->mode == font::GlyphRasterMode::coverage
            ? quantized_position(requested_x, display_scale, coverage_subpixel_divisions)
            : requested_x;
        const font::RasterPlaneBounds plane = atlas_entry->plane_bounds_layout_pixels;
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
            glyphs[begin].atlas.layout_pixel_range,
            {},
        };
        text.glyphs.reserve(end - begin);
        for (std::size_t index = begin; index < end; ++index) {
            text.glyphs.push_back(std::move(glyphs[index].glyph));
        }
        groups.push_back(std::move(text));
        begin = end;
    }
    return groups;
}

[[nodiscard]] bool same_text_content(
    const TextRunRenderCommand& left,
    const TextRunRenderCommand& right
) {
    return left.color == right.color && left.pixel_size == right.pixel_size &&
        left.font_rasterization == right.font_rasterization && left.glyphs == right.glyphs;
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

void append_warmup_requests(
    const TextRunRenderCommand& text,
    const TextEngine& text_engine,
    const SubmissionContext& context,
    std::vector<font::GlyphAtlasWarmupRequest>& requests
) {
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
            text.pixel_size,
            font::SubpixelPhase::quantize(
                requested_x * context.scale,
                coverage_subpixel_divisions
            ),
            logical.font_style_flags,
            raster_mode(text.font_rasterization),
        });
    }
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
    template <typename Value>
    void add(const Value& value) noexcept {
        static_assert(std::is_trivially_copyable_v<Value>);
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (std::size_t index = 0U; index < sizeof(Value); ++index) {
            state_ ^= bytes[index];
            state_ *= 1099511628211ULL;
        }
    }
    [[nodiscard]] std::uint64_t value() const noexcept { return state_; }

  private:
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
                                      2U * static_cast<std::uint32_t>(gradient.extend));
    const double width = std::max(bounds.width, 1.0e-9);
    const double height = std::max(bounds.height, 1.0e-9);
    // Normalized position: n = ((x - bounds.x) / width, (y - bounds.y) / height).
    if (gradient.kind == GradientKind::linear) {
        const auto [start, end] = gradient.axis(Size{bounds.width, bounds.height});
        const Point direction{end.x - start.x, end.y - start.y};
        const double length_squared = direction.x * direction.x + direction.y * direction.y;
        if (length_squared > 0.0) {
            const double ax = direction.x / length_squared;
            const double ay = direction.y / length_squared;
            result.position = {
                ax / width, ay / height,
                (-bounds.x / width - start.x) * ax + (-bounds.y / height - start.y) * ay,
                0.0, 0.0, 0.0,
            };
        }
    } else {
        const double radius_x = std::max(gradient.radius_x, 1.0e-9);
        const double radius_y = std::max(gradient.radius_y, 1.0e-9);
        result.position = {
            1.0 / (width * radius_x), 0.0, (-bounds.x / width - gradient.center.x) / radius_x,
            0.0, 1.0 / (height * radius_y), (-bounds.y / height - gradient.center.y) / radius_y,
        };
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
    for (const GradientStop& stop : gradient.stops) {
        hash.add(stop.offset);
        hash.add(stop.color);
    }
}

/**
 * Draws joined shapes (PathRenderCommand::joined) as one picture: every part of every shape is
 * rasterized to exact coverage on one device grid, then composited front to back by area into one
 * colour mask. Each shape takes what is left of a pixel, so shapes meeting along an edge split
 * its pixels between them and nothing behind shows through, while a shape over another covers it
 * as it would drawn alone (a translucent one lets the one below show through by its alpha). Paints
 * are evaluated per pixel. Returns false, drawing nothing, when there is no shape atlas or the
 * picture is larger than a page; the shapes then draw one by one.
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
    for (const PathShape* shape : shapes) {
        hash.add(shape->path.segments().size());
        for (const PathSegment& segment : shape->path.segments())
            hash.add(segment);
        hash.add(shape->fill_rule);
        hash_paint(hash, shape->fill);
        hash_paint(hash, shape->stroke);
        const StrokeStyle style = shape->stroke_style.value_or(StrokeStyle{});
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
    hash.add(grid->device_x);
    hash.add(grid->device_y);
    hash.add(grid->phase);
    char key[40];
    std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(hash.value()));

    const std::optional<font::GlyphAtlasEntry> entry = atlas->request_mask(
        key,
        [&]() -> std::optional<font::GlyphRasterBitmap> {
            // Every part's outline in device pixels relative to the whole-pixel origin, with the
            // paint it carries; parts are listed back to front.
            struct Layer final {
                std::vector<std::vector<font::CoveragePoint>> polygons;
                bool even_odd = false;
                const Paint* paint = nullptr;
            };
            std::vector<Layer> layers;
            double left = std::numeric_limits<double>::infinity();
            double top = std::numeric_limits<double>::infinity();
            double right = -std::numeric_limits<double>::infinity();
            double bottom = -std::numeric_limits<double>::infinity();
            const auto device = [&](const std::vector<PathContour>& outline) {
                std::vector<std::vector<font::CoveragePoint>> polygons;
                polygons.reserve(outline.size());
                for (const PathContour& contour : outline) {
                    std::vector<font::CoveragePoint> polygon;
                    polygon.reserve(contour.points.size());
                    for (const Point point : contour.points) {
                        const font::CoveragePoint at{grid->phase.x + point.x * grid->device_x,
                                                     grid->phase.y + point.y * grid->device_y};
                        left = std::min(left, at.x);
                        top = std::min(top, at.y);
                        right = std::max(right, at.x);
                        bottom = std::max(bottom, at.y);
                        polygon.push_back(at);
                    }
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
                    layers.push_back(Layer{device(outline),
                                           shape->fill_rule == PathFillRule::evenodd,
                                           &*shape->fill});
                }
                const StrokeStyle style = shape->stroke_style.value_or(StrokeStyle{});
                if (shape->stroke.has_value() && !shape->stroke->transparent() && style.width > 0.0) {
                    layers.push_back(Layer{device(stroke_outline(centrelines, style, tolerance)),
                                           false, &*shape->stroke});
                }
            }
            if (!std::isfinite(left) || right - left > maximum_mask_extent ||
                bottom - top > maximum_mask_extent) {
                return std::nullopt;
            }
            const double x0 = std::floor(left) - 1.0;
            const double y0 = std::floor(top) - 1.0;
            const auto width = static_cast<std::uint32_t>(std::ceil(right) + 1.0 - x0);
            const auto height = static_cast<std::uint32_t>(std::ceil(bottom) + 1.0 - y0);
            const std::size_t pixels = static_cast<std::size_t>(width) * height;
            // Front to back: `area` is how much of the pixel shapes in front already cover and
            // `alpha` how opaque they make it, so a shape takes the uncovered rest at full strength
            // and what it shares with them through their transparency.
            std::vector<float> red(pixels, 0.0F);
            std::vector<float> green(pixels, 0.0F);
            std::vector<float> blue(pixels, 0.0F);
            std::vector<float> alpha(pixels, 0.0F);
            std::vector<float> area(pixels, 0.0F);
            for (auto layer = layers.rbegin(); layer != layers.rend(); ++layer) {
                const std::optional<std::vector<std::uint8_t>> coverage = font::rasterize_polygons(
                    layer->polygons, width, height, layer->even_odd, font::CoveragePoint{x0, y0});
                if (!coverage.has_value())
                    return std::nullopt;
                const Gradient* gradient = layer->paint->gradient();
                const runtime::ColorValue solid = layer->paint->representative();
                for (std::uint32_t row = 0U; row < height; ++row) {
                    for (std::uint32_t column = 0U; column < width; ++column) {
                        const std::size_t at = static_cast<std::size_t>(row) * width + column;
                        const float covered = static_cast<float>((*coverage)[at]) / 255.0F;
                        if (covered <= 0.0F || alpha[at] >= 1.0F)
                            continue;
                        runtime::ColorValue paint = solid;
                        if (gradient != nullptr) {
                            const Point local{
                                (x0 + column + 0.5 - grid->phase.x) / grid->device_x,
                                (y0 + row + 0.5 - grid->phase.y) / grid->device_y};
                            paint = gradient->sample(gradient_parameter(
                                *gradient,
                                Point{local.x / bounds.width, local.y / bounds.height},
                                Size{bounds.width, bounds.height}));
                        }
                        const float exposed = std::min(covered, 1.0F - area[at]);
                        const float shared = covered - exposed;
                        const float through =
                            area[at] > 0.0F
                                ? std::clamp((area[at] - alpha[at]) / area[at], 0.0F, 1.0F)
                                : 1.0F;
                        const float weight =
                            static_cast<float>(paint.alpha) / 255.0F * (exposed + shared * through);
                        red[at] += static_cast<float>(paint.red) * weight;
                        green[at] += static_cast<float>(paint.green) * weight;
                        blue[at] += static_cast<float>(paint.blue) * weight;
                        alpha[at] = std::min(1.0F, alpha[at] + weight);
                        area[at] = std::min(1.0F, area[at] + exposed);
                    }
                }
            }
            font::GlyphRasterBitmap bitmap;
            bitmap.width = width;
            bitmap.height = height;
            bitmap.mode = font::GlyphRasterMode::msdf;
            bitmap.bytes.resize(pixels * 4U);
            // Straight colour, as images are drawn.
            for (std::size_t at = 0U; at < pixels; ++at) {
                const float covered = alpha[at];
                std::uint8_t* texel = bitmap.bytes.data() + at * 4U;
                if (covered <= 0.0F) {
                    texel[0] = texel[1] = texel[2] = texel[3] = 0U;
                    continue;
                }
                const auto channel = [covered](const float premultiplied) {
                    return static_cast<std::uint8_t>(
                        std::clamp(premultiplied / covered + 0.5F, 0.0F, 255.0F));
                };
                texel[0] = channel(red[at]);
                texel[1] = channel(green[at]);
                texel[2] = channel(blue[at]);
                texel[3] = static_cast<std::uint8_t>(std::clamp(covered * 255.0F + 0.5F, 0.0F, 255.0F));
            }
            bitmap.plane_bounds_layout_pixels =
                font::RasterPlaneBounds{x0, y0 + height, x0 + width, y0};
            return bitmap;
        },
        font::GlyphRasterMode::msdf);
    if (!entry.has_value())
        return false;
    const auto [quad, uv] = mask_quad(*grid, transform, bounds, entry->plane_bounds_layout_pixels,
                                      TextureRegion{entry->u, entry->v, entry->uv_width,
                                                    entry->uv_height});
    append_draw(output,
                PreparedCommand{std::make_shared<const PreparedMask>(PreparedMask{
                    quad, uv, RenderColor{255U, 255U, 255U, 255U}, entry->texture, std::nullopt,
                    true})},
                source_order, transform, clip, rounded_clips, std::nullopt, opacity, group,
                context, skipped_draws);
    return true;
}

/**
 * Draws a path's fill and stroke from exact coverage masks in the shape atlas, as text draws its
 * glyphs: the outline is flattened and (for a stroke) expanded at device resolution, rasterized
 * by area into a mask placed on the device pixel grid at the shape's own sub-pixel position, and
 * drawn with the paint: its colour, or its gradient evaluated per pixel. A shape larger than a
 * page is drawn in tiles. Returns false, drawing nothing, when the shape draws analytically or as
 * geometry instead: no shape atlas, full pages, or a shape too large even for tiles.
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
    std::unordered_map<std::uint64_t, std::uint8_t>& tile_classes,
    const std::span<const GroupRasterScale> group_scales
) {
    if (!command.joined.empty()) {
        return append_joined_masks(output, command, source_order, transform, clip, rounded_clips,
                                   opacity, group, context, skipped_draws, atlas, group_scales);
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
    for (const Part& part : parts) {
        MaskHash hash;
        for (const PathSegment& segment : shape.path.segments())
            hash.add(segment);
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

        // The part's outline in device pixels relative to the whole-pixel origin, made once.
        std::optional<std::vector<std::vector<font::CoveragePoint>>> device_outline;
        const auto outline_polygons = [&]() -> const std::vector<std::vector<font::CoveragePoint>>& {
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
                std::vector<std::vector<font::CoveragePoint>> polygons;
                polygons.reserve(outline.size());
                for (const PathContour& contour : outline) {
                    std::vector<font::CoveragePoint> polygon;
                    polygon.reserve(contour.points.size());
                    for (const Point point : contour.points)
                        polygon.push_back(font::CoveragePoint{phase.x + point.x * device_x,
                                                              phase.y + point.y * device_y});
                    polygons.push_back(std::move(polygon));
                }
                device_outline = std::move(polygons);
            }
            return *device_outline;
        };
        const bool even_odd = !part.stroke && shape.fill_rule == PathFillRule::evenodd;

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
            char key[40];
            std::snprintf(key, sizeof(key), "%016llx",
                          static_cast<unsigned long long>(hash.value()));
            const std::optional<font::GlyphAtlasEntry> entry = atlas->request_mask(
                key, [&]() -> std::optional<font::GlyphRasterBitmap> {
                    const auto& polygons = outline_polygons();
                    double outline_left = std::numeric_limits<double>::infinity();
                    double outline_top = std::numeric_limits<double>::infinity();
                    double outline_right = -std::numeric_limits<double>::infinity();
                    double outline_bottom = -std::numeric_limits<double>::infinity();
                    for (const std::vector<font::CoveragePoint>& polygon : polygons) {
                        for (const font::CoveragePoint point : polygon) {
                            outline_left = std::min(outline_left, point.x);
                            outline_top = std::min(outline_top, point.y);
                            outline_right = std::max(outline_right, point.x);
                            outline_bottom = std::max(outline_bottom, point.y);
                        }
                    }
                    if (!std::isfinite(outline_left) ||
                        outline_right - outline_left > maximum_mask_extent ||
                        outline_bottom - outline_top > maximum_mask_extent) {
                        return std::nullopt;
                    }
                    // One clear pixel around the outline, so linear sampling never reaches a
                    // neighbour in the atlas.
                    const double x0 = std::floor(outline_left) - 1.0;
                    const double y0 = std::floor(outline_top) - 1.0;
                    const auto width =
                        static_cast<std::uint32_t>(std::ceil(outline_right) + 1.0 - x0);
                    const auto height =
                        static_cast<std::uint32_t>(std::ceil(outline_bottom) + 1.0 - y0);
                    std::optional<std::vector<std::uint8_t>> coverage = font::rasterize_polygons(
                        polygons, width, height, even_odd, font::CoveragePoint{x0, y0});
                    if (!coverage.has_value())
                        return std::nullopt;
                    font::GlyphRasterBitmap bitmap;
                    bitmap.width = width;
                    bitmap.height = height;
                    bitmap.bytes = std::move(*coverage);
                    // The mask's device-pixel box relative to the whole-pixel origin.
                    bitmap.plane_bounds_layout_pixels =
                        font::RasterPlaneBounds{x0, y0 + height, x0 + width, y0};
                    return bitmap;
                });
            if (!entry.has_value())
                return false;
            pieces.push_back(Piece{&part, entry->plane_bounds_layout_pixels,
                                   TextureRegion{entry->u, entry->v, entry->uv_width,
                                                 entry->uv_height},
                                   entry->texture});
            continue;
        }

        // Tiles over the device box.
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
        std::optional<font::GlyphAtlasEntry> solid;
        constexpr std::uint32_t side = mask_tile + 2U;
        const auto band_width =
            static_cast<std::uint32_t>(last_x - first_x + 1) * mask_tile + 2U;
        for (std::int64_t tile_y = first_y; tile_y <= last_y; ++tile_y) {
            // The row of tiles rasterizes once, aprons included, when any of its tiles is missing;
            // each tile is then cut from it.
            std::optional<std::vector<std::uint8_t>> band;
            const auto band_coverage = [&]() -> const std::vector<std::uint8_t>& {
                if (!band.has_value()) {
                    band = font::rasterize_polygons(
                        outline_polygons(), band_width, side, even_odd,
                        font::CoveragePoint{static_cast<double>(first_x) * tile - 1.0,
                                            static_cast<double>(tile_y) * tile - 1.0});
                }
                return *band;
            };
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
                    tile_class = known->second;
                if (tile_class == 0U)
                    continue;
                if (tile_class == 2U) {
                    char key[48];
                    std::snprintf(key, sizeof(key), "%016llx",
                                  static_cast<unsigned long long>(identity));
                    std::optional<std::uint8_t> found_class;
                    const std::optional<font::GlyphAtlasEntry> entry = atlas->request_mask(
                        key, [&]() -> std::optional<font::GlyphRasterBitmap> {
                            const std::vector<std::uint8_t>& rows = band_coverage();
                            const std::size_t offset =
                                static_cast<std::size_t>(tile_x - first_x) * mask_tile;
                            std::optional<std::vector<std::uint8_t>> coverage(
                                std::in_place, static_cast<std::size_t>(side) * side);
                            for (std::uint32_t row = 0U; row < side; ++row) {
                                std::memcpy(coverage->data() + static_cast<std::size_t>(row) * side,
                                            rows.data() + static_cast<std::size_t>(row) * band_width +
                                                offset,
                                            side);
                            }
                            if (std::ranges::all_of(*coverage,
                                                    [](const std::uint8_t value) { return value == 0U; })) {
                                found_class = std::uint8_t{0U};
                                return std::nullopt;
                            }
                            if (std::ranges::all_of(*coverage, [](const std::uint8_t value) {
                                    return value == 255U;
                                })) {
                                found_class = std::uint8_t{1U};
                                return std::nullopt;
                            }
                            font::GlyphRasterBitmap bitmap;
                            bitmap.width = side;
                            bitmap.height = side;
                            bitmap.bytes = std::move(*coverage);
                            bitmap.plane_bounds_layout_pixels = font::RasterPlaneBounds{
                                core.left - 1.0, core.bottom + 1.0, core.right + 1.0,
                                core.top - 1.0};
                            return bitmap;
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
                        const double inset_u = entry->uv_width / static_cast<double>(mask_tile + 2U);
                        const double inset_v = entry->uv_height / static_cast<double>(mask_tile + 2U);
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
                    solid = atlas->request_mask("solid", []() -> std::optional<font::GlyphRasterBitmap> {
                        font::GlyphRasterBitmap bitmap;
                        bitmap.width = 4U;
                        bitmap.height = 4U;
                        bitmap.bytes.assign(16U, 255U);
                        bitmap.plane_bounds_layout_pixels = font::RasterPlaneBounds{0.0, 4.0, 4.0, 0.0};
                        return bitmap;
                    });
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
                    (rounded(safe_radii) || !transform_.axis_aligned()) && !clip_.empty();
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
                                           cache_.group_raster_scales)) {
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
    // A group restarts transform and opacity: its content is encoded relative to the group.
    struct GroupScope {
        Transform transform;
        double opacity = 1.0;
        std::uint32_t group = 0U;
    };

    [[nodiscard]] EffectState scoped_effect(EffectState effect) const {
        effect.opacity *= opacity_;
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
            return value != nullptr && value->mode == font::GlyphRasterMode::coverage
                ? "strata:coverage_text" : "strata:msdf_text";
        } else if constexpr (std::is_same_v<Type, PathRenderCommand>) {
            // Shape geometry is its own silhouette, so it needs no signed-distance mask.
            return "strata:solid";
        } else if constexpr (std::is_same_v<Type, CustomMeshRenderCommand>) {
            return value.material.has_value() ? std::string_view(value.material->id)
                                              : std::string_view("strata:custom_mesh");
        } else if constexpr (std::is_same_v<Type, PreparedMaskPtr>) {
            return value != nullptr && value->colored ? "strata:textured"
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
    return MaterialState{std::string(default_material_id(command))};
}

bool unified_material(const std::string_view material) noexcept {
    return material == "strata:solid" || material == "strata:textured" ||
           material == "strata:rounded_rect" || material == "strata:border" ||
           material == "strata:coverage_text" || material == "strata:msdf_text" ||
           material == "strata:custom_mesh" || material == "strata:shadow" ||
           material == "strata:coverage_mask";
}

float unified_draw_mode(const std::string_view material) noexcept {
    if (material == "strata:textured" || material == "strata:custom_mesh") return 1.0F;
    if (material == "strata:rounded_rect") return 2.0F;
    if (material == "strata:border") return 3.0F;
    if (material == "strata:coverage_text") return 4.0F;
    if (material == "strata:msdf_text") return 5.0F;
    if (material == "strata:shadow") return 6.0F;
    if (material == "strata:coverage_mask") return 10.0F;
    return 0.0F;
}

bool samples_texture(const std::string_view material) noexcept {
    return material == "strata:textured" || material == "strata:nine_patch" ||
           material == "strata:coverage_text" || material == "strata:msdf_text" ||
           material == "strata:custom_mesh" || material == "strata:coverage_mask";
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
            if (!all && retained.has_value() &&
                same_text_content(retained->source, *text) &&
                retained->text_engine == text_engine &&
                retained->atlas_generation == atlas_generation &&
                retained->display_scale == context.scale) {
                continue;
            }
            append_warmup_requests(*text, *text_engine, context, requests);
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
                    retained->display_scale == context.scale) {
                    continue;
                }
                std::vector<PreparedText> prepared = prepare_text(
                    *text, glyph_atlas, *text_engine, context.scale
                );
                std::vector<PreparedTextPtr> groups;
                groups.reserve(prepared.size());
                for (PreparedText& group : prepared) {
                    groups.push_back(std::make_shared<const PreparedText>(std::move(group)));
                }
                cache.text[index] = PreparedTextCacheEntry{
                    *text,
                    text_engine,
                    glyph_atlas.generation(),
                    context.scale,
                    std::move(groups),
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
    PreparationCache& cache
) {
    const std::vector<RenderCommand>& stream = commands.commands();
    const std::vector<bool> visible_text = visible_text_commands(commands, context);
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
                retained->display_scale == context.scale) {
                continue;
            }
            glyph_atlas.adopt_display_scale(context.scale);
            std::vector<font::GlyphAtlasWarmupRequest> requests;
            append_warmup_requests(*text, *text_engine, context, requests);
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
                prepare_text(*text, glyph_atlas, *text_engine, context.scale);
            std::vector<PreparedTextPtr> groups;
            groups.reserve(prepared.size());
            for (PreparedText& group : prepared) {
                groups.push_back(std::make_shared<const PreparedText>(std::move(group)));
            }
            cache.text[index] = PreparedTextCacheEntry{
                *text, text_engine, generation, context.scale, std::move(groups),
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
            if (plan.item_count != previous.item_count) return std::nullopt;
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
    telemetry.skipped_draws = telemetry.skipped_draws + skipped_delta_added -
                              skipped_delta_removed;
    return changed_items;
}

} // namespace strata::ui::submission_detail
