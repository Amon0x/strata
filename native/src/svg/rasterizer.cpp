#include <strata/svg.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace strata::svg {
namespace {

constexpr std::uint32_t maximum_raster_dimension = 8192U;
constexpr std::uint32_t maximum_samples_per_axis = 8U;
/** At four doubles per sample this caps the temporary coverage buffer at 128 MiB. */
constexpr std::size_t maximum_sample_count = 4U * 1024U * 1024U;
constexpr std::size_t maximum_flattened_points = 1024U * 1024U;
/** Bounds point-in-path and point-to-stroke tests before adversarial geometry can monopolize a CPU.
 */
constexpr std::size_t maximum_geometric_work = 100U * 1024U * 1024U;
constexpr double epsilon = 1.0e-12;

struct Contour final {
    std::vector<Point> points;
    bool closed = false;
};

struct Bounds final {
    double minimum_x = std::numeric_limits<double>::infinity();
    double minimum_y = std::numeric_limits<double>::infinity();
    double maximum_x = -std::numeric_limits<double>::infinity();
    double maximum_y = -std::numeric_limits<double>::infinity();

    void include(const Point point) noexcept {
        minimum_x = std::min(minimum_x, point.x);
        minimum_y = std::min(minimum_y, point.y);
        maximum_x = std::max(maximum_x, point.x);
        maximum_y = std::max(maximum_y, point.y);
    }
    [[nodiscard]] bool valid() const noexcept {
        return minimum_x <= maximum_x && minimum_y <= maximum_y;
    }
};

struct Premultiplied final {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double alpha = 0.0;
};

[[nodiscard]] AffineTransform multiply(const AffineTransform& left,
                                       const AffineTransform& right) noexcept {
    return AffineTransform{
        left.a * right.a + left.c * right.b,          left.b * right.a + left.d * right.b,
        left.a * right.c + left.c * right.d,          left.b * right.c + left.d * right.d,
        left.a * right.e + left.c * right.f + left.e, left.b * right.e + left.d * right.f + left.f,
    };
}

[[nodiscard]] std::optional<AffineTransform> inverse(const AffineTransform& value) noexcept {
    const double determinant = value.a * value.d - value.b * value.c;
    if (!std::isfinite(determinant) || std::abs(determinant) <= epsilon)
        return std::nullopt;
    return AffineTransform{
        value.d / determinant,
        -value.b / determinant,
        -value.c / determinant,
        value.a / determinant,
        (value.c * value.f - value.d * value.e) / determinant,
        (value.b * value.e - value.a * value.f) / determinant,
    };
}

[[nodiscard]] Point midpoint(const Point left, const Point right) noexcept {
    return Point{(left.x + right.x) * 0.5, (left.y + right.y) * 0.5};
}

[[nodiscard]] Point subtract(const Point left, const Point right) noexcept {
    return Point{left.x - right.x, left.y - right.y};
}

[[nodiscard]] Point add(const Point left, const Point right) noexcept {
    return Point{left.x + right.x, left.y + right.y};
}

[[nodiscard]] Point scale(const Point point, const double factor) noexcept {
    return Point{point.x * factor, point.y * factor};
}

[[nodiscard]] double dot(const Point left, const Point right) noexcept {
    return left.x * right.x + left.y * right.y;
}

[[nodiscard]] double cross(const Point left, const Point right) noexcept {
    return left.x * right.y - left.y * right.x;
}

[[nodiscard]] double squared_length(const Point point) noexcept {
    return dot(point, point);
}

[[nodiscard]] double squared_distance(const Point left, const Point right) noexcept {
    return squared_length(subtract(left, right));
}

[[nodiscard]] double point_line_distance(const Point point, const Point line_start,
                                         const Point line_end) noexcept {
    const Point direction = subtract(line_end, line_start);
    const double length = std::sqrt(squared_length(direction));
    if (length <= epsilon)
        return std::sqrt(squared_distance(point, line_start));
    return std::abs(cross(direction, subtract(point, line_start))) / length;
}

void append_flat_point(std::vector<Point>& points, const Point point,
                       std::size_t& flattened_point_count) {
    if (points.empty() || squared_distance(points.back(), point) > epsilon * epsilon) {
        if (flattened_point_count >= maximum_flattened_points) {
            throw std::invalid_argument("SVG rasterization exceeds the flattened point limit");
        }
        points.push_back(point);
        ++flattened_point_count;
    }
}

void flatten_cubic(std::vector<Point>& points, const Point first, const Point control_a,
                   const Point control_b, const Point last, const AffineTransform& device_transform,
                   const std::size_t depth, std::size_t& flattened_point_count) {
    const Point device_first = device_transform.apply(first);
    const Point device_last = device_transform.apply(last);
    const double error =
        std::max(point_line_distance(device_transform.apply(control_a), device_first, device_last),
                 point_line_distance(device_transform.apply(control_b), device_first, device_last));
    if (error <= 0.08 || depth >= 14U) {
        append_flat_point(points, last, flattened_point_count);
        return;
    }
    const Point first_a = midpoint(first, control_a);
    const Point a_b = midpoint(control_a, control_b);
    const Point b_last = midpoint(control_b, last);
    const Point left_b = midpoint(first_a, a_b);
    const Point right_a = midpoint(a_b, b_last);
    const Point center = midpoint(left_b, right_a);
    flatten_cubic(points, first, first_a, left_b, center, device_transform, depth + 1U,
                  flattened_point_count);
    flatten_cubic(points, center, right_a, b_last, last, device_transform, depth + 1U,
                  flattened_point_count);
}

[[nodiscard]] std::vector<Contour> flatten(const Path& path,
                                           const AffineTransform& device_transform) {
    std::vector<Contour> contours;
    Contour current_contour;
    std::size_t flattened_point_count = 0U;
    Point current;
    Point subpath_start;
    bool has_current = false;
    const auto finish_contour = [&] {
        if (!current_contour.points.empty()) {
            if (current_contour.closed && current_contour.points.size() > 1U &&
                squared_distance(current_contour.points.front(), current_contour.points.back()) <=
                    epsilon * epsilon) {
                current_contour.points.pop_back();
            }
            contours.push_back(std::move(current_contour));
            current_contour = {};
        }
    };
    for (const PathSegment& segment : path.segments) {
        if (segment.verb == PathVerb::move) {
            finish_contour();
            current = segment.to;
            subpath_start = current;
            has_current = true;
            append_flat_point(current_contour.points, current, flattened_point_count);
        } else if (segment.verb == PathVerb::line) {
            if (!has_current)
                continue;
            append_flat_point(current_contour.points, segment.to, flattened_point_count);
            current = segment.to;
        } else if (segment.verb == PathVerb::cubic) {
            if (!has_current)
                continue;
            flatten_cubic(current_contour.points, current, segment.control_a, segment.control_b,
                          segment.to, device_transform, 0U, flattened_point_count);
            current = segment.to;
        } else if (segment.verb == PathVerb::close) {
            if (!has_current)
                continue;
            current_contour.closed = true;
            current = subpath_start;
        }
    }
    finish_contour();
    return contours;
}

[[nodiscard]] bool fill_contains(const std::span<const Contour> contours, const Point point,
                                 const FillRule rule) noexcept {
    int winding = 0;
    bool odd = false;
    for (const Contour& contour : contours) {
        if (contour.points.size() < 3U)
            continue;
        for (std::size_t index = 0U; index < contour.points.size(); ++index) {
            const Point from = contour.points[index];
            const Point to = contour.points[(index + 1U) % contour.points.size()];
            const bool crosses_up = from.y <= point.y && to.y > point.y;
            const bool crosses_down = from.y > point.y && to.y <= point.y;
            if (!crosses_up && !crosses_down)
                continue;
            const double side = cross(subtract(to, from), subtract(point, from));
            if (rule == FillRule::evenodd) {
                if ((crosses_up && side > 0.0) || (crosses_down && side < 0.0))
                    odd = !odd;
            } else if (crosses_up && side > 0.0) {
                ++winding;
            } else if (crosses_down && side < 0.0) {
                --winding;
            }
        }
    }
    return rule == FillRule::evenodd ? odd : winding != 0;
}

[[nodiscard]] bool segment_rectangle_contains(const Point point, const Point from, const Point to,
                                              const double half_width, const double start_extension,
                                              const double end_extension) noexcept {
    const Point direction = subtract(to, from);
    const double length_squared = squared_length(direction);
    if (length_squared <= epsilon)
        return squared_distance(point, from) <= half_width * half_width;
    const double length = std::sqrt(length_squared);
    const double projection = dot(subtract(point, from), direction) / length_squared;
    if (projection < -start_extension / length || projection > 1.0 + end_extension / length) {
        return false;
    }
    const double perpendicular = std::abs(cross(direction, subtract(point, from))) / length;
    return perpendicular <= half_width;
}

[[nodiscard]] bool triangle_contains(const Point point, const Point first, const Point second,
                                     const Point third) noexcept {
    const double first_side = cross(subtract(second, first), subtract(point, first));
    const double second_side = cross(subtract(third, second), subtract(point, second));
    const double third_side = cross(subtract(first, third), subtract(point, third));
    const bool has_negative =
        first_side < -epsilon || second_side < -epsilon || third_side < -epsilon;
    const bool has_positive = first_side > epsilon || second_side > epsilon || third_side > epsilon;
    return !(has_negative && has_positive);
}

[[nodiscard]] bool join_contains(const Point point, const Point previous, const Point vertex,
                                 const Point next, const double half_width, const LineJoin join,
                                 const double miter_limit) noexcept {
    if (join == LineJoin::round) {
        return squared_distance(point, vertex) <= half_width * half_width;
    }
    Point incoming = subtract(vertex, previous);
    Point outgoing = subtract(next, vertex);
    const double incoming_length = std::sqrt(squared_length(incoming));
    const double outgoing_length = std::sqrt(squared_length(outgoing));
    if (incoming_length <= epsilon || outgoing_length <= epsilon)
        return false;
    incoming = scale(incoming, 1.0 / incoming_length);
    outgoing = scale(outgoing, 1.0 / outgoing_length);
    const double turn = cross(incoming, outgoing);
    if (std::abs(turn) <= epsilon)
        return false;
    const double outer_sign = turn > 0.0 ? -1.0 : 1.0;
    const Point incoming_normal{-incoming.y * outer_sign, incoming.x * outer_sign};
    const Point outgoing_normal{-outgoing.y * outer_sign, outgoing.x * outer_sign};
    const Point first = add(vertex, scale(incoming_normal, half_width));
    const Point second = add(vertex, scale(outgoing_normal, half_width));
    if (triangle_contains(point, vertex, first, second))
        return true;
    if (join == LineJoin::bevel)
        return false;

    const double directions_cross = cross(incoming, outgoing);
    if (std::abs(directions_cross) <= epsilon)
        return false;
    const double distance_along_incoming =
        cross(subtract(second, first), outgoing) / directions_cross;
    const Point miter = add(first, scale(incoming, distance_along_incoming));
    if (std::sqrt(squared_distance(miter, vertex)) > half_width * miter_limit)
        return false;
    return triangle_contains(point, first, miter, second);
}

[[nodiscard]] bool stroke_contains(const std::span<const Contour> contours, const Point point,
                                   const PaintStyle& style) noexcept {
    const double half_width = style.stroke_width * 0.5;
    if (half_width <= 0.0)
        return false;
    for (const Contour& contour : contours) {
        const std::size_t point_count = contour.points.size();
        if (point_count < 2U)
            continue;
        const std::size_t edge_count = contour.closed ? point_count : point_count - 1U;
        for (std::size_t edge = 0U; edge < edge_count; ++edge) {
            const double start_extension =
                !contour.closed && edge == 0U && style.line_cap == LineCap::square ? half_width
                                                                                   : 0.0;
            const double end_extension =
                !contour.closed && edge + 1U == edge_count && style.line_cap == LineCap::square
                    ? half_width
                    : 0.0;
            if (segment_rectangle_contains(point, contour.points[edge],
                                           contour.points[(edge + 1U) % point_count], half_width,
                                           start_extension, end_extension)) {
                return true;
            }
        }
        if (!contour.closed && style.line_cap == LineCap::round &&
            (squared_distance(point, contour.points.front()) <= half_width * half_width ||
             squared_distance(point, contour.points.back()) <= half_width * half_width)) {
            return true;
        }
        const std::size_t join_begin = contour.closed ? 0U : 1U;
        const std::size_t join_end = contour.closed ? point_count : point_count - 1U;
        for (std::size_t index = join_begin; index < join_end; ++index) {
            const Point previous = contour.points[(index + point_count - 1U) % point_count];
            const Point vertex = contour.points[index];
            const Point next = contour.points[(index + 1U) % point_count];
            if (join_contains(point, previous, vertex, next, half_width, style.line_join,
                              style.miter_limit)) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] AffineTransform viewport_transform(const Document& document,
                                                 const std::uint32_t output_width,
                                                 const std::uint32_t output_height) {
    const ViewBox box = document.view_box;
    double scale_x = document.width / box.width;
    double scale_y = document.height / box.height;
    double translate_x = 0.0;
    double translate_y = 0.0;
    if (document.preserve_aspect_ratio.x != AspectAlign::none) {
        const double uniform_scale = document.preserve_aspect_ratio.slice
                                         ? std::max(scale_x, scale_y)
                                         : std::min(scale_x, scale_y);
        scale_x = uniform_scale;
        scale_y = uniform_scale;
        const double remaining_x = document.width - box.width * uniform_scale;
        const double remaining_y = document.height - box.height * uniform_scale;
        const auto offset = [](const double remaining, const AspectAlign align) noexcept {
            if (align == AspectAlign::middle)
                return remaining * 0.5;
            if (align == AspectAlign::maximum)
                return remaining;
            return 0.0;
        };
        translate_x = offset(remaining_x, document.preserve_aspect_ratio.x);
        translate_y = offset(remaining_y, document.preserve_aspect_ratio.y);
    }
    const AffineTransform view_box_transform{
        scale_x,
        0.0,
        0.0,
        scale_y,
        translate_x - box.minimum_x * scale_x,
        translate_y - box.minimum_y * scale_y,
    };
    const AffineTransform output_transform{
        static_cast<double>(output_width) / document.width,   0.0, 0.0,
        static_cast<double>(output_height) / document.height, 0.0, 0.0,
    };
    return multiply(output_transform, view_box_transform);
}

void blend(Premultiplied& destination, const Color color, const double opacity) noexcept {
    const double source_alpha =
        std::clamp(static_cast<double>(color.alpha) / 255.0 * opacity, 0.0, 1.0);
    const double inverse_alpha = 1.0 - source_alpha;
    destination.red =
        static_cast<double>(color.red) / 255.0 * source_alpha + destination.red * inverse_alpha;
    destination.green =
        static_cast<double>(color.green) / 255.0 * source_alpha + destination.green * inverse_alpha;
    destination.blue =
        static_cast<double>(color.blue) / 255.0 * source_alpha + destination.blue * inverse_alpha;
    destination.alpha = source_alpha + destination.alpha * inverse_alpha;
}

[[nodiscard]] std::uint8_t byte(const double value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(std::lround(value * 255.0), 0L, 255L));
}

} // namespace

std::span<const std::uint8_t, 4U> Image::pixel(const std::uint32_t x, const std::uint32_t y) const {
    if (x >= width || y >= height)
        throw std::out_of_range("SVG image pixel is out of bounds");
    const std::size_t pixel_index =
        static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + x;
    if (pixel_index > (std::numeric_limits<std::size_t>::max() - 3U) / 4U) {
        throw std::invalid_argument("SVG image dimensions overflow addressable storage");
    }
    const std::size_t offset = pixel_index * 4U;
    if (offset + 4U > rgba.size()) {
        throw std::invalid_argument("SVG image storage does not match its dimensions");
    }
    return std::span<const std::uint8_t, 4U>(rgba.data() + offset, 4U);
}

Image rasterize(const Document& document, const RasterOptions& options) {
    if (!std::isfinite(document.width) || !std::isfinite(document.height) ||
        document.width <= 0.0 || document.height <= 0.0 ||
        !std::isfinite(document.view_box.width) || !std::isfinite(document.view_box.height) ||
        document.view_box.width <= 0.0 || document.view_box.height <= 0.0) {
        throw std::invalid_argument("SVG document has invalid viewport dimensions");
    }
    const auto derived_dimension = [](const double value) -> std::uint32_t {
        if (value > static_cast<double>(maximum_raster_dimension)) {
            throw std::invalid_argument("SVG derived raster dimension exceeds the limit");
        }
        return static_cast<std::uint32_t>(std::max(1.0, std::ceil(value)));
    };
    const std::uint32_t width =
        options.width == 0U ? derived_dimension(document.width) : options.width;
    const std::uint32_t height =
        options.height == 0U ? derived_dimension(document.height) : options.height;
    if (width == 0U || height == 0U || width > maximum_raster_dimension ||
        height > maximum_raster_dimension) {
        throw std::invalid_argument("SVG raster dimensions must be between 1 and 8192");
    }
    if (options.samples_per_axis == 0U || options.samples_per_axis > maximum_samples_per_axis) {
        throw std::invalid_argument("SVG samples-per-axis must be between 1 and 8");
    }
    const std::size_t samples_per_pixel =
        static_cast<std::size_t>(options.samples_per_axis) * options.samples_per_axis;
    const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
    if (pixel_count > maximum_sample_count / samples_per_pixel) {
        throw std::invalid_argument("SVG raster exceeds the supersample allocation limit");
    }
    const double background_alpha = static_cast<double>(options.background.alpha) / 255.0;
    const Premultiplied background{
        static_cast<double>(options.background.red) / 255.0 * background_alpha,
        static_cast<double>(options.background.green) / 255.0 * background_alpha,
        static_cast<double>(options.background.blue) / 255.0 * background_alpha,
        background_alpha,
    };
    std::vector<Premultiplied> samples(pixel_count * samples_per_pixel, background);
    const AffineTransform viewport = viewport_transform(document, width, height);
    std::size_t geometric_work = 0U;

    for (const DrawCommand& command : document.commands) {
        if ((!command.paint.has_fill && !command.paint.has_stroke) || command.path.empty())
            continue;
        const AffineTransform device = multiply(viewport, command.transform);
        const std::optional<AffineTransform> device_inverse = inverse(device);
        if (!device_inverse.has_value())
            continue;
        const std::vector<Contour> contours = flatten(command.path, device);
        Bounds bounds;
        for (const Contour& contour : contours) {
            for (const Point point : contour.points)
                bounds.include(device.apply(point));
        }
        if (!bounds.valid())
            continue;
        if (command.paint.has_stroke && command.paint.stroke_width > 0.0) {
            const double scale_bound =
                std::max(std::hypot(device.a, device.b), std::hypot(device.c, device.d));
            const double join_bound =
                command.paint.line_join == LineJoin::miter ? command.paint.miter_limit : 1.0;
            const double expansion =
                command.paint.stroke_width * 0.5 * scale_bound * join_bound + 1.0;
            bounds.minimum_x -= expansion;
            bounds.minimum_y -= expansion;
            bounds.maximum_x += expansion;
            bounds.maximum_y += expansion;
        }
        const auto lower_bound = [](const double value, const std::uint32_t maximum) {
            return static_cast<std::uint32_t>(
                std::clamp(std::floor(value), 0.0, static_cast<double>(maximum)));
        };
        const auto upper_bound = [](const double value, const std::uint32_t maximum) {
            return static_cast<std::uint32_t>(
                std::clamp(std::ceil(value), 0.0, static_cast<double>(maximum)));
        };
        const std::uint32_t minimum_x = lower_bound(bounds.minimum_x, width);
        const std::uint32_t minimum_y = lower_bound(bounds.minimum_y, height);
        const std::uint32_t maximum_x = upper_bound(bounds.maximum_x, width);
        const std::uint32_t maximum_y = upper_bound(bounds.maximum_y, height);
        std::size_t flattened_points = 0U;
        for (const Contour& contour : contours) {
            if (flattened_points > maximum_geometric_work - contour.points.size()) {
                throw std::invalid_argument("SVG raster exceeds the geometric work limit");
            }
            flattened_points += contour.points.size();
        }
        const std::size_t paint_passes = static_cast<std::size_t>(command.paint.has_fill) +
                                         static_cast<std::size_t>(command.paint.has_stroke);
        const std::size_t covered_pixels =
            static_cast<std::size_t>(maximum_x - minimum_x) * (maximum_y - minimum_y);
        std::size_t command_work = covered_pixels;
        for (const std::size_t factor : {samples_per_pixel, flattened_points, paint_passes}) {
            if (factor != 0U && command_work > maximum_geometric_work / factor) {
                throw std::invalid_argument("SVG raster exceeds the geometric work limit");
            }
            command_work *= factor;
        }
        if (geometric_work > maximum_geometric_work - command_work) {
            throw std::invalid_argument("SVG raster exceeds the geometric work limit");
        }
        geometric_work += command_work;
        for (std::uint32_t y = minimum_y; y < maximum_y; ++y) {
            for (std::uint32_t x = minimum_x; x < maximum_x; ++x) {
                for (std::uint32_t sample_y = 0U; sample_y < options.samples_per_axis; ++sample_y) {
                    for (std::uint32_t sample_x = 0U; sample_x < options.samples_per_axis;
                         ++sample_x) {
                        const Point device_point{
                            static_cast<double>(x) +
                                (static_cast<double>(sample_x) + 0.5) /
                                    static_cast<double>(options.samples_per_axis),
                            static_cast<double>(y) +
                                (static_cast<double>(sample_y) + 0.5) /
                                    static_cast<double>(options.samples_per_axis),
                        };
                        const Point local_point = device_inverse->apply(device_point);
                        const std::size_t sample_index =
                            (static_cast<std::size_t>(y) * width + x) * samples_per_pixel +
                            static_cast<std::size_t>(sample_y) * options.samples_per_axis +
                            sample_x;
                        Premultiplied& destination = samples[sample_index];
                        if (command.paint.has_fill &&
                            fill_contains(contours, local_point, command.paint.fill_rule)) {
                            blend(destination, command.paint.fill,
                                  command.paint.opacity * command.paint.fill_opacity);
                        }
                        if (command.paint.has_stroke &&
                            stroke_contains(contours, local_point, command.paint)) {
                            blend(destination, command.paint.stroke,
                                  command.paint.opacity * command.paint.stroke_opacity);
                        }
                    }
                }
            }
        }
    }

    Image image;
    image.width = width;
    image.height = height;
    image.rgba.resize(pixel_count * 4U);
    for (std::size_t pixel = 0U; pixel < pixel_count; ++pixel) {
        Premultiplied average;
        for (std::size_t sample = 0U; sample < samples_per_pixel; ++sample) {
            const Premultiplied& value = samples[pixel * samples_per_pixel + sample];
            average.red += value.red;
            average.green += value.green;
            average.blue += value.blue;
            average.alpha += value.alpha;
        }
        const double divisor = static_cast<double>(samples_per_pixel);
        average.red /= divisor;
        average.green /= divisor;
        average.blue /= divisor;
        average.alpha /= divisor;
        const std::size_t output = pixel * 4U;
        if (average.alpha <= epsilon) {
            image.rgba[output] = 0U;
            image.rgba[output + 1U] = 0U;
            image.rgba[output + 2U] = 0U;
            image.rgba[output + 3U] = 0U;
        } else {
            image.rgba[output] = byte(average.red / average.alpha);
            image.rgba[output + 1U] = byte(average.green / average.alpha);
            image.rgba[output + 2U] = byte(average.blue / average.alpha);
            image.rgba[output + 3U] = byte(average.alpha);
        }
    }
    return image;
}

std::vector<std::uint8_t> encode_pam(const Image& image) {
    const std::size_t pixel_count = static_cast<std::size_t>(image.width) * image.height;
    if (pixel_count > std::numeric_limits<std::size_t>::max() / 4U) {
        throw std::invalid_argument("SVG image dimensions overflow addressable storage");
    }
    const std::size_t expected = pixel_count * 4U;
    if (image.width == 0U || image.height == 0U || image.rgba.size() != expected) {
        throw std::invalid_argument("SVG image storage does not match its dimensions");
    }
    const std::string header = "P7\nWIDTH " + std::to_string(image.width) + "\nHEIGHT " +
                               std::to_string(image.height) +
                               "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
    std::vector<std::uint8_t> encoded;
    encoded.reserve(header.size() + image.rgba.size());
    encoded.insert(encoded.end(), header.begin(), header.end());
    encoded.insert(encoded.end(), image.rgba.begin(), image.rgba.end());
    return encoded;
}

} // namespace strata::svg
