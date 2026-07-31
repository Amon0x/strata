#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "capture_renderer.hpp"
#include "host/render_packet.hpp"
#include "image_codec.hpp"

namespace strata::headless {

/** Deterministic CPU consumer for packet/geometry reference tests. */
class SoftwareRenderer final : public CaptureRenderer {
  public:
    explicit SoftwareRenderer(const ImageCodec& image_codec);

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
    struct Texture final {
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
        std::uint32_t channels = 0U;
        std::uint32_t sampling = 0U;
        std::vector<std::uint8_t> pixels;
    };

    const ImageCodec& image_codec_;
    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
    double logical_width_ = 0.0;
    double logical_height_ = 0.0;
    std::array<std::uint8_t, 4U> clear_{9U, 11U, 15U, 255U};
    std::vector<std::uint8_t> pixels_;
    std::map<std::string, Texture, std::less<>> textures_;
    std::vector<std::string> material_fallbacks_;

    void apply(const host::ResourceOperation& operation);
    void draw(const host::DrawBatch& batch, const host::RenderPacket& packet);
    void blur(const host::BlurBatch& batch);
};

} // namespace strata::headless
