// Hyalite wallpapers.
//
// Seven scenes behind the glass, chosen by `scene` (float 8). They differ in palette and in shape
// so the material can be judged over soft colour, hard edges, neutral grey and pale paper alike:
//
//   0 Aurora    slow cool pools of colour with fine bright filaments (dark)
//   1 Dusk      a warm sky over a dark horizon, with a low sun (dark)
//   2 Marine    deep water with caustic light and shafts from above (dark)
//   3 Graphite  near-neutral grey with two soft lights and film grain (dark)
//   4 Orbs      flat, crisp-edged discs drifting over indigo (dark)
//   5 Daylight  the aurora field in pale daytime colours (light)
//   6 Paper     warm cream with soft diagonal bands and pale discs (light)
//
// Every scene moves slowly, and all share one warped coordinate field so a change of scene
// crossfades between two readings of the same motion.

float wallpaperHash(float2 value) {
    value = frac(value * float2(123.34, 456.21));
    value += dot(value, value + 45.32);
    return frac(value.x * value.y);
}

float wallpaperPool(float2 position, float2 center, float2 radius) {
    float2 local = (position - center) / radius;
    return exp(-dot(local, local));
}

// A disc with an antialiased edge about one pixel wide.
float wallpaperDisc(float2 position, float2 center, float radius, float pixel) {
    return 1.0 - smoothstep(radius - pixel, radius + pixel, length(position - center));
}

float3 wallpaperAurora(float2 flow, float2 uv, float aspect, float time, bool daylight) {
    float3 color = daylight
        ? lerp(float3(0.900, 0.940, 0.990), float3(0.985, 0.940, 0.930), uv.y)
        : lerp(float3(0.020, 0.030, 0.110), float3(0.050, 0.035, 0.180), uv.y);

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

    if (daylight) {
        color = lerp(color, float3(0.560, 0.780, 1.000), azure * 0.80);
        color = lerp(color, float3(0.640, 0.940, 0.900), cyan * 0.70);
        color = lerp(color, float3(0.800, 0.720, 1.000), violet * 0.72);
        color = lerp(color, float3(1.000, 0.760, 0.820), rose * 0.60);
    } else {
        color = lerp(color, float3(0.050, 0.290, 0.900), azure * 0.92);
        color = lerp(color, float3(0.100, 0.740, 0.900), cyan * 0.80);
        color = lerp(color, float3(0.420, 0.160, 0.880), violet * 0.86);
        color = lerp(color, float3(0.820, 0.220, 0.560), rose * 0.55);
    }

    // Folds: two broad interfering waves shade the surface like light raking across silk.
    float fold = sin(flow.x * 5.2 + flow.y * 3.4 + time * 1.2) * 0.6 +
        sin(flow.y * 6.1 - flow.x * 2.6 - time * 0.9 + 1.7) * 0.4;
    color *= daylight ? 0.955 + 0.055 * fold : 0.88 + 0.16 * fold;
    if (!daylight)
        color += pow(saturate(fold), 6.0) * 0.07 * (color + 0.25);

    // Filaments: thin bright lines along one set of isolines, fading in and out along their length.
    float strand = sin(flow.x * 7.0 - flow.y * 10.0 + sin(flow.y * 3.0 + time) * 1.8);
    float filament = pow(saturate(1.0 - abs(strand)), 30.0);
    float presence = 0.35 + 0.65 * saturate(sin(flow.x * 2.3 + flow.y * 1.1 - time * 0.6) * 0.5 + 0.5);
    color += filament * presence * (daylight ? 0.10 : 0.26) * float3(0.75, 0.90, 1.00);
    return color;
}

