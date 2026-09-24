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
    if (retained_empty) ++skipped_draws;
    MaterialState material = merged_material(command, material_override);
    // Scope opacity reaches the per-vertex material opacity, not the command's colours.
    material.opacity *= opacity;
    std::optional<std::string> texture = command_texture(command, context);
    output.emplace_back(PreparedDraw{
        source_order,
        std::move(command),
        local_bounds,
        transform,
        std::move(material),
        std::move(texture),
        resolved_scissor,
        rounded_clips,
        false,
        group,
    });
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
                const bool has_rounded_geometry = rounded(safe_radii) && !clip_.empty();
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
                    "straight_alpha",
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
