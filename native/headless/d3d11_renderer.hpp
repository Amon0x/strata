#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "capture_renderer.hpp"

namespace strata::headless {

/** Windowless D3D11/WARP renderer using the production shader and packet pipeline. */
class D3D11Renderer final : public CaptureRenderer {
  public:
    D3D11Renderer();
    ~D3D11Renderer() override;

    D3D11Renderer(const D3D11Renderer&) = delete;
    D3D11Renderer& operator=(const D3D11Renderer&) = delete;

    void resize(std::uint32_t framebuffer_width, std::uint32_t framebuffer_height,
                double logical_width, double logical_height) override;
    void set_clear_color(std::array<std::uint8_t, 4U> rgba) noexcept override;
    void declare_material(std::string_view id, std::string_view source) override;
    void render(const host::RenderPacket& packet, std::int64_t time_nanoseconds) override;
    void consume_resources(const host::RenderPacket& packet) override;

    [[nodiscard]] std::string_view backend() const noexcept override;
    [[nodiscard]] std::uint32_t width() const noexcept override;
    [[nodiscard]] std::uint32_t height() const noexcept override;
    [[nodiscard]] std::span<const std::uint8_t> pixels() const noexcept override;
    [[nodiscard]] const std::vector<std::string>& material_fallbacks() const noexcept override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::headless
