#pragma once

#include <string_view>

namespace strata::gpu::shaders {

constexpr std::string_view vertex = R"hlsl(
cbuffer FrameData : register(b0) {
    float2 logicalSize;
    float2 framebufferSize;
    float frameSeconds;
    /** The pointer over the surface in framebuffer pixels, and 1 while it is there (else 0). */
    float3 framePointer;
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
    /** xyz: a group turned in depth divides by w = dot(projection.xy, position) + projection.z. */
    float4 projection;
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
    int mode = (int)floor(input.drawData3.z + 0.5);
    float2 local = input.position.xy;
    float2 uv = input.uv;
    if (mode == 13 || mode == 14) {
        // A vector glyph's quad is its outline's bounds (uv in font units). It grows here by two
        // device pixels each way, at whatever scale and turn its group shows it, so the pixels
        // its edge partly covers, and its subpixels' neighbours, are drawn too. drawData0.w says
        // which corner this is (bit 0 right, bit 1 top), drawData1 the page's logical pixels a
        // font unit spans along x and along y.
        float corner = input.drawData0.w;
        float2 outward = float2(fmod(corner, 2.0) >= 0.5 ? 1.0 : -1.0, corner >= 1.5 ? 1.0 : -1.0);
        float2x2 groupLinear = float2x2(group.transform.x, group.opacity.z,
                                        group.opacity.w, group.transform.y);
        float2 deviceX = mul(groupLinear, input.drawData1.xy) * devicePixels;
        float2 deviceY = mul(groupLinear, input.drawData1.zw) * devicePixels;
        float2 grow = outward * float2(2.0 / max(length(deviceX), 1.0e-6),
                                       2.0 / max(length(deviceY), 1.0e-6));
        local += grow.x * input.drawData1.xy + grow.y * input.drawData1.zw;
        uv += grow;
    }
    float2 position = local * group.transform.xy + local.yx * group.opacity.zw + translation;
    // A group turned in depth: the projected position is position / w, which the rasterizer
    // divides by itself, so every attribute interpolates perspective-correctly.
    float w = max(dot(group.projection.xy, local) + group.projection.z, 1.0e-4);
    output.position = float4(
        position.x * 2.0 / logicalSize.x - w,
        w - position.y * 2.0 / logicalSize.y,
        0.0,
        w
    );
    output.uv = uv;
    // Tone belongs to ink: fills, borders, shapes and text. An image (mode 1) keeps its own colours
    // and a shadow (mode 6) stays a shadow. A rounded rect (mode 2) carries its border colour in
    // drawData2, which is toned with it.
    // A banded gradient (mode 10 with a paint) carries its band's end colour there too. A colour
    // mask (mode 11) is ink whose colours are in its texels: the pixel stage tones them, with the
    // group's tone passed in drawData1.x.
    float tone = (mode == 1 || mode == 6 || mode == 11) ? 0.0 : group.opacity.y;
    output.color = float4(strataTone(input.color.rgb, tone), input.color.a);
    output.drawData0 = input.drawData0;
    output.drawData1 = mode == 11
        ? float4(group.opacity.y, input.drawData1.yzw)
        : input.drawData1;
    output.drawData2 = (mode == 2 || (mode == 10 && input.drawData1.z > 0.5))
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
    /** The pointer over the surface in framebuffer pixels, and 1 while it is there (else 0). */
    float3 framePointer;
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
 * The alpha a grayscale glyph's coverage contributes in this ink: host::text_coverage. Coverage is
 * raised to a power that falls with the ink's lightness: light ink's faint edges are lifted, dark
 * ink's thinned, fitted to how Chromium draws the same glyphs.
 */
float strataTextCoverage(float coverage, float3 ink) {
    float lightness = dot(ink, float3(0.2126, 0.7152, 0.0722));
    return pow(saturate(coverage), 1.46 - 0.94 * lightness);
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

/** host::conic_turn: the clockwise turn of `p` from up, as a fraction of a whole turn. */
float strataConicTurn(float2 p) {
    float turn = atan2(p.x, -p.y) * 0.15915494;
    return turn < 0.0 ? turn + 1.0 : turn;
}

/** host::gradient_band_parameter and host::gradient_band_factor. */
float strataGradientBand(PixelInput input) {
    int packed = (int)floor(input.drawData1.z + 0.5) - 1;
    float2 position = input.drawData3.xy;
    int kind = packed % 3;
    float t = kind == 1 ? length(position) : (kind == 2 ? strataConicTurn(position) : position.x);
    int extend = packed / 3;
    if (extend == 1) {
        t -= floor(t);
    } else if (extend == 2) {
        float period = abs(fmod(t, 2.0));
        t = period > 1.0 ? 2.0 - period : period;
    }
    int open = (int)floor(input.drawData1.w + 0.5);
    if (((open & 1) == 0 && !(t >= input.drawData1.x)) ||
        ((open & 2) == 0 && !(t < input.drawData1.y)))
        return -1.0;
    float span = input.drawData1.y - input.drawData1.x;
    if (!(span > 0.0))
        return t >= input.drawData1.y ? 1.0 : 0.0;
    return saturate((t - input.drawData1.x) / span);
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

/** The vertex stage's strataTone, for ink whose colours come from a texture. */
float3 strataTexelTone(float3 color, float tone) {
    float high = max(color.r, max(color.g, color.b));
    float low = min(color.r, min(color.g, color.b));
    float ink = saturate((high - 0.5) * 4.0) * saturate(1.0 - (high - low) * 2.5);
    float3 mirrored = float3(0.043, 0.071, 0.125) + (1.0 - high) * 0.6;
    return lerp(color, mirrored, saturate(tone) * ink);
}

/**
 * A glyph page's texel at `uv`. Glyph pages are sampled texel-exact, which is faithful while each
 * texel lands on one pixel; in a group drawn at a scale its glyphs were not rasterized for (a zoom
 * on its way, a turn), an exact sample would drop and repeat whole columns, so the texels are
 * blended bilinearly there instead.
 */
float4 strataGlyphTexel(float2 uv) {
    uint width;
    uint height;
    Texture0.GetDimensions(width, height);
    float2 size = float2(width, height);
    float2 footprint = fwidth(uv) * size;
    if (all(abs(footprint - 1.0) < 0.01))
        return Texture0.Sample(Sampler0, uv);
    float2 texel = uv * size - 0.5;
    float2 corner = floor(texel);
    float2 blend = texel - corner;
    int2 last = int2(width, height) - 1;
    int2 base = int2(corner);
    float4 topLeft = Texture0.Load(int3(clamp(base, int2(0, 0), last), 0));
    float4 topRight = Texture0.Load(int3(clamp(base + int2(1, 0), int2(0, 0), last), 0));
    float4 bottomLeft = Texture0.Load(int3(clamp(base + int2(0, 1), int2(0, 0), last), 0));
    float4 bottomRight = Texture0.Load(int3(clamp(base + int2(1, 1), int2(0, 0), last), 0));
    return lerp(lerp(topLeft, topRight, blend.x), lerp(bottomLeft, bottomRight, blend.x), blend.y);
}

/**
 * Vector text (draw modes 13 gray and 14 per subpixel): a glyph's coverage measured per pixel from
 * its outline, host::vector_text_coverage (<strata/vector_text.hpp>) on the GPU. The pixel's
 * square is mapped into the glyph's font units through the derivatives of its glyph position, so
 * a glyph scaled, turned or presented under any group transform is measured exactly where it
 * lands, every frame, with nothing rasterized ahead of drawing.
 *
 * Buffers (STRATA_RENDER_PACKET_VERSION_24, the batch's texture naming them): VectorGlyphs
 * (records), VectorSegments (x0 y0 x1 y1, font units), VectorBands (per glyph: band count + 1
 * offsets, then each band's segment indices relative to its segment base).
 */
struct VectorGlyphRecord {
    float4 bounds;
    uint segmentBase;
    uint bandBase;
    uint bandCount;
    uint segmentCount;
};

#ifdef STRATA_VULKAN
// Vulkan numbers t registers from binding 3 (vulkan/shader.cpp): these are bindings 6 to 8.
StructuredBuffer<VectorGlyphRecord> VectorGlyphs : register(t3);
StructuredBuffer<float4> VectorSegments : register(t4);
StructuredBuffer<uint> VectorBands : register(t5);
#else
StructuredBuffer<VectorGlyphRecord> VectorGlyphs : register(t8);
StructuredBuffer<float4> VectorSegments : register(t9);
StructuredBuffer<uint> VectorBands : register(t10);
#endif

/** The integral over [x0, x0 + w] of the part of the column right of x, from x0. */
float vectorColumnArea(float x, float x0, float w) {
    float x1 = x0 + w;
    if (x <= x0)
        return w * (x - x0);
    if (x >= x1)
        return w * w * 0.5;
    return x1 * (x - x0) - (x * x - x0 * x0) * 0.5;
}

/**
 * The average width of the column [x0, x0 + w] to the right of a segment running from xFrom to
 * xTo (`span` apart) across the pixel's row; times the height it crosses, its signed area there.
 */
float vectorColumnShare(float xFrom, float xTo, float span, float x0, float w) {
    return abs(span) < 1.0e-5
        ? x0 + w - clamp(xFrom, x0, x0 + w)
        : (vectorColumnArea(xTo, x0, w) - vectorColumnArea(xFrom, x0, w)) / span;
}

/**
 * The coverage of the pixel by a glyph whose font-unit position at the pixel's centre is
 * `glyphPosition`, moving `alongX` and `alongY` font units a device pixel right and down: gray
 * coverage in every channel when `subpixel` is 0, else coverage per subpixel (red, green, blue
 * from the left; `subpixel` 2 for blue, green, red), each the pixel's thirds weighted by
 * FreeType's LCD filter, alpha the largest.
 */
float4 vectorTextCoverage(uint glyphIndex, float2 glyphPosition, float2 alongX, float2 alongY,
                          uint subpixel) {
    VectorGlyphRecord glyph = VectorGlyphs[glyphIndex];
    float determinant = alongX.x * alongY.y - alongX.y * alongY.x;
    if (abs(determinant) < 1.0e-12 || glyph.bandCount == 0u)
        return 0.0;
    // Font units to pixel offsets from the pixel's centre.
    float2x2 inverse = float2x2(alongY.y, -alongY.x, -alongX.y, alongX.x) / determinant;
    // Bands serve when a device row is a range of glyph heights; a turned glyph's row crosses
    // every band, and reads every segment.
    bool banded = abs(alongX.y) <= 1.0e-4 * abs(alongY.y);
    float reach = 0.5 * abs(alongY.y) + 1.5 * abs(alongX.y);
    float height = glyph.bounds.w - glyph.bounds.y;
    uint bands = max(glyph.bandCount, 1u);
    float bandHeight = height / float(bands);
    int firstBand = banded ? clamp(int(floor((glyphPosition.y - reach - glyph.bounds.y) /
                                             bandHeight)), 0, int(bands) - 1)
                           : 0;
    int lastBand = banded ? clamp(int(floor((glyphPosition.y + reach - glyph.bounds.y) /
                                            bandHeight)), 0, int(bands) - 1)
                          : 0;
    // The pixel's area to the right of the outline, or its seven thirds' (two each side).
    float gray = 0.0;
    float4 thirdsLeft = 0.0;
    float3 thirdsRight = 0.0;
    const float third = 1.0 / 3.0;
    for (int band = firstBand; band <= lastBand; ++band) {
        float bandBottom = !banded || band == 0 ? -3.0e38 : glyph.bounds.y + bandHeight * float(band);
        float bandTop = !banded || band == int(bands) - 1 ? 3.0e38
                                                          : glyph.bounds.y + bandHeight * float(band + 1);
        uint first = banded ? VectorBands[glyph.bandBase + uint(band)] : 0u;
        uint last = banded ? VectorBands[glyph.bandBase + uint(band) + 1] : glyph.segmentCount;
        uint indices = glyph.bandBase + bands + 1;
        for (uint entry = first; entry < last; ++entry) {
            uint index = banded ? VectorBands[indices + entry] : entry;
            float4 segment = VectorSegments[glyph.segmentBase + index];
            // The part of the segment in its band (a turned pixel takes it whole).
            float2 a = segment.xy;
            float2 b = segment.zw;
            if (banded) {
                float low = max(min(segment.y, segment.w), bandBottom);
                float high = min(max(segment.y, segment.w), bandTop);
                if (high <= low)
                    continue;
                float slope = (segment.z - segment.x) / (segment.w - segment.y);
                a = float2(segment.x + (clamp(segment.y, low, high) - segment.y) * slope,
                           clamp(segment.y, low, high));
                b = float2(segment.x + (clamp(segment.w, low, high) - segment.y) * slope,
                           clamp(segment.w, low, high));
            }
            // In pixel offsets, then the part crossing the pixel's row.
            float2 da = mul(inverse, a - glyphPosition);
            float2 db = mul(inverse, b - glyphPosition);
            if (da.y == db.y || max(da.y, db.y) <= -0.5 || min(da.y, db.y) >= 0.5)
                continue;
            float yFrom = clamp(da.y, -0.5, 0.5);
            float yTo = clamp(db.y, -0.5, 0.5);
            float slope = (db.x - da.x) / (db.y - da.y);
            float xFrom = da.x + (yFrom - da.y) * slope;
            float xTo = da.x + (yTo - da.y) * slope;
            float span = xTo - xFrom;
            float rise = yTo - yFrom;
            if (subpixel == 0u) {
                gray += rise * vectorColumnShare(xFrom, xTo, span, -0.5, 1.0);
            } else {
                thirdsLeft += rise * float4(vectorColumnShare(xFrom, xTo, span, -0.5 - 2.0 * third, third),
                                            vectorColumnShare(xFrom, xTo, span, -0.5 - third, third),
                                            vectorColumnShare(xFrom, xTo, span, -0.5, third),
                                            vectorColumnShare(xFrom, xTo, span, -0.5 + third, third));
                thirdsRight += rise * float3(vectorColumnShare(xFrom, xTo, span, -0.5 + 2.0 * third, third),
                                             vectorColumnShare(xFrom, xTo, span, 0.5, third),
                                             vectorColumnShare(xFrom, xTo, span, 0.5 + third, third));
            }
        }
    }
    if (subpixel == 0u)
        return min(abs(gray), 1.0);
    // Each subpixel weighs its own third and two neighbours each way (FreeType's default filter).
    float4 left = min(abs(thirdsLeft) * 3.0, 1.0);
    float3 right = min(abs(thirdsRight) * 3.0, 1.0);
    const float3 filter = float3(8.0, 77.0, 86.0) / 256.0;
    float3 channels = min(float3(
        filter.x * left.x + filter.y * left.y + filter.z * left.z + filter.y * left.w + filter.x * right.x,
        filter.x * left.y + filter.y * left.z + filter.z * left.w + filter.y * right.x + filter.x * right.y,
        filter.x * left.z + filter.y * left.w + filter.z * right.x + filter.y * right.y + filter.x * right.z),
        1.0);
    if (subpixel == 2u)
        channels = channels.bgr;
    return float4(channels, max(channels.r, max(channels.g, channels.b)));
}

/**
 * Subpixel text's coverage per colour channel, set by strataShade for modes 12 and 14; every
 * other draw leaves it white. The entry point turns it into the draw's per-channel alpha (strataOutput).
 */
static float3 strataSubpixelCoverage = float3(1.0, 1.0, 1.0);

/** The shading Strata applies to one draw when no material overrides it. */
float4 strataShade(PixelInput input) {
    int mode = strataDrawMode(input);
    float4 color = input.color;
    // Vector text's footprint in font units, taken where every pixel of the quad computes it.
    float2 glyphAlongX = ddx(input.uv);
    float2 glyphAlongY = ddy(input.uv);
    if (mode == 1) {
        color *= Texture0.Sample(Sampler0, input.uv);
    } else if (mode == 11) {
        // Joined shapes: their colours and coverage in straight RGBA, toned as ink.
        float4 texel = Texture0.Sample(Sampler0, input.uv);
        color *= float4(strataTexelTone(texel.rgb, input.drawData1.x), texel.a);
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
        color.a *= strataTextCoverage(strataGlyphTexel(input.uv).r, color.rgb);
    } else if (mode == 12) {
        // Subpixel text: each colour channel's coverage weighted as gray coverage is; alpha
        // carries the strongest, and strataOutput hands the rest to the blend.
        float3 sampled = strataGlyphTexel(input.uv).rgb;
        strataSubpixelCoverage = float3(strataTextCoverage(sampled.r, color.rgb),
                                        strataTextCoverage(sampled.g, color.rgb),
                                        strataTextCoverage(sampled.b, color.rgb));
        color.a *= max(strataSubpixelCoverage.r,
                       max(strataSubpixelCoverage.g, strataSubpixelCoverage.b));
    } else if (mode == 13 || mode == 14) {
        // Vector text. Where the glyph shows upright at the size it was laid out for, it draws
        // its variant fitted to the pixel grid at that size (drawData0.y, when it has one); a zoom
        // passing through sizes draws its outline as it is (drawData0.x), so its stems do not
        // jump a pixel at a time.
        float2 devicePixels = framebufferSize / max(logicalSize, 1.0);
        float laidOut = length(input.drawData1.zw) * devicePixels.y;
        float shown = abs(glyphAlongY.y) > 0.0 ? 1.0 / abs(glyphAlongY.y) : 0.0;
        bool native = abs(glyphAlongX.y) <= 1.0e-4 * abs(glyphAlongY.y) &&
                      abs(glyphAlongY.x) <= 1.0e-4 * abs(glyphAlongX.x) &&
                      abs(shown - laidOut) <= laidOut * 1.0e-3;
        float record = native && input.drawData0.y >= 0.0 ? input.drawData0.y : input.drawData0.x;
        uint subpixel = mode == 14 ? (uint)floor(input.drawData0.z + 0.5) : 0u;
        float4 coverage = vectorTextCoverage((uint)floor(record + 0.5), input.uv, glyphAlongX,
                                             glyphAlongY, subpixel);
        if (subpixel == 0u) {
            color.a *= strataTextCoverage(coverage.a, color.rgb);
        } else {
            strataSubpixelCoverage = float3(strataTextCoverage(coverage.r, color.rgb),
                                            strataTextCoverage(coverage.g, color.rgb),
                                            strataTextCoverage(coverage.b, color.rgb));
            color.a *= max(strataSubpixelCoverage.r,
                           max(strataSubpixelCoverage.g, strataSubpixelCoverage.b));
        }
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
        // The distance from the glyph's edge in screen pixels, positive inside.
        float edge = signedDistance * screenPxRange;
        float softness = min(input.drawData3.y, max(0.5 * screenPxRange - 1.0, 0.0));
        float fill = softness > 0.0 ? saturate(edge / softness + 0.5) : saturate(edge + 0.5);
        float4 text = float4(color.rgb, color.a * fill);
        // An outline around the glyph and a glow beyond it, each under what is in front.
        // Each effect stops short of where the field ends, so it never paints the glyph's box.
        float available = max(0.5 * screenPxRange - 1.0, 0.0);
        float outlineWidth = min(input.drawData0.w, available);
        if (outlineWidth > 0.0) {
            float4 outline = float4(input.drawData1.rgb,
                                    input.drawData1.a * saturate(edge + outlineWidth + 0.5));
            float a = text.a + outline.a * (1.0 - text.a);
            text = float4((text.rgb * text.a + outline.rgb * outline.a * (1.0 - text.a)) /
                              max(a, 0.0001), a);
        }
        float glowRadius = min(input.drawData3.x, available);
        if (glowRadius > 0.0) {
            float reach = saturate(1.0 + edge / glowRadius);
            float4 glow = float4(input.drawData2.rgb, input.drawData2.a * reach * reach);
            float a = text.a + glow.a * (1.0 - text.a);
            text = float4((text.rgb * text.a + glow.rgb * glow.a * (1.0 - text.a)) /
                              max(a, 0.0001), a);
        }
        color = text;
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
    } else if (mode == 10) {
        // A shape's coverage mask: exact area coverage, applied as it is, times its paint: a
        // gradient band lerps from the vertex colour to its end colour and leaves other bands'
        // pixels to them (host::gradient_band_factor).
        float coverage = Texture0.Sample(Sampler0, input.uv).r;
        if (input.drawData1.z > 0.5) {
            float band = strataGradientBand(input);
            coverage = band < 0.0 ? 0.0 : coverage;
            color = lerp(color, input.drawData2, saturate(band));
        }
        color.a *= coverage;
    }
    color.a *= input.drawData3.w;
    return color;
}
)hlsl";

/**
 * What a pixel stage writes: its colour, and the alpha each colour channel blends by (the second
 * source of a dual-source blend). Only subpixel text (STRATA_RENDER_BLEND_SUBPIXEL_TEXT) reads the
 * second; for every other draw it is the colour's alpha in all four channels.
 */
constexpr std::string_view pixel_output = R"hlsl(
// STRATA_SINGLE_OUTPUT: a device without dual-source blending, where a second output would have
// nothing to land in.
struct PixelOutput {
    float4 color : SV_TARGET0;
#ifndef STRATA_SINGLE_OUTPUT
    float4 channelAlpha : SV_TARGET1;
#endif
};

/** The output for `color`, already clipped: per-channel alpha scaled as its alpha was. */
PixelOutput strataOutput(float4 color) {
    PixelOutput output;
    output.color = color;
#ifndef STRATA_SINGLE_OUTPUT
    float strongest = max(strataSubpixelCoverage.r,
                          max(strataSubpixelCoverage.g, strataSubpixelCoverage.b));
    output.channelAlpha = float4(strataSubpixelCoverage / max(strongest, 0.000001) * color.a,
                                 color.a);
#endif
    return output;
}
)hlsl";

/** Entry point of the built-in pixel shader; the shading itself lives in the shared block. */
constexpr std::string_view builtin_entry = R"hlsl(
PixelOutput main(PixelInput input) {
    // Shading writes the encoded [0, 1] values an RGBA8 target would hold, whatever the target's
    // format: an RGBA16F target receives the same values, not extended-range or linear light.
    float4 color = saturate(strataShade(input));
    float2 logicalPixel =
        input.position.xy * logicalSize / max(framebufferSize, 1.0);
    color = strataApplyRoundedClips(color, logicalPixel);
    clip(color.a - 0.000001);
    return strataOutput(color);
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

/**
 * Where the pointer is, in the drawn shape's own logical coordinates (its top-left is 0, 0, its
 * bottom-right materialSize), and in `z` 1 while the pointer is over the surface, else 0. It is
 * mapped through the shape's own screen-space derivatives, so it holds under any scale, rotation
 * or shear, and it costs nothing on the CPU: a spotlight or a glow that follows the pointer
 * redraws without the interface changing.
 */
float3 materialPointer(PixelInput input) {
    float2 local = input.uv * materialSize(input);
    float2 toward = framePointer.xy - input.position.xy;
    float2 place = local + ddx(local) * toward.x + ddy(local) * toward.y;
    return float3(place, framePointer.z);
}

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
PixelOutput main(PixelInput input) {
    int mode = strataDrawMode(input);
    // A material describes a filled surface, so only fills reach it. Text, images, borders and
    // shadows drawn inside its scope keep the built-in shading: handing a material glyph coverage
    // to shade as though it were a rounded rectangle is what turns legible text into a smear.
    float4 color = saturate((mode == 0 || mode == 2) ? material(input) : strataShade(input));
    float2 logicalPixel =
        input.position.xy * logicalSize / max(framebufferSize, 1.0);
    color = strataApplyRoundedClips(color, logicalPixel);
    clip(color.a - 0.000001);
    return strataOutput(color);
}
)hlsl";

} // namespace strata::gpu::shaders
