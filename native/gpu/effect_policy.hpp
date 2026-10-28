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

/**
 * Whether an effect sample taken at `sampled_seconds` is due for evaluation again at
 * `now_seconds`. An effect without a refresh rate is evaluated every frame. Whether anything the
 * sample read has changed is the decoder's decision (host::EffectBatch::sample_revision); this is
 * the only part of the retention policy a backend applies itself. A nanosecond of slack keeps a
 * frame that lands exactly on the interval from waiting another frame.
 */
[[nodiscard]] inline bool effect_refresh_due(const double refresh_rate, const double sampled_seconds,
                                             const double now_seconds) noexcept {
    if (!(refresh_rate > 0.0))
        return true;
    return now_seconds < sampled_seconds ||
           now_seconds - sampled_seconds + 1.0e-9 >= 1.0 / refresh_rate;
}

} // namespace strata::gpu
