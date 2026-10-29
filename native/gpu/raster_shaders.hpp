#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace strata::gpu::raster_shaders {

/**
 * One layer of one raster job, as the kernels read it from the batch's table. The content
 * rectangle lies within the operation's region; the region's origin is its place in the atlas
 * (D3D11 writes there; Vulkan writes a region-sized part of a buffer it then copies). A batch
 * rasterizes many jobs at once, each layer with its own cells, so their bases say where.
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
    /** A stroke layer's pieces and style. */
    std::uint32_t first_piece = 0U;
    std::uint32_t piece_count = 0U;
    std::uint32_t join = 0U;
    std::uint32_t cap = 0U;
    float half_width = 0.0F;
    float miter_limit = 4.0F;
    float tolerance = 0.03F;
    /** Where an art layer's values begin in the art buffer. */
    std::uint32_t art_base = 0U;
    /** Where this layer's cells, its job's sums, its art points and its job's output begin. */
    std::uint32_t cell_base = 0U;
    std::uint32_t sum_base = 0U;
    std::uint32_t points_base = 0U;
    std::uint32_t output_base = 0U;
};
/** The table's stride in both APIs: float4s on 16-byte boundaries, the whole a multiple of 16. */
static_assert(sizeof(Constants) == 144U);

/**
 * One dispatch's constants (a D3D11 constant buffer, Vulkan push constants): its items in the
 * batch list, and how many threads (or, for a resolve, groups) it covers.
 */
struct Dispatch final {
    std::uint32_t first_item = 0U;
    std::uint32_t item_count = 0U;
    std::uint32_t total = 0U;
    std::uint32_t reserved = 0U;
};

/**
 * One item of a dispatch's list: the table entry (`x`) whose indices begin at `y` of the
 * dispatch's, `z` into the entry's own. A thread (or a group) takes the last item at or before
 * its index, so items of many layers share a dispatch.
 */
struct BatchItem final {
    std::uint32_t entry = 0U;
    std::uint32_t first = 0U;
    std::uint32_t offset = 0U;
    std::uint32_t reserved = 0U;
};

/** Threads per group of every kernel: one segment or one row each. */
inline constexpr std::uint32_t group_size = 64U;

/**
 * Threads a stroke piece is dispatched as. One thread walks every cell its polygons' edges cross,
 * one after another, so a long piece alone keeps a thread busy while the rest of the GPU idles;
 * cut across into slices, its threads walk it together. The kernels' `stroke_parts` matches.
 */
inline constexpr std::uint32_t stroke_parts = 16U;

/** The most pieces one stroke dispatch takes: a dispatch has at most 65,535 groups a side. */
inline constexpr std::uint32_t pieces_per_dispatch = 65535U * group_size / stroke_parts;

/** The kernels, in the order backends keep them. */
inline constexpr std::array<const char*, 7U> entries{
    "accumulate",        "stroke",         "resolve_coverage", "resolve_composite", "finish",
    "art_stroke_points", "art_fill_points"};

