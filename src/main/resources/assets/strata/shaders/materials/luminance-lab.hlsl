// Diagnostic wallpaper for glass materials. It deliberately spans the whole range a universal
// surface has to survive: a black-to-white ramp, fully saturated hues, hard luminance edges, and a
// high-frequency stripe band that exposes refraction ghosting.
float labStripe(float coordinate, float spacing, float width) {
    float centered = abs(frac(coordinate / spacing) - 0.5) * spacing;
    return 1.0 - smoothstep(width, width + 1.0, centered);
}

float labBox(float2 pixel, float2 minimum, float2 maximum) {
    float2 inside = step(minimum, pixel) * step(pixel, maximum);
    return inside.x * inside.y;
}

float4 material(PixelInput input) {
    float2 pixel = input.position.xy;
    float2 size = max(materialSize(input), 1.0);
    float2 uv = pixel / size;

    // Diagonal black-to-white ramp: every glass element straddles several stops of luminance.
    float ramp = saturate((uv.x * 0.78 + uv.y * 0.22));
    float3 color = lerp(float3(0.01, 0.012, 0.02), float3(1.0, 0.99, 0.97), ramp);

    // Saturated hue blocks so chroma handling is visible, not just value handling.
    float3 hues[4] = {
        float3(0.95, 0.06, 0.20),
        float3(0.05, 0.85, 0.35),
        float3(0.10, 0.35, 1.00),
        float3(1.00, 0.78, 0.05)
    };
    for (int index = 0; index < 4; ++index) {
        float2 center = float2(0.14 + float(index) * 0.24, 0.16);
        float2 local = (uv - center) / float2(0.085, 0.10);
        float blob = 1.0 - smoothstep(0.85, 1.0, length(local));
        color = lerp(color, hues[index], blob);
    }

    // Hard-edged extremes: a glass element sitting across one of these has to stay coherent.
    color = lerp(color, float3(1.0, 1.0, 1.0), labBox(uv, float2(0.06, 0.60), float2(0.30, 0.78)));
    color = lerp(color, float3(0.0, 0.0, 0.0), labBox(uv, float2(0.70, 0.60), float2(0.94, 0.78)));

    // High-frequency band: fine stripes read like body text under a surface.
    float band = labBox(uv, float2(0.0, 0.84), float2(1.0, 0.98));
    float stripes = max(
        labStripe(pixel.y, 7.0, 1.1),
        labStripe(pixel.x + pixel.y * 0.4, 23.0, 1.4)
    );
    color = lerp(color, 1.0 - color, band * stripes * 0.92);

    return float4(
        saturate(color),
        input.color.a * materialCoverage(input) * materialOpacity(input)
    );
}
