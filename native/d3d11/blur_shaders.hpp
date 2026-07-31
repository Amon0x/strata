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
    float3 padding;
    float4 sourceUv;
};

Texture2D Texture0 : register(t0);
SamplerState Sampler0 : register(s0);

struct PixelInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PixelInput input) : SV_TARGET {
    float2 sampleUv = sourceUv.xy + input.uv * sourceUv.zw;
    if (radius < 0.5) {
        return Texture0.Sample(Sampler0, sampleUv);
    }
    const int maximumRadius = 32;
    int activeRadius = (int)clamp(round(max(radius, 1.0)), 1.0, (float)maximumRadius);
    float4 color = 0.0;
    float weight = 0.0;
    [loop]
    for (int sampleOffset = -maximumRadius; sampleOffset <= maximumRadius; ++sampleOffset) {
        if (abs(sampleOffset) > activeRadius) continue;
        color += Texture0.Sample(
            Sampler0,
            sampleUv + direction * texel * (float)sampleOffset
        );
        weight += 1.0;
    }
    return color / max(weight, 1.0);
}
)hlsl";

} // namespace strata::d3d11::blur_shaders
