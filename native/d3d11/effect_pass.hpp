#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <d3d11.h>

#include <strata/render_packet.hpp>
#include <strata/strata.h>

namespace strata::d3d11 {

struct EffectPassTelemetry final {
    std::uint64_t passes = 0U;
    std::uint64_t nanos = 0U;
    std::uint32_t target_width = 0U;
    std::uint32_t target_height = 0U;
};

/** The isolation targets an effect renderer holds, for tests and diagnostics. */
struct IsolationTargetTelemetry final {
    /** Framebuffer-sized targets open layers draw into: at most gpu::resident_content_layers. */
    std::size_t framebuffer_targets = 0U;
    /** Targets holding parked layers' read bounds, and their pixels. */
    std::size_t parked_targets = 0U;
    std::uint64_t parked_pixels = 0U;
};

/** A render target and its texture: an isolated layer's, or the Surface's own. */
struct ContentTarget final {
    ID3D11Texture2D* texture = nullptr;
    ID3D11RenderTargetView* target = nullptr;
};

/**
 * Executes bounded authored effect programs over either the current backdrop or an isolated
 * content layer. Isolated layers nest without a depth limit: a few framebuffer-sized targets are
 * taken in turn by the layers being drawn, and a layer nested past them parks its shallowest
 * holder in a target sized to that layer's read bounds (gpu/isolation.hpp). Pass intermediates
 * share one bounded workspace. Targets unused for gpu::isolation_target_lifetime layer renders
 * are released, and all are dropped when the framebuffer resizes.
 */
class EffectPassRenderer final {
  public:
    explicit EffectPassRenderer(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        bool asynchronous_shader_compilation = false,
        bool synchronous_backdrop_readback = false
    );
    ~EffectPassRenderer();
    EffectPassRenderer(const EffectPassRenderer&) = delete;
    EffectPassRenderer& operator=(const EffectPassRenderer&) = delete;

    void resize(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format);
    void invalidate_cache() noexcept;
    /** Retained effect samples of one layer; end_layer drops those the frame did not use. */
    [[nodiscard]] std::size_t cached_sample_count(std::string_view layer_id) const noexcept;
    /**
     * Starts one frame of a layer whose effect batches name `effect_ids` (the packet's effect
     * table). Whether a retained sample is still current is the decoder's sample revision.
     */
    void begin_layer(std::string_view layer_id, std::span<const std::string> effect_ids);
    /** Returns whether any SURFACE effect must refresh rather than reuse its retained sample. */
    [[nodiscard]] bool surface_backdrop_required(
        std::string_view layer_id,
        std::span<const host::SubmissionBatch> batches,
        double logical_width,
        double logical_height,
        double frame_seconds
    );
    /**
     * Starts one Surface layer and captures its incoming framebuffer only when preflight found a
     * SURFACE effect whose retained sample is due for refresh.
     */
    void begin_surface(ID3D11Texture2D* target_texture, bool capture_backdrop);
    /**
     * Ends one layer: queues the backdrop probes it measured for readback and collects any earlier
     * readbacks that have completed. Readback never waits on the GPU unless the renderer was built
     * for synchronous readback, so samples normally arrive a frame or two after they were measured.
     */
    void end_layer(std::string_view layer_id);
    /** Removes and returns the layer's completed backdrop samples, oldest first. */
    [[nodiscard]] std::vector<strata_backdrop_sample> take_backdrop_samples(
        std::string_view layer_id
    );
    void release_layer(std::string_view layer_id) noexcept;
    void declare_pass(std::string_view effect_id, std::uint32_t index, std::uint32_t kind,
                      double radius, std::uint32_t downsample, std::uint32_t radius_parameter,
                      std::uint32_t downsample_parameter, std::string_view hlsl_source);

    /**
     * Plans the isolated layers of the layer being rendered (after begin_layer): which pixels
     * each keeps should a deeper one park it. `surface` is what the outermost layers composite
     * into.
     */
    void begin_content_layers(std::span<const host::SubmissionBatch> batches,
                              ContentTarget surface, double logical_width, double logical_height);
    /** Opens the next planned isolated layer and returns its cleared target. */
    [[nodiscard]] ContentTarget begin_content();
    /** Records that the innermost open layer has been drawn into, so parking it keeps it. */
    void touch_content() noexcept;
    /**
     * Closes the innermost isolated layer, compositing it into its parent; the parent's target,
     * which may have moved while it was parked, is active_content() afterwards.
     */
    [[nodiscard]] EffectPassTelemetry
    finish_content(std::string_view layer_id, const host::EffectBatch& effect,
                   double logical_width, double logical_height, double frame_seconds);
    /** The innermost open layer's target, or the Surface's when none is open. */
    [[nodiscard]] ContentTarget active_content() const noexcept;
    [[nodiscard]] IsolationTargetTelemetry isolation_targets() const noexcept;

    [[nodiscard]] EffectPassTelemetry
    apply_backdrop(std::string_view layer_id, const host::EffectBatch& effect,
                   ID3D11Texture2D* target_texture, ID3D11RenderTargetView* target,
                   double logical_width, double logical_height, double frame_seconds);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::d3d11
