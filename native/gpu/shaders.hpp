#pragma once

#include <string_view>

namespace strata::gpu::shaders {

constexpr std::string_view vertex = R"hlsl(
cbuffer FrameData : register(b0) {
    float2 logicalSize;
    float2 framebufferSize;
    float frameSeconds;
    float3 frameReserved;
};

/**
 * Presentation groups: `transform` is scale.xy then translate.xy over logical layout space,
 * `opacity.x` multiplies material opacity, `opacity.y` is the group's tone and `opacity.zw` are
 * the shears a rotated group adds (x += shear.x * y, y += shear.y * x). Entry zero (and every
 * unused entry) is the identity.
 */
struct PresentationGroup {
    float4 transform;
    float4 opacity;
};

cbuffer PresentationGroups : register(b2) {
    PresentationGroup presentationGroups[512];
};

struct VertexInput {
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 drawData0 : TEXCOORD1;
    float4 drawData1 : TEXCOORD2;
    float4 drawData2 : TEXCOORD3;
    float4 drawData3 : TEXCOORD4;
};

struct PixelInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 drawData0 : TEXCOORD1;
    float4 drawData1 : TEXCOORD2;
    float4 drawData2 : TEXCOORD3;
    float4 drawData3 : TEXCOORD4;
};

/**
 * A group's tone applied to one straight-alpha colour: host::tone_color. Light, unsaturated colours
 * are ink and have their lightness mirrored toward dark ink; saturated and dark colours pass.
 */
float3 strataTone(float3 color, float tone) {
    float high = max(color.r, max(color.g, color.b));
    float low = min(color.r, min(color.g, color.b));
    float ink = saturate((high - 0.5) * 4.0) * saturate(1.0 - (high - low) * 2.5);
    float3 mirrored = float3(0.043, 0.071, 0.125) + (1.0 - high) * 0.6;
    return lerp(color, mirrored, saturate(tone) * ink);
}

PixelInput main(VertexInput input) {
    PixelInput output;
    // Position z carries the vertex's presentation group; nothing draws with depth.
    PresentationGroup group = presentationGroups[min((uint)(input.position.z + 0.5), 511u)];
    // Whole device pixels keep pixel-snapped glyphs and hairlines crisp while a group moves.
    float2 devicePixels = framebufferSize / max(logicalSize, 1.0);
    float2 translation = round(group.transform.zw * devicePixels) / devicePixels;
    float2 position = input.position.xy * group.transform.xy +
                      input.position.yx * group.opacity.zw + translation;
    output.position = float4(
        position.x * 2.0 / logicalSize.x - 1.0,
        1.0 - position.y * 2.0 / logicalSize.y,
        0.0,
        1.0
    );
    output.uv = input.uv;
    // Tone belongs to ink: fills, borders, shapes and text. An image (mode 1) keeps its own colours
    // and a shadow (mode 6) stays a shadow. A rounded rect (mode 2) carries its border colour in
    // drawData2, which is toned with it.
    int mode = (int)floor(input.drawData3.z + 0.5);
    float tone = (mode == 1 || mode == 6) ? 0.0 : group.opacity.y;
    output.color = float4(strataTone(input.color.rgb, tone), input.color.a);
    output.drawData0 = input.drawData0;
    output.drawData1 = input.drawData1;
    output.drawData2 = mode == 2
        ? float4(strataTone(input.drawData2.rgb, tone), input.drawData2.a)
        : input.drawData2;
    output.drawData3 = float4(input.drawData3.xyz, input.drawData3.w * group.opacity.x);
    return output;
}
)hlsl";

/**
 * Everything both the built-in pixel shader and every authored material share: the vertex-stage
 * output, the bound texture, the per-frame constants, and the built-in shading itself. Authored
 * materials are compiled against this same block, so a material can always fall back to exactly
 * what Strata would have drawn instead of approximating it.
 */
constexpr std::string_view pixel_common = R"hlsl(
Texture2D Texture0 : register(t0);
SamplerState Sampler0 : register(s0);

