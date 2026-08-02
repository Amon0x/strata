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
    float3 refracted;
    refracted.r = sampleEffectBackdrop(input.uv + offset * 1.16).r;
    refracted.g = sampleEffectBackdrop(input.uv + offset).g;
    refracted.b = sampleEffectBackdrop(input.uv + offset * 0.84).b;

    float clarity = 0.42 + edge * 0.38;
    float3 color = lerp(soft.rgb, refracted, clarity);
    color = adjustSaturation(color, effectFloat(7));
    color = saturate((color - 0.5) * 1.07 + 0.5);

    float4 tint = effectColor(3);
    color = lerp(color, tint.rgb, saturate(tint.a));

    float innerRim = 1.0 - smoothstep(0.0, 9.0, max(-distance, 0.0));
    float hairline = 1.0 - smoothstep(0.7, 2.2, abs(distance + 0.8));
    float2 lightDirection = normalize(float2(-0.58, -0.82));
    float facingLight = saturate(dot(normal, lightDirection));
    float facingShade = saturate(dot(normal, -lightDirection));
    float specular = pow(facingLight, 5.0) * innerRim;
    float reflected = pow(facingShade, 9.0) * innerRim;
    float highlight = effectFloat(8);

    color += specular * highlight * float3(1.0, 0.98, 0.92);
    color += reflected * highlight * 0.34 * float3(0.32, 0.62, 1.0);
    color += hairline * highlight * 0.46;
    color += innerRim * edge * float3(0.035, 0.075, 0.12);
    color -= facingShade * innerRim * 0.055;

    float noise = (hash21(input.logicalPixel + floor(effectTime() * 24.0)) - 0.5) *
        effectFloat(9) * 0.012;
    color += noise;

    return float4(saturate(color), 1.0);
}
