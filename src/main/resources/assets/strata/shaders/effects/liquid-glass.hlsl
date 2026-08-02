float3 adjustSaturation(float3 color, float saturation) {
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    return lerp(luminance.xxx, color, saturation);
}

float hash21(float2 value) {
    value = frac(value * float2(123.34, 456.21));
    value += dot(value, value + 45.32);
    return frac(value.x * value.y);
}

float4 effect(EffectInput input) {
    float2 centered = input.localUv * 2.0 - 1.0;
    float distance = effectDistance(input.pixel);
    float edgeRange = max(min(effectBounds.z, effectBounds.w) * 0.42, 1.0);
    float edge = saturate(1.0 + distance / edgeRange);
    float2 normal = normalize(centered + float2(0.0001, 0.0001));
    float refraction = effectFloat(2);
    float noise = (hash21(input.logicalPixel + effectTime() * 17.0) - 0.5) * effectFloat(9);
    float2 offset = normal * edge * refraction / max(effectTargetSize, 1.0);
    offset += noise / max(effectTargetSize, 1.0);

    float4 glass = sampleEffectSource(input.uv + offset);
    float4 backdrop = sampleEffectBackdrop(input.uv - offset * 0.35);
    glass.rgb = adjustSaturation(lerp(glass.rgb, backdrop.rgb, 0.16), effectFloat(7));

    float4 tint = effectColor(3);
    glass.rgb = lerp(glass.rgb, tint.rgb, saturate(tint.a));
    float rim = pow(saturate(1.0 + distance / 7.0), 3.0) * effectFloat(8);
    float directional = saturate(1.0 - input.localUv.y) * 0.35 + 0.65;
    glass.rgb += rim * directional;
    glass.a = 1.0;
    return glass;
}
