float hash21(float2 value) {
    value = frac(value * float2(123.34, 456.21));
    value += dot(value, value + 45.32);
    return frac(value.x * value.y);
}

float band(float2 position, float phase, float offset, float width) {
    float wave = sin(position.x * 1.15 + phase) * 0.18;
    wave += sin(position.x * 2.7 - phase * 0.63) * 0.055;
    float distance = abs(position.y - wave - offset);
    return exp(-distance * distance / max(width * width, 0.0001));
}

float4 material(PixelInput input) {
    float2 size = max(materialSize(input), 1.0);
    float2 uv = input.uv;
    float time = materialTime() * 0.24;
    float intensity = saturate(materialFloat(input, 8));
    float3 tint = materialFloat4(input, 9).rgb;

    float aspect = size.x / size.y;
    float2 position = (uv - 0.5) * float2(aspect, 1.0);
    position += float2(
        sin(position.y * 2.2 + time) * 0.07,
        cos(position.x * 1.4 - time * 0.8) * 0.06
    );

    float cyan = band(position, time, -0.18, 0.18);
    float violet = band(position, 2.1 - time * 0.72, -0.32, 0.15);
    float pink = band(position, 4.7 + time * 0.51, 0.03, 0.12);

    float3 color = lerp(float3(0.008, 0.014, 0.055), input.color.rgb, 0.35);
    color += cyan * float3(0.02, 0.55, 0.95) * 0.9 * intensity;
    color += violet * lerp(float3(0.35, 0.08, 0.92), tint, 0.32) * 0.78 * intensity;
    color += pink * float3(0.95, 0.08, 0.48) * 0.54 * intensity;

    float2 orbPosition = position - float2(
        sin(time * 0.67) * aspect * 0.23,
        -0.14 + cos(time * 0.53) * 0.17
    );
    float orb = exp(-dot(orbPosition, orbPosition) * 2.8);
    color += orb * float3(0.22, 0.48, 1.0) * 0.34 * intensity;

    float2 starCell = floor(input.position.xy);
    float stars = step(0.9975, hash21(starCell));
    stars *= saturate(0.5 + 0.5 * sin(hash21(starCell) * 20.0 + time * 5.0));
    color += stars * 0.32;

    float grain = hash21(input.position.xy + floor(time * 30.0)) - 0.5;
    color += grain * 0.018;

    float vignette = saturate(1.0 - dot(uv - 0.5, uv - 0.5) * 1.55);
    color *= 0.62 + vignette * 0.38;
    return float4(
        saturate(color),
        input.color.a * materialCoverage(input) * materialOpacity(input)
    );
}
