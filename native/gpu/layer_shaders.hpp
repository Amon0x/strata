#pragma once

#include <cstdint>
#include <string_view>

namespace strata::gpu {

/**
 * The programs a content effect's layer runs on the GPU after its declared passes: its built-in
 * colour filters, the drop shadow it draws under itself, and the composite that combines it with
 * what is under it by any operator (STRATA_LAYER_COMPOSITE_*). They read texels exactly (Load), so
 * every backend, the reference renderer included, filters the same pixels. Each is drawn with the
 * full-target triangle of blur_shaders::vertex and writes what it returns (no blending).
 */
constexpr std::string_view layer_prelude = R"hlsl(
cbuffer LayerData : register(b0) {
    float2 layerLogicalSize;
    float2 layerTargetSize;
    float4 layerBounds;
    float4 layerRadii;
    float4 layerValues[6];
    float2 layerOffset;
    float layerOpacity;
    uint layerKind;
};

Texture2D LayerSource : register(t0);
Texture2D LayerBackdrop : register(t1);
SamplerState LayerSampler : register(s0);

struct PixelInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float layerValue(uint index) {
    return layerValues[min(index / 4, 5)][index % 4];
}

float4 layerStraight(float4 color) {
    return color.a > 0.00001 ? float4(color.rgb / color.a, color.a) : float4(0.0, 0.0, 0.0, 0.0);
}

float layerLuminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}
)hlsl";

/** Colour filters, by layerKind: 2 colour matrix, 3 gradient map, 4 levels, 6 a shadow's source. */
constexpr std::string_view layer_filter_pixel = R"hlsl(
float layerLevel(float value, uint first) {
    float black = layerValue(first);
    float white = layerValue(first + 1);
    float gamma = max(layerValue(first + 2), 0.00001);
    float span = white - black;
    float x = abs(span) > 0.00001 ? saturate((value - black) / span) : (value >= black ? 1.0 : 0.0);
    x = pow(x, 1.0 / gamma);
    return layerValue(first + 3) + (layerValue(first + 4) - layerValue(first + 3)) * x;
}

float3 layerGradient(float luminance) {
    uint count = (uint)clamp(layerValue(1), 0.0, 5.0);
    if (count == 0)
        return float3(0.0, 0.0, 0.0);
    float3 previous = float3(layerValue(3), layerValue(4), layerValue(5));
    float previousOffset = layerValue(2);
    if (luminance <= previousOffset)
        return previous;
    [loop]
    for (uint stop = 1; stop < count; ++stop) {
        uint at = 2 + stop * 4;
        float offset = layerValue(at);
        float3 color = float3(layerValue(at + 1), layerValue(at + 2), layerValue(at + 3));
        if (luminance <= offset) {
            float span = offset - previousOffset;
            float t = span > 0.00001 ? (luminance - previousOffset) / span : 1.0;
            return lerp(previous, color, t);
        }
        previous = color;
        previousOffset = offset;
    }
    return previous;
}

float4 main(PixelInput input) : SV_TARGET {
    float4 color = layerStraight(LayerSource.Load(int3(input.position.xy, 0)));
    float4 result = color;
    if (layerKind == 2) {
        float4 v = color;
        result = float4(
            dot(float4(layerValue(0), layerValue(1), layerValue(2), layerValue(3)), v) + layerValue(4),
            dot(float4(layerValue(5), layerValue(6), layerValue(7), layerValue(8)), v) + layerValue(9),
            dot(float4(layerValue(10), layerValue(11), layerValue(12), layerValue(13)), v) + layerValue(14),
            dot(float4(layerValue(15), layerValue(16), layerValue(17), layerValue(18)), v) + layerValue(19));
    } else if (layerKind == 3) {
        float3 mapped = layerGradient(saturate(layerLuminance(color.rgb)));
        result = float4(lerp(color.rgb, mapped, saturate(layerValue(0))), color.a);
    } else if (layerKind == 4) {
        result = float4(layerLevel(color.r, 0), layerLevel(color.g, 5), layerLevel(color.b, 10),
                        layerLevel(color.a, 15));
    } else if (layerKind == 6) {
        result = float4(layerValue(0), layerValue(1), layerValue(2), color.a * layerValue(3));
    }
    result = saturate(result);
    return float4(result.rgb * result.a, result.a);
}
)hlsl";

