#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11Texture2D;

namespace strata::host {
struct RenderPacket;
}

namespace strata::d3d11 {

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

/** Shared D3D11 packet-v8 pipeline for swap-chain and offscreen targets. */
class RenderContext final {
  public:
    RenderContext(ID3D11Device* device, ID3D11DeviceContext* context);
    ~RenderContext();

    RenderContext(const RenderContext&) = delete;
    RenderContext& operator=(const RenderContext&) = delete;

    void set_target(ID3D11Texture2D* texture, ID3D11RenderTargetView* target,
                    std::uint32_t framebuffer_width, std::uint32_t framebuffer_height,
                    double logical_width, double logical_height);
    void release_target();
    void declare_material(std::string_view id, std::string_view hlsl_source);
    void declare_effect_pass(std::string_view effect_id, std::uint32_t index, std::uint32_t kind,
                             double radius, std::uint32_t downsample,
                             std::uint32_t radius_parameter, std::uint32_t downsample_parameter,
                             std::string_view hlsl_source);
    void consume_resources(const host::RenderPacket& packet);
    void begin_frame(std::array<float, 4U> clear_color, double frame_seconds);
    [[nodiscard]] RenderLayerTelemetry render_layer(std::string_view id,
                                                    const host::RenderPacket& packet);
    void release_layer(std::string_view id) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::d3d11