cbuffer FrameData : register(b0) {
    float2 logicalSize;
    float2 framebufferSize;
    float frameSeconds;
    float3 frameReserved;
};

struct PixelInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 drawData0 : TEXCOORD1;
    float4 drawData1 : TEXCOORD2;
    float4 drawData2 : TEXCOORD3;
    float4 drawData3 : TEXCOORD4;
};

float roundedBoxSdf(float2 p, float2 halfSize, float4 radii) {
    float2 quadrant = step(float2(0.0, 0.0), p);
    float radius = lerp(
        lerp(radii.x, radii.w, quadrant.y),
        lerp(radii.y, radii.z, quadrant.y),
        quadrant.x
    );
    // A radius past half the shorter extent has no geometric meaning, and left unclamped it pushes
    // the field entirely outside the shape so nothing is drawn at all. Clamping matches the clip
    // mask and the effect paths, so the pill idiom of an oversized radius resolves the same way
    // whichever path draws the surface.
    radius = min(radius, min(halfSize.x, halfSize.y));
    float2 q = abs(p) - halfSize + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

/** The direction a rounded box's distance grows in: host::rounded_box_distance. */
float2 strataRoundedBoxNormal(float2 p, float2 halfSize, float4 radii) {
    float2 quadrant = step(float2(0.0, 0.0), p);
    float radius = lerp(
        lerp(radii.x, radii.w, quadrant.y),
        lerp(radii.y, radii.z, quadrant.y),
        quadrant.x
    );
    radius = min(radius, min(halfSize.x, halfSize.y));
    float2 q = abs(p) - halfSize + radius;
    if (q.x > 0.0 && q.y > 0.0) {
        return normalize(q * float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0));
    }
    return q.x >= q.y ? float2(1.0, 0.0) : float2(0.0, 1.0);
}

/** A device pixel's footprint across local direction `normal` at local position `p`. */
float strataFootprint(float2 normal, float2 p) {
    return max(abs(dot(normal, ddx(p))) + abs(dot(normal, ddy(p))), 1e-6);
}

/** A rounded rect's or border's edge coverage: host::rounded_edge_coverage. */
float strataRoundedEdge(float distance, float footprint, float softness) {
    if (softness <= 1.0) return saturate(0.5 - distance / footprint);
    return 1.0 - smoothstep(-softness, softness, distance);
}

