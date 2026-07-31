#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace strata::host {
struct RenderPacket;
}

namespace strata::headless {

/** Render backend contract consumed by the deterministic application host. */
class CaptureRenderer {
  public:
    virtual ~CaptureRenderer() = default;

    virtual void resize(std::uint32_t framebuffer_width, std::uint32_t framebuffer_height,
                        double logical_width, double logical_height) = 0;
    virtual void set_clear_color(std::array<std::uint8_t, 4U> rgba) noexcept = 0;
    virtual void declare_material(std::string_view id, std::string_view source) = 0;
    virtual void render(const host::RenderPacket& packet, std::int64_t time_nanoseconds) = 0;
    virtual void consume_resources(const host::RenderPacket& packet) = 0;

    [[nodiscard]] virtual std::string_view backend() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;
    [[nodiscard]] virtual std::span<const std::uint8_t> pixels() const noexcept = 0;
    [[nodiscard]] virtual const std::vector<std::string>& material_fallbacks() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<CaptureRenderer> create_capture_renderer(std::string_view backend);

} // namespace strata::headless