/**
 * A drop shadow under its layer: LayerSource the layer, LayerBackdrop its blurred shadow, read
 * layerOffset device pixels away. Value 7 above one half draws the shadow alone.
 */
constexpr std::string_view layer_shadow_pixel = R"hlsl(
float4 main(PixelInput input) : SV_TARGET {
    float4 layer = LayerSource.Load(int3(input.position.xy, 0));
    float2 uv = (input.position.xy - layerOffset) / max(layerTargetSize, 1.0);
    float4 shadow = LayerBackdrop.Sample(LayerSampler, uv);
    if (layerValue(7) > 0.5)
        return shadow;
    return layer + shadow * (1.0 - layer.a);
}
)hlsl";

/**
 * The layer (LayerSource, premultiplied) combined with what is under it (LayerBackdrop, a copy of
 * the destination) by operator layerKind, scaled by layerOpacity and mixed in by the bounds mask
 * and rounded clips. Needs rounded_clip_hlsl before it.
 */
constexpr std::string_view layer_composite_pixel = R"hlsl(
float layerCornerRadius(float2 centered) {
    bool right = centered.x >= 0.0;
    bool bottom = centered.y >= 0.0;
    return bottom ? (right ? layerRadii.z : layerRadii.w) : (right ? layerRadii.y : layerRadii.x);
}

float layerMask(float2 pixel) {
    float2 halfSize = layerBounds.zw * 0.5;
    float2 centered = pixel - (layerBounds.xy + halfSize);
    float radius = min(layerCornerRadius(centered), min(halfSize.x, halfSize.y));
    float2 q = abs(centered) - halfSize + radius;
    float distance = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
    return 1.0 - smoothstep(-1.0, 1.0, distance);
}

float layerHardLight(float backdrop, float source) {
    return source <= 0.5 ? backdrop * 2.0 * source
                         : backdrop + (2.0 * source - 1.0) - backdrop * (2.0 * source - 1.0);
}

float layerSoftLight(float backdrop, float source) {
    if (source <= 0.5)
        return backdrop - (1.0 - 2.0 * source) * backdrop * (1.0 - backdrop);
    float d = backdrop <= 0.25 ? ((16.0 * backdrop - 12.0) * backdrop + 4.0) * backdrop
                               : sqrt(backdrop);
    return backdrop + (2.0 * source - 1.0) * (d - backdrop);
}

float layerColorDodge(float backdrop, float source) {
    if (backdrop <= 0.0)
        return 0.0;
    if (source >= 1.0)
        return 1.0;
    return min(1.0, backdrop / (1.0 - source));
}

float layerColorBurn(float backdrop, float source) {
    if (backdrop >= 1.0)
        return 1.0;
    if (source <= 0.0)
        return 0.0;
    return 1.0 - min(1.0, (1.0 - backdrop) / source);
}

float layerLum(float3 c) {
    return dot(c, float3(0.3, 0.59, 0.11));
}

