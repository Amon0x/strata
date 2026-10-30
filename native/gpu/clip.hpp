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
    // .w of the inverse rows: the clip's shape kind and its sides (RoundedClip::shape, sides).
    float4 roundedClipInverseX[STRATA_MAX_ROUNDED_CLIPS];
    float4 roundedClipInverseY[STRATA_MAX_ROUNDED_CLIPS];
    // Fading toward the top, right, bottom and left edges, in logical pixels.
    float4 roundedClipFade[STRATA_MAX_ROUNDED_CLIPS];
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

/** Distance to an ellipse filling halfSize, close enough for its antialiased edge. */
float roundedClipEllipseSdf(float2 p, float2 halfSize) {
    float2 radii = max(halfSize, 0.0001);
    float k = length(p / radii);
    return (k - 1.0) * min(radii.x, radii.y);
}

/**
 * Distance to a regular polygon of `sides` sides (or, `star`, a star of that many points) filling
 * halfSize, pointing up: worked in a unit circle scaled to the box.
 */
float roundedClipPolygonSdf(float2 p, float2 halfSize, float sides, bool star) {
    float2 radii = max(halfSize, 0.0001);
    float2 q = p / radii;
    float n = max(sides, 3.0);
    float sector = 6.28318531 / n;
    // Half a sector on, so a corner points up: a diamond, not a square.
    float angle = atan2(q.x, -q.y) + sector * 0.5;
    float local = angle - sector * floor(angle / sector + 0.5);
    float radius = length(q);
    float distance;
    if (star) {
        // Between a point (outer, radius 1) and the notch between points (inner).
        float inner = 0.5;
        float t = abs(local) / (sector * 0.5);
        float2 a = float2(0.0, 1.0);
        float2 b = float2(sin(sector * 0.5), cos(sector * 0.5)) * inner;
        float2 spot = float2(sin(abs(local)), cos(abs(local))) * radius;
        float2 edge = b - a;
        float2 normal = normalize(float2(-edge.y, edge.x));
        distance = dot(spot - a, normal);
        distance = t > 1.0 ? radius - inner : distance;
    } else {
        distance = radius * cos(local) - cos(sector * 0.5);
    }
    return distance * min(radii.x, radii.y);
}

/**
 * Distance to a sector or ring of the ellipse filling halfSize: from `start` sweeping `sweep`
 * degrees clockwise from right, its inner radius `inner` of the outer (worked in a unit circle).
 */
float roundedClipSectorSdf(float2 p, float2 halfSize, float3 sector) {
    float2 radii = max(halfSize, 0.0001);
    float2 q = p / radii;
    float radius = length(q);
    float distance = max(radius - 1.0, sector.z - radius);
    if (sector.y < 359.999) {
        float angle = degrees(atan2(q.y, q.x));
        float into = fmod(angle - sector.x + 720.0, 360.0);
        float fromStart = into;
        float toEnd = sector.y - into;
        float outside = 360.0 - into;
        float wedge;
        if (into <= sector.y) {
            // Inside the wedge: as far as the nearer of its two edges.
            float nearer = radians(min(fromStart, toEnd));
            wedge = -(nearer < 1.5707963 ? radius * sin(nearer) : radius);
        } else {
            float nearer = radians(min(into - sector.y, outside));
            wedge = nearer < 1.5707963 ? radius * sin(nearer) : radius;
        }
        distance = max(distance, wedge);
    }
    return distance * min(radii.x, radii.y);
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
        uint shape = (uint)(roundedClipInverseX[index].w + 0.5);
        float distance;
        if (shape == 1) {
            distance = roundedClipEllipseSdf(localPixel - center, halfSize);
        } else if (shape == 2 || shape == 3) {
            distance = roundedClipPolygonSdf(localPixel - center, halfSize,
                                             roundedClipInverseY[index].w, shape == 3);
        } else if (shape == 4) {
            distance = roundedClipSectorSdf(localPixel - center, halfSize,
                                            roundedClipRadii[index].xyz);
        } else {
            distance = roundedClipBoxSdf(localPixel - center, halfSize, roundedClipRadii[index]);
        }
        float softness = max(fwidth(distance), 0.0001);
        coverage *= 1.0 - smoothstep(-softness, softness, distance);
        // Fading edges: a ramp from the edge in, over each edge's distance.
        float4 fade = roundedClipFade[index];
        float2 from = localPixel - roundedClipBounds[index].xy;
        float2 to = roundedClipBounds[index].xy + roundedClipBounds[index].zw - localPixel;
        if (fade.x > 0.0) coverage *= saturate(from.y / fade.x);
        if (fade.y > 0.0) coverage *= saturate(to.x / fade.y);
        if (fade.z > 0.0) coverage *= saturate(to.y / fade.z);
        if (fade.w > 0.0) coverage *= saturate(from.x / fade.w);
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

/** The shaders' roundedClipMode values are the packet's clip coverage modes. */
using RoundedClipMode = host::ClipCoverage;

struct RoundedClipConstants final {
    std::array<std::array<float, 4U>, host::maximum_rounded_clip_depth> bounds{};
    std::array<std::array<float, 4U>, host::maximum_rounded_clip_depth> radii{};
    std::array<std::array<float, 4U>, host::maximum_rounded_clip_depth> inverse_x{};
    std::array<std::array<float, 4U>, host::maximum_rounded_clip_depth> inverse_y{};
    std::array<std::array<float, 4U>, host::maximum_rounded_clip_depth> fade{};
    std::uint32_t count = 0U;
    std::uint32_t mode = 0U;
    std::array<float, 2U> padding{};
};
static_assert(sizeof(RoundedClipConstants) % 16U == 0U);


}
