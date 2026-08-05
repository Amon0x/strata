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

struct PreparationCache final {
    std::vector<std::optional<PreparedTextCacheEntry>> text;
    std::vector<std::optional<EncodedDrawCacheEntry>> geometry;
    std::vector<std::optional<EncodedDrawPlacement>> placements;
    std::optional<RenderSubmissionEnvironment> geometry_environment;
    std::vector<resource::TextureResourceDescriptor> geometry_textures;
    void clear() noexcept {
        text.clear();
        geometry.clear();
        placements.clear();
        geometry_environment.reset();
        geometry_textures.clear();
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

[[nodiscard]] MaterialState default_material(const PreparedCommand& command);
[[nodiscard]] bool unified_material(std::string_view material) noexcept;
[[nodiscard]] float unified_draw_mode(std::string_view material) noexcept;
[[nodiscard]] bool samples_texture(std::string_view material) noexcept;

} // namespace strata::ui::submission_detail
