#include <strata/raster_job.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

#include <strata/render_packet.hpp>

namespace strata::host {
namespace {

static_assert(std::endian::native == std::endian::little, "raster jobs are little-endian");

/** Header words: content x, y, width, height, layer count, segment count. */
constexpr std::size_t header_words = 6U;
/** Layer words before its colour table: fill, paint, colour, extend, six parameters, range. */
constexpr std::size_t layer_words = 12U;
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
    if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(bx) || !std::isfinite(by) ||
        ay == by) {
        return;
    }
    const double width = width_;
    const double height = height_;
    constexpr double unit = raster_one;
    if (ax >= 0.0 && ax <= width && bx >= 0.0 && bx <= width && ay >= 0.0 && ay <= height &&
        by >= 0.0 && by <= height) {
        add_piece(std::llround(ax * unit), std::llround(ay * unit), std::llround(bx * unit),
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
        if (middle <= 0.0) {
            add_piece(0, fixed(u.y, height), 0, fixed(v.y, height));
        } else {
            add_piece(fixed(u.x, width), fixed(u.y, height), fixed(v.x, width),
                      fixed(v.y, height));
        }
    }
}

void RasterJobBuilder::add_piece(const std::int64_t x0, const std::int64_t y0,
                                 const std::int64_t x1, const std::int64_t y1) {
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
            segments_.push_back(RasterSegment{
                static_cast<std::int32_t>(previous_x), static_cast<std::int32_t>(previous_y),
                static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)});
        }
        previous_x = x;
        previous_y = y;
    }
}

std::vector<std::uint8_t> RasterJobBuilder::encode(const std::uint32_t x,
                                                   const std::uint32_t y) const {
    std::size_t size = header_words * 4U + segments_.size() * sizeof(RasterSegment);
    for (const RasterLayer& layer : layers_)
        size += layer_words * 4U + (gradient(layer.paint) ? raster_lut_size * 4U : 0U);
    Writer output(size);
    output.u32(x);
    output.u32(y);
    output.u32(width_);
    output.u32(height_);
    output.u32(static_cast<std::uint32_t>(layers_.size()));
    output.u32(static_cast<std::uint32_t>(segments_.size()));
    for (const RasterLayer& layer : layers_) {
        output.u32(static_cast<std::uint32_t>(layer.fill));
        output.u32(static_cast<std::uint32_t>(layer.paint));
        output.u32(layer.color);
        output.u32(static_cast<std::uint32_t>(layer.extend));
        for (const float parameter : layer.parameters)
            output.f32(parameter);
        output.u32(layer.first_segment);
        output.u32(layer.segment_count);
        if (gradient(layer.paint)) {
            if (layer.lut.size() != raster_lut_size)
                throw std::invalid_argument("raster gradient layer needs a full colour table");
            output.append(layer.lut.data(), raster_lut_size * 4U);
        }
    }
    static_assert(sizeof(RasterSegment) == 16U);
    output.append(segments_.data(), segments_.size() * sizeof(RasterSegment));
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
        if (gradient(layer.paint)) {
            layer.lut.resize(raster_lut_size);
            input.copy(layer.lut.data(), raster_lut_size * 4U);
        }
    }
    if (input.remaining() != static_cast<std::uint64_t>(segment_count) * 16U)
        throw std::invalid_argument("raster job segment bytes differ from its count");
    job.segments.resize(segment_count);
    input.copy(job.segments.data(), job.segments.size() * sizeof(RasterSegment));
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
        for (std::uint32_t index = 0U; index < layer.segment_count; ++index) {
            walk_segment(job.segments[layer.first_segment + index], width,
                         [&](const std::int32_t column, const std::int32_t row,
                             const std::int32_t c, const std::int32_t a) {
                             const std::size_t at = static_cast<std::size_t>(row) * job.width +
                                                    static_cast<std::size_t>(column);
                             cover[at] += c;
                             area[at] += a;
                         });
        }
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
