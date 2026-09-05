#pragma once

#include <array>
#include <string_view>
#include <strata/render_packet.hpp>

namespace strata::gpu {
static_assert(host::maximum_rounded_clip_depth == 16U);

inline constexpr std::string_view rounded_clip_hlsl = R"hlsl(
#define STRATA_MAX_ROUNDED_CLIPS 16
cbuffer RoundedClipData : register(b1) {
    float4 roundedClipBounds[STRATA_MAX_ROUNDED_CLIPS];
    float4 roundedClipRadii[STRATA_MAX_ROUNDED_CLIPS];
    float4 roundedClipInverseX[STRATA_MAX_ROUNDED_CLIPS];
    float4 roundedClipInverseY[STRATA_MAX_ROUNDED_CLIPS];
    uint roundedClipCount;
    uint roundedClipMode;
    float2 roundedClipPadding;
};

float roundedClipBoxSdf(float2 p, float2 halfSize, float4 radii) {
    float2 quadrant = step(float2(0.0, 0.0), p);
    float radius = lerp(
        lerp(radii.x, radii.w, quadrant.y),
        lerp(radii.y, radii.z, quadrant.y),
        quadrant.x
    );
    radius = min(max(radius, 0.0), min(halfSize.x, halfSize.y));
    float2 q = abs(p) - halfSize + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

float strataRoundedClipCoverage(float2 logicalPixel) {
    float coverage = 1.0;
    [loop]
    for (uint index = 0; index < roundedClipCount; ++index) {
        float2 localPixel = float2(
            dot(roundedClipInverseX[index].xy, logicalPixel) +
                roundedClipInverseX[index].z,
            dot(roundedClipInverseY[index].xy, logicalPixel) +
                roundedClipInverseY[index].z
        );
        float2 halfSize = roundedClipBounds[index].zw * 0.5;
        float2 center = roundedClipBounds[index].xy + halfSize;
        float distance = roundedClipBoxSdf(
            localPixel - center,
            halfSize,
            roundedClipRadii[index]
        );
        float softness = max(fwidth(distance), 0.0001);
        coverage *= 1.0 - smoothstep(-softness, softness, distance);
    }
    return coverage;
}

float4 strataApplyRoundedClips(float4 color, float2 logicalPixel) {
    float coverage = strataRoundedClipCoverage(logicalPixel);
    if (roundedClipMode == 3) {
        // Multiply blending intentionally ignores source alpha for covered pixels, but alpha still
        // owns the primitive/texture silhouette and must reject transparent parts of its quad.
        clip(color.a - 0.000001);
        color.rgb *= coverage;
        color.a = coverage;
    } else if (roundedClipMode == 2) {
        clip(coverage - 0.5);
    } else if (roundedClipMode == 1) {
        color *= coverage;
    } else {
        color.a *= coverage;
    }
    return color;
}
)hlsl";

enum class RoundedClipMode : std::uint32_t {
    straight_alpha = 0U,
    premultiplied_alpha = 1U,
    hard = 2U,
    multiply = 3U,
};

struct RoundedClipConstants final {
    std::array<std::array<float, 4U>, host::maximum_rounded_clip_depth> bounds{};
    std::array<std::array<float, 4U>, host::maximum_rounded_clip_depth> radii{};
    std::array<std::array<float, 4U>, host::maximum_rounded_clip_depth> inverse_x{};
    std::array<std::array<float, 4U>, host::maximum_rounded_clip_depth> inverse_y{};
    std::uint32_t count = 0U;
    std::uint32_t mode = 0U;
    std::array<float, 2U> padding{};
};
static_assert(sizeof(RoundedClipConstants) % 16U == 0U);


}
