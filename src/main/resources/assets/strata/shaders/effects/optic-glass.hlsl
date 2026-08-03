float luminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float3 adjustSaturation(float3 color, float saturation) {
    return lerp(luminance(color).xxx, color, saturation);
}

float3 softLight(float3 base, float3 blend) {
    return saturate(
        (1.0 - 2.0 * blend) * base * base +
        2.0 * blend * base
    );
}

float hash21(float2 value) {
    value = frac(value * float2(123.34, 456.21));
    value += dot(value, value + 45.32);
    return frac(value.x * value.y);
}

float2 shapeNormal(float2 pixel) {
    float2 gradient = float2(
        effectDistance(pixel + float2(1.0, 0.0)) -
            effectDistance(pixel - float2(1.0, 0.0)),
        effectDistance(pixel + float2(0.0, 1.0)) -
            effectDistance(pixel - float2(0.0, 1.0))
    );
    return normalize(gradient + float2(0.0001, 0.0001));
}

float4 effect(EffectInput input) {
    float signedDistance = effectDistance(input.pixel);
    float insideDistance = max(-signedDistance, 0.0);
    float2 normal2d = shapeNormal(input.pixel);
    float2 texel = 1.0 / max(effectTargetSize, 1.0);

    float transparency = saturate(effectFloat(10));
    float thickness = max(effectFloat(11), 0.05);
    float scattering = saturate(effectFloat(12));
    float edgeContrast = max(effectFloat(13), 0.0);
    float bodyContrast = max(effectFloat(14), 0.0);
    float luminosity = max(effectFloat(15), 0.0);

    float minimumExtent = max(min(effectBounds.z, effectBounds.w), 1.0);
    float sizeFactor = smoothstep(48.0, 260.0, minimumExtent);
    float bendZone = clamp(
        minimumExtent * 0.24 * thickness,
        10.0,
        90.0
    );
    float bendCoordinate = saturate(insideDistance / bendZone);
    float surfaceCurve = pow(1.0 - bendCoordinate, 1.35);
    float refractionField = pow(1.0 - bendCoordinate, 2.0);

    float displacementPixels = bendZone * 0.35 *
        max(effectFloat(2), 0.0);
    float2 refractedOffset = -normal2d * displacementPixels *
        refractionField * texel;
    float chromaticFraction =
        1.2 / max(displacementPixels, 1.0) *
        smoothstep(0.0, 2.5, insideDistance);

    float2 surfaceCenter = (
        effectBounds.xy + effectBounds.zw * 0.5
    ) / max(effectTargetSize, 1.0);
    float magnification = lerp(
        0.01,
        0.03,
        saturate((thickness - 0.45) / 1.35)
    );
    magnification *= lerp(0.78, 1.12, sizeFactor);
    float2 bodyUv = surfaceCenter +
        (input.uv - surfaceCenter) * (1.0 - magnification);
    float3 body = sampleEffectSource(bodyUv).rgb;
    // EffectBackdrop is vertically inverted relative to EffectSource/input.uv
    // in the render-target path. Flip only offset deltas so both samplers move
    // through their images in the same visual direction.
    float2 backdropDelta = refractedOffset * float2(1.0, -1.0);
    float3 rawRefraction = float3(
        sampleEffectBackdrop(
            input.uv + backdropDelta * (1.0 - chromaticFraction)
        ).r,
        sampleEffectBackdrop(
            input.uv + backdropDelta
        ).g,
        sampleEffectBackdrop(
            input.uv + backdropDelta * (1.0 + chromaticFraction)
        ).b
    );
    float3 color = lerp(
        body,
        rawRefraction,
        refractionField * 0.96
    );

    float saturation = max(effectFloat(7), 0.0);
    color = adjustSaturation(color, saturation);
    body = adjustSaturation(body, saturation);

    // The filtered scene is the transmitted layer and tint is the authored
    // surface layer. Transparency attenuates transmission; tint alpha remains
    // the minimum surface density at full transmission.
    float4 tint = effectColor(3);
    float tintAmount = saturate(tint.a);
    float surfaceOpacity = saturate(
        tintAmount +
        (1.0 - transparency) * (1.0 - tintAmount) *
            lerp(0.42, 0.58, sizeFactor)
    );
    float3 tintedSurface = softLight(color, tint.rgb);
    color = lerp(color, tintedSurface, surfaceOpacity);
    float3 tonalAnchor = lerp(
        body,
        softLight(body, tint.rgb),
        surfaceOpacity
    );
    float effectiveContrast = bodyContrast * lerp(0.72, 1.0, transparency);
    color = tonalAnchor + (color - tonalAnchor) * effectiveContrast;
    color *= luminosity;

    // The supplied controls share a two-pixel bright boundary followed by a
    // one-pixel darker contact seam. Interpolation toward white reproduces the
    // same response on colored, dark, and already-white surfaces.
    float brightBoundary = 1.0 - smoothstep(
        0.9,
        2.25,
        insideDistance
    );
    float contactSeam = smoothstep(1.75, 2.35, insideDistance) *
        (1.0 - smoothstep(2.9, 3.65, insideDistance));
    float lowerFacing = normal2d.y * 0.5 + 0.5;
    float seamStrength = lerp(0.08, 0.22, lowerFacing);
    float highlight = max(effectFloat(8), 0.0);
    float3 compressedBackground = saturate(rawRefraction * 1.14);
    float3 refractedRim = 1.0 -
        (1.0 - color) * (1.0 - compressedBackground);
    color = lerp(
        color,
        refractedRim,
        saturate(brightBoundary * edgeContrast * 0.24)
    );

    // The virtual light is fixed in surface space, so moving or morphing a
    // glass element changes the reflected lobe without autonomous shimmer.
    float2 lightVector = float2(0.24, 0.08) - surfaceCenter;
    float3 lightDirection = normalize(float3(lightVector * 0.85, 0.78));
    float3 surfaceNormal = normalize(float3(
        normal2d * surfaceCurve * 1.18,
        1.0
    ));
    float3 halfDirection = normalize(
        lightDirection + float3(0.0, 0.0, 1.0)
    );
    float specularLobe = pow(
        saturate(dot(surfaceNormal, halfDirection)),
        96.0
    ) * refractionField;
    float2 planarLight = normalize(lightVector + float2(0.0001, 0.0001));
    float lightFacing = pow(saturate(dot(normal2d, planarLight)), 5.0);
    float counterFacing = pow(saturate(dot(normal2d, -planarLight)), 7.0);
    float whiteGlint = brightBoundary * lightFacing *
        edgeContrast * highlight * 0.34;
    color = lerp(
        color,
        1.0,
        saturate(specularLobe * highlight * 0.62 + whiteGlint)
    );
    color = lerp(
        color,
        compressedBackground,
        saturate(
            brightBoundary * counterFacing *
            edgeContrast * 0.11
        )
    );
    color *= 1.0 - contactSeam * edgeContrast * seamStrength;

    float noise = (hash21(floor(input.logicalPixel)) - 0.5) *
        max(effectFloat(9), 0.0) * 0.01;
    color += noise;

    return float4(saturate(color), 1.0);
}
