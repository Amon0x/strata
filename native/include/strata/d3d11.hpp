#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <strata/d3d11.h>
#include <strata/strata.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11Texture2D;

namespace strata {
class Runtime;
class Surface;
}

namespace strata::host {
struct RenderPacket;
}

namespace strata::d3d11 {

enum class TargetLoadAction {
    preserve,
    clear,
};

enum class ContextStatePolicy {
    preserve,
    host_managed,
};

struct RendererOptions final {
    ContextStatePolicy context_state = ContextStatePolicy::preserve;
    bool asynchronous_shader_compilation = false;
};

using ProgramSourceLoader = std::function<std::string(std::string_view)>;

struct PresenterOptions final {
    RendererOptions renderer;
    ProgramSourceLoader load_program_source;
};

struct RenderTarget final {
    ID3D11Texture2D* texture = nullptr;
    ID3D11RenderTargetView* view = nullptr;
    std::uint32_t framebuffer_width = 0U;
    std::uint32_t framebuffer_height = 0U;
    double logical_width = 0.0;
    double logical_height = 0.0;
};

struct FrameOptions final {
    TargetLoadAction load_action = TargetLoadAction::preserve;
    std::array<float, 4U> clear_color{};
    double time_seconds = 0.0;
};

struct RenderLayerTelemetry final {
    std::uint64_t blur_passes = 0U;
    std::uint32_t blur_target_width = 0U;
    std::uint32_t blur_target_height = 0U;
    std::uint64_t blur_nanos = 0U;
    std::uint64_t effect_passes = 0U;
    std::uint32_t effect_target_width = 0U;
    std::uint32_t effect_target_height = 0U;
    std::uint64_t effect_nanos = 0U;
};

struct PresentedFrame final {
    strata_surface_frame_info surface{};
    std::size_t packet_bytes = 0U;
    RenderLayerTelemetry rendering;
};

/**
 * D3D11 packet renderer for a device, immediate context, and render target owned by an embedding
 * host. The renderer never creates a swap chain or presents. Context state is isolated by default,
 * and the target contents are preserved unless a frame explicitly requests a clear.
 */
class Renderer final {
  public:
    Renderer(ID3D11Device* device, ID3D11DeviceContext* context, RendererOptions options = {});
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;

    void declare_material(std::string_view id, std::string_view hlsl_source);
    void declare_effect_pass(std::string_view effect_id, std::uint32_t index, std::uint32_t kind,
                             double radius, std::uint32_t downsample,
                             std::uint32_t radius_parameter, std::uint32_t downsample_parameter,
                             std::string_view hlsl_source);

    /**
     * Renders one retained packet layer into the supplied host target. Packet streams need stable,
     * unique layer ids so geometry and effect caches cannot cross Surface boundaries.
     */
    [[nodiscard]] RenderLayerTelemetry render(std::string_view layer_id,
                                              const host::RenderPacket& packet,
                                              const RenderTarget& target,
                                              FrameOptions options = {});

    /** Applies terminal or otherwise resource-only packets without requiring a render target. */
    void consume_resources(const host::RenderPacket& packet);
    void release_layer(std::string_view layer_id) noexcept;

    /**
     * Releases references to the current host target. Call this before the host destroys or resizes
     * that target.
     */
    void release_target();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Surface-to-D3D11 presentation adapter. It owns ordered packet decoding, shader declaration
 * synchronization, and backend layer resources while the embedding host retains ownership of the
 * Runtime, Surfaces, D3D device, context, and targets.
 */
class Presenter final {
  public:
    Presenter(
        Runtime& runtime,
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        PresenterOptions options = {}
    );
    Presenter(
        strata_runtime* runtime,
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        PresenterOptions options = {}
    );
    ~Presenter();

    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;
    Presenter(Presenter&&) noexcept;
    Presenter& operator=(Presenter&&) noexcept;

    /** Re-reads the active application's HLSL declarations and their source resources. */
    void synchronize_programs();

    /**
     * Replaces every active material/effect program sourced from one resource. Returns false when
     * the active application does not reference that resource.
     */
    [[nodiscard]] bool reload_program_source(
        std::string_view resource_id,
        std::string_view hlsl_source
    );

    /** Attaches a stable backend layer id before its first presentation. */
    void attach(std::string_view layer_id, Surface& surface);
    void attach(std::string_view layer_id, strata_surface* surface);

    /**
     * Frames and renders one Surface. A layer id becomes attached to the first Surface presented
     * through it and cannot be reused for another Surface until detached.
     */
    [[nodiscard]] PresentedFrame present(
        std::string_view layer_id,
        Surface& surface,
        const RenderTarget& target,
        std::int64_t time_nanoseconds,
        FrameOptions options = {}
    );
    [[nodiscard]] PresentedFrame present(
        std::string_view layer_id,
        strata_surface* surface,
        const RenderTarget& target,
        std::int64_t time_nanoseconds,
        FrameOptions options = {}
    );

    /**
     * Delivers the Surface's terminal resource packet, acknowledges it, and releases the backend
     * layer. The Surface remains live and can then be closed normally.
     */
    void detach(std::string_view layer_id);

    /** Drops backend state when terminal packet delivery is impossible. */
    void discard(std::string_view layer_id) noexcept;
    [[nodiscard]] bool attached(std::string_view layer_id) const noexcept;
    void release_target();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::d3d11
