#pragma once

#include <string_view>

namespace strata::d3d11::shaders {

constexpr std::string_view vertex = R"hlsl(
cbuffer FrameData : register(b0) {
    float2 logicalSize;
    float2 framebufferSize;
    float frameSeconds;
    float3 frameReserved;
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

PixelInput main(VertexInput input) {
    PixelInput output;
    output.position = float4(
        input.position.x * 2.0 / logicalSize.x - 1.0,
        1.0 - input.position.y * 2.0 / logicalSize.y,
        saturate(input.position.z),
        1.0
    );
    output.uv = input.uv;
    output.color = input.color;
    output.drawData0 = input.drawData0;
    output.drawData1 = input.drawData1;
    output.drawData2 = input.drawData2;
    output.drawData3 = input.drawData3;
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

float median3(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
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
        float softness = max(input.drawData0.z, 0.5);
        float borderWidth = max(input.drawData0.w, 0.0);
        float2 p = (input.uv - 0.5) * size;
        float distance = roundedBoxSdf(p, size * 0.5, radii);
        float alpha = 1.0 - smoothstep(-softness, softness, distance);
        float borderMix = 1.0 - smoothstep(
            borderWidth - softness, borderWidth + softness, -distance
        );
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
        float softness = max(input.drawData0.w, 0.5);
        float2 p = (input.uv - 0.5) * size;
        float distance = roundedBoxSdf(p, size * 0.5, radii);
        float outer = 1.0 - smoothstep(-softness, softness, distance);
        float inner = 1.0 - smoothstep(-softness, softness, distance + width);
        color.a *= saturate(outer - inner);
    } else if (mode == 4) {
        color.a *= Texture0.Sample(Sampler0, input.uv).r;
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
        float outside = smoothstep(-1.0, 1.0, distance);
        float falloff = 1.0 - smoothstep(
            0.0,
            blurRadius,
            max(distance - spread, 0.0)
        );
        color.a *= outside * falloff;
    }
    color.a *= input.drawData3.w;
    return color;
}
)hlsl";

/** Entry point of the built-in pixel shader; the shading itself lives in the shared block. */
constexpr std::string_view builtin_entry = R"hlsl(
float4 main(PixelInput input) : SV_TARGET {
    float4 color = strataShade(input);
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
    float4 color = (mode == 0 || mode == 2) ? material(input) : strataShade(input);
    float2 logicalPixel =
        input.position.xy * logicalSize / max(framebufferSize, 1.0);
    color = strataApplyRoundedClips(color, logicalPixel);
    clip(color.a - 0.000001);
    return color;
}
)hlsl";

} // namespace strata::d3d11::shaders
