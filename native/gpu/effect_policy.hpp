#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

#include <strata/render_packet.hpp>

#include "gpu/effect_shaders.hpp"

namespace strata::gpu {

/**
 * The downsample a blur pass runs at, from its declared value or the effect parameter that
 * overrides it: the nearest whole factor in [1, 8]. Every backend resolves it here, so one packet
 * blurs at the same resolution on D3D11, Vulkan and the reference renderer.
 */
[[nodiscard]] inline std::uint32_t blur_downsample(const double requested) noexcept {
    if (!(requested > 1.0))
        return 1U;
    if (requested >= 8.0)
        return 8U;
    return static_cast<std::uint32_t>(std::llround(requested));
}

/**
 * How far outside its bounds, in logical pixels, an effect's output depends on what was drawn
 * before it. Authored shader passes may sample up to the prelude's effect_sample_reach; a blur
 * reads its radius around the bounds. Passes write inside the bounds only, so the widest pass
 * decides. Two pixels of slack cover rounding the bounds and the blur padding to device pixels.
 */
[[nodiscard]] inline double effect_read_reach(const bool shader_passes,
                                              const double widest_blur_radius) noexcept {
    const double blur = std::isfinite(widest_blur_radius) ? std::max(0.0, widest_blur_radius) : 0.0;
    return std::max(shader_passes ? effect_sample_reach : 0.0, blur) + 2.0;
}

/**
 * What one packet changed in a layer, against which every backend tests the effect samples it
 * retains. The decoder reports the changes of each packet relative to the one before it, so a
 * backend that did not render the previous epoch (the first packet of a layer, or a frame that
 * failed after the decoder had moved on) cannot know what changed and treats everything as
 * changed.
 */
class EffectChanges final {
  public:
    EffectChanges(const std::uint64_t geometry_epoch,
                  const std::optional<std::uint64_t> previous_epoch, const bool dirty_all,
                  const std::span<const host::GeometryDirtyRegion> regions) noexcept
        : all_(dirty_all || !previous_epoch.has_value() ||
               (geometry_epoch != *previous_epoch && geometry_epoch != *previous_epoch + 1U)),
          regions_(regions) {}

    EffectChanges(const host::RenderPacket& packet,
                  const std::optional<std::uint64_t> previous_epoch) noexcept
        : EffectChanges(packet.geometry_epoch, previous_epoch, packet.geometry_dirty_all,
                        packet.geometry_dirty_regions) {}

    [[nodiscard]] bool none() const noexcept {
        return !all_ && regions_.empty();
    }

    /**
     * Whether anything an effect read changed: its bounds grown by `reach` logical pixels (see
     * effect_read_reach). A sample that this returns false for is still exactly what the effect
     * would produce now.
     */
    [[nodiscard]] bool touch(const host::EffectBatch& effect, const double reach) const noexcept {
        if (all_)
            return true;
        const double left = effect.x - reach;
        const double top = effect.y - reach;
        const double right = effect.x + effect.width + reach;
        const double bottom = effect.y + effect.height + reach;
        return std::ranges::any_of(regions_, [&](const host::GeometryDirtyRegion& region) {
            return region.x <= right && region.x + region.width >= left && region.y <= bottom &&
                   region.y + region.height >= top;
        });
    }

  private:
    bool all_ = true;
    std::span<const host::GeometryDirtyRegion> regions_;
};

} // namespace strata::gpu
