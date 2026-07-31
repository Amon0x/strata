#include "font/raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace strata::font {
namespace {

constexpr std::uint8_t red = 1U;
constexpr std::uint8_t green = 2U;
constexpr std::uint8_t blue = 4U;
constexpr std::uint8_t white = red | green | blue;

struct Point final {
    double x = 0.0;
    double y = 0.0;
};

struct Edge final {
    Point from;
    Point to;
    std::uint8_t channels = white;
};

struct Contour final {
    std::vector<Edge> edges;
    double signed_area = 0.0;
};

struct Distances final {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double monochrome = 0.0;
};

[[nodiscard]] double distance_squared(const Point left, const Point right) noexcept {
    const double x = left.x - right.x;
    const double y = left.y - right.y;
    return x * x + y * y;
}

void append_distinct(std::vector<Point>& points, const Point value) {
    if (points.empty() || distance_squared(points.back(), value) > 1.0e-12) {
        points.push_back(value);
    }
}

[[nodiscard]] Point midpoint(const Point left, const Point right) noexcept {
    return Point{(left.x + right.x) * 0.5, (left.y + right.y) * 0.5};
}

[[nodiscard]] double point_line_distance(
    const Point point,
    const Point start,
    const Point end
) noexcept {
    const double x = end.x - start.x;
    const double y = end.y - start.y;
    const double length = std::hypot(x, y);
    if (length <= 1.0e-12) return std::sqrt(distance_squared(point, start));
    return std::abs((point.x - start.x) * y - (point.y - start.y) * x) / length;
}

void flatten_quadratic(
    std::vector<Point>& output,
    const Point from,
    const Point control,
    const Point to,
    const double tolerance,
    const std::size_t depth = 0U
) {
    if (depth >= 24U || point_line_distance(control, from, to) <= tolerance) {
        append_distinct(output, to);
        return;
    }
    const Point left = midpoint(from, control);
    const Point right = midpoint(control, to);
    const Point split = midpoint(left, right);
    flatten_quadratic(output, from, left, split, tolerance, depth + 1U);
    flatten_quadratic(output, split, right, to, tolerance, depth + 1U);
}

[[nodiscard]] std::vector<Point> flatten(
    const GlyphContour& contour,
    const double tolerance
) {
    std::vector<Point> points;
    points.reserve(contour.segments.size() * 2U);
    for (const GlyphSegment& segment : contour.segments) {
        const Point from{segment.from.x, segment.from.y};
        const Point to{segment.to.x, segment.to.y};
        append_distinct(points, from);
        if (segment.kind == GlyphSegmentKind::quadratic) {
            flatten_quadratic(
                points,
                from,
                Point{segment.control.x, segment.control.y},
                to,
                tolerance
            );
        } else {
            append_distinct(points, to);
        }
    }
    if (points.size() > 1U && distance_squared(points.front(), points.back()) <= 1.0e-12) {
        points.pop_back();
    }
    return points;
}

[[nodiscard]] double signed_area(const std::vector<Point>& points) noexcept {
    double twice_area = 0.0;
    for (std::size_t index = 0U; index < points.size(); ++index) {
        const Point& current = points[index];
        const Point& next = points[(index + 1U) % points.size()];
        twice_area += current.x * next.y - next.x * current.y;
    }
    return twice_area * 0.5;
}

[[nodiscard]] Point outward_normal(
    const Point from,
    const Point to,
    const double orientation
) noexcept {
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const double length = std::hypot(dx, dy);
    if (length <= 1.0e-7) return {};
    return Point{orientation * dy / length, orientation * -dx / length};
}

[[nodiscard]] Point miter_offset(
    const Point incoming,
    const Point outgoing,
    const double expansion
) noexcept {
    constexpr double denominator_epsilon = 0.08;
    constexpr double miter_limit = 2.0;
    const double denominator = 1.0 +
        incoming.x * outgoing.x + incoming.y * outgoing.y;
    Point offset = denominator <= denominator_epsilon
        ? Point{outgoing.x * expansion, outgoing.y * expansion}
        : Point{
            (incoming.x + outgoing.x) * expansion / denominator,
            (incoming.y + outgoing.y) * expansion / denominator,
        };
    const double length = std::hypot(offset.x, offset.y);
    const double maximum_length = expansion * miter_limit;
    if (length > maximum_length && length > 1.0e-7) {
        offset.x *= maximum_length / length;
        offset.y *= maximum_length / length;
    }
    return offset;
}

void embolden(
    std::vector<std::vector<Point>>& contours,
    const double strength
) {
    if (contours.empty() || strength <= 1.0e-7) return;
    double minimum_y = contours.front().front().y;
    double maximum_y = minimum_y;
    for (const std::vector<Point>& contour : contours) {
        for (const Point point : contour) {
            minimum_y = std::min(minimum_y, point.y);
            maximum_y = std::max(maximum_y, point.y);
        }
    }
    const auto dominant = std::ranges::max_element(
        contours,
        {},
        [](const std::vector<Point>& contour) { return std::abs(signed_area(contour)); }
    );
    const double orientation = signed_area(*dominant) < 0.0 ? -1.0 : 1.0;
    const double expansion = strength * 0.5;
    for (std::vector<Point>& contour : contours) {
        if (contour.size() < 3U) continue;
        const std::vector<Point> source = contour;
        for (std::size_t index = 0U; index < source.size(); ++index) {
            const Point incoming = outward_normal(
                source[(index + source.size() - 1U) % source.size()], source[index], orientation
            );
            const Point outgoing = outward_normal(
                source[index], source[(index + 1U) % source.size()], orientation
            );
            const Point offset = miter_offset(incoming, outgoing, expansion);
            contour[index] = Point{
                source[index].x + offset.x,
                std::clamp(source[index].y + offset.y, minimum_y, maximum_y),
            };
        }
    }
}

[[nodiscard]] std::vector<std::size_t> corners(
    const std::vector<Point>& points,
    const double threshold
) {
    std::vector<std::size_t> result;
    for (std::size_t index = 0U; index < points.size(); ++index) {
        const Point previous = points[(index + points.size() - 1U) % points.size()];
        const Point current = points[index];
        const Point next = points[(index + 1U) % points.size()];
        const double in_x = current.x - previous.x;
        const double in_y = current.y - previous.y;
        const double out_x = next.x - current.x;
        const double out_y = next.y - current.y;
        const double in_length = std::hypot(in_x, in_y);
        const double out_length = std::hypot(out_x, out_y);
        if (in_length <= 1.0e-6 || out_length <= 1.0e-6) continue;
        const double dot = (in_x * out_x + in_y * out_y) / (in_length * out_length);
        if (dot <= threshold) result.push_back(index);
    }
    return result;
}

void add_edge(std::vector<Edge>& output, const Point from, const Point to, const std::uint8_t color) {
    if (distance_squared(from, to) > 1.0e-12) output.push_back(Edge{from, to, color});
}

[[nodiscard]] Contour color_edges(const std::vector<Point>& points, const double threshold) {
    Contour result;
    const std::vector<std::size_t> corner_indices = corners(points, threshold);
    if (corner_indices.size() >= 3U) {
        constexpr std::uint8_t colors[]{green | blue, red | blue, red | green};
        for (std::size_t corner = 0U; corner < corner_indices.size(); ++corner) {
            const std::size_t start = corner_indices[corner];
            const std::size_t end = corner_indices[(corner + 1U) % corner_indices.size()];
            std::size_t point = start;
            do {
                const std::size_t next = (point + 1U) % points.size();
                add_edge(result.edges, points[point], points[next], colors[corner % 3U]);
                point = next;
            } while (point != end);
        }
    } else {
        for (std::size_t index = 0U; index < points.size(); ++index) {
            add_edge(result.edges, points[index], points[(index + 1U) % points.size()], white);
        }
    }
    double twice_area = 0.0;
    for (const Edge edge : result.edges) {
        twice_area += edge.from.x * edge.to.y - edge.to.x * edge.from.y;
    }
    result.signed_area = twice_area * 0.5;
    return result;
}

[[nodiscard]] bool ray_crosses(const Point point, const Edge edge) noexcept {
    if ((edge.from.y > point.y) == (edge.to.y > point.y)) return false;
    const double at_y = (edge.to.x - edge.from.x) * (point.y - edge.from.y) /
                        (edge.to.y - edge.from.y) + edge.from.x;
    return point.x < at_y;
}

[[nodiscard]] double edge_distance_squared(const Point point, const Edge edge) noexcept {
    const double x = edge.to.x - edge.from.x;
    const double y = edge.to.y - edge.from.y;
    const double length_squared = x * x + y * y;
    if (length_squared <= 1.0e-10) return distance_squared(point, edge.from);
    const double projection = std::clamp(
        ((point.x - edge.from.x) * x + (point.y - edge.from.y) * y) / length_squared,
        0.0,
        1.0
    );
    return distance_squared(point, Point{edge.from.x + x * projection, edge.from.y + y * projection});
}

[[nodiscard]] Distances signed_distances(
    const Point point,
    const std::vector<Contour>& contours
) {
    int winding = 0;
    double red_squared = std::numeric_limits<double>::infinity();
    double green_squared = red_squared;
    double blue_squared = red_squared;
    double mono_squared = red_squared;
    for (const Contour& contour : contours) {
        bool inside = false;
        for (const Edge edge : contour.edges) {
            if (ray_crosses(point, edge)) inside = !inside;
            const double distance = edge_distance_squared(point, edge);
            mono_squared = std::min(mono_squared, distance);
            if ((edge.channels & red) != 0U) red_squared = std::min(red_squared, distance);
            if ((edge.channels & green) != 0U) green_squared = std::min(green_squared, distance);
            if ((edge.channels & blue) != 0U) blue_squared = std::min(blue_squared, distance);
        }
        if (inside) winding += contour.signed_area >= 0.0 ? 1 : -1;
    }
    if (!std::isfinite(red_squared)) red_squared = mono_squared;
    if (!std::isfinite(green_squared)) green_squared = mono_squared;
    if (!std::isfinite(blue_squared)) blue_squared = mono_squared;
    const double sign = winding != 0 ? 1.0 : -1.0;
    return Distances{
        std::sqrt(red_squared) * sign,
        std::sqrt(green_squared) * sign,
        std::sqrt(blue_squared) * sign,
        std::sqrt(mono_squared) * sign,
    };
}

[[nodiscard]] std::uint8_t encode_distance(const double pixels, const double range) noexcept {
    const double normalized = std::clamp(pixels / range + 0.5, 0.0, 1.0);
    return static_cast<std::uint8_t>(std::clamp(std::lround(normalized * 255.0), 0L, 255L));
}

void validate(const MsdfRasterConfig& config) {
    if (config.maximum_dimension == 0U || config.oversampling == 0U ||
        !std::isfinite(config.pixel_range) || config.pixel_range <= 0.0 ||
        !std::isfinite(config.flattening_tolerance_pixels) ||
        config.flattening_tolerance_pixels <= 0.0 ||
        !std::isfinite(config.corner_dot_threshold) ||
        config.corner_dot_threshold < -1.0 || config.corner_dot_threshold > 1.0) {
        throw FontError("MSDF raster configuration is invalid");
    }
}

[[nodiscard]] double snap_outward(
    const double font_units,
    const double layout_scale,
    const bool down
) {
    const double pixels = font_units * layout_scale;
    return (down ? std::floor(pixels) : std::ceil(pixels)) / layout_scale;
}

} // namespace

