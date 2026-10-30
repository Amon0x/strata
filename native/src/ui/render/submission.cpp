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

SubmissionBlend submission_blend(const std::string_view name) noexcept {
    if (name == "opaque")
        return SubmissionBlend::opaque;
    if (name == "premultiplied_alpha")
        return SubmissionBlend::premultiplied_alpha;
    if (name == "additive")
        return SubmissionBlend::additive;
    if (name == "multiply")
        return SubmissionBlend::multiply;
    if (name == "subpixel_text")
        return SubmissionBlend::subpixel_text;
    return SubmissionBlend::straight_alpha;
}

namespace submission_detail {
namespace {

/**
 * Updates the submission for a stream that keeps the planned stream's shape: the same commands
 * except a few draws, so every other command's items, placements and batches stand. False,
 * leaving the full plan to run, when the stream or the text atlas moved on in any other way.
 */
/**
 * Tells the shape atlas which pages the planned draws use, leaving out the items of `skipped`
 * command ranges: their draws are planned again, so their old masks may go.
 */
void retain_drawn_pages(PreparationCache& cache, const std::span<const CommandRange> skipped) {
    std::vector<std::string> drawn;
    const auto retain = [&](const std::size_t first, const std::size_t last) {
        for (std::size_t index = first; index < last; ++index) {
            const auto* draw = std::get_if<PreparedDraw>(&cache.planned_items[index].value);
            if (draw == nullptr)
                continue;
            if (const auto* mask = std::get_if<PreparedMaskPtr>(&draw->command);
                mask != nullptr && *mask != nullptr &&
                std::ranges::find(drawn, (*mask)->texture) == drawn.end()) {
                drawn.push_back((*mask)->texture);
            }
        }
    };
    // Animated shapes' slots stay too: their draws are replanned into them this frame.
    for (const auto& [index, shape] : cache.volatile_shapes) {
        for (const std::optional<font::RasterSlot>& slot : shape.slots) {
            if (slot.has_value() && std::ranges::find(drawn, slot->texture) == drawn.end())
                drawn.push_back(slot->texture);
        }
    }
    std::size_t cursor = 0U;
    for (const CommandRange& range : skipped) {
        const CommandPlan& first = cache.command_plans[range.first];
        const CommandPlan& last = cache.command_plans[range.last];
        retain(cursor, first.first_item);
        cursor = last.first_item + last.item_count;
    }
    retain(cursor, cache.planned_items.size());
    cache.shape_atlas->retain_pages(drawn);
}

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
    if (cache.shape_atlas != nullptr &&
        (cache.shape_atlas->reclamation_pending() ||
         cache.shape_atlas->generation() != cache.planned_shape_generation)) {
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
    // A path draw replanned in consecutive updates is animated: it rasterizes into a slot of its
    // own (see PreparationCache::volatile_shapes). Ones that stopped long ago let theirs go.
    const std::uint64_t serial = ++cache.update_serial;
    for (const CommandRange& range : ranges) {
        if (range.first != range.last ||
            !std::holds_alternative<PathRenderCommand>(current[range.first])) {
            continue;
        }
        PreparationCache::VolatileShape& shape =
            cache.volatile_shapes[static_cast<std::uint32_t>(range.first)];
        shape.previously_changed = shape.changed;
        shape.changed = serial;
    }
    constexpr std::uint64_t volatile_lifetime = 120U;
    std::erase_if(cache.volatile_shapes, [serial](const auto& entry) {
        return entry.second.changed + volatile_lifetime < serial;
    });
    // The changed draws' masks are about to be replaced: only the unchanged draws' pages stay.
    if (cache.shape_atlas != nullptr)
        retain_drawn_pages(cache, ranges);

    output.planning_nanos = 0;
    output.atlas_warmup_nanos = 0;
    output.text_preparation_nanos = 0;
    output.mesh_encoding_nanos = 0;
    const std::size_t previous_full_geometry_bytes =
        output.vertex_bytes.size() + output.indices.size() * sizeof(std::uint32_t);
    const auto planning_started = std::chrono::steady_clock::now();
    const std::uint64_t shape_generation =
        cache.shape_atlas != nullptr ? cache.shape_atlas->generation() : 0U;
    bool resized = false;
    const std::optional<std::vector<std::size_t>> changed_items = replan_ranges(
        commands, ranges, glyph_atlas, text_engine, context, output, cache, resized);
    if (!changed_items.has_value()) return false;
    // A shape atlas that recycled while these draws planned released every mask the unchanged
    // draws use too: the whole stream plans again.
    if (cache.shape_atlas != nullptr && cache.shape_atlas->generation() != shape_generation)
        return false;
    output.planning_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - planning_started
    ).count();
    const auto encoding_started = std::chrono::steady_clock::now();
    output.vertex_patches.clear();
    output.index_patches.clear();
    if (!resized && encode_changed_draws(*changed_items, context, output, cache)) {
        output.topology_change = SubmissionTopologyChange::none;
        output.topology_change_item = 0U;
    } else {
        // A draw changed batch (a moved subtree's scissors), outgrew its slot or changed its
        // number of items: encode the items again, as a full update would after its plan.
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
    if (cache.shape_atlas != nullptr)
        cache.planned_shape_generation = cache.shape_atlas->generation();
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
        cache.text_subpixel,
    };
    const RenderSubmissionEnvironment geometry_environment{
        display_scale,
        framebuffer_width,
        framebuffer_height,
        logical_width,
        logical_height,
    };
    const bool textures_changed = !std::ranges::equal(cache.geometry_textures, textures);
    if (textures_changed)
        validate_texture_descriptors(textures);
    if (cache.geometry_environment != geometry_environment || textures_changed) {
        cache.geometry.clear();
        cache.geometry_environment = geometry_environment;
        cache.geometry_textures.assign(textures.begin(), textures.end());
        cache.plan_current = false;
    }
    struct PreparationGuard final {
        font::GlyphAtlas* atlas = nullptr;
        font::GlyphAtlas* shapes = nullptr;
        ~PreparationGuard() {
            if (atlas != nullptr) atlas->end_frame_preparation();
            if (shapes != nullptr) shapes->end_frame_preparation();
        }
    };
    PreparationGuard preparation;
    if (text_engine != nullptr) {
        static_cast<void>(glyph_atlas.begin_frame_preparation());
        preparation.atlas = &glyph_atlas;
    }
    if (cache.shape_atlas != nullptr) {
        cache.shape_atlas->adopt_display_scale(display_scale);
        if (cache.shape_atlas->begin_frame_preparation())
            cache.plan_current = false;
        preparation.shapes = cache.shape_atlas;
        // The pages the current plan draws from stay; a full atlas reuses another page rather
        // than recycling them all, which would plan every draw again.
        retain_drawn_pages(cache, {});
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
    const std::uint64_t shape_generation =
        cache.shape_atlas != nullptr ? cache.shape_atlas->generation() : 0U;
    std::vector<PlannedItem> items = plan(
        commands, glyph_atlas, text_engine, context, output, cache
    );
    // A shape atlas that filled and recycled mid-plan released the masks placed before it: plan
    // again on its fresh pages. It recycles at most once a frame, so this plans at most twice.
    if (cache.shape_atlas != nullptr && cache.shape_atlas->generation() != shape_generation) {
        output.skipped_draws = 0U;
        items = plan(commands, glyph_atlas, text_engine, context, output, cache);
    }
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
    if (cache.shape_atlas != nullptr)
        cache.planned_shape_generation = cache.shape_atlas->generation();
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
