float3 adjustSaturation(float3 color, float saturation) {
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    return lerp(luminance.xxx, color, saturation);
}

float luminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
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

float3 refractBlurredField(float2 uv, float2 offset, float dispersion) {
    float red = sampleEffectSource(uv + offset * (1.0 + dispersion)).r;
    float green = sampleEffectSource(uv + offset).g;
    float blue = sampleEffectSource(uv + offset * (1.0 - dispersion)).b;
    return float3(red, green, blue);
}

float4 effect(EffectInput input) {
    float distance = effectDistance(input.pixel);
    float insideDistance = max(-distance, 0.0);
    float2 normal = glassNormal(input.pixel);
    float2 texel = 1.0 / max(effectTargetSize, 1.0);

    // A stable center and a curved boundary read as a lens without warping the
    // entire field. Both samples come from the filtered source: raw backdrop
    // detail must never be reintroduced after the blur pass.
    float minimumExtent = max(min(effectBounds.z, effectBounds.w), 1.0);
    float lensWidth = clamp(minimumExtent * 0.105, 7.0, 24.0);
    float edgeProximity = 1.0 - smoothstep(0.0, lensWidth, insideDistance);
    float lens = pow(saturate(edgeProximity), 1.35);
    float refraction = max(effectFloat(2), 0.0);
    float2 offset = -normal * refraction * lens * texel;
    float dispersion = 0.055 * lens;

    float3 base = sampleEffectSource(input.uv).rgb;
    float3 refracted = refractBlurredField(input.uv, offset, dispersion);
    float3 color = lerp(base, refracted, lens * 0.82);
    color = adjustSaturation(color, effectFloat(7));
    color = saturate((color - 0.5) * 1.04 + 0.5);

    float4 tint = effectColor(3);
    color = lerp(color, tint.rgb, saturate(tint.a));

    // The boundary has an ambient response on every side. A coherent virtual
    // overhead light adds shape, while the opposing lobe stays broad and dim.
    // Rounded corners choose their own response through the SDF normal.
    float innerRim = 1.0 - smoothstep(0.0, 4.5, insideDistance);
    float hairline = 1.0 - smoothstep(0.3, 1.2, abs(distance + 0.42));
    float fresnel = pow(saturate(edgeProximity), 3.2) * innerRim;
    float2 lightDirection = normalize(float2(-0.22, -0.98));
    float keyFacing = saturate(dot(normal, lightDirection));
    float opposingFacing = saturate(dot(normal, -lightDirection));
    float keyLobe = pow(keyFacing, 4.5) * innerRim;
    float opposingLobe = pow(opposingFacing, 3.2) * innerRim;

    float3 environment = sampleEffectSource(input.uv - normal * 3.0 * texel).rgb;
    float environmentLuminance = luminance(environment);
    float darkBackdrop = 1.0 - smoothstep(0.08, 0.5, environmentLuminance);
    float3 neutralRim = lerp(
        float3(0.60, 0.67, 0.72),
        float3(0.90, 0.94, 0.96),
        darkBackdrop
    );
    float3 bledRim = adjustSaturation(environment, 0.55);
    float3 ambientRimColor = lerp(neutralRim, bledRim, 0.20);
    float highlight = max(effectFloat(8), 0.0);

    color += hairline * highlight * (0.045 + darkBackdrop * 0.075) * ambientRimColor;
    color += fresnel * highlight * 0.025 * ambientRimColor;
    color += keyLobe * highlight * (0.24 + darkBackdrop * 0.16) *
        float3(1.0, 0.985, 0.955);
    color += opposingLobe * highlight * 0.055 *
        lerp(float3(0.48, 0.56, 0.62), ambientRimColor, 0.45);
    color -= opposingLobe * (0.012 + (1.0 - darkBackdrop) * 0.012);

    // Static grain prevents banding without turning temporal sampling into a
    // shimmer. It is deliberately subordinate to the material lighting.
    float noise = (hash21(floor(input.logicalPixel)) - 0.5) *
        max(effectFloat(9), 0.0) * 0.01;
    color += noise;

    return float4(saturate(color), 1.0);
}
