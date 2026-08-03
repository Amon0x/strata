float vibrancyLuminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float3 vibrancySaturation(float3 color, float saturation) {
    return lerp(vibrancyLuminance(color).xxx, color, saturation);
}

float4 effect(EffectInput input) {
    float4 content = sampleEffectSource(input.uv);
    if (content.a <= 0.00001) {
        return 0.0;
    }

    float2 contextUv = (
        effectBounds.xy + effectBounds.zw * 0.5
    ) / max(effectTargetSize, 1.0);
    float3 backdrop = sampleEffectBackdrop(contextUv).rgb;
    float threshold = saturate(effectFloat(9));
    float softness = max(effectFloat(10), 0.001);
    float brightBackdrop = smoothstep(
        threshold - softness,
        threshold + softness,
        vibrancyLuminance(backdrop)
    );

    float3 onDark = effectColor(1).rgb;
    float3 onLight = effectColor(5).rgb;
    float3 adaptiveInk = lerp(onDark, onLight, brightBackdrop);
    float3 backdropLight = vibrancySaturation(backdrop, 1.25);
    float3 illuminatedInk = saturate(
        adaptiveInk * (0.84 + backdropLight * 0.16) +
        backdropLight * 0.08
    );
    float3 vibrantInk = lerp(
        adaptiveInk,
        illuminatedInk,
        saturate(effectFloat(11))
    );
    float3 result = lerp(
        content.rgb,
        vibrantInk,
        saturate(effectFloat(0))
    );
    return float4(result, content.a);
}
