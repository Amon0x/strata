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

float3 sampleLowFrequencyBackdrop(float2 uv, float radius) {
    float2 distance = radius / max(effectTargetSize, 1.0);
    float2 diagonal = distance * 0.70710678;
    float2 inner = distance * 0.42;
    float3 color = 0.0;

    color += sampleEffectBackdrop(uv + float2(distance.x, 0.0)).rgb * 0.10;
    color += sampleEffectBackdrop(uv - float2(distance.x, 0.0)).rgb * 0.10;
    color += sampleEffectBackdrop(uv + float2(0.0, distance.y)).rgb * 0.10;
    color += sampleEffectBackdrop(uv - float2(0.0, distance.y)).rgb * 0.10;

    color += sampleEffectBackdrop(uv + float2(diagonal.x, diagonal.y)).rgb * 0.08;
    color += sampleEffectBackdrop(uv + float2(diagonal.x, -diagonal.y)).rgb * 0.08;
    color += sampleEffectBackdrop(uv + float2(-diagonal.x, diagonal.y)).rgb * 0.08;
    color += sampleEffectBackdrop(uv - float2(diagonal.x, diagonal.y)).rgb * 0.08;

    color += sampleEffectBackdrop(uv + float2(inner.x, 0.0)).rgb * 0.07;
    color += sampleEffectBackdrop(uv - float2(inner.x, 0.0)).rgb * 0.07;
    color += sampleEffectBackdrop(uv + float2(0.0, inner.y)).rgb * 0.07;
    color += sampleEffectBackdrop(uv - float2(0.0, inner.y)).rgb * 0.07;
    return color;
}

float4 effect(EffectInput input) {
    float distance = effectDistance(input.pixel);
    float insideDistance = max(-distance, 0.0);
    float2 normal = glassNormal(input.pixel);
    float2 texel = 1.0 / max(effectTargetSize, 1.0);

    // A stable center and a curved boundary read as a lens without warping the
    // entire field. The body uses the filtered source; the boundary later
    // reconstructs only low frequencies and never takes a raw center sample.
    float minimumExtent = max(min(effectBounds.z, effectBounds.w), 1.0);
    float lensWidth = clamp(minimumExtent * 0.12, 8.0, 28.0);
    float edgeProximity = 1.0 - smoothstep(0.0, lensWidth, insideDistance);
    float lens = pow(saturate(edgeProximity), 1.15);
    float transparency = saturate(effectFloat(10));
    float refraction = max(effectFloat(2), 0.0) *
        lerp(0.62, 1.0, transparency);
    float2 offset = -normal * refraction * lens * texel;
    float dispersion = 0.018 * lens;

    float3 base = sampleEffectSource(input.uv).rgb;
    float3 filteredRefraction = refractBlurredField(input.uv, offset, dispersion);
    float backdropFilterRadius = clamp(refraction * 0.52, 7.0, 15.0);
    float3 boundaryField = sampleLowFrequencyBackdrop(
        input.uv + offset,
        backdropFilterRadius
    );
    float3 refracted = lerp(
        filteredRefraction,
        boundaryField,
        lens * 0.52
    );
    float3 counterSample = refractBlurredField(
        input.uv,
        -offset * 0.34,
        dispersion * 0.3
    );
    float3 color = lerp(base, refracted, lens * 0.94);
    color += (refracted - counterSample) * lens * 0.24;
    color = adjustSaturation(
        color,
        max(effectFloat(7), 0.0) * lerp(0.76, 1.0, transparency)
    );
    color = saturate((color - 0.5) * 1.04 + 0.5);

    float4 tint = effectColor(3);
    float density = 1.0 - transparency;
    float3 diffusedBackdrop = adjustSaturation(base, 0.45);
    diffusedBackdrop = lerp(
        diffusedBackdrop,
        luminance(diffusedBackdrop).xxx,
        0.12
    );
    float2 contextUv = (
        effectBounds.xy + effectBounds.zw * 0.5
    ) / max(effectTargetSize, 1.0);
    float3 contextualBackdrop = adjustSaturation(
        sampleEffectSource(contextUv).rgb,
        0.34
    );
    contextualBackdrop = lerp(
        contextualBackdrop,
        luminance(contextualBackdrop).xxx,
        0.16
    );
    float contextMix = lerp(0.55, 0.92, density);
    float3 veilColor = lerp(
        diffusedBackdrop,
        contextualBackdrop,
        contextMix
    );
    veilColor = lerp(veilColor, tint.rgb, saturate(tint.a));
    color = lerp(color, veilColor, density);
    color = lerp(
        color,
        tint.rgb,
        saturate(tint.a) * lerp(0.35, 1.0, 1.0 - transparency)
    );

    // The boundary has an ambient response on every side. A coherent virtual
    // overhead light adds shape, while the opposing lobe stays broad and dim.
    // Rounded corners choose their own response through the SDF normal.
    float innerRim = 1.0 - smoothstep(0.0, 4.5, insideDistance);
    float hairline = 1.0 - smoothstep(0.3, 1.2, abs(distance + 0.42));
    float darkSeam = smoothstep(0.45, 1.0, insideDistance) *
        (1.0 - smoothstep(1.25, 2.15, insideDistance));
    float fresnel = pow(saturate(edgeProximity), 3.2) * innerRim;
    float meniscus = smoothstep(0.3, 2.0, insideDistance) *
        (1.0 - smoothstep(2.5, 8.0, insideDistance));
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
    float brightResponse = 1.0 - smoothstep(0.45, 0.88, environmentLuminance);
    float darkResponse = smoothstep(0.42, 0.82, environmentLuminance);
    float highlight = max(effectFloat(8), 0.0) *
        lerp(0.88, 1.08, transparency);
    float rimVisibility = 0.58 + brightResponse * 0.42;

    color += hairline * highlight * rimVisibility *
        (0.08 + darkBackdrop * 0.12) * ambientRimColor;
    color += fresnel * highlight * rimVisibility * 0.045 * ambientRimColor;
    color += meniscus * highlight * rimVisibility *
        (0.025 + keyFacing * 0.095) * ambientRimColor;
    color += keyLobe * highlight * (0.24 + darkBackdrop * 0.16) *
        float3(1.0, 0.985, 0.955);
    color += opposingLobe * highlight * 0.055 *
        lerp(float3(0.48, 0.56, 0.62), ambientRimColor, 0.45);
    color -= opposingLobe * (0.012 + (1.0 - darkBackdrop) * 0.012);
    color -= meniscus * opposingFacing * 0.018;
    color -= darkSeam * highlight * (0.018 + darkResponse * 0.055);

    // Static grain prevents banding without turning temporal sampling into a
    // shimmer. It is deliberately subordinate to the material lighting.
    float noise = (hash21(floor(input.logicalPixel)) - 0.5) *
        max(effectFloat(9), 0.0) * 0.01;
    color += noise;

    return float4(saturate(color), 1.0);
}
