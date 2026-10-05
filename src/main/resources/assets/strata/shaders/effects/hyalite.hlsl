// Hyalite glass.
//
// One sheet of clear glass with a rounded edge, lit from the upper left.
//
// Body: the backdrop passes through almost untouched - a light blur from the pass before this one,
// a little extra saturation, and a tint. Nothing is tone-mapped, so the surface reads as glass over
// whatever is behind it instead of as a painted panel.
//
// Bezel: the outer band is a convex lens. Its slope follows a superellipse profile and the ray is
// bent with Snell's law, so the displacement is near zero across most of the band and rises
// steeply at the silhouette. Rays bend inward, which keeps every sample inside the blurred rect and
// folds the outermost pixels into a compressed mirror of the content beside them - the signature
// of a thick glass edge.
//
// Light: a soft two-pixel specular rim on the silhouette that is bright only where the edge faces
// the key light and, more weakly, where that light leaves the glass on the far side. The face
// itself carries no shading, so the surface stays a sheet rather than a bevelled slab.
//
// Parameter slots, in declaration order:
//   0 blur        1 downsample   (consumed by the blur pass)
//   2 tint (rgb = colour, a = strength)
//   6 refraction  7 bezel        8 dispersion   9 saturation
//  10 specular   11 brightness  12 shade
//
// The widget's tone (effectTone) selects the scheme: see "Scheme" below.

static const float glassIor = 1.5;
static const float3 glassLuma = float3(0.2126, 0.7152, 0.0722);

float glassHash(float2 value) {
    value = frac(value * float2(123.34, 456.21));
    value += dot(value, value + 45.32);
    return frac(value.x * value.y);
}

