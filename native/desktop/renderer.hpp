#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "d3d11/render_context.hpp"

struct HWND__;
using HWND = HWND__*;

namespace strata::host {
struct RenderPacket;
}

namespace strata::desktop {

using RenderLayerTelemetry = d3d11::RenderLayerTelemetry;

struct RendererInfo final {
    std::string adapter;
    std::string driver_version;
    std::uint32_t vendor_id = 0U;
    std::uint32_t device_id = 0U;
    std::uint64_t dedicated_video_memory = 0U;
    bool vsync = true;
};

/** A presented frame's pixels: straight RGBA8 rows, top first. */
struct FrameCapture final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<std::uint8_t> rgba;
};

/** Win32 fidelity renderer for the backend-ready native render submission protocol. */
class Renderer final {
  public:
    explicit Renderer(HWND window, bool vsync = true, bool asynchronous_shader_compilation = true);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;

    /**
     * Declares one authored material's pixel stage. The shared vertex stage and vertex format are
     * fixed, so a material owns only its shading; a batch naming an undeclared material falls back
     * to the built-in unified shader rather than failing the frame.
     */
    void declare_material(std::string_view id, std::string_view hlsl_source);
    void declare_effect_pass(std::string_view effect_id, std::uint32_t index, std::uint32_t kind,
                             double radius, std::uint32_t downsample,
                             std::uint32_t radius_parameter, std::uint32_t downsample_parameter,
                             std::string_view hlsl_source);

    void resize(std::uint32_t framebuffer_width, std::uint32_t framebuffer_height,
                double logical_width, double logical_height);
    /** Applies packet resource operations without clearing, drawing, or presenting a frame. */
    void consume_resources(const host::RenderPacket& packet);
    void begin_frame();
    [[nodiscard]] RenderLayerTelemetry render_layer(std::string_view id,
                                                    const host::RenderPacket& packet);
    /** What the layer's backdrop probes have measured since the last call, oldest first. */
    [[nodiscard]] std::vector<strata_backdrop_sample> take_backdrop_samples(std::string_view id);
    void release_layer(std::string_view id) noexcept;
    /** Presents and returns false when DXGI reports the window as occluded. */
    [[nodiscard]] bool end_frame();
    /**
     * Keeps the next frame end_frame presents, read back from the swap chain before it is
     * presented (so nothing else on screen can show up in it); take_capture hands it over.
     */
    void capture_next_frame() noexcept;
    /** The pointer over the window in framebuffer pixels, or nothing once it left (materials). */
    void set_pointer(std::optional<std::array<double, 2U>> framebuffer_position) noexcept;
    /** Frames clear to transparent, so a system backdrop (Mica) shows where nothing draws. */
    void set_transparent(bool transparent) noexcept;
    [[nodiscard]] std::optional<FrameCapture> take_capture();
    bool render(const host::RenderPacket& packet);
    [[nodiscard]] RendererInfo info() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::desktop
