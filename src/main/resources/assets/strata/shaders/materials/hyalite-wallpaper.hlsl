// Hyalite wallpaper.
//
// Slow pools of colour over a deep base, shaded by broad silk folds and crossed by a few fine
// bright filaments. Glass has nothing to show over a flat gradient: the folds give its bezel edges
// to bend, and the filaments give its blur something to soften.
//
// `daylight` (float 8) selects the palette: 0 is the night field, 1 a pale daytime one with the
// same motion. Glass that follows its backdrop reads the two very differently.

float wallpaperPool(float2 position, float2 center, float2 radius) {
    float2 local = (position - center) / radius;
    return exp(-dot(local, local));
}

float4 material(PixelInput input) {
    float2 size = max(materialSize(input), 1.0);
    float aspect = size.x / size.y;
    float2 position = (input.uv - 0.5) * float2(aspect, 1.0);
    float time = materialTime() * 0.10;

    // One warped coordinate field carries everything, so pools, folds and filaments move as a
    // single material instead of as stacked layers.
    float2 flow = position;
    flow += 0.22 * float2(sin(flow.y * 1.7 + time * 1.1), cos(flow.x * 1.4 - time * 0.9));
    flow += 0.10 * float2(sin(flow.y * 3.6 - time * 0.7 + 1.3), cos(flow.x * 3.1 + time * 0.8 + 2.1));

    float daylight = saturate(materialFloat(input, 8));
    float3 color = lerp(float3(0.020, 0.030, 0.110), float3(0.050, 0.035, 0.180), input.uv.y);
    float3 day = lerp(float3(0.900, 0.940, 0.990), float3(0.985, 0.940, 0.930), input.uv.y);

    float azure = wallpaperPool(flow,
        float2(-0.46 * aspect + sin(time * 0.9) * 0.10, -0.26 + cos(time * 0.7) * 0.06),
        float2(0.62, 0.46));
    float cyan = wallpaperPool(flow,
        float2(0.10 * aspect + cos(time * 0.6 + 1.0) * 0.12, -0.42 + sin(time * 0.8) * 0.05),
        float2(0.50, 0.30));
    float violet = wallpaperPool(flow,
        float2(0.44 * aspect + sin(time * 0.5 + 2.0) * 0.08, 0.02 + cos(time * 0.9 + 1.0) * 0.08),
        float2(0.52, 0.50));
    float rose = wallpaperPool(flow,
        float2(-0.04 * aspect + cos(time * 0.7 + 3.0) * 0.12, 0.40 + sin(time * 0.6 + 2.0) * 0.06),
        float2(0.60, 0.34));
    float amber = wallpaperPool(flow,
        float2(-0.48 * aspect + sin(time * 0.8 + 4.0) * 0.07, 0.44 + cos(time * 0.5) * 0.05),
        float2(0.36, 0.30));

    color = lerp(color, float3(0.050, 0.290, 0.900), azure * 0.92);
    color = lerp(color, float3(0.100, 0.740, 0.900), cyan * 0.80);
    color = lerp(color, float3(0.420, 0.160, 0.880), violet * 0.86);
    color = lerp(color, float3(0.960, 0.250, 0.520), rose * 0.84);
    color = lerp(color, float3(1.000, 0.600, 0.260), amber * 0.90);

    day = lerp(day, float3(0.560, 0.780, 1.000), azure * 0.80);
    day = lerp(day, float3(0.640, 0.940, 0.900), cyan * 0.70);
    day = lerp(day, float3(0.800, 0.720, 1.000), violet * 0.72);
    day = lerp(day, float3(1.000, 0.720, 0.800), rose * 0.74);
    day = lerp(day, float3(1.000, 0.860, 0.600), amber * 0.80);

    // Folds: two broad interfering waves shade the surface like light raking across silk.
    float foldA = sin(flow.x * 5.2 + flow.y * 3.4 + time * 1.2);
    float foldB = sin(flow.y * 6.1 - flow.x * 2.6 - time * 0.9 + 1.7);
    float fold = foldA * 0.6 + foldB * 0.4;
    color *= 0.88 + 0.16 * fold;
    color += pow(saturate(fold), 6.0) * 0.07 * (color + 0.25);
    day *= 0.955 + 0.055 * fold;

    // Filaments: thin bright lines along one set of isolines, fading in and out along their length
    // so they read as caustics rather than as contour lines.
    float strand = sin(flow.x * 7.0 - flow.y * 10.0 + sin(flow.y * 3.0 + time) * 1.8);
    float filament = pow(saturate(1.0 - abs(strand)), 30.0);
    float presence = 0.35 + 0.65 * saturate(sin(flow.x * 2.3 + flow.y * 1.1 - time * 0.6) * 0.5 + 0.5);
    color += filament * presence * float3(0.75, 0.90, 1.00) * 0.26;
    day += filament * presence * 0.10;

    float vignette = saturate(1.0 - dot(position, position) * 0.38);
    color *= 0.74 + 0.26 * vignette;
    color = color / (1.0 + color * 0.12) * 1.08;
    color = lerp(color, day * (0.94 + 0.06 * vignette), daylight);

    return float4(
        saturate(color),
        input.color.a * materialCoverage(input) * materialOpacity(input)
    );
}
