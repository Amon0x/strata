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
namespace {

/**
 * Updates the submission for a stream that keeps the planned stream's shape: the same commands
 * except a few draws, so every other command's items, placements and batches stand. False,
 * leaving the full plan to run, when the stream or the text atlas moved on in any other way.
 */
[[nodiscard]] bool update_changed_draws(
    const RenderCommandBuffer& commands,
    font::GlyphAtlas& glyph_atlas,
    const TextEngine* const text_engine,
    const SubmissionContext& context,
    PreparationCache& cache,
    RenderSubmission& output
) {
    if (!cache.changed_draw_updates || !cache.plan_current) return false;
    const std::vector<RenderCommand>& previous = cache.planned_commands.commands();
    const std::vector<RenderCommand>& current = commands.commands();
    if (&previous == &current || previous.size() != current.size() ||
        cache.command_plans.size() != current.size() || cache.text.size() != current.size()) {
        return false;
    }
    if (text_engine != nullptr && (glyph_atlas.reclamation_pending() ||
                                   glyph_atlas.generation() != cache.planned_atlas_generation)) {
        return false;
    }
    // A changed draw is planned again alone; a changed scope (a moved subtree's transform, a
    // resized clip) with everything up to its end. Either way only the changed draws are encoded.
    std::vector<CommandRange> ranges;
    std::size_t covered = 0U;
    for (std::size_t index = 0U; index < current.size(); ++index) {
        if (current[index] == previous[index]) continue;
        if (current[index].index() != previous[index].index()) return false;
        if (index < covered) continue;
        CommandRange range{index, index};
        if (!draw_command(current[index])) {
            if (scope_step(current[index]) != 1) return false;
            int depth = 0;
            std::size_t last = index;
            for (; last < current.size(); ++last) {
                depth += scope_step(current[last]);
                if (depth == 0) break;
            }
            if (last == current.size()) return false;
            range.last = last;
        }
        ranges.push_back(range);
        covered = range.last + 1U;
    }
    if (ranges.empty()) return false;

    output.planning_nanos = 0;
    output.atlas_warmup_nanos = 0;
    output.text_preparation_nanos = 0;
    output.mesh_encoding_nanos = 0;
    const std::size_t previous_full_geometry_bytes =
        output.vertex_bytes.size() + output.indices.size() * sizeof(std::uint32_t);
    const auto planning_started = std::chrono::steady_clock::now();
    const std::optional<std::vector<std::size_t>> changed_items =
        replan_ranges(commands, ranges, glyph_atlas, text_engine, context, output, cache);
    if (!changed_items.has_value()) return false;
    output.planning_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - planning_started
    ).count();
    const auto encoding_started = std::chrono::steady_clock::now();
    output.vertex_patches.clear();
    output.index_patches.clear();
    if (encode_changed_draws(*changed_items, context, output, cache)) {
        output.topology_change = SubmissionTopologyChange::none;
        output.topology_change_item = 0U;
    } else {
        // A draw changed batch (a moved subtree's scissors) or outgrew its slot: encode the
        // items again, as a full update would after its plan.
        output.planned_draws = 0U;
        output.texture_batch_breaks = 0U;
        output.clip_batch_breaks = 0U;
        output.material_batch_breaks = 0U;
        output.effect_batch_breaks = 0U;
        output.geometry_topology_reused = false;
        output.candidate_geometry_patch_bytes = 0U;
        output.full_geometry_bytes = 0U;
        output.topology_change = SubmissionTopologyChange::none;
        output.topology_change_item = 0U;
        output.previous_item_count = 0U;
        output.item_count = 0U;
        output.patch_from_previous = false;
        encode(cache.planned_items, context, output, cache);
    }
    output.mesh_encoding_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - encoding_started
    ).count();
    output.previous_full_geometry_bytes = previous_full_geometry_bytes;
    cache.planned_commands = commands;
    cache.planned_atlas_generation = glyph_atlas.generation();
    ++cache.changed_draw_update_count;
    return true;
}

} // namespace

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
        cache.plan_current = false;
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
    if (update_changed_draws(commands, glyph_atlas, text_engine, context, cache, output)) return;
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
    cache.planned_commands = commands;
    cache.planned_items = std::move(items);
    cache.planned_atlas_generation = glyph_atlas.generation();
    cache.plan_current = true;
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
