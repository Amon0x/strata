#include "ui/render/surface_packets.hpp"

#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

#include "runtime/profiler.hpp"
#include "ui/surface.hpp"

namespace strata::ui {
namespace {

/**
 * Shape masks are resampled under rotation and scaled groups, so they are sampled bilinearly, and
 * the backend rasterizes them from their outlines (raster jobs), so no texels stay on the CPU.
 * Animated shapes replace their masks every frame while the rest keep theirs, so the atlas may
 * grow to more pages (made only as needed) than text's: a page frees only once nothing drawn
 * uses it, and too few pages would leave every one holding some mask still in use.
 */
[[nodiscard]] font::GlyphAtlasConfig shape_atlas_config() noexcept {
    font::GlyphAtlasConfig config;
    config.filtered_coverage = true;
    config.backend_rasterized = true;
    config.maximum_pages = 16U;
    return config;
}

} // namespace


SurfacePackets::SurfacePackets(std::string resource_namespace,
                               std::vector<resource::EncodedTextureResource> static_textures)
    : glyph_atlas_(resource_namespace), shape_atlas_(resource_namespace + ".shapes", shape_atlas_config()),
      textures_(resource_namespace, std::move(static_textures)) {
    cache_.set_shape_atlas(&shape_atlas_);
}

const std::vector<std::uint8_t>& SurfacePackets::encode(Surface& surface, const SurfaceFrame& frame,
                                                        const resource::RuntimeImageStore& images) {
    if (release_prepared_) {
        throw std::logic_error("a released Surface cannot encode another packet");
    }
    const auto started = std::chrono::steady_clock::now();
    const bool textures_changed = textures_.sync(images);
    const bool settled = frame.operations.render.nodes_visited == 0U &&
                         frame.operations.render.commands_emitted == 0U && !textures_changed;
    if (!settled || !cache_.reuse(frame.frame_index)) {
        const SurfaceEnvironment& environment = surface.environment();
        static_cast<void>(cache_.encode(surface.render_commands(), frame.frame_index, &textures_,
                                        glyph_atlas_, surface.text_engine(), environment.scale,
                                        environment.framebuffer_width,
                                        environment.framebuffer_height, environment.logical_width,
                                        environment.logical_height));
    }
    const std::int64_t packet_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - started)
                                          .count();

    using runtime::ProfilerCounter;
    const HostRenderPacketTelemetry& packet = cache_.telemetry();
    runtime::Profiler& profiler = surface.profiler();
    profiler.record_counters({
        {ProfilerCounter::packet_bytes, static_cast<std::uint64_t>(this->packet().size())},
        {ProfilerCounter::batch_texture_breaks, packet.texture_batch_breaks},
        {ProfilerCounter::batch_clip_breaks, packet.clip_batch_breaks},
        {ProfilerCounter::batch_material_breaks, packet.material_batch_breaks},
        {ProfilerCounter::batch_effect_breaks, packet.effect_batch_breaks},
        {ProfilerCounter::encoded_geometry_cache_hits, packet.geometry_reused ? 1U : 0U},
        {ProfilerCounter::encoded_geometry_cache_misses, packet.geometry_reused ? 0U : 1U},
        {ProfilerCounter::render_submission_nanos, static_cast<std::uint64_t>(packet_nanos)},
    });
    profiler.record_external_timing("packet-encode", packet_nanos);
    if (!packet.geometry_reused) {
        profiler.record_external_timing("packet-submission", packet.submission_nanos);
        profiler.record_external_timing("packet-submission-plan", packet.submission_planning_nanos);
        profiler.record_external_timing("packet-submission-atlas",
                                        packet.submission_atlas_warmup_nanos);
        profiler.record_external_timing("packet-submission-text",
                                        packet.submission_text_preparation_nanos);
        profiler.record_external_timing("packet-submission-mesh",
                                        packet.submission_mesh_encoding_nanos);
        profiler.record_external_timing("packet-geometry", packet.geometry_packet_nanos);
    }
    if (packet.cold_encode_profiled) {
        profiler.record_external_timing("packet-cold-submission", packet.cold_submission_nanos);
        profiler.record_external_timing("packet-cold-submission-plan",
                                        packet.cold_submission_planning_nanos);
        profiler.record_external_timing("packet-cold-submission-atlas",
                                        packet.cold_submission_atlas_warmup_nanos);
        profiler.record_external_timing("packet-cold-submission-text",
                                        packet.cold_submission_text_preparation_nanos);
        profiler.record_external_timing("packet-cold-submission-mesh",
                                        packet.cold_submission_mesh_encoding_nanos);
        profiler.record_external_timing("packet-cold-geometry", packet.cold_geometry_packet_nanos);
        profiler.record_external_timing("packet-cold-resources", packet.cold_resource_packet_nanos);
    }
    return this->packet();
}

const std::vector<std::uint8_t>& SurfacePackets::prepare_release(Surface& surface) {
    if (!release_prepared_) {
        surface.cancel_interactions();
        const std::uint64_t last_frame = surface.last_frame().frame_index;
        const std::uint64_t release_frame =
            last_frame == std::numeric_limits<std::uint64_t>::max() ? last_frame : last_frame + 1U;
        const std::vector<std::string> resident = textures_.resident();
        static_cast<void>(cache_.prepare_resource_release(release_frame, glyph_atlas_, resident));
        release_prepared_ = true;
    }
    return packet();
}

const std::vector<std::uint8_t>& SurfacePackets::packet() const noexcept {
    return cache_.packet();
}

const HostRenderPacketTelemetry& SurfacePackets::telemetry() const noexcept {
    return cache_.telemetry();
}

} // namespace strata::ui
