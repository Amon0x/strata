#include <strata/raster_job.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>

#include <strata/render_packet.hpp>

namespace strata::host {
namespace {

static_assert(std::endian::native == std::endian::little, "raster jobs are little-endian");

/** Header words: content x, y, width, height, layer count, segment count, piece count. */
constexpr std::size_t header_words = 7U;
/**
 * Layer words before its colour table: fill, paint, colour, extend, six parameters, segment range,
 * stroke half width, join, cap, mitre limit and tolerance, piece range, and whether art follows.
 */
constexpr std::size_t layer_words = 20U;
/** Art words before its source: count, flags, canvas x, y, width, height, 16 parameters, length. */
constexpr std::size_t art_words = 23U;
/** One pixel's full coverage as resolved area: 2 x raster_one x raster_one. */
constexpr std::int32_t full_area = 2 * raster_one * raster_one;
/** Composite fractions are fifteen-bit fixed point. */
constexpr std::int32_t one = 32768;

[[nodiscard]] bool gradient(const RasterPaint paint) noexcept {
    return paint == RasterPaint::linear || paint == RasterPaint::radial;
}

[[nodiscard]] bool r8(const std::uint32_t format) noexcept {
    return format == texture_format_r8 || format == texture_format_r8_filtered;
}

// The integer core every backend shares (gpu/raster_shaders.hpp mirrors it line for line).

/** Floor division by a positive divisor, built from non-negative division only. */
[[nodiscard]] std::int32_t floor_divide(const std::int32_t numerator,
                                        const std::int32_t divisor) noexcept {
    return numerator >= 0 ? numerator / divisor : -((-numerator + divisor - 1) / divisor);
}

/**
 * Adds one edge's exact area to the cells it crosses: per cell its cover (signed height) and its
 * area (cover times the summed distances of its ends from the cell's left side), as in FreeType's
 * gray rasterizer. Cells right of the content are skipped: they reach nothing to their right.
 */
template <class Add>
void walk_segment(const RasterSegment segment, const std::int32_t width, Add&& add) {
    std::int32_t x0 = segment.x0;
    std::int32_t y0 = segment.y0;
    std::int32_t x1 = segment.x1;
    std::int32_t y1 = segment.y1;
    if (y0 == y1)
        return;
    std::int32_t sign = 1;
    if (y0 > y1) {
        std::swap(x0, x1);
        std::swap(y0, y1);
        sign = -1;
    }
    const std::int32_t dx = x1 - x0;
    const std::int32_t dy = y1 - y0;
    const auto emit = [&](const std::int32_t column, const std::int32_t row, const std::int32_t xa,
                          const std::int32_t xb, const std::int32_t ya, const std::int32_t yb) {
        if (column >= width)
            return;
        const std::int32_t left = column * raster_one;
        const std::int32_t height = yb - ya;
        add(column, row, sign * height, sign * (xa - left + xb - left) * height);
    };
    std::int32_t xa = x0;
    std::int32_t ya = y0;
    const std::int32_t last_row = (y1 - 1) / raster_one;
    for (std::int32_t row = y0 / raster_one; row <= last_row; ++row) {
        const std::int32_t top = row * raster_one;
        const std::int32_t yb = std::min(y1, top + raster_one);
        const std::int32_t xb = yb == y1 ? x1 : x0 + floor_divide(dx * (yb - y0), dy);
        // Across the row's cells, splitting where the edge crosses a cell's side.
        std::int32_t cx = xa;
        std::int32_t cy = ya - top;
        const std::int32_t end_y = yb - top;
        if (xa == xb) {
            emit(xa / raster_one, row, xa, xb, cy, end_y);
        } else if (xa < xb) {
            std::int32_t column = xa / raster_one;
            for (;;) {
                const std::int32_t side = (column + 1) * raster_one;
                if (side >= xb) {
                    emit(column, row, cx, xb, cy, end_y);
                    break;
                }
                const std::int32_t sy = cy + (side - cx) * (end_y - cy) / (xb - cx);
                emit(column, row, cx, side, cy, sy);
                cx = side;
                cy = sy;
                ++column;
            }
        } else {
            std::int32_t column = (xa - 1) / raster_one;
            for (;;) {
                const std::int32_t side = column * raster_one;
                if (side <= xb) {
                    emit(column, row, cx, xb, cy, end_y);
                    break;
                }
                const std::int32_t sy = cy + (cx - side) * (end_y - cy) / (cx - xb);
                emit(column, row, cx, side, cy, sy);
                cx = side;
                cy = sy;
                --column;
            }
        }
        xa = xb;
        ya = yb;
    }
}

/** A pixel's covered area, out of full_area, from its row's running cover and its own area. */
[[nodiscard]] std::int32_t resolved(const std::int32_t cover, const std::int32_t area,
                                    const RasterFill fill) noexcept {
    std::int32_t value = cover * (2 * raster_one) - area;
    value = value < 0 ? -value : value;
    if (fill == RasterFill::even_odd) {
        value &= 2 * full_area - 1;
        if (value > full_area)
            value = 2 * full_area - value;
    }
    return std::min(value, full_area);
}

[[nodiscard]] std::uint32_t coverage_byte(const std::int32_t area) noexcept {
    return static_cast<std::uint32_t>((area * 255 + full_area / 2) / full_area);
}

/** A pixel of a composite: premultiplied colour sums, opacity and covered area. */
struct Sum final {
    std::uint32_t red = 0U;
    std::uint32_t green = 0U;
    std::uint32_t blue = 0U;
    std::uint32_t alpha = 0U;
    std::uint32_t area = 0U;
};

/** Composites a layer's coverage (out of `one`) and colour into a pixel, front to back. */
void composite(Sum& sum, const std::int32_t covered, const std::uint32_t color) noexcept {
    const auto c = static_cast<std::uint32_t>(covered);
    const std::uint32_t exposed = std::min(c, static_cast<std::uint32_t>(one) - sum.area);
    const std::uint32_t shared = c - exposed;
    const std::uint32_t through =
        sum.area > 0U ? (sum.area > sum.alpha ? (sum.area - sum.alpha) * one / sum.area : 0U)
                      : static_cast<std::uint32_t>(one);
    const std::uint32_t weight =
        ((color >> 24U) * (exposed + shared * through / one) + 127U) / 255U;
    sum.red += (color & 0xFFU) * weight;
    sum.green += ((color >> 8U) & 0xFFU) * weight;
    sum.blue += ((color >> 16U) & 0xFFU) * weight;
    sum.alpha = std::min(static_cast<std::uint32_t>(one), sum.alpha + weight);
    sum.area = std::min(static_cast<std::uint32_t>(one), sum.area + exposed);
}

[[nodiscard]] std::uint32_t straight(const Sum& sum) noexcept {
    if (sum.alpha == 0U)
        return 0U;
    const auto channel = [&](const std::uint32_t premultiplied) {
        return std::min(255U, (premultiplied + sum.alpha / 2U) / sum.alpha);
    };
    const std::uint32_t alpha = (sum.alpha * 255U + one / 2) / one;
    return channel(sum.red) | (channel(sum.green) << 8U) | (channel(sum.blue) << 16U) |
           (alpha << 24U);
}

/** A layer's colour at a content pixel: its own, or its gradient's at the pixel centre. */
[[nodiscard]] std::uint32_t layer_color(const RasterLayer& layer, const std::uint32_t column,
                                        const std::uint32_t row) noexcept {
    if (!gradient(layer.paint))
        return layer.color;
    const float x = static_cast<float>(column) + 0.5F;
    const float y = static_cast<float>(row) + 0.5F;
    const std::array<float, 6U>& p = layer.parameters;
    float t = p[0] * x + p[1] * y + p[2];
    if (layer.paint == RasterPaint::radial) {
        const float v = p[3] * x + p[4] * y + p[5];
        t = std::sqrt(t * t + v * v);
    }
    if (layer.extend == RasterExtend::repeat) {
        t = t - std::floor(t);
    } else if (layer.extend == RasterExtend::mirror) {
        t = t - 2.0F * std::floor(t * 0.5F);
        t = t > 1.0F ? 2.0F - t : t;
    }
    const float index = std::clamp(t * 255.0F + 0.5F, 0.0F, 255.0F);
    return layer.lut[static_cast<std::uint32_t>(index)];
}

/** Writes into bytes sized up front. */
class Writer final {
  public:
    explicit Writer(const std::size_t size) : bytes_(size) {}
    void u32(const std::uint32_t value) { append(&value, 4U); }
    void f32(const float value) { append(&value, 4U); }
    void append(const void* const value, const std::size_t size) {
        std::memcpy(bytes_.data() + offset_, value, size);
        offset_ += size;
    }
    [[nodiscard]] std::vector<std::uint8_t> take() && { return std::move(bytes_); }

