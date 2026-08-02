float hash21(float2 value) {
    value = frac(value * float2(123.34, 456.21));
    value += dot(value, value + 45.32);
    return frac(value.x * value.y);
}

float2 rotatePoint(float2 value, float angle) {
    float sine = sin(angle);
    float cosine = cos(angle);
    return float2(
        value.x * cosine - value.y * sine,
        value.x * sine + value.y * cosine
    );
}

float paintField(float2 position, float2 center, float2 radius, float angle) {
    float2 local = rotatePoint(position - center, angle) / max(radius, 0.0001);
    return exp(-dot(local, local) * 2.15);
}

float paintPool(float field, float edge) {
    return smoothstep(edge - 0.075, edge + 0.075, field);
}

float4 material(PixelInput input) {
    float2 size = max(materialSize(input), 1.0);
    float aspect = size.x / size.y;
    float2 position = (input.uv - 0.5) * float2(aspect, 1.0);

    float3 color = lerp(
        float3(0.018, 0.025, 0.16),
        float3(0.16, 0.025, 0.27),
        saturate(input.uv.x * 0.62 + input.uv.y * 0.38)
    );

    float cyan = paintPool(
        paintField(position, float2(-0.72, -0.32), float2(0.46, 0.24), -0.24) +
        paintField(position, float2(-0.46, -0.18), float2(0.36, 0.21), 0.42) +
        paintField(position, float2(-0.29, -0.34), float2(0.24, 0.15), -0.12),
        0.29
    );
    float orange = paintPool(
        paintField(position, float2(0.34, -0.34), float2(0.31, 0.19), -0.32) +
        paintField(position, float2(0.60, -0.25), float2(0.42, 0.25), 0.18) +
        paintField(position, float2(0.78, -0.04), float2(0.25, 0.19), -0.46),
        0.31
    );
    float magenta = paintPool(
        paintField(position, float2(-0.73, 0.28), float2(0.38, 0.27), 0.25) +
        paintField(position, float2(-0.43, 0.37), float2(0.46, 0.25), -0.28) +
        paintField(position, float2(-0.16, 0.25), float2(0.29, 0.19), 0.38),
        0.30
    );
    float violet = paintPool(
        paintField(position, float2(0.24, 0.27), float2(0.39, 0.26), -0.35) +
        paintField(position, float2(0.55, 0.34), float2(0.48, 0.29), 0.26) +
        paintField(position, float2(0.81, 0.18), float2(0.27, 0.19), -0.18),
        0.30
    );
    float lime = paintPool(
        paintField(position, float2(-0.10, -0.29), float2(0.24, 0.14), 0.18) +
        paintField(position, float2(0.08, -0.22), float2(0.23, 0.13), -0.34),
        0.34
    );

    color = lerp(color, float3(0.0, 0.76, 0.98), cyan * 0.94);
    color = lerp(color, float3(1.0, 0.20, 0.025), orange * 0.96);
    color = lerp(color, float3(1.0, 0.015, 0.46), magenta * 0.95);
    color = lerp(color, float3(0.46, 0.035, 1.0), violet * 0.91);
    color = lerp(color, float3(0.62, 1.0, 0.025), lime * 0.88);

    float cream = paintPool(
        paintField(position, float2(-0.09, -0.03), float2(0.12, 0.20), -0.38) +
        paintField(position, float2(0.02, 0.06), float2(0.11, 0.18), 0.32),
        0.34
    );
    float turquoiseStreak = paintPool(
        paintField(position, float2(-0.36, 0.08), float2(0.52, 0.055), -0.48),
        0.29
    );
    float pinkStreak = paintPool(
        paintField(position, float2(0.43, 0.02), float2(0.50, 0.052), 0.42),
        0.29
    );
    float opticTestStreak = paintPool(
        paintField(position, float2(-0.49, -0.21), float2(0.43, 0.034), 0.27),
        0.26
    );
    float roundedTestStreak = paintPool(
        paintField(position, float2(0.10, 0.08), float2(0.42, 0.032), -0.36),
        0.26
    );

    color = lerp(color, float3(1.0, 0.91, 0.70), cream * 0.88);
    color = lerp(color, float3(0.0, 0.98, 0.79), turquoiseStreak * 0.72);
    color = lerp(color, float3(1.0, 0.08, 0.68), pinkStreak * 0.70);
    color = lerp(color, float3(1.0, 0.46, 0.01), opticTestStreak * 0.94);
    color = lerp(color, float3(0.0, 0.94, 1.0), roundedTestStreak * 0.90);

    float grain = hash21(floor(input.position.xy)) - 0.5;
    color += grain * 0.01;

    return float4(
        saturate(color),
        input.color.a * materialCoverage(input) * materialOpacity(input)
    );
}
