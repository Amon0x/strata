float gridLine(float coordinate, float spacing, float width) {
    float centered = abs(frac(coordinate / spacing) - 0.5) * spacing;
    return 1.0 - smoothstep(width, width + 1.0, centered);
}

float4 material(PixelInput input) {
    float2 pixel = input.position.xy;
    float fineGrid = max(
        gridLine(pixel.x, 12.0, 0.8),
        gridLine(pixel.y, 12.0, 0.8)
    );
    float majorGrid = max(
        gridLine(pixel.x, 60.0, 1.6),
        gridLine(pixel.y, 60.0, 1.6)
    );
    float diagonal = gridLine(pixel.x + pixel.y * 0.72, 86.0, 2.2);
    float wave = gridLine(
        pixel.y + sin(pixel.x * 0.026) * 24.0,
        118.0,
        2.4
    );

    float3 color = float3(0.025, 0.035, 0.055);
    color = lerp(color, float3(0.20, 0.68, 0.94), fineGrid * 0.78);
    color = lerp(color, float3(0.86, 0.94, 1.0), majorGrid * 0.92);
    color = lerp(color, float3(1.0, 0.15, 0.38), diagonal * 0.96);
    color = lerp(color, float3(1.0, 0.82, 0.18), wave * 0.90);
    return float4(
        saturate(color),
        input.color.a * materialCoverage(input) * materialOpacity(input)
    );
}
