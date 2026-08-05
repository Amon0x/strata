#include "ui/render/submission.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <variant>

#include "font/atlas.hpp"
#include "ui/render/submission_internal.hpp"
#include "ui/text.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] bool requires_text_engine(const RenderCommandBuffer& commands) noexcept {
    for (const RenderCommand& command : commands.commands()) {
        if (std::holds_alternative<TextRunRenderCommand>(command)) return true;
    }
    return false;
}

void validate_texture_descriptors(
    const std::span<const resource::TextureResourceDescriptor> textures
) {
    for (std::size_t index = 0U; index < textures.size(); ++index) {
        const resource::TextureResourceDescriptor& texture = textures[index];
        if (texture.logical_id.empty() || texture.host_id.empty()) {
            throw std::invalid_argument(
                "render texture descriptor requires non-empty logical and surface host ids"
            );
        }
        for (std::size_t prior = 0U; prior < index; ++prior) {
            if (textures[prior].logical_id == texture.logical_id) {
                throw std::invalid_argument("render texture logical ids must be unique");
            }
            if (textures[prior].host_id == texture.host_id) {
                throw std::invalid_argument("render texture surface host ids must be unique");
            }
        }
    }
}

} // namespace

namespace submission_detail {

void update_cached(
    const RenderCommandBuffer& commands,
    font::GlyphAtlas& glyph_atlas,
    const TextEngine* const text_engine,
    const double display_scale,
    const std::int64_t framebuffer_width,
    const std::int64_t framebuffer_height,
    const double logical_width,
    const double logical_height,
    const std::span<const resource::TextureResourceDescriptor> textures,
    PreparationCache& cache,
    RenderSubmission& output
) {
    if (!std::isfinite(display_scale) || display_scale <= 0.0 || framebuffer_width < 0 ||
        framebuffer_height < 0 || !std::isfinite(logical_width) || logical_width < 0.0 ||
        !std::isfinite(logical_height) || logical_height < 0.0 ||
        framebuffer_width > static_cast<std::int64_t>(UINT32_MAX) ||
        framebuffer_height > static_cast<std::int64_t>(UINT32_MAX)) {
        throw std::invalid_argument("render submission environment is invalid");
    }
    validate_texture_descriptors(textures);
    const bool text_required = requires_text_engine(commands);
    if (text_required && text_engine == nullptr) {
        throw std::invalid_argument(
            "render submission contains text commands but has no TextEngine"
        );
    }
    const SubmissionContext context{
        display_scale,
        framebuffer_width,
        framebuffer_height,
        logical_width,
        logical_height,
        textures,
    };
    const RenderSubmissionEnvironment geometry_environment{
        display_scale,
        framebuffer_width,
        framebuffer_height,
        logical_width,
        logical_height,
    };
    if (cache.geometry_environment != geometry_environment ||
        !std::ranges::equal(cache.geometry_textures, textures)) {
        cache.geometry.clear();
        cache.geometry_environment = geometry_environment;
        cache.geometry_textures.assign(textures.begin(), textures.end());
    }
    struct PreparationGuard final {
        font::GlyphAtlas* atlas = nullptr;
        ~PreparationGuard() { if (atlas != nullptr) atlas->end_frame_preparation(); }
    };
    PreparationGuard preparation;
    if (text_engine != nullptr) {
        static_cast<void>(glyph_atlas.begin_frame_preparation());
        preparation.atlas = &glyph_atlas;
    }
    output.planned_draws = 0U;
    output.skipped_draws = 0U;
    output.texture_batch_breaks = 0U;
    output.clip_batch_breaks = 0U;
    output.material_batch_breaks = 0U;
    output.effect_batch_breaks = 0U;
    output.planning_nanos = 0;
    output.atlas_warmup_nanos = 0;
    output.text_preparation_nanos = 0;
    output.mesh_encoding_nanos = 0;
    output.geometry_topology_reused = false;
    output.candidate_geometry_patch_bytes = 0U;
    output.previous_full_geometry_bytes =
        output.vertex_bytes.size() +
        output.indices.size() * sizeof(std::uint32_t);
    output.full_geometry_bytes = 0U;
    output.topology_change = SubmissionTopologyChange::none;
    output.topology_change_item = 0U;
    output.previous_item_count = 0U;
    output.item_count = 0U;
    output.patch_from_previous = false;
    output.vertex_patches.clear();
    output.index_patches.clear();
    const auto planning_started = std::chrono::steady_clock::now();
    std::vector<PlannedItem> items = plan(
        commands, glyph_atlas, text_engine, context, output, cache
    );
    output.planning_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - planning_started
    ).count();
    const auto encoding_started = std::chrono::steady_clock::now();
    encode(items, context, output, cache);
    output.mesh_encoding_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - encoding_started
    ).count();
}

RenderSubmission build_cached(
    const RenderCommandBuffer& commands,
    font::GlyphAtlas& glyph_atlas,
    const TextEngine* const text_engine,
    const double display_scale,
    const std::int64_t framebuffer_width,
    const std::int64_t framebuffer_height,
    const double logical_width,
    const double logical_height,
    const std::span<const resource::TextureResourceDescriptor> textures,
    PreparationCache& cache
) {
    RenderSubmission output;
    update_cached(
        commands,
        glyph_atlas,
        text_engine,
        display_scale,
        framebuffer_width,
        framebuffer_height,
        logical_width,
        logical_height,
        textures,
        cache,
        output
    );
    return output;
}

} // namespace submission_detail

RenderSubmission build_render_submission(
    const RenderCommandBuffer& commands,
    font::GlyphAtlas& glyph_atlas,
    const TextEngine* const text_engine,
    const double display_scale,
    const std::int64_t framebuffer_width,
    const std::int64_t framebuffer_height,
    const double logical_width,
    const double logical_height,
    const std::span<const resource::TextureResourceDescriptor> textures
) {
    submission_detail::PreparationCache cache;
    return submission_detail::build_cached(
        commands,
        glyph_atlas,
        text_engine,
        display_scale,
        framebuffer_width,
        framebuffer_height,
        logical_width,
        logical_height,
        textures,
        cache
    );
}

RenderSubmission build_render_submission(
    const RenderCommandBuffer& commands,
    font::GlyphAtlas& glyph_atlas,
    const TextEngine& text_engine,
    const double display_scale,
    const std::int64_t framebuffer_width,
    const std::int64_t framebuffer_height,
    const double logical_width,
    const double logical_height,
    const std::span<const resource::TextureResourceDescriptor> textures
) {
    return build_render_submission(
        commands,
        glyph_atlas,
        &text_engine,
        display_scale,
        framebuffer_width,
        framebuffer_height,
        logical_width,
        logical_height,
        textures
    );
}

} // namespace strata::ui