/**
 * The raster job kernels (strata/raster_job.hpp), a line-for-line port of host/raster_job.cpp's
 * integer core so the GPU's coverage is the reference's exactly:
 *
 *   accumulate         one thread a segment: its exact area into the cells it crosses (atomics)
 *   stroke             one thread a stroke piece: its band, join and caps as convex polygons,
 *                      clipped and quantized in float, then accumulated like segments
 *   resolve_coverage   one group a region row, in 64 chunks: cells to coverage, to the R8 atlas
 *   resolve_composite  one group a content row, in 64 chunks: coverage composited into the sums
 *   finish             one thread a texel: the sums to straight RGBA, written to the atlas
 *   art_stroke_points  an art layer's outline from the points its program wrote, stroked as
 *                      pieces; art_fill_points the same, its edges accumulated like segments
 *
 * Each resolve clears the cells it reads and finish the sums, so the next job starts from zero.
 * A dispatch may work for many jobs' layers at once: its items say which table entry each index
 * belongs to (enter). STRATA_VULKAN selects push constants, explicit bindings and a buffer
 * output.
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
    uint first_piece;
    uint piece_count;
    uint join;
    uint cap;
    float half_width;
    float miter_limit;
    float tolerance;
    uint art_base;
    uint cell_base;
    uint sum_base;
    uint points_base;
    uint output_base;
};

struct StrokePiece {
    float2 p0;
    float2 p1;
    float2 p2;
    uint flags;
    uint reserved;
};

struct DispatchData {
    uint first_item;
    uint item_count;
    uint total;
    uint reserved;
};

#ifdef STRATA_VULKAN
[[vk::push_constant]] ConstantBuffer<DispatchData> dispatch_data;
#else
cbuffer DispatchBuffer : register(b0) {
    DispatchData dispatch_data;
};
#endif

BIND(0) StructuredBuffer<int4> segments : register(t0);
BIND(1) StructuredBuffer<uint> luts : register(t1);
BIND(2) RWStructuredBuffer<int> cells : register(u0);
BIND(3) RWStructuredBuffer<uint> sums : register(u1);
BIND(5) StructuredBuffer<StrokePiece> pieces : register(t2);
BIND(6) StructuredBuffer<float> art_values : register(t3);
BIND(7) RWStructuredBuffer<float4> art_positions : register(u3);
BIND(8) StructuredBuffer<RasterData> table : register(t4);
BIND(9) StructuredBuffer<uint4> batch : register(t5);

/** The layer this thread (or group) works for, which enter() loads. */
static RasterData raster;

/**
 * Finds the layer `index` (a thread's, or a resolve's group's) of this dispatch works for and
 * loads it as `raster`, with `local` the index within that layer; false past the dispatch's end.
 */
bool enter(uint index, out uint local) {
    local = 0;
    if (index >= dispatch_data.total)
        return false;
    uint low = dispatch_data.first_item;
    uint high = low + dispatch_data.item_count - 1;
    [loop] while (low < high) {
        uint middle = (low + high + 1) / 2;
        if (batch[middle].y <= index)
            low = middle;
        else
            high = middle - 1;
    }
    uint4 item = batch[low];
    raster = table[item.x];
    local = index - item.y + item.z;
    return true;
}
#ifdef STRATA_VULKAN
BIND(4) RWStructuredBuffer<uint> output : register(u2);
#elif defined(STRATA_RASTER_RGBA)
RWTexture2D<unorm float4> output : register(u2);
#else
RWTexture2D<unorm float> output : register(u2);
#endif

static const int one = 256;
static const int full_area = 2 * 256 * 256;
/**
 * Cells hold contributions times weight_one, so an art piece can add a fraction of its coverage
 * (artSetWeight); resolved() divides it back out. Every other edge adds exactly weight_one.
 */
static const int weight_one = 32;
static const uint composite_one = 32768;

int floor_divide(int numerator, int divisor) {
    return numerator >= 0 ? numerator / divisor : -((-numerator + divisor - 1) / divisor);
}

void emit(int column, int row, int xa, int xb, int ya, int yb, int sign) {
    if (column >= int(raster.content_width))
        return;
    int left = column * one;
    int height = yb - ya;
    uint at = raster.cell_base + (uint(row) * raster.content_width + uint(column)) * 2;
    InterlockedAdd(cells[at], sign * height);
    InterlockedAdd(cells[at + 1], sign * (xa - left + xb - left) * height);
}

