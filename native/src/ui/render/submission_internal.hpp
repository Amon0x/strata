#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "font/atlas.hpp"
#include "resource/image.hpp"
#include "ui/render.hpp"
#include "ui/render/submission.hpp"

namespace strata::ui::submission_detail {

struct Transform final {
    double m00 = 1.0;
    double m01 = 0.0;
    double m02 = 0.0;
    double m10 = 0.0;
    double m11 = 1.0;
    double m12 = 0.0;

    [[nodiscard]] Point apply(Point point) const noexcept;
    [[nodiscard]] Rect bounds(Rect rect) const noexcept;
    [[nodiscard]] Transform concatenate(const Transform& next) const noexcept;
    [[nodiscard]] std::optional<Transform> inverse() const noexcept;
    [[nodiscard]] bool axis_aligned_translation() const noexcept;
    [[nodiscard]] friend bool operator==(const Transform&, const Transform&) = default;
};

struct PreparedGlyph final {
    Rect bounds;
    TextureRegion uv;
    double baseline = 0.0;
};

struct PreparedText final {
    Point origin;
    RenderColor color;
    double pixel_size = 12.0;
    std::string texture;
    font::GlyphRasterMode mode = font::GlyphRasterMode::coverage;
    double atlas_pixel_range = 1.0;
    double layout_pixel_range = 1.0;
    std::vector<PreparedGlyph> glyphs;
};

using PreparedTextPtr = std::shared_ptr<const PreparedText>;

using PreparedCommand = std::variant<
    SolidRectRenderCommand,
    RoundedRectRenderCommand,
    BorderRenderCommand,
    ImageRenderCommand,
    NinePatchRenderCommand,
    PreparedTextPtr,
    CustomMeshRenderCommand,
    PathRenderCommand,
    ShadowRenderCommand
>;

struct PreparedDraw final {
    std::uint32_t source_order = 0U;
    PreparedCommand command;
    Rect local_bounds;
    Transform transform;
    MaterialState material;
    std::optional<std::string> texture;
    SubmissionScissor scissor;
    std::vector<SubmissionRoundedClip> rounded_clips;
    bool texture_sampled = false;
    /** Presentation group whose per-frame transform places this draw (vertex z); zero is none. */
    std::uint32_t group = 0U;
    [[nodiscard]] friend bool operator==(const PreparedDraw&, const PreparedDraw&) = default;
};

struct PlannedItem final {
    explicit PlannedItem(PreparedDraw draw)
        : value(std::in_place_type<PreparedDraw>, std::move(draw)) {}
    explicit PlannedItem(SubmissionBatch batch)
        : value(std::in_place_type<SubmissionBatch>, std::move(batch)) {}