float3 wallpaperDusk(float2 flow, float2 position, float2 uv, float time) {
    // Sky: indigo above, plum through the middle, ember at the horizon.
    float3 color = lerp(float3(0.045, 0.035, 0.150), float3(0.300, 0.085, 0.260),
                        smoothstep(0.05, 0.62, uv.y));
    color = lerp(color, float3(0.930, 0.420, 0.180), smoothstep(0.55, 0.86, uv.y) * 0.85);
    // Ground: a dark band at the bottom, the way a skyline cuts a sunset.
    float ground = smoothstep(0.84, 0.88, uv.y + sin(position.x * 3.0) * 0.004);
    // A low sun: a soft corona and a firmer disc, drifting very slowly.
    float2 sun = float2(-0.38 + sin(time * 0.15) * 0.04, 0.33);
    float2 toSun = position - sun;
    float corona = exp(-dot(toSun, toSun) * 9.0);
    float disc = 1.0 - smoothstep(0.075, 0.095, length(toSun));
    color += float3(1.0, 0.62, 0.25) * corona * 0.55;
    color = lerp(color, float3(1.0, 0.86, 0.60), disc * 0.9);
    // Haze bands drifting across the horizon.
    float haze = sin(flow.y * 26.0 + flow.x * 2.0 + time * 0.8) * 0.5 + 0.5;
    color *= 1.0 - smoothstep(0.45, 0.85, uv.y) * (1.0 - smoothstep(0.80, 0.9, uv.y)) * haze * 0.10;
    color = lerp(color, float3(0.030, 0.022, 0.050), ground);
    return color;
}

float3 wallpaperMarine(float2 flow, float2 position, float2 uv, float time) {
    float3 color = lerp(float3(0.010, 0.150, 0.210), float3(0.004, 0.045, 0.090), uv.y);
    // Caustics: two interfering sine fields, sharpened, strongest near the surface.
    float field = sin(flow.x * 9.5 + flow.y * 6.0 + time * 1.3) *
        sin(flow.y * 8.0 - flow.x * 5.0 - time * 1.0);
    float caustic = pow(saturate(field * 0.5 + 0.5), 6.0);
    float depth = 1.0 - smoothstep(0.0, 0.9, uv.y);
    color += float3(0.45, 0.95, 0.95) * caustic * (0.12 + 0.30 * depth);
    // Shafts: soft diagonal columns of light falling from the upper left.
    float shaft = sin(position.x * 7.0 - position.y * 3.2 + time * 0.25) * 0.5 + 0.5;
    shaft = pow(shaft, 5.0) * (1.0 - smoothstep(-0.5, 0.45, position.y));
    color += float3(0.30, 0.80, 0.85) * shaft * 0.22;
    // Slow drift in the body colour.
    color = lerp(color, float3(0.000, 0.360, 0.420),
                 wallpaperPool(flow, float2(0.3, -0.1), float2(0.9, 0.6)) * 0.45);
    return color;
}

float3 wallpaperGraphite(float2 flow, float2 position, float2 uv, float time, float2 pixel) {
    float3 color = lerp(float3(0.090, 0.092, 0.100), float3(0.060, 0.062, 0.070), uv.y);
    // Two soft lights, one warm and one cool, so the grey is not dead.
    float warm = wallpaperPool(position, float2(-0.45 + sin(time * 0.3) * 0.05, -0.25),
                               float2(0.55, 0.45));
    float cool = wallpaperPool(position, float2(0.50 + cos(time * 0.25) * 0.05, 0.30),
                               float2(0.60, 0.50));
    color += float3(0.16, 0.14, 0.12) * warm;
    color += float3(0.10, 0.12, 0.16) * cool;
    // One broad diagonal band, barely lighter, for a shape to bend.
    float band = smoothstep(-0.05, 0.05, position.x * 0.6 + position.y - 0.1) *
        (1.0 - smoothstep(0.25, 0.35, position.x * 0.6 + position.y - 0.1));
    color += band * 0.035;
    // Film grain, fixed in place.
    color += (wallpaperHash(floor(pixel)) - 0.5) * 0.028;
    return color;
}