float3 layerClipColor(float3 c) {
    float l = layerLum(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    if (n < 0.0)
        c = l + (c - l) * l / max(l - n, 0.00001);
    if (x > 1.0)
        c = l + (c - l) * (1.0 - l) / max(x - l, 0.00001);
    return c;
}

float3 layerSetLum(float3 c, float l) {
    return layerClipColor(c + (l - layerLum(c)));
}

float layerSat(float3 c) {
    return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
}

float3 layerSetSat(float3 c, float s) {
    float high = max(c.r, max(c.g, c.b));
    float low = min(c.r, min(c.g, c.b));
    float span = high - low;
    if (span <= 0.00001)
        return float3(0.0, 0.0, 0.0);
    return (c - low) * s / span;
}

float3 layerBlend(float3 cb, float3 cs, uint mode) {
    if (mode == 1) return cb * cs;
    if (mode == 2) return cb + cs - cb * cs;
    if (mode == 3) return float3(layerHardLight(cs.r, cb.r), layerHardLight(cs.g, cb.g),
                                 layerHardLight(cs.b, cb.b));
    if (mode == 4) return min(cb, cs);
    if (mode == 5) return max(cb, cs);
    if (mode == 6) return float3(layerColorDodge(cb.r, cs.r), layerColorDodge(cb.g, cs.g),
                                 layerColorDodge(cb.b, cs.b));
    if (mode == 7) return float3(layerColorBurn(cb.r, cs.r), layerColorBurn(cb.g, cs.g),
                                 layerColorBurn(cb.b, cs.b));
    if (mode == 8) return float3(layerHardLight(cb.r, cs.r), layerHardLight(cb.g, cs.g),
                                 layerHardLight(cb.b, cs.b));
    if (mode == 9) return float3(layerSoftLight(cb.r, cs.r), layerSoftLight(cb.g, cs.g),
                                 layerSoftLight(cb.b, cs.b));
    if (mode == 10) return abs(cb - cs);
    if (mode == 11) return cb + cs - 2.0 * cb * cs;
    if (mode == 12) return layerSetLum(layerSetSat(cs, layerSat(cb)), layerLum(cb));
    if (mode == 13) return layerSetLum(layerSetSat(cb, layerSat(cs)), layerLum(cb));
    if (mode == 14) return layerSetLum(cs, layerLum(cb));
    return layerSetLum(cb, layerLum(cs));
}

float4 layerComposite(float4 s, float4 d, uint mode) {
    if (mode == 0)
        return s + d * (1.0 - s.a);
    if (mode <= 15) {
        float3 cs = s.a > 0.00001 ? saturate(s.rgb / s.a) : float3(0.0, 0.0, 0.0);
        float3 cb = d.a > 0.00001 ? saturate(d.rgb / d.a) : float3(0.0, 0.0, 0.0);
        float3 mixed = saturate(layerBlend(cb, cs, mode));
        return float4(s.rgb * (1.0 - d.a) + d.rgb * (1.0 - s.a) + s.a * d.a * mixed,
                      s.a + d.a - s.a * d.a);
    }
    float luminance = saturate(layerLuminance(s.rgb));
    if (mode == 16) return d * s.a;
    if (mode == 17) return d * (1.0 - s.a);
    if (mode == 18) return d * luminance;
    if (mode == 19) return d * (1.0 - luminance);
    if (mode == 20) return max(s, d);
    if (mode == 21) return min(s, d);
    if (mode == 23) return min(s + d, 1.0);
    return abs(s - d);
}

float4 main(PixelInput input) : SV_TARGET {
    float4 source = LayerSource.Load(int3(input.position.xy, 0)) * layerOpacity;
    float4 backdrop = LayerBackdrop.Load(int3(input.position.xy, 0));
    float4 result = layerComposite(source, backdrop, layerKind);
    float2 logicalPixel = input.position.xy * layerLogicalSize / max(layerTargetSize, 1.0);
    float coverage = layerMask(input.position.xy) * strataRoundedClipCoverage(logicalPixel);
    return lerp(backdrop, result, coverage);
}
)hlsl";

/** The constant buffer of every layer program (LayerData). */
struct LayerConstants final {
    float logical_size[2]{};
    float target_size[2]{};
    float bounds[4]{};
    float radii[4]{};
    float values[24]{};
    float offset[2]{};
    float opacity = 1.0F;
    std::uint32_t kind = 0U;
};
static_assert(sizeof(LayerConstants) == 160U);

/** The internal colour-filter kind that turns a layer into its drop shadow's source. */
inline constexpr std::uint32_t layer_shadow_source_kind = 6U;

/**
 * The downsample a layer blur of this physical radius runs at: whole factors that keep the kernel
 * within the 32 texels a blur pass reaches, so a wide blur stays a blur rather than clamping.
 */
[[nodiscard]] inline std::uint32_t layer_blur_downsample(const double physical_radius) noexcept {
    if (!(physical_radius > 32.0))
        return 1U;
    const double factor = physical_radius / 32.0;
    return factor >= 8.0 ? 8U : static_cast<std::uint32_t>(factor) + 1U;
}

} // namespace strata::gpu
