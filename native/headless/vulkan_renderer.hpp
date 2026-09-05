#pragma once
#include "capture_renderer.hpp"
#include "vulkan/offscreen.hpp"
#include <cstdlib>
namespace strata::headless {
class VulkanRenderer final : public CaptureRenderer {
    vulkan::detail::Offscreen context_{std::getenv("STRATA_VULKAN_VALIDATION") != nullptr};
    vulkan::Renderer renderer_{context_.device};
    std::vector<std::uint8_t> pixels_;
    std::vector<std::string> fallbacks_;
    double logical_width_{}, logical_height_{};
    std::array<float, 4> clear_{};

  public:
    void resize(std::uint32_t w, std::uint32_t h, double lw, double lh) override {
        renderer_.release_target();
        context_.resize(w, h);
        logical_width_ = lw;
        logical_height_ = lh;
    }
    void set_clear_color(std::array<std::uint8_t, 4> color) noexcept override {
        for (std::size_t i = 0; i < 4; ++i)
            clear_[i] = color[i] / 255.0F;
    }
    void declare_material(std::string_view id, std::string_view source) override {
        renderer_.declare_material(id, source);
    }
    void declare_effect_pass(std::string_view id, std::uint32_t index, std::uint32_t kind,
                             double radius, std::uint32_t downsample, std::uint32_t rp,
                             std::uint32_t dp, std::string_view source) override {
        renderer_.declare_effect_pass(id, index, kind, radius, downsample, rp, dp, source);
    }
    void render(const host::RenderPacket& packet, std::int64_t time) override {
        (void)renderer_.render(
            "headless", packet, context_.target(logical_width_, logical_height_),
            {vulkan::TargetLoadAction::clear, clear_, static_cast<double>(time) / 1e9});
        pixels_ = context_.pixels();
        if (context_.validation_errors.load())
            throw std::runtime_error("Vulkan validation reported errors");
    }
    void consume_resources(const host::RenderPacket& packet) override {
        renderer_.consume_resources(packet);
    }
    std::string_view backend() const noexcept override { return "vulkan"; }
    std::uint32_t width() const noexcept override {
        return context_.image ? context_.image->width : 0;
    }
    std::uint32_t height() const noexcept override {
        return context_.image ? context_.image->height : 0;
    }
    std::span<const std::uint8_t> pixels() const noexcept override { return pixels_; }
    const std::vector<std::string>& material_fallbacks() const noexcept override {
        return fallbacks_;
    }
};
} // namespace strata::headless
