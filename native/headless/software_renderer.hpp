#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "capture_renderer.hpp"
#include "gpu/isolation.hpp"
#include <strata/render_packet.hpp>

namespace strata::headless {

/** Deterministic CPU consumer for packet/geometry reference tests. */
class SoftwareRenderer final : public CaptureRenderer {
  public:
    SoftwareRenderer();

    void resize(std::uint32_t framebuffer_width, std::uint32_t framebuffer_height,
                double logical_width, double logical_height) override;
    void set_clear_color(std::array<std::uint8_t, 4U> rgba) noexcept override;
    void declare_material(std::string_view id, std::string_view source) override;
    void declare_effect_pass(std::string_view effect_id, std::uint32_t index, std::uint32_t kind,
                             double radius, std::uint32_t downsample,
                             std::uint32_t radius_parameter, std::uint32_t downsample_parameter,
                             std::string_view source) override;
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
    struct EffectPass final {
        std::uint32_t kind = 0U;
        double radius = 0.0;
        std::uint32_t downsample = 1U;
        std::uint32_t radius_parameter = UINT32_MAX;
        std::uint32_t downsample_parameter = UINT32_MAX;
    };
    /** An open isolated layer and what it composites into. */
    struct ContentEffect final {
        host::EffectBatch effect;
        /**
         * The parent's pixels: the whole Surface, or only a parent layer's read bounds (empty when
         * nothing had been drawn into it).
         */
        std::vector<std::uint8_t> parent;
        gpu::PixelBounds parent_read;
        bool parent_is_layer = false;
    };
    struct CachedEffect final {
        std::uint32_t left = 0U;
        std::uint32_t top = 0U;
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
        std::int64_t sampled_nanoseconds = 0;
        /** The decoder's sample revision (host::EffectBatch::sample_revision) it was taken at. */
        std::uint64_t revision = 0U;
        std::vector<std::uint8_t> pixels;
    };

    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
    double logical_width_ = 0.0;
    double logical_height_ = 0.0;
    std::array<std::uint8_t, 4U> clear_{9U, 11U, 15U, 255U};
    std::vector<std::uint8_t> pixels_;
    std::map<std::string, Texture, std::less<>> textures_;
    /** A Surface's vector glyph buffers, as appended (host::VectorGlyphBuffers). */
    struct VectorGlyphs final {
        std::vector<std::uint8_t> records;
        std::vector<float> segments;
        std::vector<std::uint32_t> bands;
    };
    std::map<std::string, VectorGlyphs, std::less<>> vector_glyphs_;
    std::vector<std::string> material_fallbacks_;
    std::map<std::string, std::vector<EffectPass>, std::less<>> effects_;
    std::map<std::uint64_t, CachedEffect> cached_effects_;
    /** The packet's effect table resolved to declared passes for the frame being rendered. */
    std::vector<const std::vector<EffectPass>*> layer_effects_;

    void apply(const host::ResourceOperation& operation);
    void draw(const host::DrawBatch& batch, const host::RenderPacket& packet);
    /** Blurs as a GPU blur pass does; `radius_x`/`radius_y` (logical) override the batch's. */
    void blur(const host::BlurBatch& batch, double radius_x = -1.0, double radius_y = -1.0);
    void blur_downsampled(const host::BlurBatch& batch, std::uint32_t left, std::uint32_t top,
                          std::uint32_t right, std::uint32_t bottom, std::uint32_t downsample,
                          const std::vector<std::uint8_t>& clipped_original, double radius_x,
                          double radius_y);
    /** Runs a content layer's built-in filters over the isolated pixels, as the GPUs do. */
    void apply_layer_filters(const host::EffectBatch& effect);
    void apply_effect(const host::EffectBatch& effect);
    [[nodiscard]] const std::vector<EffectPass>* program_of(
        const host::EffectBatch& effect) const noexcept;
    [[nodiscard]] bool effect_refresh_due(const host::EffectBatch& effect, bool content,
                                          std::int64_t time_nanoseconds) const;
    void capture_effect(const host::EffectBatch& effect, bool content,
                        std::int64_t time_nanoseconds);
    void composite_cached_effect(const host::EffectBatch& effect, bool content);
    void composite_effect(std::vector<std::uint8_t> foreground, std::vector<std::uint8_t> backdrop,
                          const host::EffectBatch& effect);
};

} // namespace strata::headless
