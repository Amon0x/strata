#include "ui/render/submission_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "font/raster.hpp"
#include "ui/text.hpp"

namespace strata::ui::submission_detail {
namespace {

constexpr double transform_epsilon = 0.000001;
constexpr std::uint16_t coverage_subpixel_divisions = 4U;
constexpr std::size_t maximum_content_effect_depth = 4U;

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

[[nodiscard]] std::optional<std::string> command_texture(
    const PreparedCommand& command,
    const SubmissionContext& context
) {
    return std::visit([&context](const auto& value) -> std::optional<std::string> {
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
            if (!logical_texture.has_value()) return std::nullopt;
            const auto declared = std::ranges::find(
                context.textures,
                *logical_texture,
                &resource::TextureResourceDescriptor::logical_id
            );
            // Undeclared ids retain the pre-existing host-managed texture path. Surface-declared
            // static images are always translated to their collision-free host identity.
            return declared == context.textures.end()
                ? logical_texture
                : std::optional<std::string>{declared->host_id};
        } else if constexpr (std::is_same_v<Type, PreparedTextPtr>) {
            return value != nullptr ? std::optional<std::string>(value->texture) : std::nullopt;
        } else {
            return std::nullopt;
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

[[nodiscard]] std::vector<bool> visible_text_commands(
    const RenderCommandBuffer& commands,
    const SubmissionContext& context
) {
    std::vector<bool> result(commands.size(), false);
    std::vector<Rect> clip_stack;
    std::vector<Transform> transform_stack;
    Rect clip{0.0, 0.0, context.logical_width, context.logical_height};
    Transform transform;
    for (std::size_t index = 0U; index < commands.commands().size(); ++index) {
        const RenderCommand& source = commands.commands()[index];
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
            result[index] = !text->cull_bounds.has_value() ||
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
    const std::optional<MaterialState>& material_override,
    const SubmissionContext& context,
    std::size_t& skipped_draws
) {
    const Rect local_bounds = command_bounds(command);
    const Rect transformed_bounds = transform.bounds(local_bounds);
    const Rect visible = intersect(clip, transformed_bounds);
    const SubmissionScissor resolved_scissor = scissor(clip, context);
    if (local_bounds.empty() || transformed_bounds.empty() || visible.empty() ||
        resolved_scissor.width == 0U || resolved_scissor.height == 0U) {
        ++skipped_draws;
        return;
    }
    MaterialState material = merged_material(command, material_override);
    std::optional<std::string> texture = command_texture(command, context);
    output.emplace_back(PreparedDraw{
        source_order,
        std::move(command),
        local_bounds,
        transform,
        std::move(material),
        std::move(texture),
        resolved_scissor,
        false,
    });
}

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

bool Transform::axis_aligned_translation() const noexcept {
    return std::abs(m00 - 1.0) <= transform_epsilon &&
           std::abs(m11 - 1.0) <= transform_epsilon &&
           std::abs(m01) <= transform_epsilon && std::abs(m10) <= transform_epsilon;
}

MaterialState default_material(const PreparedCommand& command) {
    return std::visit([](const auto& value) -> MaterialState {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, SolidRectRenderCommand>) {
            // A gradient is tessellated geometry, and the rounded-rect material is what masks that
            // geometry to the shape. A plain rectangle keeps the cheaper solid material.
            return MaterialState{value.fill.is_gradient() ? "strata:rounded_rect" : "strata:solid"};
        } else if constexpr (std::is_same_v<Type, RoundedRectRenderCommand>) {
            return MaterialState{"strata:rounded_rect"};
        } else if constexpr (std::is_same_v<Type, BorderRenderCommand>) {
            return MaterialState{"strata:border"};
        } else if constexpr (std::is_same_v<Type, ImageRenderCommand>) {
            return MaterialState{"strata:textured"};
        } else if constexpr (std::is_same_v<Type, NinePatchRenderCommand>) {
            return MaterialState{"strata:nine_patch"};
        } else if constexpr (std::is_same_v<Type, PreparedTextPtr>) {
            return MaterialState{value != nullptr &&
                    value->mode == font::GlyphRasterMode::coverage
                ? "strata:coverage_text" : "strata:msdf_text"};
        } else if constexpr (std::is_same_v<Type, PathRenderCommand>) {
            // Shape geometry is its own silhouette, so it needs no signed-distance mask.
            return MaterialState{"strata:solid"};
        } else if constexpr (std::is_same_v<Type, CustomMeshRenderCommand>) {
            MaterialState material = value.material.value_or(MaterialState{"strata:custom_mesh"});
            material.opacity *= value.opacity;
            return material;
        } else {
            static_assert(std::is_same_v<Type, ShadowRenderCommand>);
            return MaterialState{"strata:shadow"};
        }
    }, command);
}

bool unified_material(const std::string_view material) noexcept {
    return material == "strata:solid" || material == "strata:textured" ||
           material == "strata:rounded_rect" || material == "strata:border" ||
           material == "strata:coverage_text" || material == "strata:msdf_text" ||
           material == "strata:custom_mesh" || material == "strata:shadow";
}

float unified_draw_mode(const std::string_view material) noexcept {
    if (material == "strata:textured" || material == "strata:custom_mesh") return 1.0F;
    if (material == "strata:rounded_rect") return 2.0F;
    if (material == "strata:border") return 3.0F;
    if (material == "strata:coverage_text") return 4.0F;
    if (material == "strata:msdf_text") return 5.0F;
    if (material == "strata:shadow") return 6.0F;
    return 0.0F;
}

bool samples_texture(const std::string_view material) noexcept {
    return material == "strata:textured" || material == "strata:nine_patch" ||
           material == "strata:coverage_text" || material == "strata:msdf_text" ||
           material == "strata:custom_mesh";
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
    cache.text.resize(commands.size());
    const std::vector<bool> visible_text = visible_text_commands(commands, context);
    std::vector<font::GlyphAtlasWarmupRequest> warmup_requests;
    if (text_required) {
        for (std::size_t index = 0U; index < commands.commands().size(); ++index) {
            if (!visible_text[index]) continue;
            const auto* text = std::get_if<TextRunRenderCommand>(
                &commands.commands()[index]
            );
            if (text == nullptr) continue;
            warmup_requests.reserve(warmup_requests.size() + text->glyphs.size());
            for (const LogicalGlyph& logical : text->glyphs) {
                if (logical.glyph_id > std::numeric_limits<std::uint16_t>::max()) {
                    throw font::FontError(
                        "logical glyph id exceeds the native OpenType glyph range"
                    );
                }
                const double requested_x = logical.x + logical.x_placement;
                warmup_requests.push_back(font::GlyphAtlasWarmupRequest{
                    logical.font_id,
                    &text_engine->font(logical.font_id),
                    static_cast<std::uint16_t>(logical.glyph_id),
                    text->pixel_size,
                    font::SubpixelPhase::quantize(
                        requested_x * context.scale,
                        coverage_subpixel_divisions
                    ),
                    logical.font_style_flags,
                    raster_mode(text->font_rasterization),
                });
            }
        }
    }
    // Populate only changed text runs. Capacity reclamation is generation-wide, so a recycle
    // during warmup invalidates older prepared UVs and repeats until every retained run belongs
    // to one stable generation.
    std::uint64_t warm_generation = glyph_atlas.generation();
    if (text_required) {
        do {
            warm_generation = glyph_atlas.generation();
            const auto warmup_started = std::chrono::steady_clock::now();
            const bool warmed = glyph_atlas.warm(warmup_requests);
            telemetry.atlas_warmup_nanos +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - warmup_started
                ).count();
            if (!warmed) continue;
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
        } while (glyph_atlas.generation() != warm_generation);
    }
    std::vector<PlannedItem> output;
    output.reserve(commands.size());
    std::vector<Rect> clip_stack;
    std::vector<Transform> transform_stack;
    std::vector<std::optional<MaterialState>> material_stack;
    Rect clip{0.0, 0.0, context.logical_width, context.logical_height};
    Transform transform;
    std::optional<MaterialState> material_override;
    std::size_t content_effect_depth = 0U;

    for (std::size_t index = 0U; index < commands.commands().size(); ++index) {
        if (index > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("render source order exceeds the submission packet range");
        }
        const std::uint32_t source_order = static_cast<std::uint32_t>(index);
        const RenderCommand& source = commands.commands()[index];
        std::visit([&](const auto& value) {
            using Type = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Type, ClipPushRenderCommand>) {
                clip_stack.push_back(clip);
                clip = intersect(clip, transform.bounds(value.rect));
            } else if constexpr (std::is_same_v<Type, ClipPopRenderCommand>) {
                if (clip_stack.empty()) throw std::logic_error("render clip stack underflow");
                clip = clip_stack.back();
                clip_stack.pop_back();
            } else if constexpr (std::is_same_v<Type, TransformPushRenderCommand>) {
                transform_stack.push_back(transform);
                transform = transform.concatenate(Transform{
                    value.m00, value.m01, value.m02, value.m10, value.m11, value.m12,
                });
            } else if constexpr (std::is_same_v<Type, TransformPopRenderCommand>) {
                if (transform_stack.empty()) throw std::logic_error("render transform stack underflow");
                transform = transform_stack.back();
                transform_stack.pop_back();
            } else if constexpr (std::is_same_v<Type, MaterialPushRenderCommand>) {
                material_stack.push_back(material_override);
                material_override = value.material;
            } else if constexpr (std::is_same_v<Type, MaterialPopRenderCommand>) {
                if (material_stack.empty()) throw std::logic_error("render material stack underflow");
                material_override = material_stack.back();
                material_stack.pop_back();
            } else if constexpr (std::is_same_v<Type, BlurRegionRenderCommand>) {
                const Rect visible = intersect(clip, transform.bounds(value.bounds));
                if (visible.empty()) {
                    ++skipped_draws;
                    return;
                }
                output.emplace_back(SubmissionBatch{
                    SubmissionBatchKind::blur,
                    "strata:blur_region",
                    "straight_alpha",
                    std::nullopt,
                    scissor(clip, context),
                    0U, 0U, 0U, source_order,
                    transform.bounds(value.bounds),
                    value.radius,
                    static_cast<std::uint32_t>(std::min<std::size_t>(
                        value.downsample, std::numeric_limits<std::uint32_t>::max()
                    )),
                });
            } else if constexpr (std::is_same_v<Type, BackdropEffectRenderCommand>) {
                const Rect visible = intersect(clip, transform.bounds(value.bounds));
                if (visible.empty()) {
                    ++skipped_draws;
                    return;
                }
                const double scale = std::sqrt(std::abs(
                    transform.m00 * transform.m11 - transform.m01 * transform.m10
                ));
                output.emplace_back(SubmissionBatch{
                    .kind = SubmissionBatchKind::backdrop_effect,
                    .material = value.effect.id,
                    .scissor = scissor(clip, context),
                    .source_order = source_order,
                    .effect_bounds = transform.bounds(value.bounds),
                    .effect_radii = CornerRadii{
                        value.radii.top_left * scale,
                        value.radii.top_right * scale,
                        value.radii.bottom_right * scale,
                        value.radii.bottom_left * scale,
                    },
                    .effect = value.effect,
                });
            } else if constexpr (std::is_same_v<Type, ContentEffectPushRenderCommand>) {
                if (content_effect_depth == maximum_content_effect_depth) {
                    throw std::length_error(
                        "render content effect nesting exceeds the packet limit"
                    );
                }
                ++content_effect_depth;
                const double scale = std::sqrt(std::abs(
                    transform.m00 * transform.m11 - transform.m01 * transform.m10
                ));
                output.emplace_back(SubmissionBatch{
                    .kind = SubmissionBatchKind::content_effect_begin,
                    .material = value.effect.id,
                    .scissor = scissor(clip, context),
                    .source_order = source_order,
                    .effect_bounds = transform.bounds(value.bounds),
                    .effect_radii = CornerRadii{
                        value.radii.top_left * scale,
                        value.radii.top_right * scale,
                        value.radii.bottom_right * scale,
                        value.radii.bottom_left * scale,
                    },
                    .effect = value.effect,
                });
            } else if constexpr (std::is_same_v<Type, ContentEffectPopRenderCommand>) {
                if (content_effect_depth == 0U) {
                    throw std::logic_error("render content effect stack underflow");
                }
                --content_effect_depth;
                output.emplace_back(SubmissionBatch{
                    .kind = SubmissionBatchKind::content_effect_end,
                    .scissor = scissor(clip, context),
                    .source_order = source_order,
                });
            } else if constexpr (std::is_same_v<Type, TextRunRenderCommand>) {
                if (!visible_text[index]) {
                    ++skipped_draws;
                    return;
                }
                const PreparedTextCacheEntry& retained = *cache.text[index];
                const std::vector<PreparedTextPtr>& groups = retained.groups;
                if (groups.empty()) {
                    ++skipped_draws;
                    return;
                }
                const Transform positioned = transform.concatenate(Transform{
                    1.0,
                    0.0,
                    value.origin.x - retained.source.origin.x,
                    0.0,
                    1.0,
                    value.origin.y - retained.source.origin.y,
                });
                for (const PreparedTextPtr& group : groups) {
                    append_draw(
                        output, PreparedCommand{group}, source_order, positioned,
                        clip, material_override, context, skipped_draws
                    );
                }
            } else {
                append_draw(
                    output, PreparedCommand{value}, source_order, transform, clip,
                    material_override, context, skipped_draws
                );
            }
        }, source);
    }
    if (!clip_stack.empty() || !transform_stack.empty() || !material_stack.empty() ||
        content_effect_depth != 0U) {
        throw std::logic_error("render command state stacks are unbalanced");
    }
    return output;
}

} // namespace strata::ui::submission_detail
