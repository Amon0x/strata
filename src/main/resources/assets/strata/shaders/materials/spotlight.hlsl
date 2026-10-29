// A card lit by the pointer: a soft spotlight inside it and a rim of light along its edge, both
// strongest where the pointer is. The pointer comes from materialPointer(), so the light follows
// it on the GPU every frame and reaches across neighbouring cards without the interface changing.

float4 material(PixelInput input) {
    float2 size = max(materialSize(input), 1.0);
    float3 pointer = materialPointer(input);
    float reach = max(materialFloat(input, 8), 1.0);
    float3 glow = materialFloat4(input, 9).rgb;

    float2 local = input.uv * size;
    float near = pointer.z * saturate(1.0 - length(local - pointer.xy) / reach);
    float spot = near * near;

    // The rim: a band just inside the silhouette, lit farther out than the spot.
    float distance = materialDistance(input);
    float band = 1.0 - smoothstep(0.0, 1.6, abs(distance + 1.0));
    float rim = band * pointer.z * saturate(1.0 - length(local - pointer.xy) / (reach * 1.7));

    // A soft gradient on a dark card bands in 8 bits; a pixel of noise hides the steps.
    float noise = frac(sin(dot(input.position.xy, float2(12.9898, 78.233))) * 43758.5453) - 0.5;
    float3 color = input.color.rgb + glow * (spot * 0.20 + rim * 0.95) + noise / 255.0;
    float alpha = input.color.a + rim * 0.6;
    return float4(color, saturate(alpha) * materialCoverage(input) * materialOpacity(input));
}
