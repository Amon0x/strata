#pragma once

#include <string_view>

namespace strata::d3d11::blur_shaders {

constexpr std::string_view vertex = R"hlsl(
struct PixelInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

PixelInput main(uint id : SV_VertexID) {
    PixelInput output;
    float2 uv = float2((id << 1) & 2, id & 2);
    output.uv = uv;
    output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return output;
}
)hlsl";

constexpr std::string_view pixel = R"hlsl(
cbuffer BlurData : register(b0) {
    float2 texel;
    float2 direction;
    float radius;
    float compositeClipped;
    float2 padding;
    float4 sourceUv;
    float4 originalUv;
    float2 logicalSize;
    float2 targetSize;
};

Texture2D Texture0 : register(t0);
Texture2D Texture1 : register(t1);
SamplerState Sampler0 : register(s0);

struct PixelInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PixelInput input) : SV_TARGET {
    float2 sampleUv = sourceUv.xy + input.uv * sourceUv.zw;
    const int maximumRadius = 32;
    float boundedRadius = clamp(radius, 0.5, (float)maximumRadius);
    int activeRadius = (int)ceil(boundedRadius);
    float sigma = max(boundedRadius / 3.0, 0.5);
    float inverseTwoSigmaSquared = 0.5 / (sigma * sigma);
    float4 color = 0.0;
    float totalWeight = 0.0;
    if (radius < 0.5) {
        color = Texture0.Sample(Sampler0, sampleUv);
    } else {
        [loop]
        for (int sampleOffset = -maximumRadius; sampleOffset <= maximumRadius; ++sampleOffset) {
            if (abs(sampleOffset) > activeRadius) continue;
            float distance = (float)abs(sampleOffset);
            float coverage = saturate(boundedRadius + 0.5 - distance);
            float weight = exp(-distance * distance * inverseTwoSigmaSquared) * coverage;
            color += Texture0.Sample(
                Sampler0,
                sampleUv + direction * texel * (float)sampleOffset
            ) * weight;
            totalWeight += weight;
        }
        color /= max(totalWeight, 0.0001);
    }
    if (compositeClipped > 0.5) {
        float2 originalSampleUv = originalUv.xy + input.uv * originalUv.zw;
        float4 original = Texture1.Sample(Sampler0, originalSampleUv);
        float2 logicalPixel =
            input.position.xy * logicalSize / max(targetSize, 1.0);
        color = lerp(original, color, strataRoundedClipCoverage(logicalPixel));
    }
    return color;
}
)hlsl";

} // namespace strata::d3d11::blur_shaders
