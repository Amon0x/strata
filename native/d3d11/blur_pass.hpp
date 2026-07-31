#pragma once

#include <cstdint>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11Texture2D;

namespace strata::host {
struct BlurBatch;
}

namespace strata::d3d11 {

struct BlurPassTelemetry final {
    std::uint64_t passes = 0U;
    std::uint32_t target_width = 0U;
    std::uint32_t target_height = 0U;
    std::uint64_t nanos = 0U;
};

/** Ordered separable blur effect over the currently bound D3D11 render target. */
class BlurPass final {
  public:
    BlurPass(ID3D11Device* device, ID3D11DeviceContext* context);
    ~BlurPass();

    BlurPass(const BlurPass&) = delete;
    BlurPass& operator=(const BlurPass&) = delete;

    void resize(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] BlurPassTelemetry
    execute(const host::BlurBatch& batch, ID3D11Texture2D* back_buffer,
            ID3D11RenderTargetView* target, std::uint32_t framebuffer_width,
            std::uint32_t framebuffer_height, double logical_width, double logical_height);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::d3d11
