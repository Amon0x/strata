#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "font/atlas.hpp"
#include "resource/image.hpp"
#include "ui/render/submission.hpp"
#include "ui/render/surface_textures.hpp"

namespace strata::ui {

struct HostRenderPacketTelemetry final {
    std::size_t vertices = 0U;
    std::size_t batches = 0U;
    std::size_t texture_batch_breaks = 0U;
    std::size_t clip_batch_breaks = 0U;
    std::size_t material_batch_breaks = 0U;
    std::size_t effect_batch_breaks = 0U;
    bool geometry_reused = false;
    bool cold_encode_profiled = false;
    std::int64_t cold_submission_nanos = 0;
    std::int64_t cold_submission_planning_nanos = 0;
    std::int64_t cold_submission_atlas_warmup_nanos = 0;
    std::int64_t cold_submission_text_preparation_nanos = 0;
    std::int64_t cold_submission_mesh_encoding_nanos = 0;
    std::int64_t cold_geometry_packet_nanos = 0;
    std::int64_t cold_resource_packet_nanos = 0;
    std::int64_t submission_nanos = 0;
    std::int64_t submission_planning_nanos = 0;
    std::int64_t submission_atlas_warmup_nanos = 0;
    std::int64_t submission_text_preparation_nanos = 0;
    std::int64_t submission_mesh_encoding_nanos = 0;
    std::int64_t geometry_packet_nanos = 0;
    bool geometry_patched = false;
    std::size_t geometry_patch_bytes = 0U;
    bool geometry_topology_reused = false;
    std::size_t candidate_geometry_patch_bytes = 0U;
    std::size_t previous_full_geometry_bytes = 0U;
    std::size_t full_geometry_bytes = 0U;
    SubmissionTopologyChange topology_change = SubmissionTopologyChange::none;
    std::size_t topology_change_item = 0U;
    std::size_t previous_item_count = 0U;
    std::size_t item_count = 0U;
};

class RenderCommandBuffer;
class TextEngine;

/** Little-endian logical-command packet v3 used by command-stream inspection/tests. */
[[nodiscard]] std::vector<std::uint8_t> encode_render_packet(const RenderCommandBuffer& commands,
                                                             std::uint64_t frame_index);

} // namespace strata::ui

namespace strata::ui {

/**
 * Packet v12: retained geometry epochs, incremental geometry patches, GPU presentation groups,
 * rate-limited effects, explicit current/surface backdrop sources, ordered application effect
 * programs, and rounded descendant masks. The logical v3 encoder remains available only to command-stream inspection tooling.
 */
class HostRenderPacketCache final {
  public:
    HostRenderPacketCache() = default;
    HostRenderPacketCache(const HostRenderPacketCache&) = delete;
    HostRenderPacketCache& operator=(const HostRenderPacketCache&) = delete;
    HostRenderPacketCache(HostRenderPacketCache&&) = delete;
    HostRenderPacketCache& operator=(HostRenderPacketCache&&) = delete;

    /**
     * A null TextEngine selects the packet-v12 non-text path; text runs are then rejected. Null
     * textures means the stream draws no raster images.
     */
    [[nodiscard]] const std::vector<std::uint8_t>&
    encode(const RenderCommandBuffer& commands, std::uint64_t frame_index,
           SurfaceTextures* textures, font::GlyphAtlas& glyph_atlas, const TextEngine* text_engine,
           double display_scale, std::int64_t framebuffer_width, std::int64_t framebuffer_height,
           double logical_width, double logical_height);
    /** Compatibility overload for text-backed surfaces. */
    [[nodiscard]] const std::vector<std::uint8_t>&
    encode(const RenderCommandBuffer& commands, std::uint64_t frame_index,
           SurfaceTextures* textures, font::GlyphAtlas& glyph_atlas, const TextEngine& text_engine,
           double display_scale, std::int64_t framebuffer_width, std::int64_t framebuffer_height,
           double logical_width, double logical_height);
    /** Emits a compact packet referencing the settled geometry epoch. */
    [[nodiscard]] bool reuse(std::uint64_t frame_index);
    /**
     * Encodes terminal releases for the glyph atlas and every texture the host holds. The encoded
     * packet is retained before either resource set is committed/drained.
     */
    [[nodiscard]] const std::vector<std::uint8_t>&
    prepare_resource_release(std::uint64_t frame_index, font::GlyphAtlas& glyph_atlas,
                             std::span<const std::string> textures);
    void clear() noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>& packet() const noexcept;
    [[nodiscard]] const HostRenderPacketTelemetry& telemetry() const noexcept;

  private:
    RenderSubmissionCache submission_cache_;
    std::vector<std::uint8_t> geometry_packet_;
    std::vector<std::uint8_t> reuse_packet_;
    std::vector<std::uint8_t> resource_packet_;
    const std::vector<std::uint8_t>* current_packet_ = &geometry_packet_;
    // World-composed presentation groups of the latest frame; every packet carries them.
    std::vector<RenderGroup> groups_;
    HostRenderPacketTelemetry telemetry_;
    std::size_t planned_draws_ = 0U;
    std::size_t skipped_draws_ = 0U;
    // A failed packet encode must be retried even if the retained UI becomes settled meanwhile.
    bool frame_encoding_incomplete_ = false;
    bool terminal_release_prepared_ = false;
    std::uint64_t geometry_epoch_ = 0U;
};

} // namespace strata::ui
