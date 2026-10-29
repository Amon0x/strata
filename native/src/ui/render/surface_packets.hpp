#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "font/atlas.hpp"
#include "resource/image.hpp"
#include "resource/runtime_images.hpp"
#include "ui/render/packet.hpp"
#include "ui/render/surface_textures.hpp"

namespace strata::ui {

class Surface;
struct SurfaceFrame;

/**
 * The host-facing output of one Surface: its glyph atlas, the textures the host holds, and the
 * packet a host renders. Each frame turns into exactly one packet here, and the terminal packet
 * releasing every host resource is prepared here too, so a host only decodes what this produces.
 */
class SurfacePackets final {
  public:
    /**
     * `resource_namespace` prefixes every host resource id so ids from two Surfaces never collide;
     * `static_textures` are the Surface's own encoded images.
     */
    SurfacePackets(std::string resource_namespace,
                   std::vector<resource::EncodedTextureResource> static_textures);
    SurfacePackets(const SurfacePackets&) = delete;
    SurfacePackets& operator=(const SurfacePackets&) = delete;

    /**
     * Encodes `frame` of `surface` against the runtime's published `images`. A settled frame with no
     * texture change re-sends the retained geometry epoch instead of encoding it again. Packet size,
     * batch breaks, cache hits and encode timings are recorded in the Surface's profiler.
     */
    const std::vector<std::uint8_t>& encode(Surface& surface, const SurfaceFrame& frame,
                                            const resource::RuntimeImageStore& images);
    /**
     * Prepares the terminal packet releasing the glyph atlas and every texture the host holds, and
     * cancels the Surface's interactions. Repeating it returns the same packet.
     */
    const std::vector<std::uint8_t>& prepare_release(Surface& surface);

    [[nodiscard]] bool release_prepared() const noexcept { return release_prepared_; }
    /** The latest packet; empty until the first frame. */
    [[nodiscard]] const std::vector<std::uint8_t>& packet() const noexcept;
    [[nodiscard]] const HostRenderPacketTelemetry& telemetry() const noexcept;
    [[nodiscard]] const font::GlyphAtlas& glyph_atlas() const noexcept { return glyph_atlas_; }
    [[nodiscard]] const SurfaceTextures& textures() const noexcept { return textures_; }

  private:
    font::GlyphAtlas glyph_atlas_;
    /** Shape coverage masks, on pages of their own so shapes never evict glyphs. */
    font::GlyphAtlas shape_atlas_;
    SurfaceTextures textures_;
    HostRenderPacketCache cache_;
    bool release_prepared_ = false;
};

} // namespace strata::ui
