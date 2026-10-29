#pragma once

#include <cstdint>
#include <string_view>

namespace strata::gpu::raster_shaders {

/**
 * Per-dispatch constants of the raster kernels: a D3D11 constant buffer, Vulkan push constants.
 * The content rectangle lies within the operation's region; the region's origin is its place in
 * the atlas (D3D11 writes there; Vulkan writes a region-sized buffer it then copies).
 */
struct Constants final {
    std::uint32_t content_x = 0U;
    std::uint32_t content_y = 0U;
    std::uint32_t content_width = 0U;
    std::uint32_t content_height = 0U;
    std::uint32_t region_x = 0U;
    std::uint32_t region_y = 0U;
    std::uint32_t region_width = 0U;
    std::uint32_t region_height = 0U;
    std::uint32_t first_segment = 0U;
    std::uint32_t segment_count = 0U;
    std::uint32_t fill = 0U;
    std::uint32_t paint = 0U;
    std::uint32_t color = 0U;
    std::uint32_t extend = 0U;
    std::uint32_t lut_offset = 0U;
    /** Uints between output rows (Vulkan's buffer output only). */
    std::uint32_t output_stride = 0U;
    float parameters[8]{};
};
static_assert(sizeof(Constants) == 96U);

/** Threads per group of every kernel: one segment or one row each. */
inline constexpr std::uint32_t group_size = 64U;

/**
 * The raster job kernels (strata/raster_job.hpp), a line-for-line port of host/raster_job.cpp's
 * integer core so the GPU's coverage is the reference's exactly:
 *
 *   accumulate         one thread a segment: its exact area into the cells it crosses (atomics)
 *   resolve_coverage   one thread a region row: cells to coverage, written to the R8 atlas
 *   resolve_composite  one thread a content row: cells to coverage, composited into the sums
 *   finish             one thread a region row: the sums to straight RGBA, written to the atlas
 *
 * Each resolve clears the cells it reads and finish the sums, so the next job starts from zero.
 * STRATA_VULKAN selects push constants, explicit bindings and a buffer output.
 */
constexpr std::string_view source = R"hlsl(
#ifdef STRATA_VULKAN
#define BIND(n) [[vk::binding(n, 0)]]
#else
#define BIND(n)
#endif

struct RasterData {
    uint content_x;
    uint content_y;
    uint content_width;
    uint content_height;
    uint region_x;
    uint region_y;
    uint region_width;
    uint region_height;
    uint first_segment;
    uint segment_count;
    uint fill;
    uint paint;
    uint color;
    uint extend;
    uint lut_offset;
    uint output_stride;
    float4 parameters_a;
    float4 parameters_b;
};

#ifdef STRATA_VULKAN
[[vk::push_constant]] ConstantBuffer<RasterData> raster;
#else
cbuffer RasterBuffer : register(b0) {
    RasterData raster;
};
#endif

BIND(0) StructuredBuffer<int4> segments : register(t0);
BIND(1) StructuredBuffer<uint> luts : register(t1);
BIND(2) RWStructuredBuffer<int> cells : register(u0);
BIND(3) RWStructuredBuffer<uint> sums : register(u1);
#ifdef STRATA_VULKAN
BIND(4) RWStructuredBuffer<uint> output : register(u2);
#elif defined(STRATA_RASTER_RGBA)
RWTexture2D<unorm float4> output : register(u2);
#else
RWTexture2D<unorm float> output : register(u2);
#endif

static const int one = 256;
static const int full_area = 2 * 256 * 256;
static const uint composite_one = 32768;

int floor_divide(int numerator, int divisor) {
    return numerator >= 0 ? numerator / divisor : -((-numerator + divisor - 1) / divisor);
}

void emit(int column, int row, int xa, int xb, int ya, int yb, int sign) {
    if (column >= int(raster.content_width))
        return;
    int left = column * one;
    int height = yb - ya;
    uint at = (uint(row) * raster.content_width + uint(column)) * 2;
    InterlockedAdd(cells[at], sign * height);
    InterlockedAdd(cells[at + 1], sign * (xa - left + xb - left) * height);
}

[numthreads(64, 1, 1)]
void accumulate(uint3 id : SV_DispatchThreadID) {
    if (id.x >= raster.segment_count)
        return;
    int4 segment = segments[raster.first_segment + id.x];
    int x0 = segment.x;
    int y0 = segment.y;
    int x1 = segment.z;
    int y1 = segment.w;
    if (y0 == y1)
        return;
    int sign = 1;
    if (y0 > y1) {
        int swap_x = x0;
        x0 = x1;
        x1 = swap_x;
        int swap_y = y0;
        y0 = y1;
        y1 = swap_y;
        sign = -1;
    }
    int dx = x1 - x0;
    int dy = y1 - y0;
    int xa = x0;
    int ya = y0;
    int last_row = (y1 - 1) / one;
    for (int row = y0 / one; row <= last_row; ++row) {
        int top = row * one;
        int yb = min(y1, top + one);
        int xb = yb == y1 ? x1 : x0 + floor_divide(dx * (yb - y0), dy);
        int cx = xa;
        int cy = ya - top;
        int end_y = yb - top;
        if (xa == xb) {
            emit(xa / one, row, xa, xb, cy, end_y, sign);
        } else if (xa < xb) {
            int column = xa / one;
            for (;;) {
                int side = (column + 1) * one;
                if (side >= xb) {
                    emit(column, row, cx, xb, cy, end_y, sign);
                    break;
                }
                int sy = cy + (side - cx) * (end_y - cy) / (xb - cx);
                emit(column, row, cx, side, cy, sy, sign);
                cx = side;
                cy = sy;
                ++column;
            }
        } else {
            int column = (xa - 1) / one;
            for (;;) {
                int side = column * one;
                if (side <= xb) {
                    emit(column, row, cx, xb, cy, end_y, sign);
                    break;
                }
                int sy = cy + (cx - side) * (end_y - cy) / (cx - xb);
                emit(column, row, cx, side, cy, sy, sign);
                cx = side;
                cy = sy;
                --column;
            }
        }
        xa = xb;
        ya = yb;
    }
}

int resolved(int cover, int area) {
    int value = abs(cover * (2 * one) - area);
    if (raster.fill == 1) {
        value = value & (2 * full_area - 1);
        if (value > full_area)
            value = 2 * full_area - value;
    }
    return min(value, full_area);
}

uint coverage_byte(int area) {
    return uint((area * 255 + full_area / 2) / full_area);
}

/** Reads a content cell's cover and area and clears it. */
int2 take_cell(uint column, uint row) {
    uint at = (row * raster.content_width + column) * 2;
    int2 cell = int2(cells[at], cells[at + 1]);
    cells[at] = 0;
    cells[at + 1] = 0;
    return cell;
}

[numthreads(64, 1, 1)]
void resolve_coverage(uint3 id : SV_DispatchThreadID) {
    uint row = id.x;
    if (row >= raster.region_height)
        return;
    bool content_row = row >= raster.content_y && row < raster.content_y + raster.content_height;
    int running = 0;
#ifdef STRATA_VULKAN
    uint packed = 0;
#endif
    for (uint column = 0; column < raster.region_width; ++column) {
        uint value = 0;
        if (content_row && column >= raster.content_x &&
            column < raster.content_x + raster.content_width) {
            int2 cell = take_cell(column - raster.content_x, row - raster.content_y);
            running += cell.x;
            value = coverage_byte(resolved(running, cell.y));
        }
#ifdef STRATA_VULKAN
        packed |= value << ((column & 3) * 8);
        if ((column & 3) == 3 || column + 1 == raster.region_width) {
            output[row * raster.output_stride + column / 4] = packed;
            packed = 0;
        }
#else
        output[uint2(raster.region_x + column, raster.region_y + row)] = float(value) / 255.0;
#endif
    }
}

uint layer_color(uint column, uint row) {
    if (raster.paint < 2)
        return raster.color;
    float x = float(column) + 0.5;
    float y = float(row) + 0.5;
    float t = raster.parameters_a.x * x + raster.parameters_a.y * y + raster.parameters_a.z;
    if (raster.paint == 3) {
        float v = raster.parameters_a.w * x + raster.parameters_b.x * y + raster.parameters_b.y;
        t = sqrt(t * t + v * v);
    }
    if (raster.extend == 1) {
        t = t - floor(t);
    } else if (raster.extend == 2) {
        t = t - 2.0 * floor(t * 0.5);
        t = t > 1.0 ? 2.0 - t : t;
    }
    float index = clamp(t * 255.0 + 0.5, 0.0, 255.0);
    return luts[raster.lut_offset + uint(index)];
}

[numthreads(64, 1, 1)]
void resolve_composite(uint3 id : SV_DispatchThreadID) {
    uint row = id.x;
    if (row >= raster.content_height)
        return;
    int running = 0;
    for (uint column = 0; column < raster.content_width; ++column) {
        int2 cell = take_cell(column, row);
        running += cell.x;
        int covered = resolved(running, cell.y);
        if (covered == 0)
            continue;
        uint color = layer_color(column, row);
        uint at = (row * raster.content_width + column) * 5;
        uint red = sums[at];
        uint green = sums[at + 1];
        uint blue = sums[at + 2];
        uint alpha = sums[at + 3];
        uint area = sums[at + 4];
        uint c = uint((covered + 2) / 4);
        uint exposed = min(c, composite_one - area);
        uint shared_part = c - exposed;
        uint through = area > 0 ? (area > alpha ? (area - alpha) * composite_one / area : 0)
                                : composite_one;
        uint weight = ((color >> 24) * (exposed + shared_part * through / composite_one) + 127) / 255;
        sums[at] = red + (color & 0xFF) * weight;
        sums[at + 1] = green + ((color >> 8) & 0xFF) * weight;
        sums[at + 2] = blue + ((color >> 16) & 0xFF) * weight;
        sums[at + 3] = min(composite_one, alpha + weight);
        sums[at + 4] = min(composite_one, area + exposed);
    }
}

uint straight_channel(uint premultiplied, uint alpha) {
    return min(255u, (premultiplied + alpha / 2) / alpha);
}

[numthreads(64, 1, 1)]
void finish(uint3 id : SV_DispatchThreadID) {
    uint row = id.x;
    if (row >= raster.region_height)
        return;
    bool content_row = row >= raster.content_y && row < raster.content_y + raster.content_height;
    for (uint column = 0; column < raster.region_width; ++column) {
        uint texel = 0;
        if (content_row && column >= raster.content_x &&
            column < raster.content_x + raster.content_width) {
            uint at = ((row - raster.content_y) * raster.content_width +
                       (column - raster.content_x)) * 5;
            uint alpha = sums[at + 3];
            if (alpha > 0) {
                texel = straight_channel(sums[at], alpha) |
                        (straight_channel(sums[at + 1], alpha) << 8) |
                        (straight_channel(sums[at + 2], alpha) << 16) |
                        (((alpha * 255 + composite_one / 2) / composite_one) << 24);
            }
            sums[at] = 0;
            sums[at + 1] = 0;
            sums[at + 2] = 0;
            sums[at + 3] = 0;
            sums[at + 4] = 0;
        }
#ifdef STRATA_VULKAN
        output[row * raster.output_stride + column] = texel;
#else
        output[uint2(raster.region_x + column, raster.region_y + row)] =
            float4(texel & 0xFF, (texel >> 8) & 0xFF, (texel >> 16) & 0xFF, texel >> 24) / 255.0;
#endif
    }
}
)hlsl";

} // namespace strata::gpu::raster_shaders
