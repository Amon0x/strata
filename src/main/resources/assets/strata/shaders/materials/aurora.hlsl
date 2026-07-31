// Authored material: cool light moving through a dark header surface. Motion must remain readable
// at a glance: a broad travelling window carries two independently moving ribbons, while the
// unlit part stays close to the authored surface colour.
// Declared parameters, in declaration order: intensity (float, slot 8), tint (color, slots 9..12).

float ribbon(float2 position, float phase, float thickness, float amplitude) {
    const float wave = sin(position.x * 1.45 + phase) * amplitude +
                       sin(position.x * 3.6 - phase * 0.7) * amplitude * 0.30;
    const float distance = abs(position.y - wave);
    return smoothstep(thickness, 0.0, distance) * 0.45 +
           smoothstep(thickness * 3.0, 0.0, distance) * 0.55;
}

float4 material(PixelInput input) {
    const float2 size = max(materialSize(input), 1.0);
    const float intensity = saturate(materialFloat(input, 8));
    const float4 tint = materialFloat4(input, 9);
    const float2 uv = input.uv;
    const float time = materialTime();

    // Aspect-correct x keeps waves the same physical size as the header changes width.
    const float aspect = size.x / size.y;
    const float2 position = float2(uv.x * aspect, uv.y - 0.5);

    // These speeds move the ribbons by roughly half a header-height per second. The previous
    // near-static phase shift was technically animated but visually indistinguishable from noise.
    float glow = ribbon(position, 0.6 + time * 0.82, 0.18, 0.11);
    glow += ribbon(position, 2.4 - time * 0.47, 0.12, 0.15) * 0.62;

    // A broad light window crosses the complete header every eighteen seconds. The seam is wrapped
    // and soft, so the animation loops without a pop.
    const float travel = abs(frac(uv.x - time * 0.055 + 0.5) - 0.5);
    const float movingWindow = smoothstep(0.34, 0.04, travel);
    glow *= 0.28 + movingWindow * 0.72;
    glow = saturate(glow * intensity);

    const float sheen = smoothstep(0.09, 0.0, uv.y) * intensity * 0.10;

    const float3 light = lerp(float3(0.62, 0.72, 1.0), tint.rgb, saturate(tint.a));
    const float3 shaded = input.color.rgb + light * (glow * 0.22 + sheen);
    return float4(saturate(shaded), input.color.a * materialCoverage(input) * materialOpacity(input));
}