float median3(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

/**
 * The alpha a grayscale glyph's coverage contributes in this ink: host::text_coverage. Light ink
 * has its midtones weighted up, as a linear-light blend would; dark ink stays near its outline.
 */
float strataTextCoverage(float coverage, float3 ink) {
    float strength = 0.12 + 0.68 * dot(ink, float3(0.2126, 0.7152, 0.0722));
    return saturate(coverage + strength * coverage * (1.0 - coverage));
}

/**
 * Analytic shapes (modes 7 ellipse, 8 ring sector, 9 segment): host/render_shapes.hpp documents the
 * draw data and the coverage model, and every CPU backend implements the same functions.
 */
struct StrataShape {
    /** A solid edge's signed distance, or a stroke's distance from its centreline. */
    float distance;
    /** The stroke's half width; negative for a solid. */
    float halfWidth;
    /** The local direction `distance` grows in; its sign is irrelevant. */
    float2 normal;
    /** How far past a straight stroke end the point lies; very negative when it has none. */
    float beyond;
    float2 along;
};

StrataShape strataShape(float distance, float halfWidth, float2 normal) {
    StrataShape shape;
    shape.distance = distance;
    shape.halfWidth = halfWidth;
    shape.normal = dot(normal, normal) > 1e-24 ? normalize(normal) : float2(1.0, 0.0);
    shape.beyond = -1e9;
    shape.along = float2(1.0, 0.0);
    return shape;
}

StrataShape strataEllipseDistance(float2 p, float2 radii, float halfWidth) {
    float width = halfWidth > 0.0 ? halfWidth : -1.0;
    if (abs(radii.x - radii.y) <= 1e-4 * max(radii.x, radii.y))
        return strataShape(length(p) - radii.x, width, p);
    float2 r = max(radii, 1e-4);
    float k0 = length(p / r);
    float2 gradient = p / (r * r);
    float k1 = length(gradient);
    return strataShape(k0 < 1e-6 ? -min(r.x, r.y) : k0 * (k0 - 1.0) / k1, width, gradient);
}

StrataShape strataSectorDistance(float2 p, float outer, float inner, float start, float sweep,
                                 bool round) {
    float radius = length(p);
    float middle = (outer + inner) * 0.5;
    float halfWidth = (outer - inner) * 0.5;
    if (sweep >= 6.2831850) {
        if (round || inner > 0.0) return strataShape(radius - middle, halfWidth, p);
        return strataShape(radius - outer, -1.0, p);
    }
    float mid = start + sweep * 0.5;
    float c = cos(mid);
    float s = sin(mid);
    float turned = -s * p.x + c * p.y;
    float fold = turned < 0.0 ? -1.0 : 1.0;
    float2 q = float2(c * p.x + s * p.y, turned * fold);
    float halfSweep = sweep * 0.5;
    float2 edge = float2(cos(halfSweep), sin(halfSweep));
    bool within = atan2(q.y, q.x) <= halfSweep;
    if (round) {
        if (within) return strataShape(radius - middle, halfWidth, p);
        float2 fromEnd = q - edge * middle;
        float2 unfolded = float2(fromEnd.x, fromEnd.y * fold);
        return strataShape(length(fromEnd), halfWidth,
                           float2(c * unfolded.x - s * unfolded.y, s * unfolded.x + c * unfolded.y));
    }
    float along = dot(q, edge);
    float across = abs(q.y * edge.x - q.x * edge.y);
    float toRay = along >= 0.0 ? across : radius;
    float wedge = within ? -toRay : toRay;
    float ring = inner > 0.0 ? max(radius - outer, inner - radius) : radius - outer;
    float2 normal = p;
    if (wedge > ring && along >= 0.0) {
        float2 unfolded = float2(-edge.y, edge.x * fold);
        normal = float2(c * unfolded.x - s * unfolded.y, s * unfolded.x + c * unfolded.y);
    }
    return strataShape(max(ring, wedge), -1.0, normal);
}

StrataShape strataSegmentDistance(float2 p, float2 end, float halfWidth, int caps) {
    float len = length(end);
    float2 dir = len > 1e-6 ? end / len : float2(1.0, 0.0);
    float t = dot(p, dir);
    if (caps == 1) {
        float2 offset = p - dir * clamp(t, 0.0, len);
        float distance = length(offset);
        return strataShape(distance, halfWidth, distance > 1e-6 ? offset : float2(-dir.y, dir.x));
    }
    float extension = caps == 2 ? halfWidth : 0.0;
    StrataShape shape = strataShape(dir.x * p.y - dir.y * p.x, halfWidth, float2(-dir.y, dir.x));
    shape.beyond = max(-t - extension, t - len - extension);
    shape.along = dir;
    return shape;
}

/** Box-filtered coverage of an analytic shape: host::analytic_shape_coverage. */
float strataAnalyticCoverage(int mode, PixelInput input) {
    float2 p = input.uv * input.drawData0.xy - input.drawData0.zw;
    StrataShape shape;
    if (mode == 7) {
        shape = strataEllipseDistance(p, input.drawData1.xy, input.drawData1.z);
    } else if (mode == 8) {
        shape = strataSectorDistance(p, input.drawData1.x, input.drawData1.y, input.drawData1.z,
                                     input.drawData1.w, input.drawData2.x > 0.5);
    } else {
        shape = strataSegmentDistance(p, input.drawData1.xy, input.drawData1.z,
                                      (int)floor(input.drawData1.w + 0.5));
    }
    // A device pixel's footprint across the direction the distance grows in.
    float2 alongX = ddx(p);
    float2 alongY = ddy(p);
    float footprint = max(abs(dot(shape.normal, alongX)) + abs(dot(shape.normal, alongY)), 1e-6);
    float coverage;
    if (shape.halfWidth >= 0.0) {
        float centre = abs(shape.distance) / footprint;
        float band = shape.halfWidth / footprint;
        coverage = saturate(min(centre + band, 0.5) - max(centre - band, -0.5));
    } else {
        coverage = saturate(0.5 - shape.distance / footprint);
    }
    if (shape.beyond <= -1e8) return coverage;
    float endFootprint = max(abs(dot(shape.along, alongX)) + abs(dot(shape.along, alongY)), 1e-6);
    return coverage * saturate(0.5 - shape.beyond / endFootprint);
}

/** The draw mode this vertex was submitted with; selects which silhouette the draw data describes. */
int strataDrawMode(PixelInput input) {
    return (int)floor(input.drawData3.z + 0.5);
}

/** The shading Strata applies to one draw when no material overrides it. */
float4 strataShade(PixelInput input) {
    int mode = strataDrawMode(input);
    float4 color = input.color;
    if (mode == 1) {
        color *= Texture0.Sample(Sampler0, input.uv);
    } else if (mode == 2) {
        float2 size = max(input.drawData0.xy, 1.0);
        float4 radii = max(input.drawData1, 0.0);
        float softness = input.drawData0.z;
        float borderWidth = max(input.drawData0.w, 0.0);
        float2 p = (input.uv - 0.5) * size;
        float distance = roundedBoxSdf(p, size * 0.5, radii);
        float footprint = strataFootprint(strataRoundedBoxNormal(p, size * 0.5, radii), p);
        float alpha = strataRoundedEdge(distance, footprint, softness);
        float borderMix = 1.0 - strataRoundedEdge(distance + borderWidth, footprint, softness);
        color = lerp(
            color,
            input.drawData2,
            saturate(borderMix * step(0.001, borderWidth))
        );
        color.a *= alpha;
    } else if (mode == 3) {
        float2 size = max(input.drawData0.xy, 1.0);
        float4 radii = max(input.drawData1, 0.0);
        float width = max(input.drawData0.z, 0.0);
        float softness = input.drawData0.w;
        float2 p = (input.uv - 0.5) * size;
        float distance = roundedBoxSdf(p, size * 0.5, radii);
        float footprint = strataFootprint(strataRoundedBoxNormal(p, size * 0.5, radii), p);
        float outer = strataRoundedEdge(distance, footprint, softness);
        float inner = strataRoundedEdge(distance + width, footprint, softness);
        color.a *= saturate(outer - inner);
    } else if (mode == 4) {
        color.a *= strataTextCoverage(Texture0.Sample(Sampler0, input.uv).r, color.rgb);
    } else if (mode == 5) {
        float3 sampled = Texture0.Sample(Sampler0, input.uv).rgb;
        float signedDistance = median3(sampled.r, sampled.g, sampled.b) - 0.5;
        uint textureWidth;
        uint textureHeight;
        Texture0.GetDimensions(textureWidth, textureHeight);
        float2 unitRange = max(input.drawData0.x, 0.0001) /
            max(float2(textureWidth, textureHeight), 1.0);
        float2 screenTexSize = 1.0 / max(fwidth(input.uv), 0.000001);
        float screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);
        color.a *= saturate(signedDistance * screenPxRange + 0.5);
    } else if (mode == 6) {
        float2 shapeSize = max(input.drawData0.xy, 1.0);
        float2 quadSize = max(input.drawData2.xy, shapeSize);
        float blurRadius = max(input.drawData0.z, 0.5);
        float spread = input.drawData0.w;
        float4 radii = max(input.drawData1, 0.0);
        float2 p = (input.uv - 0.5) * quadSize;
        float distance = roundedBoxSdf(p, shapeSize * 0.5, radii);
        // Only the source's own silhouette stays uncovered; it sits `offset` behind the shadow.
        float source = roundedBoxSdf(p + input.drawData2.zw, shapeSize * 0.5, radii);
        float outside = 1.0 - strataRoundedEdge(
            source,
            strataFootprint(strataRoundedBoxNormal(p + input.drawData2.zw, shapeSize * 0.5, radii), p),
            1.0
        );
        float falloff = 1.0 - smoothstep(
            0.0,
            blurRadius,
            max(distance - spread, 0.0)
        );
        color.a *= outside * falloff;
    } else if (mode >= 7 && mode <= 9) {
        color.a *= strataAnalyticCoverage(mode, input);
    }
    color.a *= input.drawData3.w;
    return color;
}
)hlsl";

