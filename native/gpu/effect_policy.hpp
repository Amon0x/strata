#pragma once

#include <cmath>
#include <cstdint>

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

} // namespace strata::gpu
