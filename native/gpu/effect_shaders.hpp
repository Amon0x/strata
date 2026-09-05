#pragma once

#include <string_view>

namespace strata::gpu {
constexpr std::string_view effect_prelude = R"hlsl(
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
float4 effectUnpremultiply(float4 color) {
    color.rgb = color.a > 0.00001 ? color.rgb / color.a : 0.0;
    return color;
}
float4 sampleEffectSource(float2 uv) {
    return effectUnpremultiply(
        EffectSource.Sample(EffectSampler, clamp(uv, 0.0, 1.0))
    );
}
float4 sampleEffectBackdrop(float2 uv) {
    return effectUnpremultiply(
        EffectBackdrop.Sample(EffectSampler, clamp(uv, 0.0, 1.0))
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
    float padding[2]{};
};
static_assert(sizeof(EffectConstants) % 16U == 0U);


}