/** Entry point of the built-in pixel shader; the shading itself lives in the shared block. */
constexpr std::string_view builtin_entry = R"hlsl(
float4 main(PixelInput input) : SV_TARGET {
    // Shading writes the encoded [0, 1] values an RGBA8 target would hold, whatever the target's
    // format: an RGBA16F target receives the same values, not extended-range or linear light.
    float4 color = saturate(strataShade(input));
    float2 logicalPixel =
        input.position.xy * logicalSize / max(framebufferSize, 1.0);
    color = strataApplyRoundedClips(color, logicalPixel);
    clip(color.a - 0.000001);
    return color;
}
)hlsl";

/**
 * Typed access to the draw data, prepended to every authored material after the shared block. An
 * authored material writes only `float4 material(PixelInput input)`, so it never restates the
 * vertex contract and cannot drift from it.
 */
constexpr std::string_view material_prelude = R"hlsl(
/** Logical size of the drawn shape; always draw-data floats 0 and 1. */
float2 materialSize(PixelInput input) { return input.drawData0.xy; }

/** Material opacity, already multiplied by the widget's inherited opacity. */
float materialOpacity(PixelInput input) { return input.drawData3.w; }

/** Seconds since the surface began presenting, for materials that animate. */
float materialTime() { return frameSeconds; }