/** Adds a fixed-point edge's exact area, times `orientation`, to the cells it crosses. */
void walk(int x0, int y0, int x1, int y1, int orientation) {
    if (y0 == y1)
        return;
    int sign = orientation;
    if (y0 > y1) {
        int swap_x = x0;
        x0 = x1;
        x1 = swap_x;
        int swap_y = y0;
        y0 = y1;
        y1 = swap_y;
        sign = -orientation;
    }
    int dx = x1 - x0;
    int dy = y1 - y0;
    int xa = x0;
    int ya = y0;
    int last_row = (y1 - 1) / one;
    [loop] for (int row = y0 / one; row <= last_row; ++row) {
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
            [loop] for (;;) {
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
            [loop] for (;;) {
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

[numthreads(64, 1, 1)]
void accumulate(uint3 id : SV_DispatchThreadID) {
    uint local;
    if (!enter(id.x, local) || local >= raster.segment_count)
        return;
    int4 segment = segments[raster.first_segment + local];
    walk(segment.x, segment.y, segment.z, segment.w, weight_one);
}

/** A fixed-point edge in pieces no longer than 16 pixels, so every product fits 32 bits. */
void walk_long(int x0, int y0, int x1, int y1, int orientation) {
    if (y0 == y1)
        return;
    int dx = x1 - x0;
    int dy = y1 - y0;
    int count = max(1, (max(abs(dx), abs(dy)) + 16 * one - 1) / (16 * one));
    int px = x0;
    int py = y0;
    [loop] for (int piece = 1; piece <= count; ++piece) {
        int x = piece == count ? x1 : x0 + floor_divide(dx * piece, count);
        int y = piece == count ? y1 : y0 + floor_divide(dy * piece, count);
        if (y != py)
            walk(px, py, x, y, orientation);
        px = x;
        py = y;
    }
}

int2 to_fixed(float2 position, float2 size) {
    return int2(round(clamp(position, 0.0, size) * float(one)));
}

/**
 * An edge in content pixels, clipped as the encoder clips: above and below dropped, right of the
 * content dropped, left of it kept as a vertical edge on its left side. HLSL inlines every call,
 * so the stroke kernel calls this from one place: the walk behind it is most of the kernel.
 */
void edge(float2 a, float2 b, int orientation) {
    float2 size = float2(raster.content_width, raster.content_height);
    if (a.y == b.y || (a.y <= 0.0 && b.y <= 0.0) || (a.y >= size.y && b.y >= size.y))
        return;
    float2 d = b - a;
    float enter = (0.0 - a.y) / d.y;
    float leave = (size.y - a.y) / d.y;
    if (enter > leave) {
        float swap_value = enter;
        enter = leave;
        leave = swap_value;
    }
    float from = max(0.0, enter);
    float to = min(1.0, leave);
    if (!(from < to))
        return;
    float2 p = from > 0.0 ? float2(a.x + d.x * from, d.y > 0.0 ? 0.0 : size.y) : a;
    float2 q = to < 1.0 ? float2(a.x + d.x * to, d.y > 0.0 ? size.y : 0.0) : b;
    // Split where it crosses the content's left and right sides.
    float2 points[4];
    points[0] = p;
    int count = 1;
    float ex = q.x - p.x;
    if (ex != 0.0) {
        float c0 = (0.0 - p.x) / ex;
        float c1 = (size.x - p.x) / ex;
        float first = min(c0, c1);
        float second = max(c0, c1);
        if (first > 0.0 && first < 1.0) {
            points[count] = float2(ex > 0.0 ? 0.0 : size.x, p.y + (q.y - p.y) * first);
            ++count;
        }
        if (second > 0.0 && second < 1.0) {
            points[count] = float2(ex > 0.0 ? size.x : 0.0, p.y + (q.y - p.y) * second);
            ++count;
        }
    }
    points[count] = q;
    ++count;
    [loop] for (int index = 0; index + 1 < count; ++index) {
        float2 u = points[index];
        float2 v = points[index + 1];
        float middle = (u.x + v.x) * 0.5;
        if (middle >= size.x)
            continue;
        int2 fu = to_fixed(u, size);
        int2 fv = to_fixed(v, size);
        if (middle <= 0.0) {
            fu.x = 0;
            fv.x = 0;
        }
        walk_long(fu.x, fu.y, fv.x, fv.y, orientation);
    }
}

/** Which way a convex polygon through a, b and c winds: every stroke polygon counts one way. */
int winding_of(float2 a, float2 b, float2 c) {
    float z = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
    return z > 0.0 ? 1 : (z < 0.0 ? -1 : 0);
}

float2 rotated(float2 value, float angle) {
    float cosine = cos(angle);
    float sine = sin(angle);
    return float2(value.x * cosine - value.y * sine, value.x * sine + value.y * cosine);
}

/**
 * One of a stroke piece's convex polygons, as data: three or four corners, or a fan of chords
 * about c0 starting at c0 + c1 and turning `sweep` in `steps`. `edges` is zero for none.
 */
struct Polygon {
    int edges;
    int fan;
    float2 c0;
    float2 c1;
    float2 c2;
    float2 c3;
    float sweep;
    int steps;
};

Polygon no_polygon() {
    Polygon polygon;
    polygon.edges = 0;
    polygon.fan = 0;
    polygon.c0 = float2(0.0, 0.0);
    polygon.c1 = float2(0.0, 0.0);
    polygon.c2 = float2(0.0, 0.0);
    polygon.c3 = float2(0.0, 0.0);
    polygon.sweep = 0.0;
    polygon.steps = 0;
    return polygon;
}

Polygon corners(float2 a, float2 b, float2 c, float2 d, int count) {
    Polygon polygon = no_polygon();
    polygon.edges = count;
    polygon.c0 = a;
    polygon.c1 = b;
    polygon.c2 = c;
    polygon.c3 = d;
    return polygon;
}

/** The disc sector about `center` from `center + from` turning `sweep`, in chords. */
Polygon fan_of(float2 center, float2 from, float sweep) {
    float radius = raster.half_width;
    float step = radius > raster.tolerance
                     ? max(2.0 * acos(clamp(1.0 - raster.tolerance / radius, -1.0, 1.0)), 0.02)
                     : 0.39269908;
    Polygon polygon = no_polygon();
    polygon.fan = 1;
    polygon.c0 = center;
    polygon.c1 = from;
    polygon.sweep = sweep;
    polygon.steps = clamp(int(ceil(abs(sweep) / step)), 1, 256);
    polygon.edges = polygon.steps + 2;
    return polygon;
}

float2 corner(Polygon polygon, int index) {
    if (index == 0)
        return polygon.c0;
    if (index == 1)
        return polygon.c1;
    return index == 2 ? polygon.c2 : polygon.c3;
}

float2 fan_point(Polygon polygon, int index) {
    return polygon.c0 + rotated(polygon.c1, polygon.sweep * index / polygon.steps);
}

void edge_of(Polygon polygon, int index, out float2 a, out float2 b) {
    if (polygon.fan != 0) {
        a = index == 0 ? polygon.c0 : fan_point(polygon, index - 1);
        b = index + 1 == polygon.edges ? polygon.c0 : fan_point(polygon, index);
    } else {
        a = corner(polygon, index);
        b = corner(polygon, index + 1 == polygon.edges ? 0 : index + 1);
    }
}

Polygon cap_of(float2 position, float2 outward) {
    if (raster.cap == 0)
        return no_polygon();
    float radius = raster.half_width;
    float2 across = float2(-outward.y, outward.x) * radius;
    if (raster.cap == 2) {
        return corners(position + across, position + across + outward * radius,
                       position - across + outward * radius, position - across, 4);
    }
    // From one side round the outward end to the other.
    float sweep = (across.x * outward.y - across.y * outward.x) > 0.0 ? 3.14159265 : -3.14159265;
    return fan_of(position, across, sweep);
}

/** Threads a stroke piece is dispatched as (the C++ `stroke_parts`). */
static const uint stroke_parts = 16;

/**
 * Slice `part` of one stroke piece: its band cut across into up to stroke_parts butt-ended slices
 * of about 16 pixels, the first with the start cap and the last with the join and end cap. The
 * slices tile the band, and the edge two share is walked once each way, so it cancels.
 */
void stroke_piece(StrokePiece piece, uint part, int weight) {
    float radius = raster.half_width;
    float2 d = piece.p1 - piece.p0;
    float length_value = length(d);
    if (!(radius > 0.0) || weight <= 0)
        return;
    // A piece that does not move and is capped is a dot, as SVG draws one: just its caps.
    bool still = !(length_value > 1.0e-6);
    if (still && ((piece.flags & 6) == 0 || raster.cap == 0))
        return;
    uint parts = still ? 1u : clamp(uint(ceil(length_value / 16.0)), 1u, stroke_parts);
    if (part >= parts)
        return;
    bool first = part == 0;
    bool last = part + 1 == parts;
    float2 from = first ? piece.p0 : piece.p0 + d * (float(part) / float(parts));
    float2 to = last ? piece.p1 : piece.p0 + d * (float(part + 1) / float(parts));
    // A slice's polygons are closed and lie within its reach: one that misses the content adds
    // nothing to it.
    float reach = radius * max(max(1.0, raster.join == 0 ? raster.miter_limit : 1.0),
                               raster.cap == 2 ? 1.4142136 : 1.0) + 1.0;
    float2 low = min(from, to) - reach;
    float2 high = max(from, to) + reach;
    if (any(high < 0.0) || low.x > float(raster.content_width) ||
        low.y > float(raster.content_height))
        return;
    float2 along = still ? float2(1.0, 0.0) : d / length_value;
    float2 side = float2(-along.y, along.x) * radius;
    // The band; the join with the next piece, on the side the path turns away from (the bands
    // overlap on the other); the caps of a run's ends.
    Polygon band = no_polygon();
    if (!still)
        band = corners(from + side, to + side, to - side, from - side, 4);
    Polygon join = no_polygon();
    if (!still && last && (piece.flags & 1) != 0) {
        float2 e = piece.p2 - piece.p1;
        float next_length = length(e);
        if (next_length > 1.0e-6) {
            float2 next = e / next_length;
            float2 next_side = float2(-next.y, next.x) * radius;
            float turn = along.x * next.y - along.y * next.x;
            if (abs(turn) > 1.0e-6 || dot(along, next) < 0.0) {
                float outer = turn > 0.0 ? -1.0 : 1.0;
                float2 a = piece.p1 + side * outer;
                float2 b = piece.p1 + next_side * outer;
                float2 from = a - piece.p1;
                float2 to = b - piece.p1;
                if (raster.join == 1) {
                    join = fan_of(piece.p1, from,
                                  atan2(from.x * to.y - from.y * to.x, dot(from, to)));
                } else {
                    float2 bisector = from + to;
                    float bisector_length = length(bisector);
                    float cosine = bisector_length > 1.0e-9
                                       ? dot(bisector, from) / (bisector_length * radius)
                                       : 0.0;
                    if (raster.join == 0 && cosine > 1.0e-6 && 1.0 / cosine <= raster.miter_limit)
                        join = corners(piece.p1, a,
                                       piece.p1 + bisector * (radius / cosine / bisector_length),
                                       b, 4);
                    else
                        join = corners(piece.p1, a, b, b, 3);
                }
            }
        }
    }
    Polygon start = no_polygon();
    if (first && (piece.flags & 2) != 0)
        start = cap_of(piece.p0, -along);
    Polygon end = no_polygon();
    if (last && (piece.flags & 4) != 0)
        end = cap_of(piece.p1, along);
    [loop] for (int shape = 0; shape < 4; ++shape) {
        Polygon polygon = band;
        if (shape == 1)
            polygon = join;
        else if (shape == 2)
            polygon = start;
        else if (shape == 3)
            polygon = end;
        if (polygon.edges == 0)
            continue;
        float2 a0;
        float2 b0;
        float2 a1;
        float2 b1;
        edge_of(polygon, 0, a0, b0);
        edge_of(polygon, 1, a1, b1);
        int orientation = winding_of(a0, b0, b1);
        if (orientation == 0)
            continue;
        [loop] for (int index = 0; index < polygon.edges; ++index) {
            float2 a;
            float2 b;
            edge_of(polygon, index, a, b);
            edge(a, b, orientation * weight);
        }
    }
}

[numthreads(64, 1, 1)]
void stroke(uint3 id : SV_DispatchThreadID) {
    uint local;
    if (!enter(id.x, local) || local / stroke_parts >= raster.piece_count)
        return;
    stroke_piece(pieces[raster.first_piece + local / stroke_parts], local % stroke_parts,
                 weight_one);
}

bool art_finite(float2 value) {
    return all(abs(value) <= 1.0e6);
}

/**
 * An art layer's outline stroked from the points its program wrote to art_positions: each
 * segment stroke_parts threads, weighted by its first point. In segments mode points pair off,
 * each pair a line capped at both ends.
 */
[numthreads(64, 1, 1)]
void art_stroke_points(uint3 id : SV_DispatchThreadID) {
    uint local;
    if (!enter(id.x, local))
        return;
    uint count = uint(art_values[raster.art_base + 1]);
    bool closed = art_values[raster.art_base + 2] > 0.5;
    bool pairs = art_values[raster.art_base + 7] > 0.5;
    uint segments = pairs ? count / 2 : (closed ? count : count - 1);
    uint segment = local / stroke_parts;
    if (segment >= segments)
        return;
    uint base = raster.points_base;
    float4 start = art_positions[base + (pairs ? segment * 2 : segment)];
    float4 end = art_positions[base + (pairs ? segment * 2 + 1 : (segment + 1) % count)];
    float2 offset = float2(art_values[raster.art_base + 3], art_values[raster.art_base + 4]);
    int weight = int(round(saturate(start.z) * float(weight_one)));
    if (weight <= 0)
        return;
    bool joined = !pairs && (closed || segment + 2 < count);
    StrokePiece piece;
    piece.p0 = offset + start.xy;
    piece.p1 = offset + end.xy;
    piece.p2 = joined ? offset + art_positions[base + (segment + 2) % count].xy : piece.p1;
    piece.flags = pairs ? 6
                        : (joined ? 1 : 0) | (!closed && segment == 0 ? 2 : 0) |
                              (!closed && segment + 1 == segments ? 4 : 0);
    piece.reserved = 0;
    if (!art_finite(piece.p0) || !art_finite(piece.p1) || !art_finite(piece.p2))
        return;
    stroke_piece(piece, local % stroke_parts, weight);
}

/** An art layer's outline filled from its points: one thread an edge, closed. */
[numthreads(64, 1, 1)]
void art_fill_points(uint3 id : SV_DispatchThreadID) {
    uint local;
    if (!enter(id.x, local))
        return;
    uint count = uint(art_values[raster.art_base + 1]);
    if (local >= count)
        return;
    float2 offset = float2(art_values[raster.art_base + 3], art_values[raster.art_base + 4]);
    float2 a = offset + art_positions[raster.points_base + local].xy;
    float2 b = offset + art_positions[raster.points_base + (local + 1) % count].xy;
    if (art_finite(a) && art_finite(b))
        edge(a, b, weight_one);
}

int resolved(int cover, int area) {
    int value = (abs(cover * (2 * one) - area) + weight_one / 2) / weight_one;
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
    uint at = raster.cell_base + (row * raster.content_width + column) * 2;
    int2 cell = int2(cells[at], cells[at + 1]);
    // Most cells of a line drawing are never touched: clearing only the touched ones spares the
    // writes.
    if (cell.x != 0 || cell.y != 0) {
        cells[at] = 0;
        cells[at + 1] = 0;
    }
    return cell;
}

groupshared int lane_cover[64];

/**
 * A row's cover left of this lane's chunk: each lane sums the cover of its chunk of the content
 * row, then counts the chunks before its own. Rows are a group each, so a wide row resolves in
 * 64 chunks at once rather than one column after another.
 */
int cover_before(uint row, uint begin, uint end, uint lane) {
    int sum = 0;
    [loop] for (uint column = begin; column < end; ++column)
        sum += cells[raster.cell_base + (row * raster.content_width + column) * 2];
    lane_cover[lane] = sum;
    GroupMemoryBarrierWithGroupSync();
    int before = 0;
    [loop] for (uint index = 0; index < lane; ++index)
        before += lane_cover[index];
    return before;
}

/** This lane's chunk of `width` columns: sixty-four chunks, each a multiple of four wide. */
uint2 lane_chunk(uint width, uint lane) {
    uint chunk = (((width + 63) / 64) + 3) & ~3u;
    uint begin = min(lane * chunk, width);
    return uint2(begin, min(begin + chunk, width));
}

/**
 * A group a region row, walked in tiles of 256 columns, each lane four neighbouring columns: a
 * tile's cells are read and its texels written as one contiguous run. A scan of the lanes' cover
 * gives each the cover left of its columns, and the tile's total carries to the next.
 */
[numthreads(64, 1, 1)]
void resolve_coverage(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
    // Every lane of a group finds the same layer, or leaves together.
    uint row;
    if (!enter(group.x, row))
        return;
    uint lane = thread.x;
    bool content_row = row >= raster.content_y && row < raster.content_y + raster.content_height;
    uint content_end = raster.content_x + raster.content_width;
    int carry = 0;
    [loop] for (uint tile = 0; tile < raster.region_width; tile += 256) {
        uint first = tile + lane * 4;
        int2 cell[4];
        int sum = 0;
        [unroll] for (uint k = 0; k < 4; ++k) {
            uint column = first + k;
            cell[k] = int2(0, 0);
            if (content_row && column >= raster.content_x && column < content_end)
                cell[k] = take_cell(column - raster.content_x, row - raster.content_y);
            sum += cell[k].x;
        }
        lane_cover[lane] = sum;
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint offset = 1; offset < 64; offset <<= 1) {
            int add = lane >= offset ? lane_cover[lane - offset] : 0;
            GroupMemoryBarrierWithGroupSync();
            lane_cover[lane] += add;
            GroupMemoryBarrierWithGroupSync();
        }
        int running = carry + lane_cover[lane] - sum;
        int total = lane_cover[63];
        GroupMemoryBarrierWithGroupSync();
#ifdef STRATA_VULKAN
        uint packed = 0;
#endif
        [unroll] for (uint k = 0; k < 4; ++k) {
            uint column = first + k;
            uint value = 0;
            if (content_row && column >= raster.content_x && column < content_end) {
                running += cell[k].x;
                value = coverage_byte(resolved(running, cell[k].y));
            }
#ifdef STRATA_VULKAN
            packed |= value << (k * 8);
#else
            if (column < raster.region_width)
                output[uint2(raster.region_x + column, raster.region_y + row)] = float(value) / 255.0;
#endif
        }
#ifdef STRATA_VULKAN
        if (first < raster.region_width)
            output[raster.output_base + row * raster.output_stride + first / 4] = packed;
#endif
        carry += total;
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
void resolve_composite(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
    uint row;
    if (!enter(group.x, row))
        return;
    uint2 chunk = lane_chunk(raster.content_width, thread.x);
    int running = cover_before(row, chunk.x, chunk.y, thread.x);
    [loop] for (uint column = chunk.x; column < chunk.y; ++column) {
        int2 cell = take_cell(column, row);
        running += cell.x;
        int covered = resolved(running, cell.y);
        if (covered == 0)
            continue;
        uint color = layer_color(column, row);
        uint at = raster.sum_base + (row * raster.content_width + column) * 5;
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
    uint local;
    if (!enter(id.x, local) || local >= raster.region_width * raster.region_height)
        return;
    uint row = local / raster.region_width;
    uint column = local - row * raster.region_width;
    uint texel = 0;
    if (row >= raster.content_y && row < raster.content_y + raster.content_height &&
        column >= raster.content_x && column < raster.content_x + raster.content_width) {
        uint at = raster.sum_base + ((row - raster.content_y) * raster.content_width +
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
    output[raster.output_base + row * raster.output_stride + column] = texel;
#else
    output[uint2(raster.region_x + column, raster.region_y + row)] =
        float4(texel & 0xFF, (texel >> 8) & 0xFF, (texel >> 16) & 0xFF, texel >> 24) / 255.0;
#endif
}
)hlsl";

/**
 * An art program's one kernel, compiled once per program: its points, in canvas pixels, to
 * art_positions, which art_stroke_points or art_fill_points then outline.
 */
inline constexpr const char* art_entry = "art_points";

/**
 * Values an art kernel reads besides the job's constants, as floats: the time in seconds, the
 * point count, whether the outline closes, the canvas (x, y, width, height) in content pixels,
 * whether points pair off into segments, then sixteen parameters.
 */
inline constexpr std::size_t art_value_count = 24U;

/** What an art program may call, ahead of its source. */
constexpr std::string_view art_prelude = R"hlsl(
/** Seconds since the renderer began presenting. */
float artTime() { return art_values[raster.art_base + 0]; }
/** One of the sixteen parameters the canvas was drawn with. */
float artParameter(uint index) { return index < 16 ? art_values[raster.art_base + 8 + index] : 0.0; }
/** The canvas's size in device pixels. */
float2 artSize() { return float2(art_values[raster.art_base + 5], art_values[raster.art_base + 6]); }
static float art_weight;
/**
 * From artPoint: how much of its coverage the stroke piece this point starts adds, 0 to 1 (1
 * unless set); a piece of weight 0 is skipped. Lines that fade one by one, say.
 */
void artSetWeight(float weight) { art_weight = saturate(weight); }
/**
 * `unit`, a point of a figure drawn round the origin with -1 to 1 spanning `reach` of the canvas's
 * shorter side, in the canvas's 0-1 space: centred, and round whatever the canvas's shape.
 */
float2 artCentred(float2 unit, float reach) {
    float2 size = artSize();
    return 0.5 + unit * (min(size.x, size.y) * 0.5 * reach) / max(size, 1.0);
}
)hlsl";

/** The art entry point, after the program, which defines artPoint(index, count). */
constexpr std::string_view art_kernels = R"hlsl(
// A point in canvas pixels; the outline kernels add where the canvas lies in the job, so every
// tile of one art reads the same points.
float2 art_position(uint index, uint count) {
    return artPoint(index, count) * float2(art_values[raster.art_base + 5], art_values[raster.art_base + 6]);
}

[numthreads(64, 1, 1)]
void art_points(uint3 id : SV_DispatchThreadID) {
    uint local;
    if (!enter(id.x, local))
        return;
    uint count = uint(art_values[raster.art_base + 1]);
    if (local < count) {
        art_weight = 1.0;
        float2 position = art_position(local, count);
        art_positions[raster.points_base + local] = float4(position, art_weight, 0.0);
    }
}
)hlsl";

/** The source an art program compiles from: the raster kernels, the prelude, it, the entries. */
inline std::string art_source(const std::string_view program) {
    std::string result;
    result.reserve(source.size() + art_prelude.size() + program.size() + art_kernels.size() + 2U);
    result += source;
    result += art_prelude;
    result += program;
    result += "\n";
    result += art_kernels;
    return result;
}

} // namespace strata::gpu::raster_shaders
