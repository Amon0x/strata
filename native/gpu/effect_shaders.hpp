#pragma once

#include <string_view>

namespace strata::gpu {
// How far outside its bounds, in logical pixels, an effect's source and backdrop stay defined. The
// prelude's effectSampleReach clamps samples to the same distance; the two must agree.
inline constexpr double effect_sample_reach = 64.0;

constexpr std::string_view effect_prelude = R"hlsl(
cbuffer EffectData : register(b0) {
    float2 effectLogicalSize;
    float2 effectTargetSize;
    float4 effectBounds;
    float4 effectRadii;
    float4 effectParameters[4];
    float effectOpacityValue;
    float effectTimeValue;
    float effectToneValue;
    float effectPadding;
};

Texture2D EffectSource : register(t0);
Texture2D EffectBackdrop : register(t1);
SamplerState EffectSampler : register(s0);

struct PixelInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

struct EffectInput {
    float2 uv;
    float2 localUv;
    float2 pixel;
    float2 logicalPixel;
};

float effectFloat(uint slot) {
    uint vectorIndex = min(slot / 4, 3);
    uint component = slot % 4;
    return effectParameters[vectorIndex][component];
}
float2 effectFloat2(uint slot) {
    return float2(effectFloat(slot), effectFloat(slot + 1));
}
float4 effectFloat4(uint slot) {
    return float4(
        effectFloat(slot), effectFloat(slot + 1),
        effectFloat(slot + 2), effectFloat(slot + 3)
    );
}
float4 effectColor(uint slot) { return effectFloat4(slot); }
float effectTime() { return effectTimeValue; }
float effectOpacity() { return effectOpacityValue; }
// The tone of the widget this effect belongs to: 0 over a dark backdrop (light ink), 1 over a light
// one (dark ink), and anything between while it eases from one to the other.
float effectTone() { return effectToneValue; }
float4 effectUnpremultiply(float4 color) {
    color.rgb = color.a > 0.00001 ? color.rgb / color.a : 0.0;
    return color;
}
// Source and backdrop are defined inside the effect's bounds extended by effectSampleReach logical
// pixels; a coordinate beyond that reads the nearest defined texel. A backend may therefore capture
// only that region for each effect instead of the whole framebuffer.
static const float effectSampleReach = 64.0;
float2 effectSampleUv(float2 uv) {
    float2 targetSize = max(effectTargetSize, 1.0);
    float2 reach = effectSampleReach * targetSize / max(effectLogicalSize, 1.0) - 0.5;
    float2 low = max(effectBounds.xy - reach, 0.5);
    float2 high = min(effectBounds.xy + effectBounds.zw + reach, targetSize - 0.5);
    return clamp(uv * targetSize, low, high) / targetSize;
}
float4 sampleEffectSource(float2 uv) {
    return effectUnpremultiply(
        EffectSource.Sample(EffectSampler, effectSampleUv(uv))
    );
}
float4 sampleEffectBackdrop(float2 uv) {
    return effectUnpremultiply(
        EffectBackdrop.Sample(EffectSampler, effectSampleUv(uv))
    );
}

float effectCornerRadius(float2 centered) {
    bool right = centered.x >= 0.0;
    bool bottom = centered.y >= 0.0;
    return bottom
        ? (right ? effectRadii.z : effectRadii.w)
        : (right ? effectRadii.y : effectRadii.x);
}

float effectDistance(float2 pixel) {
    float2 halfSize = effectBounds.zw * 0.5;
    float2 centered = pixel - (effectBounds.xy + halfSize);
    float radius = min(effectCornerRadius(centered), min(halfSize.x, halfSize.y));
    float2 q = abs(centered) - halfSize + radius;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

float effectMask(float2 pixel) {
    return 1.0 - smoothstep(-1.0, 1.0, effectDistance(pixel));
}
)hlsl";

constexpr std::string_view effect_entry = R"hlsl(
float4 main(PixelInput pixelInput) : SV_TARGET {
    EffectInput input;
    input.uv = pixelInput.uv;
    input.pixel = pixelInput.position.xy;
    input.logicalPixel = input.pixel * effectLogicalSize / max(effectTargetSize, 1.0);
    input.localUv = (input.pixel - effectBounds.xy) / max(effectBounds.zw, 1.0);
    float4 color = effect(input);
    color.rgb *= color.a;
    return color;
}
)hlsl";

/**
 * Backdrop probe: one output texel holding the mean colour of the effect's bounds in its captured
 * source, from a 12 x 12 grid of bilinear taps. Colours are averaged as stored (premultiplied), so
 * the reading is what the backdrop looks like composited over black.
 */