float4 effect(EffectInput input) {
    float4 tint = effectColor(2);
    float refraction = max(effectFloat(6), 0.0);
    float bezelLogical = max(effectFloat(7), 0.0);
    float dispersion = effectFloat(8);
    float saturation = max(effectFloat(9), 0.0);
    float specular = max(effectFloat(10), 0.0);
    float brightness = max(effectFloat(11), 0.0);
    float shade = saturate(effectFloat(12));

    float2 targetSize = max(effectTargetSize, 1.0);
    float2 texel = 1.0 / targetSize;
    float pixelScale = targetSize.x / max(effectLogicalSize.x, 1.0);

    // Shape. The lens uses a field whose corners are at least as round as the bezel is wide, so the
    // surface normal stays continuous across the whole band even on a tightly cornered card. The
    // silhouette itself is still the authored one.
    float2 halfSize = effectBounds.zw * 0.5;
    float2 centered = input.pixel - (effectBounds.xy + halfSize);
    float minimumHalf = min(halfSize.x, halfSize.y);
    float cornerRadius = min(effectCornerRadius(centered), minimumHalf);
    float bezel = clamp(bezelLogical * pixelScale, 0.001, minimumHalf);
    float lensRadius = min(max(cornerRadius, bezel), minimumHalf);

    float2 q = abs(centered) - halfSize + lensRadius;
    float2 corner = max(q, 0.0);
    float cornerLength = length(corner);
    float lensDepth = lensRadius - cornerLength - min(max(q.x, q.y), 0.0);
    float2 side = float2(centered.x < 0.0 ? -1.0 : 1.0, centered.y < 0.0 ? -1.0 : 1.0);
    float2 outward = cornerLength > 0.0001
        ? corner / cornerLength * side
        : (q.x > q.y ? float2(side.x, 0.0) : float2(0.0, side.y));
    float edgeDepth = max(-effectDistance(input.pixel), 0.0);

    // Bezel profile: u is 1 on the silhouette and 0 where the bezel meets the flat face.
    float u = saturate(1.0 - lensDepth / bezel);
    float u2 = u * u;
    float slope = pow(u, 1.4) / pow(max(1.0 - pow(u, 2.4), 0.0) + 0.03, 0.583);

    // Snell: a vertical ray meets the tilted surface at atan(slope) and leaves at the refracted
    // angle. tan(incident - refracted) is how far it drifts per unit of glass depth.
    float inverseHypotenuse = rsqrt(1.0 + slope * slope);
    float sinIncident = slope * inverseHypotenuse;
    float cosIncident = inverseHypotenuse;
    float sinRefracted = sinIncident / glassIor;
    float cosRefracted = sqrt(1.0 - sinRefracted * sinRefracted);
    float drift = (sinIncident * cosRefracted - cosIncident * sinRefracted) /
        (cosIncident * cosRefracted + sinIncident * sinRefracted);
    float displacement = bezel * refraction * drift;

    // Dispersion splits the bent ray by wavelength. It scales with the bend itself, so the flat face
    // stays perfectly neutral and only the edge fringes.
    float2 bend = -outward * displacement * texel;
    float spread = dispersion * 0.16;
    float3 transmitted = float3(
        sampleEffectSource(input.uv + bend * (1.0 - spread)).r,
        sampleEffectSource(input.uv + bend).g,
        sampleEffectSource(input.uv + bend * (1.0 + spread)).b
    );

    // Body.
    float luma = dot(transmitted, glassLuma);
    float3 color = lerp(luma.xxx, transmitted, saturation) * brightness;
    float bodyLuma = dot(color, glassLuma);

    // Scheme. The widget's tone says which ink its content is drawn in: 0 is light ink over a dark
    // backdrop, 1 is dark ink over a light one. The body is held inside the band that ink needs,
    // and `shade` is how firmly. Dark glass rolls bright backdrops off and takes the tint as
    // authored; light glass lifts dark backdrops toward white and mirrors a dark tint to a pale
    // one, while a tint that is already light or saturated (lit or accent glass) stays as it is.
    float scheme = saturate(effectTone());
    float3 darkBody = color * (1.0 - shade * smoothstep(0.25, 1.0, bodyLuma) * 0.55);
    darkBody = lerp(darkBody, tint.rgb, saturate(tint.a));

    float lift = (0.30 + 0.55 * shade) * (1.0 - smoothstep(0.30, 0.95, bodyLuma));
    float3 lightBody = lerp(color, float3(1.0, 1.0, 1.0), lift);
    float tintHigh = max(tint.r, max(tint.g, tint.b));
    float tintLow = min(tint.r, min(tint.g, tint.b));
    float darkTint = saturate((0.5 - dot(tint.rgb, glassLuma)) * 4.0) *
        saturate(1.0 - (tintHigh - tintLow) * 2.5);
    float3 paleTint = lerp(tint.rgb, float3(0.97, 0.98, 1.0), darkTint);
    lightBody = lerp(lightBody, paleTint, saturate(tint.a * (1.0 + 0.6 * darkTint)));

    color = lerp(darkBody, lightBody, scheme);

    // Light. Screen space has y pointing down, so the upper-left key light is (-x, -y). The lobes
    // are narrow: the rim is bright only along the arc that really faces the light and where that
    // light leaves the glass on the far side, and falls to almost nothing in between. A rim lit
    // evenly all the way round is what turns glass into a framed slab.
    float2 keyLight = normalize(float2(-0.62, -0.78));
    float facing = dot(outward, keyLight);
    float keyLobe = pow(saturate(facing), 2.0);
    float counterLobe = pow(saturate(-facing), 2.6);
    float caught = keyLobe + counterLobe * 0.45;

    // Rim: a band of specular that starts on the silhouette and falls off smoothly over about two
    // pixels. Its outer side is the silhouette itself, which the compositor's coverage already
    // antialiases, and its inner side is a gaussian wider than a pixel, so it never beads along a
    // curve the way a line narrower than the pixel grid does. It keeps a trace of itself on the
    // unlit arcs so the outline never breaks, and takes some colour from the glass beneath it so
    // it belongs to the scene rather than being drawn on top of it.
    float rimDepth = edgeDepth / (1.7 * pixelScale);
    float rim = exp(-rimDepth * rimDepth);
    float3 rimColor = lerp(float3(1.0, 1.0, 1.0), saturate(color * 1.5 + 0.3), 0.3);
    color += rimColor * rim * (0.10 + caught * 0.90) * specular * 0.74;

    // Spill: the caught light bleeds a few pixels into the glass, so the rim reads as light
    // entering the edge instead of as a stroke.
    float spill = exp(-edgeDepth / (3.2 * pixelScale)) * caught;
    color += spill * specular * 0.10;

    color += (glassHash(floor(input.pixel)) - 0.5) * (1.5 / 255.0);
    return float4(saturate(color), 1.0);
}
