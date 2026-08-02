#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include <d3d11.h>

#include <strata/render_packet.hpp>

namespace strata::d3d11 {

struct EffectPassTelemetry final {
    std::uint64_t passes = 0U;
    std::uint64_t nanos = 0U;
    std::uint32_t target_width = 0U;
    std::uint32_t target_height = 0U;
};

/**
 * Executes bounded authored effect programs over either the current backdrop or an isolated
 * content layer. Isolated content targets are retained per nesting depth while pass intermediates
 * share one bounded workspace; all are resized only with the framebuffer.
 */
class EffectPassRenderer final {
  public:
    EffectPassRenderer(ID3D11Device* device, ID3D11DeviceContext* context);
    ~EffectPassRenderer();
    EffectPassRenderer(const EffectPassRenderer&) = delete;
    EffectPassRenderer& operator=(const EffectPassRenderer&) = delete;

    void resize(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format);
    void invalidate_cache() noexcept;
    /** Starts one ordered layer stream and invalidates cached samples when its geometry changes. */
    void begin_layer(std::string_view layer_id, std::uint64_t geometry_epoch);
    void release_layer(std::string_view layer_id) noexcept;
    void declare_pass(std::string_view effect_id, std::uint32_t index, std::uint32_t kind,
                      double radius, std::uint32_t downsample, std::uint32_t radius_parameter,
                      std::uint32_t downsample_parameter, std::string_view hlsl_source);

    /** Clears and returns the render target used to isolate one content-effect subtree. */
    [[nodiscard]] ID3D11RenderTargetView* begin_content(std::size_t depth);
    [[nodiscard]] ID3D11Texture2D* content_texture(std::size_t depth);

    [[nodiscard]] EffectPassTelemetry
    apply_backdrop(std::string_view layer_id, std::size_t depth, const host::EffectBatch& effect,
                   ID3D11Texture2D* target_texture, ID3D11RenderTargetView* target,
                   double logical_width, double logical_height, double frame_seconds);
    [[nodiscard]] EffectPassTelemetry
    finish_content(std::string_view layer_id, std::size_t depth, const host::EffectBatch& effect,
                   ID3D11Texture2D* backdrop_texture, ID3D11RenderTargetView* destination,
                   double logical_width, double logical_height, double frame_seconds);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::d3d11