/** The drawn shape's corner radii, in the order top-left, top-right, bottom-right, bottom-left. */
float4 materialRadii(PixelInput input) { return max(input.drawData1, 0.0); }

/**
 * Signed distance to the shape's own rounded silhouette, negative inside. A material masks itself
 * with this instead of restating the geometry it was applied to.
 */
float materialDistance(PixelInput input) {
    float2 size = max(materialSize(input), 1.0);
    float2 p = (input.uv - 0.5) * size;
    return roundedBoxSdf(p, size * 0.5, materialRadii(input));
}

/** Coverage of the shape's silhouette, antialiased at its edge. */
float materialCoverage(PixelInput input) {
    return 1.0 - smoothstep(-1.0, 1.0, materialDistance(input));
}

/** One declared parameter float. Parameters occupy floats 8..13 in declaration order. */
float materialFloat(PixelInput input, int slot) {
    float4 data[4] = { input.drawData0, input.drawData1, input.drawData2, input.drawData3 };
    return data[slot / 4][slot % 4];
}

float2 materialFloat2(PixelInput input, int slot) {
    return float2(materialFloat(input, slot), materialFloat(input, slot + 1));
}

float4 materialFloat4(PixelInput input, int slot) {
    return float4(
        materialFloat(input, slot),
        materialFloat(input, slot + 1),
        materialFloat(input, slot + 2),
        materialFloat(input, slot + 3)
    );
}
)hlsl";

/** Entry point appended after an authored material, which routes each draw to its shading. */
constexpr std::string_view material_entry = R"hlsl(
float4 main(PixelInput input) : SV_TARGET {
    int mode = strataDrawMode(input);
    // A material describes a filled surface, so only fills reach it. Text, images, borders and
    // shadows drawn inside its scope keep the built-in shading: handing a material glyph coverage
    // to shade as though it were a rounded rectangle is what turns legible text into a smear.
    float4 color = saturate((mode == 0 || mode == 2) ? material(input) : strataShade(input));
    float2 logicalPixel =
        input.position.xy * logicalSize / max(framebufferSize, 1.0);
    color = strataApplyRoundedClips(color, logicalPixel);
    clip(color.a - 0.000001);
    return color;
}
)hlsl";

} // namespace strata::gpu::shaders
