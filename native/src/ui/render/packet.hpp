#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "font/atlas.hpp"
#include "resource/image.hpp"
#include "ui/render/submission.hpp"

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

/** Fully allocated releases for raster images already published to a host. */
struct HostRenderResourceInvalidationPlan final {
    std::vector<font::AtlasOperation> releases;
};

/**
 * Packet v10: retained geometry epochs, incremental geometry patches, rate-limited effects,
 * explicit current/surface backdrop sources, ordered application effect programs, and rounded
 * descendant masks. The
 * logical v3 encoder remains available only to command-stream inspection tooling.
 */
class HostRenderPacketCache final {
  public:
    HostRenderPacketCache() = default;
    HostRenderPacketCache(const HostRenderPacketCache&) = delete;
    HostRenderPacketCache& operator=(const HostRenderPacketCache&) = delete;
    HostRenderPacketCache(HostRenderPacketCache&&) = delete;
    HostRenderPacketCache& operator=(HostRenderPacketCache&&) = delete;

    /** A null TextEngine selects the packet-v10 non-text path; text runs are then rejected. */
    [[nodiscard]] const std::vector<std::uint8_t>&
    encode(const RenderCommandBuffer& commands, std::uint64_t frame_index,
           std::span<const resource::EncodedTextureResource> texture_resources,
           font::GlyphAtlas& glyph_atlas, const TextEngine* text_engine, double display_scale,
           std::int64_t framebuffer_width, std::int64_t framebuffer_height, double logical_width,
           double logical_height);
    /** Compatibility overload for text-backed surfaces. */
    [[nodiscard]] const std::vector<std::uint8_t>&
    encode(const RenderCommandBuffer& commands, std::uint64_t frame_index,
           std::span<const resource::EncodedTextureResource> texture_resources,
           font::GlyphAtlas& glyph_atlas, const TextEngine& text_engine, double display_scale,
           std::int64_t framebuffer_width, std::int64_t framebuffer_height, double logical_width,
           double logical_height);
    /** Emits a compact packet referencing the settled geometry epoch. */
    [[nodiscard]] bool reuse(std::uint64_t frame_index);
    /** Plans release records for raster images already published by this cache. */
    [[nodiscard]] HostRenderResourceInvalidationPlan plan_resource_invalidation() const;
    /** Clears retained geometry and queues a prepared release plan for the next frame packet. */
    void commit_resource_invalidation(HostRenderResourceInvalidationPlan plan) noexcept;
    /**
     * Compatibility terminal entry point. It releases the surface-owned glyph atlas and every
     * static texture descriptor already retained by this cache. Call prepare_resource_release()
     * when teardown must also cover textures that have never reached a frame.
     */
    [[nodiscard]] const std::vector<std::uint8_t>&
    prepare_atlas_release(std::uint64_t frame_index, font::GlyphAtlas& glyph_atlas);
    /**
     * Encodes terminal releases for every supplied surface-owned static texture together with the
     * atlas. The encoded packet is retained before either resource set is committed/drained.
     */
    [[nodiscard]] const std::vector<std::uint8_t>&
    prepare_resource_release(std::uint64_t frame_index, font::GlyphAtlas& glyph_atlas,
                             std::span<const resource::EncodedTextureResource> static_textures);
    void clear() noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>& packet() const noexcept;
    [[nodiscard]] const HostRenderPacketTelemetry& telemetry() const noexcept;

  private:
    RenderSubmissionCache submission_cache_;
    // Submission planning still needs the complete descriptor table after one-shot encoded
    // resource payloads have been consumed by the host.
    std::vector<resource::TextureResourceDescriptor> texture_descriptors_;
    std::vector<std::uint8_t> geometry_packet_;
    std::vector<std::uint8_t> reuse_packet_;
    std::vector<std::uint8_t> resource_packet_;
    const std::vector<std::uint8_t>* current_packet_ = &geometry_packet_;
    HostRenderPacketTelemetry telemetry_;
    std::size_t planned_draws_ = 0U;
    std::size_t skipped_draws_ = 0U;
    std::vector<font::AtlasOperation> pending_static_releases_;
    // A failed packet encode must be retried even if the retained UI becomes settled meanwhile.
    bool frame_encoding_incomplete_ = false;
    bool terminal_release_prepared_ = false;
    std::uint64_t geometry_epoch_ = 0U;
};

} // namespace strata::ui
