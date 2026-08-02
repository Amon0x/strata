float3 adjustSaturation(float3 color, float saturation) {
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    return lerp(luminance.xxx, color, saturation);
}

float hash21(float2 value) {
    value = frac(value * float2(123.34, 456.21));
    value += dot(value, value + 45.32);
    return frac(value.x * value.y);
}

float2 glassNormal(float2 pixel) {
    float2 gradient = float2(
        effectDistance(pixel + float2(1.0, 0.0)) -
            effectDistance(pixel - float2(1.0, 0.0)),
        effectDistance(pixel + float2(0.0, 1.0)) -
            effectDistance(pixel - float2(0.0, 1.0))
    );
    return normalize(gradient + float2(0.0001, 0.0001));
}

float4 effect(EffectInput input) {
    float distance = effectDistance(input.pixel);
    float2 normal = glassNormal(input.pixel);
    float2 texel = 1.0 / max(effectTargetSize, 1.0);

    float edgeWidth = max(min(effectBounds.z, effectBounds.w) * 0.24, 10.0);
    float edge = 1.0 - smoothstep(0.0, edgeWidth, max(-distance, 0.0));
    float lens = pow(saturate(edge), 1.35);
    float refraction = effectFloat(2);
    float2 offset = normal * refraction * lens * texel;

    float4 soft = sampleEffectSource(input.uv + offset * 0.28);
    float3 refracted = sampleEffectBackdrop(input.uv + offset).rgb;

    float clarity = 0.54 + edge * 0.22;
    float3 color = lerp(soft.rgb, refracted, clarity);
    color = adjustSaturation(color, effectFloat(7));
    color = saturate((color - 0.5) * 1.07 + 0.5);

    float4 tint = effectColor(3);
    color = lerp(color, tint.rgb, saturate(tint.a));

    float innerRim = 1.0 - smoothstep(0.0, 4.0, max(-distance, 0.0));
    float hairline = 1.0 - smoothstep(0.35, 1.15, abs(distance + 0.4));
    float2 lightDirection = normalize(float2(-0.58, -0.82));
    float facingLight = saturate(dot(normal, lightDirection));
    float facingShade = saturate(dot(normal, -lightDirection));
    float leadingGlint = pow(saturate(1.0 - input.localUv.x), 1.7);
    float trailingGlint = pow(saturate(input.localUv.x), 2.2);
    float specular = pow(facingLight, 8.0) * innerRim * leadingGlint;
    float reflected = pow(facingShade, 11.0) * innerRim * trailingGlint;
    float highlight = effectFloat(8);

    color += specular * highlight * 0.52 * float3(1.0, 0.98, 0.94);
    color += reflected * highlight * 0.18 * float3(0.28, 0.56, 1.0);
    color += hairline * 0.035 * float3(0.62, 0.84, 1.0);
    color += innerRim * edge * float3(0.012, 0.025, 0.04);
    color -= facingShade * innerRim * 0.025;

    float noise = (hash21(input.logicalPixel + floor(effectTime() * 24.0)) - 0.5) *
        effectFloat(9) * 0.012;
    color += noise;

    return float4(saturate(color), 1.0);
}