std::optional<GlyphRasterBitmap> rasterize_msdf(
    const OpenTypeFont& font,
    const std::uint16_t glyph,
    const double pixel_size,
    const MsdfRasterConfig& config,
    const std::uint32_t font_style_flags
) {
    validate(config);
    if (!std::isfinite(pixel_size) || pixel_size <= 0.0) {
        throw FontError("MSDF pixel size must be finite and positive");
    }
    const std::shared_ptr<const GlyphOutline> outline = font.glyph_outline(glyph);
    if (outline == nullptr || outline->empty() || !outline->bounds.has_value()) return std::nullopt;
    const double layout_scale = pixel_size / static_cast<double>(font.units_per_em());
    const double atlas_scale = layout_scale * static_cast<double>(config.oversampling);
    if (!std::isfinite(atlas_scale) || atlas_scale <= 0.0) {
        throw FontError("MSDF glyph scale cannot be represented");
    }
    const double tolerance = std::max(
        config.flattening_tolerance_pixels /
            (atlas_scale * static_cast<double>(config.oversampling)),
        0.01
    );
    std::vector<std::vector<Point>> prepared_points;
    prepared_points.reserve(outline->contours.size());
    for (const GlyphContour& contour : outline->contours) {
        std::vector<Point> points = flatten(contour, tolerance);
        if (points.size() >= 3U) prepared_points.push_back(std::move(points));
    }
    if (prepared_points.empty()) return std::nullopt;

    const FontStyleGeometry style = resolve_font_style_geometry(
        font_style_flags, font.metadata().style_flags
    );
    if (style.italic()) {
        for (std::vector<Point>& contour : prepared_points) {
            for (Point& point : contour) point.x = style.transform_x(point.x, point.y);
        }
    }
    if (style.bold()) {
        embolden(prepared_points, style.bold_strength(font.units_per_em()));
    }

    double minimum_x = prepared_points.front().front().x;
    double minimum_y = prepared_points.front().front().y;
    double maximum_x = minimum_x;
    double maximum_y = minimum_y;
    for (const std::vector<Point>& contour : prepared_points) {
        for (const Point point : contour) {
            minimum_x = std::min(minimum_x, point.x);
            minimum_y = std::min(minimum_y, point.y);
            maximum_x = std::max(maximum_x, point.x);
            maximum_y = std::max(maximum_y, point.y);
        }
    }
    const GlyphBounds& source = *outline->bounds;
    // Neither synthetic transform changes vertical extents. Retain the analytic source bounds so
    // coverage/MSDF plane geometry and default-v1 snapping stay vertically identical.
    minimum_y = source.bottom;
    maximum_y = source.top;
    if (style.synthetic_flags == 0U) {
        // Preserve the exact default-v1 plane contract; flattened points are only authoritative
        // after a synthetic transform has invalidated the source outline's analytic bounds.
        minimum_x = source.left;
        maximum_x = source.right;
    }
    const double padding_units = static_cast<double>(config.padding_pixels) / atlas_scale;
    const RasterPlaneBounds font_bounds{
        snap_outward(minimum_x - padding_units, layout_scale, true),
        snap_outward(minimum_y - padding_units, layout_scale, true),
        snap_outward(maximum_x + padding_units, layout_scale, false),
        snap_outward(maximum_y + padding_units, layout_scale, false),
    };
    const double width_value = std::ceil((font_bounds.right - font_bounds.left) * atlas_scale);
    const double height_value = std::ceil((font_bounds.top - font_bounds.bottom) * atlas_scale);
    if (!std::isfinite(width_value) || !std::isfinite(height_value) ||
        width_value < 1.0 || height_value < 1.0 ||
        width_value > config.maximum_dimension || height_value > config.maximum_dimension) {
        throw FontError("MSDF glyph bitmap dimensions exceed configured bounds");
    }
    const auto width = static_cast<std::uint32_t>(width_value);
    const auto height = static_cast<std::uint32_t>(height_value);
    const std::uint64_t byte_count = static_cast<std::uint64_t>(width) * height * 4U;
    if (byte_count > std::numeric_limits<std::size_t>::max()) {
        throw FontError("MSDF glyph bitmap byte count overflows the host size");
    }

    std::vector<Contour> contours;
    contours.reserve(prepared_points.size());
    for (const std::vector<Point>& points : prepared_points) {
        Contour prepared = color_edges(points, config.corner_dot_threshold);
        if (!prepared.edges.empty()) contours.push_back(std::move(prepared));
    }
    if (contours.empty()) return std::nullopt;

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(byte_count));
    for (std::uint32_t y = 0U; y < height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            const Point sample{
                font_bounds.left + (static_cast<double>(x) + 0.5) / atlas_scale,
                font_bounds.bottom + (static_cast<double>(height - y) - 0.5) / atlas_scale,
            };
            const Distances distance = signed_distances(sample, contours);
            const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4U;
            bytes[index] = encode_distance(distance.red * atlas_scale, config.pixel_range);
            bytes[index + 1U] = encode_distance(distance.green * atlas_scale, config.pixel_range);
            bytes[index + 2U] = encode_distance(distance.blue * atlas_scale, config.pixel_range);
            bytes[index + 3U] = encode_distance(distance.monochrome * atlas_scale, config.pixel_range);
        }
    }
    return GlyphRasterBitmap{
        width,
        height,
        std::move(bytes),
        font_bounds,
        RasterPlaneBounds{
            font_bounds.left * layout_scale,
            font_bounds.bottom * layout_scale,
            font_bounds.right * layout_scale,
            font_bounds.top * layout_scale,
        },
        config.pixel_range,
        config.pixel_range / static_cast<double>(config.oversampling),
        layout_scale,
        atlas_scale,
        GlyphRasterMode::msdf,
    };
}

} // namespace strata::font