constexpr std::string_view backdrop_probe_pixel = R"hlsl(
cbuffer EffectData : register(b0) {
    float2 effectLogicalSize;
    float2 effectTargetSize;
    float4 effectBounds;
    float4 effectRadii;
    float4 effectParameters[4];
    float effectOpacityValue;
    float effectTimeValue;
    float2 effectPadding;
};
Texture2D EffectSource : register(t0);
SamplerState EffectSampler : register(s0);
struct PixelInput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(PixelInput input) : SV_TARGET {
    float2 targetSize = max(effectTargetSize, 1.0);
    float2 low = clamp(effectBounds.xy, 0.0, targetSize);
    float2 high = clamp(effectBounds.xy + effectBounds.zw, 0.0, targetSize);
    float3 sum = 0.0;
    for (int row = 0; row < 12; ++row) {
        for (int column = 0; column < 12; ++column) {
            float2 at = lerp(low, high, (float2(column, row) + 0.5) / 12.0);
            sum += EffectSource.SampleLevel(EffectSampler, at / targetSize, 0.0).rgb;
        }
    }
    return float4(sum / 144.0, 1.0);
}
)hlsl";

constexpr std::string_view composite_pixel = R"hlsl(
cbuffer EffectData : register(b0) {
    float2 effectLogicalSize;
    float2 effectTargetSize;
    float4 effectBounds;
    float4 effectRadii;
    float4 effectParameters[4];
    float effectOpacityValue;
    float effectTimeValue;
    float2 effectPadding;
};
Texture2D EffectSource : register(t0);
SamplerState EffectSampler : register(s0);
struct PixelInput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
float cornerRadius(float2 centered) {
    bool right = centered.x >= 0.0;
    bool bottom = centered.y >= 0.0;
    return bottom ? (right ? effectRadii.z : effectRadii.w)
                  : (right ? effectRadii.y : effectRadii.x);
}
float mask(float2 pixel) {
    float2 halfSize = effectBounds.zw * 0.5;
    float2 centered = pixel - (effectBounds.xy + halfSize);
    float radius = min(cornerRadius(centered), min(halfSize.x, halfSize.y));
    float2 q = abs(centered) - halfSize + radius;
    float distance = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
    return 1.0 - smoothstep(-1.0, 1.0, distance);
}
float4 main(PixelInput input) : SV_TARGET {
    float4 color = EffectSource.Sample(EffectSampler, clamp(input.uv, 0.0, 1.0));
    color *= mask(input.position.xy) * effectOpacityValue;
    float2 logicalPixel =
        input.position.xy * effectLogicalSize / max(effectTargetSize, 1.0);
    return strataApplyRoundedClips(color, logicalPixel);
}
)hlsl";

constexpr std::string_view cached_composite_pixel = R"hlsl(
cbuffer EffectData : register(b0) {
    float2 effectLogicalSize;
    float2 effectTargetSize;
    float4 effectBounds;
    float4 effectRadii;
    float4 effectParameters[4];
    float effectOpacityValue;
    float effectTimeValue;
    float2 effectCacheOrigin;
};
Texture2D EffectSource : register(t0);
SamplerState EffectSampler : register(s0);
struct PixelInput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
float cornerRadius(float2 centered) {
    bool right = centered.x >= 0.0;
    bool bottom = centered.y >= 0.0;
    return bottom ? (right ? effectRadii.z : effectRadii.w)
                  : (right ? effectRadii.y : effectRadii.x);
}
float mask(float2 pixel) {
    float2 halfSize = effectBounds.zw * 0.5;
    float2 centered = pixel - (effectBounds.xy + halfSize);
    float radius = min(cornerRadius(centered), min(halfSize.x, halfSize.y));
    float2 q = abs(centered) - halfSize + radius;
    float distance = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
    return 1.0 - smoothstep(-1.0, 1.0, distance);
}
float4 main(PixelInput input) : SV_TARGET {
    uint width;
    uint height;
    EffectSource.GetDimensions(width, height);
    float2 uv = (input.position.xy - effectCacheOrigin) /
        max(float2(width, height), 1.0);
    float4 color = EffectSource.Sample(EffectSampler, clamp(uv, 0.0, 1.0));
    color *= mask(input.position.xy) * effectOpacityValue;
    float2 logicalPixel =
        input.position.xy * effectLogicalSize / max(effectTargetSize, 1.0);
    return strataApplyRoundedClips(color, logicalPixel);
}
)hlsl";

struct EffectConstants final {
    float logical_size[2]{};
    float target_size[2]{};
    float bounds[4]{};
    float radii[4]{};
    float parameters[16]{};
    float opacity = 1.0F;
    float time = 0.0F;
    // Authored passes read [0] as the effect's tone; the cached composite reads both as its origin.
    float padding[2]{};
};
static_assert(sizeof(EffectConstants) % 16U == 0U);


}