  private:
    std::vector<std::uint8_t> bytes_;
    std::size_t offset_ = 0U;
};

class Reader final {
  public:
    explicit Reader(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    [[nodiscard]] std::uint32_t u32() { return read<std::uint32_t>(); }
    [[nodiscard]] float f32() { return read<float>(); }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    void copy(void* const target, const std::size_t size) {
        if (remaining() < size)
            throw std::invalid_argument("raster job is truncated");
        std::memcpy(target, bytes_.data() + offset_, size);
        offset_ += size;
    }

  private:
    template <class Value>
    [[nodiscard]] Value read() {
        if (remaining() < sizeof(Value))
            throw std::invalid_argument("raster job is truncated");
        Value value;
        std::memcpy(&value, bytes_.data() + offset_, sizeof(Value));
        offset_ += sizeof(Value);
        return value;
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0U;
};

/**
 * Clips an edge to a `width` x `height` content in pixels and quantizes it: parts above or below
 * are dropped, the part right of it too, and the part left of it becomes a vertical edge on its
 * left side. `emit` takes each kept part's fixed-point ends.
 */
template <class Emit>
void clip_edge(const double ax, const double ay, const double bx, const double by,
               const double width, const double height, Emit&& emit) {
    if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(bx) || !std::isfinite(by) ||
        ay == by) {
        return;
    }
    constexpr double unit = raster_one;
    if (ax >= 0.0 && ax <= width && bx >= 0.0 && bx <= width && ay >= 0.0 && ay <= height &&
        by >= 0.0 && by <= height) {
        emit(std::llround(ax * unit), std::llround(ay * unit), std::llround(bx * unit),
             std::llround(by * unit));
        return;
    }
    if ((ay <= 0.0 && by <= 0.0) || (ay >= height && by >= height))
        return;
    // Within the content's rows.
    const double dx = bx - ax;
    const double dy = by - ay;
    double enter = (0.0 - ay) / dy;
    double leave = (height - ay) / dy;
    if (enter > leave)
        std::swap(enter, leave);
    const double from = std::max(0.0, enter);
    const double to = std::min(1.0, leave);
    if (!(from < to))
        return;
    RasterPoint p{ax, ay};
    RasterPoint q{bx, by};
    if (from > 0.0)
        p = RasterPoint{ax + dx * from, dy > 0.0 ? 0.0 : height};
    if (to < 1.0)
        q = RasterPoint{ax + dx * to, dy > 0.0 ? height : 0.0};
    // Split where it crosses the content's left and right sides.
    std::array<RasterPoint, 4U> points{p};
    std::size_t count = 1U;
    const double ex = q.x - p.x;
    if (ex != 0.0) {
        std::array<double, 2U> crossings{(0.0 - p.x) / ex, (width - p.x) / ex};
        if (crossings[0] > crossings[1])
            std::swap(crossings[0], crossings[1]);
        for (std::size_t index = 0U; index < 2U; ++index) {
            const double s = crossings[index];
            if (s > 0.0 && s < 1.0) {
                const double side = (index == 0U) == (ex > 0.0) ? 0.0 : width;
                points[count++] = RasterPoint{side, p.y + (q.y - p.y) * s};
            }
        }
    }
    points[count++] = q;
    const auto fixed = [](const double value, const double limit) {
        return static_cast<std::int64_t>(
            std::llround(std::clamp(value, 0.0, limit) * static_cast<double>(raster_one)));
    };
    for (std::size_t index = 0U; index + 1U < count; ++index) {
        const RasterPoint u = points[index];
        const RasterPoint v = points[index + 1U];
        const double middle = (u.x + v.x) * 0.5;
        if (middle >= width)
            continue;
        if (middle <= 0.0)
            emit(0, fixed(u.y, height), 0, fixed(v.y, height));
        else
            emit(fixed(u.x, width), fixed(u.y, height), fixed(v.x, width), fixed(v.y, height));
    }
}

/** Splits a fixed-point edge into segments no longer than raster_piece along either axis. */
template <class Emit>
void split_edge(const std::int64_t x0, const std::int64_t y0, const std::int64_t x1,
                const std::int64_t y1, Emit&& emit) {
    if (y0 == y1)
        return;
    const std::int64_t dx = x1 - x0;
    const std::int64_t dy = y1 - y0;
    const std::int64_t span = std::max(std::abs(dx), std::abs(dy));
    const std::int64_t pieces = std::max<std::int64_t>(1, (span + raster_piece - 1) / raster_piece);
    std::int64_t previous_x = x0;
    std::int64_t previous_y = y0;
    for (std::int64_t piece = 1; piece <= pieces; ++piece) {
        const std::int64_t x = piece == pieces ? x1 : x0 + dx * piece / pieces;
        const std::int64_t y = piece == pieces ? y1 : y0 + dy * piece / pieces;
        if (y != previous_y) {
            emit(RasterSegment{static_cast<std::int32_t>(previous_x),
                               static_cast<std::int32_t>(previous_y), static_cast<std::int32_t>(x),
                               static_cast<std::int32_t>(y)});
        }
        previous_x = x;
        previous_y = y;
    }
}

/**
 * The convex polygons one stroke piece expands to (gpu/raster_shaders.hpp's stroke kernel does
 * the same in float): its band, its join with the next piece (on the outer side; the bands
 * overlap on the inner one) and the caps of a run's ends.
 */
template <class Polygon>
void expand_piece(const RasterStrokePiece& piece, const RasterStroke& stroke, Polygon&& polygon) {
    const double half = stroke.half_width;
    const RasterPoint p0{piece.x0, piece.y0};
    const RasterPoint p1{piece.x1, piece.y1};
    const double length = std::hypot(p1.x - p0.x, p1.y - p0.y);
    if (!(length > 1.0e-9) || !(half > 0.0))
        return;
    const RasterPoint along{(p1.x - p0.x) / length, (p1.y - p0.y) / length};
    const RasterPoint side{-along.y * half, along.x * half};
    const auto at = [](const RasterPoint point, const RasterPoint offset, const double scale) {
        return RasterPoint{point.x + offset.x * scale, point.y + offset.y * scale};
    };
    std::vector<RasterPoint> vertices;
    const auto fan = [&](const RasterPoint center, const RasterPoint from, const double sweep) {
        const double step = half > stroke.tolerance
                                ? std::max(2.0 * std::acos(std::clamp(
                                                     1.0 - stroke.tolerance / half, -1.0, 1.0)),
                                           0.02)
                                : std::numbers::pi / 8.0;
        const int steps = std::clamp(static_cast<int>(std::ceil(std::abs(sweep) / step)), 1, 256);
        vertices.assign(1U, center);
        for (int index = 0; index <= steps; ++index) {
            const double angle = sweep * index / steps;
            const double cosine = std::cos(angle);
            const double sine = std::sin(angle);
            vertices.push_back(RasterPoint{center.x + from.x * cosine - from.y * sine,
                                           center.y + from.x * sine + from.y * cosine});
        }
        polygon(std::span<const RasterPoint>(vertices));
    };
    const std::array<RasterPoint, 4U> band{at(p0, side, 1.0), at(p1, side, 1.0),
                                           at(p1, side, -1.0), at(p0, side, -1.0)};
    polygon(std::span<const RasterPoint>(band));
    if ((piece.flags & raster_piece_join) != 0U) {
        const RasterPoint p2{piece.x2, piece.y2};
        const double next_length = std::hypot(p2.x - p1.x, p2.y - p1.y);
        if (next_length > 1.0e-9) {
            const RasterPoint next{(p2.x - p1.x) / next_length, (p2.y - p1.y) / next_length};
            const RasterPoint next_side{-next.y * half, next.x * half};
            const double turn = along.x * next.y - along.y * next.x;
            const double straight = along.x * next.x + along.y * next.y;
            if (std::abs(turn) > 1.0e-9 || straight < 0.0) {
                // The outer side is the one the path turns away from.
                const double outer = turn > 0.0 ? -1.0 : 1.0;
                const RasterPoint a = at(p1, side, outer);
                const RasterPoint b = at(p1, next_side, outer);
                const RasterPoint from{a.x - p1.x, a.y - p1.y};
                const RasterPoint to{b.x - p1.x, b.y - p1.y};
                if (stroke.join == RasterJoin::round) {
                    fan(p1, from, std::atan2(from.x * to.y - from.y * to.x,
                                             from.x * to.x + from.y * to.y));
                } else {
                    const double bx = from.x + to.x;
                    const double by = from.y + to.y;
                    const double bisector = std::hypot(bx, by);
                    const double cosine = bisector > 1.0e-12
                                              ? (bx * from.x + by * from.y) / (bisector * half)
                                              : 0.0;
                    if (stroke.join == RasterJoin::miter && cosine > 1.0e-9 &&
                        1.0 / cosine <= stroke.miter_limit) {
                        const double reach = half / cosine / bisector;
                        const std::array<RasterPoint, 4U> mitre{
                            p1, a, RasterPoint{p1.x + bx * reach, p1.y + by * reach}, b};
                        polygon(std::span<const RasterPoint>(mitre));
                    } else {
                        const std::array<RasterPoint, 3U> bevel{p1, a, b};
                        polygon(std::span<const RasterPoint>(bevel));
                    }
                }
            }
        }
    }
    const auto cap = [&](const RasterPoint point, const RasterPoint outward) {
        if (stroke.cap == RasterCap::butt)
            return;
        const RasterPoint across{-outward.y * half, outward.x * half};
        if (stroke.cap == RasterCap::square) {
            const std::array<RasterPoint, 4U> square{
                at(point, across, 1.0), at(at(point, across, 1.0), outward, half),
                at(at(point, across, -1.0), outward, half), at(point, across, -1.0)};
            polygon(std::span<const RasterPoint>(square));
            return;
        }
        // From one side round the outward end to the other.
        const double sweep = (across.x * outward.y - across.y * outward.x) > 0.0
                                 ? std::numbers::pi
                                 : -std::numbers::pi;
        fan(point, across, sweep);
    };
    if ((piece.flags & raster_piece_start_cap) != 0U)
        cap(p0, RasterPoint{-along.x, -along.y});
    if ((piece.flags & raster_piece_end_cap) != 0U)
        cap(p1, along);
}

} // namespace

RasterJobBuilder::RasterJobBuilder(const std::uint32_t width, const std::uint32_t height)
    : width_(width), height_(height) {
    if (width == 0U || height == 0U || width > maximum_raster_extent ||
        height > maximum_raster_extent) {
        throw std::invalid_argument("raster job content must be between 1 and 4096 pixels");
    }
}

void RasterJobBuilder::add_layer(const RasterFill fill, const RasterLayer& paint,
                                 const std::span<const std::vector<RasterPoint>> polygons,
                                 const RasterPoint origin) {
    if (layers_.size() >= maximum_raster_layers)
        throw std::length_error("raster job has too many layers");
    RasterLayer layer = paint;
    layer.fill = fill;
    layer.first_segment = static_cast<std::uint32_t>(segments_.size());
    for (const std::vector<RasterPoint>& polygon : polygons) {
        if (polygon.size() < 2U)
            continue;
        for (std::size_t index = 0U; index < polygon.size(); ++index) {
            const RasterPoint a = polygon[index];
            const RasterPoint b = polygon[(index + 1U) % polygon.size()];
            add_edge(a.x - origin.x, a.y - origin.y, b.x - origin.x, b.y - origin.y);
        }
    }
    layer.segment_count = static_cast<std::uint32_t>(segments_.size()) - layer.first_segment;
    layers_.push_back(std::move(layer));
}

void RasterJobBuilder::add_edge(const double ax, const double ay, const double bx,
                                const double by) {
    clip_edge(ax, ay, bx, by, width_, height_,
              [&](const std::int64_t x0, const std::int64_t y0, const std::int64_t x1,
                  const std::int64_t y1) { add_piece(x0, y0, x1, y1); });
}

void RasterJobBuilder::add_piece(const std::int64_t x0, const std::int64_t y0,
                                 const std::int64_t x1, const std::int64_t y1) {
    split_edge(x0, y0, x1, y1,
               [&](const RasterSegment segment) { segments_.push_back(segment); });
}

void RasterJobBuilder::add_stroke_layer(const RasterLayer& paint, const RasterStroke& stroke,
                                        const std::span<const RasterStrokeRun> runs,
                                        const RasterPoint origin) {
    if (layers_.size() >= maximum_raster_layers)
        throw std::length_error("raster job has too many layers");
    RasterLayer layer = paint;
    layer.fill = RasterFill::nonzero;
    layer.stroke = stroke;
    layer.first_segment = static_cast<std::uint32_t>(segments_.size());
    layer.segment_count = 0U;
    layer.first_piece = static_cast<std::uint32_t>(pieces_.size());
    // A piece's polygons are closed and lie within its reach, so one whose reach misses the
    // content adds nothing to it.
    const double reach = raster_stroke_reach(stroke) + 1.0;
    const double width = width_;
    const double height = height_;
    std::vector<RasterPoint> points;
    for (const RasterStrokeRun& run : runs) {
        points.clear();
        for (const RasterPoint point : run.points) {
            const RasterPoint local{point.x - origin.x, point.y - origin.y};
            if (!std::isfinite(local.x) || !std::isfinite(local.y))
                continue;
            if (points.empty() || std::abs(local.x - points.back().x) > 1.0e-9 ||
                std::abs(local.y - points.back().y) > 1.0e-9) {
                points.push_back(local);
            }
        }
        bool closed = run.closed;
        if (closed && points.size() > 1U && std::abs(points.front().x - points.back().x) <= 1.0e-9 &&
            std::abs(points.front().y - points.back().y) <= 1.0e-9) {
            points.pop_back();
        }
        const std::size_t count = points.size();
        if (count < 2U)
            continue;
        closed = closed && count >= 3U;
        const std::size_t pieces = closed ? count : count - 1U;
        for (std::size_t index = 0U; index < pieces; ++index) {
            const RasterPoint p0 = points[index];
            const RasterPoint p1 = points[(index + 1U) % count];
            if (std::max(p0.x, p1.x) < -reach || std::min(p0.x, p1.x) > width + reach ||
                std::max(p0.y, p1.y) < -reach || std::min(p0.y, p1.y) > height + reach) {
                continue;
            }
            RasterStrokePiece piece;
            piece.x0 = static_cast<float>(p0.x);
            piece.y0 = static_cast<float>(p0.y);
            piece.x1 = static_cast<float>(p1.x);
            piece.y1 = static_cast<float>(p1.y);
            if (closed || index + 2U < count) {
                const RasterPoint p2 = points[(index + 2U) % count];
                piece.x2 = static_cast<float>(p2.x);
                piece.y2 = static_cast<float>(p2.y);
                piece.flags |= raster_piece_join;
            }
            if (!closed && index == 0U)
                piece.flags |= raster_piece_start_cap;
            if (!closed && index + 1U == pieces)
                piece.flags |= raster_piece_end_cap;
            pieces_.push_back(piece);
        }
    }
    layer.piece_count = static_cast<std::uint32_t>(pieces_.size()) - layer.first_piece;
    layers_.push_back(std::move(layer));
}

void RasterJobBuilder::add_art_layer(const RasterFill fill, const RasterLayer& paint,
                                     const RasterStroke& stroke, RasterArt art) {
    if (layers_.size() >= maximum_raster_layers)
        throw std::length_error("raster job has too many layers");
    if (art.count < 2U || art.count > maximum_art_points || art.source.empty() ||
        art.source.size() > maximum_art_source) {
        throw std::invalid_argument("raster art needs 2 to 2^20 points and a program");
    }
    RasterLayer layer = paint;
    layer.fill = art.stroked ? RasterFill::nonzero : fill;
    layer.stroke = stroke;
    layer.first_segment = static_cast<std::uint32_t>(segments_.size());
    layer.segment_count = 0U;
    layer.first_piece = static_cast<std::uint32_t>(pieces_.size());
    layer.piece_count = 0U;
    layer.art = std::move(art);
    layers_.push_back(std::move(layer));
}

bool raster_job_has_art(const RasterJob& job) noexcept {
    return std::ranges::any_of(job.layers,
                               [](const RasterLayer& layer) { return layer.art.has_value(); });
}

double raster_stroke_reach(const RasterStroke& stroke) noexcept {
    double factor = 1.0;
    if (stroke.join == RasterJoin::miter)
        factor = std::max(factor, static_cast<double>(stroke.miter_limit));
    if (stroke.cap == RasterCap::square)
        factor = std::max(factor, 1.4142135623730951);
    return static_cast<double>(stroke.half_width) * factor;
}

std::vector<std::uint8_t> RasterJobBuilder::encode(const std::uint32_t x,
                                                   const std::uint32_t y) const {
    std::size_t size = header_words * 4U + segments_.size() * sizeof(RasterSegment) +
                       pieces_.size() * sizeof(RasterStrokePiece);
    for (const RasterLayer& layer : layers_) {
        size += layer_words * 4U + (gradient(layer.paint) ? raster_lut_size * 4U : 0U);
        if (layer.art.has_value())
            size += art_words * 4U + (layer.art->source.size() + 3U) / 4U * 4U;
    }
    Writer output(size);
    output.u32(x);
    output.u32(y);
    output.u32(width_);
    output.u32(height_);
    output.u32(static_cast<std::uint32_t>(layers_.size()));
    output.u32(static_cast<std::uint32_t>(segments_.size()));
    output.u32(static_cast<std::uint32_t>(pieces_.size()));
    for (const RasterLayer& layer : layers_) {
        output.u32(static_cast<std::uint32_t>(layer.fill));
        output.u32(static_cast<std::uint32_t>(layer.paint));
        output.u32(layer.color);
        output.u32(static_cast<std::uint32_t>(layer.extend));
        for (const float parameter : layer.parameters)
            output.f32(parameter);
        output.u32(layer.first_segment);
        output.u32(layer.segment_count);
        output.f32(layer.stroke.half_width);
        output.u32(static_cast<std::uint32_t>(layer.stroke.join));
        output.u32(static_cast<std::uint32_t>(layer.stroke.cap));
        output.f32(layer.stroke.miter_limit);
        output.f32(layer.stroke.tolerance);
        output.u32(layer.first_piece);
        output.u32(layer.piece_count);
        output.u32(layer.art.has_value() ? 1U : 0U);
        if (layer.art.has_value()) {
            const RasterArt& art = *layer.art;
            output.u32(art.count);
            output.u32((art.closed ? 1U : 0U) | (art.stroked ? 2U : 0U) | (art.segments ? 4U : 0U));
            for (const float value : {art.x, art.y, art.width, art.height})
                output.f32(value);
            for (const float parameter : art.parameters)
                output.f32(parameter);
            output.u32(static_cast<std::uint32_t>(art.source.size()));
            output.append(art.source.data(), art.source.size());
            const std::uint32_t zero = 0U;
            output.append(&zero, (4U - art.source.size() % 4U) % 4U);
        }
        if (gradient(layer.paint)) {
            if (layer.lut.size() != raster_lut_size)
                throw std::invalid_argument("raster gradient layer needs a full colour table");
            output.append(layer.lut.data(), raster_lut_size * 4U);
        }
    }
    static_assert(sizeof(RasterSegment) == 16U);
    output.append(segments_.data(), segments_.size() * sizeof(RasterSegment));
    static_assert(sizeof(RasterStrokePiece) == 32U);
    output.append(pieces_.data(), pieces_.size() * sizeof(RasterStrokePiece));
    return std::move(output).take();
}

RasterJob decode_raster_job(const std::span<const std::uint8_t> bytes,
                            const std::uint32_t region_width, const std::uint32_t region_height,
                            const std::uint32_t format) {
    if (format > texture_format_r8_filtered)
        throw std::invalid_argument("raster job targets an unknown atlas format");
    Reader input(bytes);
    RasterJob job;
    job.x = input.u32();
    job.y = input.u32();
    job.width = input.u32();
    job.height = input.u32();
    const std::uint32_t layer_count = input.u32();
    const std::uint32_t segment_count = input.u32();
    const std::uint32_t piece_count = input.u32();
    if (job.width == 0U || job.height == 0U || job.width > maximum_raster_extent ||
        job.height > maximum_raster_extent ||
        static_cast<std::uint64_t>(job.x) + job.width > region_width ||
        static_cast<std::uint64_t>(job.y) + job.height > region_height) {
        throw std::invalid_argument("raster job content lies outside its region");
    }
    if (layer_count == 0U || layer_count > maximum_raster_layers ||
        (r8(format) && layer_count != 1U)) {
        throw std::invalid_argument("raster job layer count is invalid for its atlas");
    }
    job.layers.resize(layer_count);
    for (RasterLayer& layer : job.layers) {
        const std::uint32_t fill = input.u32();
        const std::uint32_t paint = input.u32();
        layer.color = input.u32();
        const std::uint32_t extend = input.u32();
        if (fill > 1U || paint > 3U || extend > 2U)
            throw std::invalid_argument("raster job layer is outside the supported domain");
        layer.fill = static_cast<RasterFill>(fill);
        layer.paint = static_cast<RasterPaint>(paint);
        layer.extend = static_cast<RasterExtend>(extend);
        if (r8(format) != (layer.paint == RasterPaint::coverage))
            throw std::invalid_argument("raster job layer paint does not suit its atlas");
        for (float& parameter : layer.parameters) {
            parameter = input.f32();
            if (!std::isfinite(parameter))
                throw std::invalid_argument("raster job gradient is not finite");
        }
        layer.first_segment = input.u32();
        layer.segment_count = input.u32();
        if (static_cast<std::uint64_t>(layer.first_segment) + layer.segment_count > segment_count)
            throw std::invalid_argument("raster job layer names missing segments");
        layer.stroke.half_width = input.f32();
        const std::uint32_t join = input.u32();
        const std::uint32_t cap = input.u32();
        layer.stroke.miter_limit = input.f32();
        layer.stroke.tolerance = input.f32();
        layer.first_piece = input.u32();
        layer.piece_count = input.u32();
        if (join > 2U || cap > 2U)
            throw std::invalid_argument("raster job stroke is outside the supported domain");
        layer.stroke.join = static_cast<RasterJoin>(join);
        layer.stroke.cap = static_cast<RasterCap>(cap);
        if (static_cast<std::uint64_t>(layer.first_piece) + layer.piece_count > piece_count)
            throw std::invalid_argument("raster job layer names missing stroke pieces");
        if (input.u32() != 0U) {
            RasterArt art;
            art.count = input.u32();
            const std::uint32_t flags = input.u32();
            art.closed = (flags & 1U) != 0U;
            art.stroked = (flags & 2U) != 0U;
            art.segments = (flags & 4U) != 0U;
            art.x = input.f32();
            art.y = input.f32();
            art.width = input.f32();
            art.height = input.f32();
            for (float& parameter : art.parameters)
                parameter = input.f32();
            const std::uint32_t length = input.u32();
            if (art.count < 2U || art.count > maximum_art_points || flags > 7U ||
                (art.segments && (art.closed || !art.stroked)) || length == 0U ||
                length > maximum_art_source) {
                throw std::invalid_argument("raster art is outside the supported domain");
            }
            for (const float value : {art.x, art.y, art.width, art.height}) {
                if (!(std::abs(value) <= 1.0e6F))
                    throw std::invalid_argument("raster art canvas is out of range");
            }
            for (const float parameter : art.parameters) {
                if (!std::isfinite(parameter))
                    throw std::invalid_argument("raster art parameter is not finite");
            }
            art.source.resize(length);
            input.copy(art.source.data(), length);
            std::array<std::uint8_t, 4U> padding{};
            input.copy(padding.data(), (4U - length % 4U) % 4U);
            if (art.stroked && (layer.fill != RasterFill::nonzero ||
                                !(layer.stroke.half_width > 0.0F) ||
                                !(layer.stroke.miter_limit >= 1.0F) ||
                                !(layer.stroke.tolerance >= 1.0e-3F))) {
                throw std::invalid_argument("raster art stroke is invalid");
            }
            layer.art = std::move(art);
        }
        if (layer.piece_count > 0U &&
            (layer.fill != RasterFill::nonzero || !(layer.stroke.half_width > 0.0F) ||
             !(layer.stroke.half_width <= 1.0e5F) || !(layer.stroke.miter_limit >= 1.0F) ||
             !(layer.stroke.miter_limit <= 1.0e3F) || !(layer.stroke.tolerance >= 1.0e-3F) ||
             !(layer.stroke.tolerance <= 1.0e3F))) {
            throw std::invalid_argument("raster job stroke style is invalid");
        }
        if (gradient(layer.paint)) {
            layer.lut.resize(raster_lut_size);
            input.copy(layer.lut.data(), raster_lut_size * 4U);
        }
    }
    if (input.remaining() != static_cast<std::uint64_t>(segment_count) * sizeof(RasterSegment) +
                                 static_cast<std::uint64_t>(piece_count) *
                                     sizeof(RasterStrokePiece)) {
        throw std::invalid_argument("raster job segment bytes differ from its count");
    }
    job.segments.resize(segment_count);
    input.copy(job.segments.data(), job.segments.size() * sizeof(RasterSegment));
    job.pieces.resize(piece_count);
    input.copy(job.pieces.data(), job.pieces.size() * sizeof(RasterStrokePiece));
    for (const RasterStrokePiece& piece : job.pieces) {
        for (const float value : {piece.x0, piece.y0, piece.x1, piece.y1, piece.x2, piece.y2}) {
            if (!(std::abs(value) <= 1.0e6F))
                throw std::invalid_argument("raster job stroke piece is out of range");
        }
        if (piece.flags > 7U)
            throw std::invalid_argument("raster job stroke piece flags are unknown");
    }
    const std::int64_t right = static_cast<std::int64_t>(job.width) * raster_one;
    const std::int64_t bottom = static_cast<std::int64_t>(job.height) * raster_one;
    for (const RasterSegment& segment : job.segments) {
        const auto inside = [](const std::int32_t value, const std::int64_t limit) {
            return value >= 0 && value <= limit;
        };
        if (!inside(segment.x0, right) || !inside(segment.x1, right) ||
            !inside(segment.y0, bottom) || !inside(segment.y1, bottom) ||
            std::abs(segment.x1 - segment.x0) > raster_piece ||
            std::abs(segment.y1 - segment.y0) > raster_piece) {
            throw std::invalid_argument("raster job segment lies outside its content");
        }
    }
    return job;
}

void rasterize_job(const RasterJob& job, const std::uint32_t format,
                   const std::uint32_t region_width, const std::uint32_t region_height,
                   const std::span<std::uint8_t> pixels, const std::size_t row_bytes) {
    const std::size_t channels = r8(format) ? 1U : 4U;
    if (row_bytes < static_cast<std::size_t>(region_width) * channels ||
        pixels.size() < row_bytes * (region_height == 0U ? 0U : region_height - 1U) +
                            static_cast<std::size_t>(region_width) * channels) {
        throw std::invalid_argument("raster job output is smaller than its region");
    }
    for (std::uint32_t row = 0U; row < region_height; ++row)
        std::fill_n(pixels.begin() + static_cast<std::ptrdiff_t>(row * row_bytes),
                    region_width * channels, std::uint8_t{0U});
    const std::size_t cells = static_cast<std::size_t>(job.width) * job.height;
    std::vector<std::int32_t> cover(cells);
    std::vector<std::int32_t> area(cells);
    const auto width = static_cast<std::int32_t>(job.width);
    const auto accumulate = [&](const RasterLayer& layer) {
        std::fill(cover.begin(), cover.end(), 0);
        std::fill(area.begin(), area.end(), 0);
        std::int32_t orientation = 1;
        const auto add = [&](const std::int32_t column, const std::int32_t row,
                             const std::int32_t c, const std::int32_t a) {
            const std::size_t at = static_cast<std::size_t>(row) * job.width +
                                   static_cast<std::size_t>(column);
            cover[at] += c * orientation;
            area[at] += a * orientation;
        };
        for (std::uint32_t index = 0U; index < layer.segment_count; ++index)
            walk_segment(job.segments[layer.first_segment + index], width, add);
        // A stroke's polygons all count one way, so where they overlap they union.
        const auto polygon = [&](const std::span<const RasterPoint> vertices) {
            double twice_area = 0.0;
            for (std::size_t index = 0U; index < vertices.size(); ++index) {
                const RasterPoint a = vertices[index];
                const RasterPoint b = vertices[(index + 1U) % vertices.size()];
                twice_area += a.x * b.y - b.x * a.y;
            }
            if (twice_area == 0.0)
                return;
            orientation = twice_area > 0.0 ? 1 : -1;
            for (std::size_t index = 0U; index < vertices.size(); ++index) {
                const RasterPoint a = vertices[index];
                const RasterPoint b = vertices[(index + 1U) % vertices.size()];
                clip_edge(a.x, a.y, b.x, b.y, job.width, job.height,
                          [&](const std::int64_t x0, const std::int64_t y0, const std::int64_t x1,
                              const std::int64_t y1) {
                              split_edge(x0, y0, x1, y1, [&](const RasterSegment segment) {
                                  walk_segment(segment, width, add);
                              });
                          });
            }
            orientation = 1;
        };
        for (std::uint32_t index = 0U; index < layer.piece_count; ++index)
            expand_piece(job.pieces[layer.first_piece + index], layer.stroke, polygon);
    };
    if (channels == 1U) {
        const RasterLayer& layer = job.layers.front();
        accumulate(layer);
        for (std::uint32_t row = 0U; row < job.height; ++row) {
            std::int32_t running = 0;
            std::uint8_t* out = pixels.data() + (job.y + row) * row_bytes + job.x;
            for (std::uint32_t column = 0U; column < job.width; ++column) {
                const std::size_t at = static_cast<std::size_t>(row) * job.width + column;
                running += cover[at];
                out[column] = static_cast<std::uint8_t>(
                    coverage_byte(resolved(running, area[at], layer.fill)));
            }
        }
        return;
    }
    std::vector<Sum> sums(cells);
    for (const RasterLayer& layer : job.layers) {
        accumulate(layer);
        for (std::uint32_t row = 0U; row < job.height; ++row) {
            std::int32_t running = 0;
            for (std::uint32_t column = 0U; column < job.width; ++column) {
                const std::size_t at = static_cast<std::size_t>(row) * job.width + column;
                running += cover[at];
                const std::int32_t covered = resolved(running, area[at], layer.fill);
                if (covered == 0)
                    continue;
                composite(sums[at], (covered + 2) / 4, layer_color(layer, column, row));
            }
        }
    }
    for (std::uint32_t row = 0U; row < job.height; ++row) {
        std::uint8_t* out = pixels.data() + (job.y + row) * row_bytes + job.x * 4U;
        for (std::uint32_t column = 0U; column < job.width; ++column) {
            const std::uint32_t texel = straight(sums[static_cast<std::size_t>(row) * job.width + column]);
            std::memcpy(out + column * 4U, &texel, 4U);
        }
    }
}

} // namespace strata::host