float3 wallpaperOrbs(float2 flow, float2 position, float2 uv, float time, float pixel) {
    float3 color = lerp(float3(0.095, 0.075, 0.260), float3(0.060, 0.045, 0.170), uv.y);
    // Flat discs with crisp edges. Each has a faint gradient across it so it reads as a shape, and
    // they overlap a little translucently.
    const float3 tints[5] = {
        float3(0.980, 0.420, 0.380),
        float3(1.000, 0.780, 0.300),
        float3(0.150, 0.760, 0.700),
        float3(0.600, 0.500, 1.000),
        float3(0.960, 0.560, 0.780)
    };
    const float2 anchors[5] = {
        float2(-0.55, -0.28), float2(0.05, 0.30), float2(0.58, -0.22),
        float2(-0.15, -0.55), float2(0.75, 0.42)
    };
    const float radii[5] = {0.30, 0.36, 0.26, 0.17, 0.23};
    [unroll]
    for (int index = 0; index < 5; ++index) {
        float phase = time * (0.18 + 0.05 * index) + index * 1.7;
        float2 center = anchors[index] + float2(sin(phase), cos(phase * 0.8)) * 0.05;
        float disc = wallpaperDisc(position, center, radii[index], pixel);
        float shade = 0.85 + 0.25 * (position.y - center.y) / radii[index];
        color = lerp(color, tints[index] * shade, disc * 0.88);
    }
    return color;
}

float3 wallpaperPaper(float2 flow, float2 position, float2 uv, float time, float pixel) {
    float3 color = lerp(float3(0.965, 0.945, 0.905), float3(0.935, 0.905, 0.860), uv.y);
    // Soft diagonal bands, a few percent apart.
    float band = sin((position.x + position.y * 1.4) * 5.0 + time * 0.2);
    color *= 1.0 + band * 0.022;
    // Pale discs: one sage, one peach, one sky.
    color = lerp(color, float3(0.78, 0.86, 0.76),
                 wallpaperDisc(position, float2(-0.50 + sin(time * 0.2) * 0.03, 0.28), 0.34, pixel) * 0.55);
    color = lerp(color, float3(0.98, 0.80, 0.68),
                 wallpaperDisc(position, float2(0.52, -0.30 + cos(time * 0.17) * 0.03), 0.30, pixel) * 0.55);
    color = lerp(color, float3(0.74, 0.83, 0.96),
                 wallpaperDisc(position, float2(0.18, 0.44), 0.18, pixel) * 0.50);
    // Paper tooth.
    color += (wallpaperHash(floor(float2(position.x, position.y) * 900.0)) - 0.5) * 0.012;
    return color;
}

float4 material(PixelInput input) {
    float2 size = max(materialSize(input), 1.0);
    float aspect = size.x / size.y;
    float2 position = (input.uv - 0.5) * float2(aspect, 1.0);
    float time = materialTime() * 0.10;
    float pixel = 1.0 / size.y;
    int scene = (int)floor(materialFloat(input, 8) + 0.5);

    // One warped coordinate field carries the soft scenes, so pools, folds and filaments move as a
    // single material instead of as stacked layers.
    float2 flow = position;
    flow += 0.22 * float2(sin(flow.y * 1.7 + time * 1.1), cos(flow.x * 1.4 - time * 0.9));
    flow += 0.10 * float2(sin(flow.y * 3.6 - time * 0.7 + 1.3), cos(flow.x * 3.1 + time * 0.8 + 2.1));

    float3 color;
    if (scene == 1) {
        color = wallpaperDusk(flow, position, input.uv, time);
    } else if (scene == 2) {
        color = wallpaperMarine(flow, position, input.uv, time);
    } else if (scene == 3) {
        color = wallpaperGraphite(flow, position, input.uv, time, input.position.xy);
    } else if (scene == 4) {
        color = wallpaperOrbs(flow, position, input.uv, time, pixel);
    } else if (scene == 5) {
        color = wallpaperAurora(flow, input.uv, aspect, time, true);
    } else if (scene == 6) {
        color = wallpaperPaper(flow, position, input.uv, time, pixel);
    } else {
        color = wallpaperAurora(flow, input.uv, aspect, time, false);
    }

    float vignette = saturate(1.0 - dot(position, position) * 0.38);
    bool light = scene == 5 || scene == 6;
    color *= light ? 0.94 + 0.06 * vignette : 0.74 + 0.26 * vignette;
    if (!light)
        color = color / (1.0 + color * 0.12) * 1.08;

    return float4(
        saturate(color),
        input.color.a * materialCoverage(input) * materialOpacity(input)
    );
}