    std::variant<PreparedDraw, SubmissionBatch> value;
};

struct SubmissionContext final {
    double scale = 1.0;
    std::int64_t framebuffer_width = 0;
    std::int64_t framebuffer_height = 0;
    double logical_width = 0.0;
    double logical_height = 0.0;
    std::span<const resource::TextureResourceDescriptor> textures;
};

struct PreparedTextCacheEntry final {
    TextRunRenderCommand source;
    const TextEngine* text_engine = nullptr;
    std::uint64_t atlas_generation = 0U;
    double display_scale = 1.0;
    std::vector<PreparedTextPtr> groups;
};

struct EncodedDrawCacheEntry final {
    PreparedDraw source;
    std::vector<std::uint8_t> vertex_bytes;
    std::vector<std::uint32_t> indices;
};

struct EncodedDrawPlacement final {
    std::size_t vertex_byte_offset = 0U;
    std::size_t index_offset = 0U;
    std::uint32_t batch_local_vertex = 0U;
    std::size_t vertex_byte_count = 0U;
    std::size_t index_count = 0U;
    std::size_t vertex_byte_capacity = 0U;
    std::size_t index_capacity = 0U;
    [[nodiscard]] friend bool operator==(
        const EncodedDrawPlacement&,
        const EncodedDrawPlacement&
    ) = default;
};

/**
 * The planner state before a command: enough to plan a balanced range of commands from it again.
 * Clip lists and material overrides are shared between the commands that see the same ones.
 */
struct PlannerState final {
    Transform transform;
    Rect clip;
    std::shared_ptr<const std::vector<SubmissionRoundedClip>> rounded_clips;
    /** Where the rounded clips of the innermost content effect begin. */
    std::size_t rounded_clip_baseline = 0U;
    std::size_t content_effect_depth = 0U;
    std::shared_ptr<const MaterialState> material_override;
    double opacity = 1.0;
    std::uint32_t group = 0U;
    bool in_group = false;
};

/** What planning made of one command: its items and skipped draws, and the state before it. */
struct CommandPlan final {
    std::size_t first_item = 0U;
    std::size_t item_count = 0U;
    std::size_t skipped_draws = 0U;
    PlannerState state;
};

struct PreparationCache final {
    std::vector<std::optional<PreparedTextCacheEntry>> text;
    std::vector<PreparedTextCacheEntry> detached_text;
    std::vector<std::optional<EncodedDrawCacheEntry>> geometry;
    std::vector<EncodedDrawCacheEntry> detached_geometry;
    std::vector<std::optional<EncodedDrawPlacement>> placements;
    std::optional<RenderSubmissionEnvironment> geometry_environment;
    std::vector<resource::TextureResourceDescriptor> geometry_textures;
    /**
     * The last planned command stream, its items and each command's plan. A stream that keeps
     * its shape and changes only some draws is planned and encoded for those draws alone.
     */
    RenderCommandBuffer planned_commands;
    std::vector<PlannedItem> planned_items;
    std::vector<CommandPlan> command_plans;
    std::uint64_t planned_atlas_generation = 0U;
    bool plan_current = false;
    /** Tests compare the fast path against a full plan of the same stream. */
    bool changed_draw_updates = true;
    std::size_t changed_draw_update_count = 0U;
    void clear() noexcept {
        text.clear();
        detached_text.clear();
        geometry.clear();
        detached_geometry.clear();
        placements.clear();
        geometry_environment.reset();
        geometry_textures.clear();
        planned_commands.clear();
        planned_items.clear();
        command_plans.clear();
        planned_atlas_generation = 0U;
        plan_current = false;
    }
};

[[nodiscard]] RenderSubmission build_cached(
    const RenderCommandBuffer& commands,
    font::GlyphAtlas& glyph_atlas,
    const TextEngine* text_engine,
    double display_scale,
    std::int64_t framebuffer_width,
    std::int64_t framebuffer_height,
    double logical_width,
    double logical_height,
    std::span<const resource::TextureResourceDescriptor> textures,
    PreparationCache& cache
);

void update_cached(
    const RenderCommandBuffer& commands,
    font::GlyphAtlas& glyph_atlas,
    const TextEngine* text_engine,
    double display_scale,
    std::int64_t framebuffer_width,
    std::int64_t framebuffer_height,
    double logical_width,
    double logical_height,
    std::span<const resource::TextureResourceDescriptor> textures,
    PreparationCache& cache,
    RenderSubmission& output
);

[[nodiscard]] std::vector<PlannedItem> plan(
    const RenderCommandBuffer& commands,
    font::GlyphAtlas& glyph_atlas,
    const TextEngine* text_engine,
    const SubmissionContext& context,
    RenderSubmission& telemetry,
    PreparationCache& cache
);

void encode(
    const std::vector<PlannedItem>& items,
    const SubmissionContext& context,
    RenderSubmission& output,
    PreparationCache& cache
);

/** A range of commands planned again as a whole: a changed draw, or a changed push to its pop. */
struct CommandRange final {
    std::size_t first = 0U;
    std::size_t last = 0U;
};

/**
 * Plans the given ranges of a stream whose other commands equal the planned one, into their
 * commands' items. Returns the items whose draws changed, or nothing, with the planned items as
 * they were, when a command's items would change in number or kind, an effect batch would
 * change, or text could not be prepared against the current atlas generation.
 */
[[nodiscard]] std::optional<std::vector<std::size_t>> replan_ranges(
    const RenderCommandBuffer& commands,
    std::span<const CommandRange> ranges,
    font::GlyphAtlas& glyph_atlas,
    const TextEngine* text_engine,
    const SubmissionContext& context,
    RenderSubmission& telemetry,
    PreparationCache& cache
);

/**
 * Encodes changed planned draws in place: each keeps its batch and fits its retained slot, so the
 * submission changes by patches alone. False, leaving the cache and submission as they were,
 * otherwise.
 */
[[nodiscard]] bool encode_changed_draws(
    std::span<const std::size_t> changed_items,
    const SubmissionContext& context,
    RenderSubmission& output,
    PreparationCache& cache
);

/** Whether a command only draws: it changes no planner state and makes no effect batch. */
[[nodiscard]] bool draw_command(const RenderCommand& command) noexcept;
/** +1 for a command that opens a scope (clip, transform, material, opacity, group or content
 * effect), -1 for one that closes it, 0 otherwise. */
[[nodiscard]] int scope_step(const RenderCommand& command) noexcept;

[[nodiscard]] MaterialState default_material(const PreparedCommand& command);
[[nodiscard]] bool unified_material(std::string_view material) noexcept;
[[nodiscard]] float unified_draw_mode(std::string_view material) noexcept;
[[nodiscard]] bool samples_texture(std::string_view material) noexcept;

} // namespace strata::ui::submission_detail
