float4 effect(EffectInput input) {
    float strength = effectFloat(0);
    float2 centered = input.localUv * 2.0 - 1.0;
    float2 offset = float2(centered.y, -centered.x) * strength /
        max(effectTargetSize, 1.0);
    float4 center = sampleEffectSource(input.uv);
    float red = sampleEffectSource(input.uv + offset).r;
    float blue = sampleEffectSource(input.uv - offset).b;
    center.rgb = float3(red, center.g, blue);
    return center;
}
