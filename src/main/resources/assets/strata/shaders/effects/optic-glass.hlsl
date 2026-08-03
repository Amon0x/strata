// Liquid optic glass.
//
// Body: the transmitted scene is remapped into a bounded luminance band while its chroma is carried
// through, so the backdrop still reads as colour and structure but foreground text keeps a stable
// contrast floor over white, black, and saturated content alike.
//
// Edge: refraction is confined to a narrow bevel. A wide, weak lens smears background text across
// the whole surface; a narrow, steep one reads as real glass thickness and cannot ghost inward.
//
// Rim: a thin metal band that reflects the immediate surroundings, so it belongs to whatever is
// behind the element instead of imposing a fixed colour. EffectSource is blurred only inside the
// effect rect, so sampling outward returns the unfiltered neighbourhood - exactly what a polished
// edge shows - while every inward sample stays filtered.

float luminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
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

float3 sourceAt(float2 uv) {
    return sampleEffectSource(uv).rgb;
}

float4 effect(EffectInput input) {
    float2 texel = 1.0 / max(effectTargetSize, 1.0);
    float insideDistance = max(-effectDistance(input.pixel), 0.0);
    float2 normal = shapeNormal(input.pixel);

    float refractionAmount = max(effectFloat(2), 0.0);
    float4 tint = effectColor(3);
    float vibrancy = max(effectFloat(7), 0.0);
    float highlight = max(effectFloat(8), 0.0);
    float dither = max(effectFloat(9), 0.0);
    float transparency = saturate(effectFloat(10));
    float thickness = max(effectFloat(11), 0.05);
    float metallic = saturate(effectFloat(12));
    float edgeContrast = max(effectFloat(13), 0.0);
    float bodyContrast = max(effectFloat(14), 0.0);
    float luminosity = max(effectFloat(15), 0.0);

    float minimumExtent = max(min(effectBounds.z, effectBounds.w), 1.0);
    float sizeFactor = smoothstep(40.0, 260.0, minimumExtent);

    // Edge lens. The bevel is a quarter-round profile: its slope is near zero across the interior
    // and rises steeply at the boundary, so displacement is concentrated in a few pixels.
    float lensWidth = clamp(minimumExtent * 0.17 * thickness, 5.0, 26.0);
    float rise = 1.0 - saturate(insideDistance / lensWidth);
    float slope = rise * rise / (sqrt(saturate(1.0 - rise * rise)) + 0.19);
    float lens = saturate(slope * 0.6);
    float displacement = min(lensWidth * 0.46 * refractionAmount * slope, lensWidth * 1.15);
    float2 lensUv = input.uv + normal * displacement * texel;

    // Dispersion stays inside the outermost band, where the image is compressed anyway, so it never
    // doubles legible background text.
    float2 dispersion = normal * displacement * 0.09 * lens * texel;
    float3 transmitted = float3(
        sourceAt(lensUv - dispersion).r,
        sourceAt(lensUv).g,
        sourceAt(lensUv + dispersion).b
    );

    // Local backdrop average. The surface adapts to what is directly behind it rather than to one
    // element-wide constant, so a single card can cross black and white and stay coherent.
    float2 neighbourStep = clamp(minimumExtent * 0.16, 9.0, 34.0) * texel;
    float3 neighbourhood = (
        sourceAt(input.uv) +
        sourceAt(input.uv + float2(neighbourStep.x, 0.0)) +
        sourceAt(input.uv - float2(neighbourStep.x, 0.0)) +
        sourceAt(input.uv + float2(0.0, neighbourStep.y)) +
        sourceAt(input.uv - float2(0.0, neighbourStep.y))
    ) * 0.2;
    float meanLuma = luminance(neighbourhood);
    float brightField = smoothstep(0.10, 0.62, meanLuma);

    // Bounded band. The transmitted scene is tone-compressed into [floor, ceiling] rather than
    // blended back toward its own brightness, so the surface can never run away on a white backdrop.
    // The floor is lifted over dark content and dropped under bright content, so the element always
    // separates itself from what it sits on instead of dissolving into a black backdrop.
    // `transparency` sets the knee: an open surface keeps more of the scene's contrast.
    float bandLow = 0.028 + lerp(0.085, 0.010, brightField);
    float bandHigh = lerp(0.36, 0.30, brightField) * lerp(0.90, 1.0, sizeFactor);
    float knee = lerp(2.8, 1.05, transparency) + saturate(tint.a) * 2.5;
    float normalizer = 1.0 + knee;

    float sceneLuma = luminance(transmitted);
    float3 chroma = transmitted - sceneLuma;
    float bodyLuma = lerp(
        bandLow,
        bandHigh,
        saturate(sceneLuma / (sceneLuma + knee) * normalizer)
    );
    float anchor = lerp(
        bandLow,
        bandHigh,
        saturate(meanLuma / (meanLuma + knee) * normalizer)
    );
    bodyLuma = anchor + (bodyLuma - anchor) * bodyContrast;

    // Chroma is rescaled with the luminance change so saturation survives the compression instead
    // of collapsing to grey, then `vibrancy` lifts it the way tinted glass exaggerates colour.
    float chromaScale = clamp(bodyLuma / max(sceneLuma, 0.02), 0.0, 1.7);
    float3 body = bodyLuma + chroma * lerp(1.0, chromaScale, 0.85) * vibrancy * 1.15;

    // Bounding luminance alone is not enough: a saturated backdrop can hold its luminance inside the
    // band while a single channel runs away, which is how tinted glass turns into coloured plastic.
    // Scaling the whole triple back keeps the hue and restores the ceiling.
    float peak = max(body.r, max(body.g, body.b));
    body *= min(1.0, bandHigh * 1.55 / max(peak, 0.0001));

    // The tint colours the glass as a normalised gain, so it filters the scene rather than painting
    // an opaque layer over it.
    // Bounded, because normalising by luminance alone makes a darker or more saturated tint push
    // harder - the opposite of what picking a subtle dark tint should do.
    float3 tintGain = clamp(tint.rgb / max(luminance(tint.rgb), 0.05), 0.72, 1.28);
    body *= lerp(1.0, tintGain, saturate(tint.a * 6.0));

    // A fixed virtual key light. It is fixed in surface space, so moving or resizing an element
    // changes which part of its perimeter is lit without introducing autonomous shimmer.
    float facing = dot(normal, normalize(float2(-0.30, -0.95)));
    float keyArc = smoothstep(0.05, 0.92, facing);
    float counterArc = smoothstep(0.10, 0.98, -facing);

    // Gloss. Grazing reflection approaches total at the silhouette, so the surface brightens toward
    // its outline, driven by the shape's own distance field so it stays symmetric and follows the
    // real outline including corner radii. The band has a bounded width rather than one proportional
    // to the element: scaling it with the size makes every surface read as half a glass cylinder,
    // because on a short element the glow from opposite edges meets and no flat face is left.
    float sheenWidth = clamp(minimumExtent * 0.20, 7.0, 34.0) * thickness;
    float sheenDepth = saturate(insideDistance / sheenWidth);
    float sheen = pow(1.0 - sheenDepth, 2.2);
    body *= 1.0 - sheenDepth * 0.035 * highlight;
    body += min(sheen * 0.11 * highlight, 0.13);
    body *= luminosity;


    // Metal rim. The reflection is taken from outside the silhouette, partially desaturated and
    // contrast-expanded: metal keeps some of the environment's hue but compresses its value range.
    float rimWidth = clamp(minimumExtent * 0.045, 1.5, 3.4) * lerp(0.90, 1.15, thickness);
    float reflectDistance = insideDistance + rimWidth * 1.6 + 3.0;
    float2 reflectUv = input.uv + normal * reflectDistance * texel;
    // The reflection is dispersed along the normal: a curved polished edge separates wavelengths by
    // reflection depth, so sampling each channel from a different depth produces a fringe made of
    // the actual surroundings rather than a painted-on gradient. It stays inside the chamfer, so it
    // never reaches anything legible.
    // The split is blended back against the undispersed reflection rather than used raw: where the
    // reflection happens to straddle a hard black/white boundary, a raw split fringes into a
    // saturated stripe that reads as a rendering fault. Blending bounds the worst case to a tint
    // while a smooth environment still separates visibly.
    float2 dispersionStep = normal * (rimWidth * 0.25 + 0.5) * texel;
    float3 flatReflection = sourceAt(reflectUv);
    float3 environment = lerp(
        flatReflection,
        float3(
            sourceAt(reflectUv - dispersionStep).r,
            flatReflection.g,
            sourceAt(reflectUv + dispersionStep).b
        ),
        0.35
    );
    // Conductors keep their chroma under reflection, so the desaturation stays mild. The peak is
    // rescaled rather than clipped, because clipping each channel at 1.0 independently is exactly
    // what collapses a bright coloured reflection into white.
    float3 metal = max(lerp(luminance(environment).xxx, environment, 0.68) * 1.22 + 0.015, 0.0);
    metal *= min(1.0, 1.0 / max(max(metal.r, max(metal.g, metal.b)), 0.0001));

    // The key light splits the perimeter into a lit arc, a dimmer counter-reflection, and darker
    // flanks. That uneven distribution is what separates metal from a uniformly bright border.
    float angularGain = 0.42 + keyArc * 0.80 + counterArc * 0.45;
    // A small ambient term keeps the unlit flanks present, so the element reads as a closed ring
    // rather than a highlight strip. It leans on dark backdrops, where there is nothing to reflect.
    float rimAmbient = 0.030 + 0.055 * (1.0 - brightField);

    // Bevel profile. A chamfer reads as metal because of how fast it changes: a tight specular line
    // just inside the silhouette, a reflective mid-tone body, then an occluded line where the metal
    // meets the glass. A single smooth ramp across the same width reads as plastic instead.
    float rimDepth = saturate(insideDistance / rimWidth);
    float specularLine = exp(-pow((rimDepth - 0.28) / 0.34, 2.0));
    float reflectiveBody = 1.0 - smoothstep(0.25, 0.95, rimDepth);
    float occlusion = smoothstep(0.62, 1.0, rimDepth);

    float3 rimColor = (metal * angularGain + rimAmbient) * lerp(0.30, 0.95, reflectiveBody);

    // A conductor's specular carries colour - a white highlight is the dielectric signature, and is
    // what makes a metal rim read as plastic. Normalising the reflection to unit brightness keeps
    // its hue at full specular intensity, and the surface's own tint takes over as the reflection
    // runs out of energy, so the outline still reads with nothing bright behind it.
    float3 metalTint = tint.rgb / max(luminance(tint.rgb), 0.06);
    float3 environmentHue = lerp(1.0, metal / max(luminance(metal), 0.05), 0.65);
    float environmentEnergy = saturate(luminance(metal) * 2.4);
    float3 specularTone = lerp(metalTint, environmentHue, environmentEnergy) * 0.82;
    rimColor += specularTone * specularLine * (0.40 + keyArc * 0.50) * highlight;

    // A faint warm-to-cool split across the chamfer. The two ends of the curvature reflect from
    // different angles, and that fringe is what stops the rim reading as a flat grey stroke.
    rimColor *= lerp(float3(1.045, 0.995, 0.955), float3(0.955, 0.995, 1.055), rimDepth);
    rimColor *= 1.0 - occlusion * 0.55;

    // Rescale the rim toward a ceiling instead of clipping it: clipping the channels independently
    // discards the hue at exactly the brightest point, which is what makes a lit arc read as a hot
    // white stroke rather than as lit metal. The ceiling follows the light actually available to
    // the surface, so a dark interface gets a restrained edge instead of the same blown highlight
    // it would get against white.
    float rimCeiling = lerp(0.46, 0.88, max(brightField, environmentEnergy * 0.7));
    float rimPeak = max(rimColor.r, max(rimColor.g, rimColor.b));
    rimColor *= min(1.0, rimCeiling / max(rimPeak, 0.0001));

    // A crisp termination, closed by a contact seam. Metal needs a hard inner boundary; a soft
    // falloff reads as a glow instead of an edge.
    float rimMask = 1.0 - smoothstep(rimWidth * 0.80, rimWidth * 1.05, insideDistance);
    float seam = smoothstep(rimWidth * 0.85, rimWidth * 1.15, insideDistance) *
        (1.0 - smoothstep(rimWidth * 1.15, rimWidth * 1.9 + 1.2, insideDistance));

    float rimStrength = saturate(edgeContrast * metallic);
    float3 color = lerp(body, max(rimColor, 0.0), rimMask * rimStrength);
    color *= 1.0 - seam * rimStrength * 0.35;

    color += (hash21(floor(input.logicalPixel)) - 0.5) * dither * 0.01;
    return float4(saturate(color), 1.0);
}
