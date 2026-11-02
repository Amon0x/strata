#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <strata/presentation.hpp>

#include "d3d11/render_context.hpp"

struct HWND__;
using HWND = HWND__*;

namespace strata::host {
struct RenderPacket;
}

namespace strata::desktop {

using RenderLayerTelemetry = d3d11::RenderLayerTelemetry;

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
    void release_programs(std::string_view prefix);
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
    [[nodiscard]] std::optional<host::PresentationCapture> take_capture();
    [[nodiscard]] host::PresentationInfo info() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** Quiesces GPU ownership before constructor unwinding can let the HWND be destroyed. */
struct PresentationDeleter final {
    void operator()(host::Presentation* presentation) const noexcept;
};

/** Owns the swap chain on its presentation thread; the HWND stays live until stopped(). */
[[nodiscard]] std::unique_ptr<host::Presentation, PresentationDeleter> create_presentation(
    HWND window, bool vsync, bool asynchronous_shader_compilation,
    host::PresentationExecution execution);

/** Services only sent window-system messages during an exceptional/destructor shutdown barrier. */
void dispatch_presentation_shutdown_messages() noexcept;

} // namespace strata::desktop
