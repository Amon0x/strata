#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ui/layout.hpp"
#include "ui/render.hpp"
#include "resource/image.hpp"

namespace strata::font {
class GlyphAtlas;
}

namespace strata::ui {

class TextEngine;
namespace submission_detail {
struct PreparationCache;
}

struct SubmissionScissor final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    [[nodiscard]] friend bool operator==(const SubmissionScissor&, const SubmissionScissor&) = default;
};

inline constexpr std::size_t maximum_rounded_clip_depth = 16U;

/**
 * One rounded clip in its authored coordinate space. The inverse transform maps presented
 * logical pixels back into that space, preserving corners under scale, reflection and rotation.
 */
struct SubmissionRoundedClip final {
    Rect bounds;
    CornerRadii radii;
    std::array<double, 6U> inverse_transform{1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    [[nodiscard]] friend bool operator==(const SubmissionRoundedClip&,
                                         const SubmissionRoundedClip&) = default;
};

enum class SubmissionBatchKind : std::uint32_t {
    draw = 0U,
    blur = 1U,
    backdrop_effect = 2U,
    content_effect_begin = 3U,
    content_effect_end = 4U,
};

struct SubmissionBatch final {
    SubmissionBatchKind kind = SubmissionBatchKind::draw;
    std::string material;
    std::string blend_mode = "straight_alpha";
    std::optional<std::string> texture;
    SubmissionScissor scissor;
    std::uint32_t base_vertex = 0U;
    std::uint32_t first_index = 0U;
    std::uint32_t index_count = 0U;
    std::uint32_t source_order = 0U;
    Rect effect_bounds;
    double effect_radius = 0.0;
    std::uint32_t effect_downsample = 1U;
    CornerRadii effect_radii;
    std::optional<EffectState> effect;
    std::vector<SubmissionRoundedClip> rounded_clips;
    [[nodiscard]] friend bool operator==(const SubmissionBatch&, const SubmissionBatch&) = default;
};

/** One aligned byte range replacing retained geometry from the preceding submission. */
struct SubmissionGeometryPatch final {
    std::uint32_t offset = 0U;
    std::vector<std::uint8_t> bytes;
};

enum class SubmissionTopologyChange : std::uint32_t {
    none = 0U,
    item_count = 1U,
    effect_placement = 2U,
    missing_placement = 3U,
    vertex_offset = 4U,
    index_offset = 5U,
    batch_local_vertex = 6U,
    vertex_capacity = 7U,
    index_capacity = 8U,
    buffer_size = 9U,
};

struct RenderSubmission final {
    std::vector<std::uint8_t> vertex_bytes;
    std::vector<std::uint32_t> indices;
    std::size_t used_vertex_bytes = 0U;
    std::size_t used_indices = 0U;
    std::vector<SubmissionBatch> batches;
    std::size_t planned_draws = 0U;
    std::size_t skipped_draws = 0U;
    std::size_t texture_batch_breaks = 0U;
    std::size_t clip_batch_breaks = 0U;
    std::size_t material_batch_breaks = 0U;
    std::size_t effect_batch_breaks = 0U;
    std::int64_t planning_nanos = 0;
    std::int64_t atlas_warmup_nanos = 0;
    std::int64_t text_preparation_nanos = 0;
    std::int64_t mesh_encoding_nanos = 0;
    bool geometry_topology_reused = false;
    std::size_t candidate_geometry_patch_bytes = 0U;
    std::size_t previous_full_geometry_bytes = 0U;
    std::size_t full_geometry_bytes = 0U;
    SubmissionTopologyChange topology_change = SubmissionTopologyChange::none;
    std::size_t topology_change_item = 0U;
    std::size_t previous_item_count = 0U;
    std::size_t item_count = 0U;
    bool patch_from_previous = false;
    std::vector<SubmissionGeometryPatch> vertex_patches;
    std::vector<SubmissionGeometryPatch> index_patches;
};

struct RenderSubmissionEnvironment final {
    double display_scale = 1.0;
    std::int64_t framebuffer_width = 0;
    std::int64_t framebuffer_height = 0;
    double logical_width = 0.0;
    double logical_height = 0.0;
    [[nodiscard]] friend bool operator==(
        const RenderSubmissionEnvironment&,
        const RenderSubmissionEnvironment&
    ) = default;
};

/** Surface-owned geometry cache. Cache hits perform no planning, glyph request, or mesh encoding. */
class RenderSubmissionCache final {
public:
    RenderSubmissionCache();
    ~RenderSubmissionCache();
    RenderSubmissionCache(const RenderSubmissionCache&) = delete;
    RenderSubmissionCache& operator=(const RenderSubmissionCache&) = delete;
    RenderSubmissionCache(RenderSubmissionCache&&) = delete;
    RenderSubmissionCache& operator=(RenderSubmissionCache&&) = delete;

    /** A null TextEngine is valid only when the command buffer contains no text runs. */
    [[nodiscard]] const RenderSubmission& resolve(
        const RenderCommandBuffer& commands,
        font::GlyphAtlas& glyph_atlas,
        const TextEngine* text_engine,
        const RenderSubmissionEnvironment& environment,
        std::span<const resource::TextureResourceDescriptor> textures = {}
    );
    /** Compatibility overload for text-backed surfaces. */
    [[nodiscard]] const RenderSubmission& resolve(
        const RenderCommandBuffer& commands,
        font::GlyphAtlas& glyph_atlas,
        const TextEngine& text_engine,
        const RenderSubmissionEnvironment& environment,
        std::span<const resource::TextureResourceDescriptor> textures = {}
    );
    void clear() noexcept;
    [[nodiscard]] std::size_t hit_count() const noexcept;
    [[nodiscard]] std::size_t miss_count() const noexcept;

private:
    RenderCommandBuffer commands_;
    std::optional<RenderSubmissionEnvironment> environment_;
    std::vector<resource::TextureResourceDescriptor> texture_descriptors_;
    const font::GlyphAtlas* glyph_atlas_ = nullptr;
    std::uint64_t glyph_atlas_generation_ = 0U;
    const TextEngine* text_engine_ = nullptr;
    std::optional<RenderSubmission> submission_;
    std::unique_ptr<submission_detail::PreparationCache> preparation_cache_;
    std::size_t hits_ = 0U;
    std::size_t misses_ = 0U;
};

/**
 * Plans, batches and encodes backend-ready geometry entirely inside the native core. The host only
 * owns GPU resource creation, buffer upload and draw/effect submission.
 */
[[nodiscard]] RenderSubmission build_render_submission(
    const RenderCommandBuffer& commands,
    font::GlyphAtlas& glyph_atlas,
    const TextEngine* text_engine,
    double display_scale,
    std::int64_t framebuffer_width,
    std::int64_t framebuffer_height,
    double logical_width,
    double logical_height,
    std::span<const resource::TextureResourceDescriptor> textures = {}
);
/** Compatibility overload for text-backed surfaces. */
[[nodiscard]] RenderSubmission build_render_submission(
    const RenderCommandBuffer& commands,
    font::GlyphAtlas& glyph_atlas,
    const TextEngine& text_engine,
    double display_scale,
    std::int64_t framebuffer_width,
    std::int64_t framebuffer_height,
    double logical_width,
    double logical_height,
    std::span<const resource::TextureResourceDescriptor> textures = {}
);

} // namespace strata::ui
