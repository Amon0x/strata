#include "ui/svg_image.hpp"

#include "ui/render/path_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace strata::ui {
namespace {

[[nodiscard]] svg::AffineTransform multiply(
    const svg::AffineTransform& left,
    const svg::AffineTransform& right
) noexcept {
    return svg::AffineTransform{
        left.a * right.a + left.c * right.b,
        left.b * right.a + left.d * right.b,
        left.a * right.c + left.c * right.d,
        left.b * right.c + left.d * right.d,
        left.a * right.e + left.c * right.f + left.e,
        left.b * right.e + left.d * right.f + left.f,
    };
}

[[nodiscard]] bool finite(const svg::AffineTransform& value) noexcept {
    return std::isfinite(value.a) && std::isfinite(value.b) && std::isfinite(value.c) &&
        std::isfinite(value.d) && std::isfinite(value.e) && std::isfinite(value.f);
}

[[nodiscard]] bool finite(const Rect value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.width) &&
        std::isfinite(value.height);
}

[[nodiscard]] double aligned_offset(
    const double remaining,
    const svg::AspectAlign alignment
) noexcept {
    if (alignment == svg::AspectAlign::middle) return remaining * 0.5;
    if (alignment == svg::AspectAlign::maximum) return remaining;
    return 0.0;
}

[[nodiscard]] svg::AffineTransform viewport_transform(
    const svg::Document& document,
    const Rect viewport
) noexcept {
    const svg::ViewBox box = document.view_box;
    double scale_x = viewport.width / box.width;
    double scale_y = viewport.height / box.height;
    double offset_x = 0.0;
    double offset_y = 0.0;
    if (document.preserve_aspect_ratio.x != svg::AspectAlign::none) {
        const double uniform = document.preserve_aspect_ratio.slice
            ? std::max(scale_x, scale_y)
            : std::min(scale_x, scale_y);
        scale_x = uniform;
        scale_y = uniform;
        offset_x = aligned_offset(
            viewport.width - box.width * uniform,
            document.preserve_aspect_ratio.x
        );
        offset_y = aligned_offset(
            viewport.height - box.height * uniform,
            document.preserve_aspect_ratio.y
        );
    }
    return svg::AffineTransform{
        scale_x,
        0.0,
        0.0,
        scale_y,
        viewport.x + offset_x - box.minimum_x * scale_x,
        viewport.y + offset_y - box.minimum_y * scale_y,
    };
}

[[nodiscard]] RenderColor color(
    const svg::Color value,
    const bool current_color,
    const RenderColor tint,
    const double opacity
) noexcept {
    const auto modulated = [](const std::uint8_t left, const std::uint8_t right) {
        return static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(left) * static_cast<std::uint32_t>(right) + 127U) / 255U
        );
    };
    const double alpha = std::isfinite(opacity) ? std::clamp(opacity, 0.0, 1.0) : 0.0;
    const std::uint8_t resolved_alpha = static_cast<std::uint8_t>(std::lround(
        static_cast<double>(modulated(value.alpha, tint.alpha)) * alpha
    ));
    if (current_color) {
        return RenderColor{tint.red, tint.green, tint.blue, resolved_alpha};
    }
    return RenderColor{
        modulated(value.red, tint.red),
        modulated(value.green, tint.green),
        modulated(value.blue, tint.blue),
        resolved_alpha,
    };
}

[[nodiscard]] Rect path_bounds(
    const svg::Path& path,
    const svg::PaintStyle& paint
) noexcept {
    double minimum_x = std::numeric_limits<double>::infinity();
    double minimum_y = std::numeric_limits<double>::infinity();
    double maximum_x = -std::numeric_limits<double>::infinity();
    double maximum_y = -std::numeric_limits<double>::infinity();
    const auto include = [&](const svg::Point point) {
        minimum_x = std::min(minimum_x, point.x);
        minimum_y = std::min(minimum_y, point.y);
        maximum_x = std::max(maximum_x, point.x);
        maximum_y = std::max(maximum_y, point.y);
    };
    for (const svg::PathSegment& segment : path.segments) {
        if (segment.verb == svg::PathVerb::close) continue;
        include(segment.to);
        if (segment.verb == svg::PathVerb::cubic) {
            include(segment.control_a);
            include(segment.control_b);
        }
    }
    if (!(minimum_x <= maximum_x && minimum_y <= maximum_y)) return {};
    const double stroke_expansion = paint.has_stroke
        ? paint.stroke_width * 0.5 *
              (paint.line_join == svg::LineJoin::miter ? paint.miter_limit : 1.0)
        : 0.0;
    if (!std::isfinite(stroke_expansion)) return {};
    minimum_x -= stroke_expansion;
    minimum_y -= stroke_expansion;
    maximum_x += stroke_expansion;
    maximum_y += stroke_expansion;
    if (maximum_x - minimum_x <= 1.0e-12) {
        minimum_x -= 0.5;
        maximum_x += 0.5;
    }
    if (maximum_y - minimum_y <= 1.0e-12) {
        minimum_y -= 0.5;
        maximum_y += 0.5;
    }
    const Rect result{minimum_x, minimum_y, maximum_x - minimum_x, maximum_y - minimum_y};
    return finite(result) ? result : Rect{};
}

[[nodiscard]] Point normalized(const svg::Point point, const Rect bounds) noexcept {
    return Point{
        (point.x - bounds.x) / bounds.width,
        (point.y - bounds.y) / bounds.height,
    };
}

[[nodiscard]] Path path(const svg::Path& source, const Rect bounds) {
    Path result;
    for (const svg::PathSegment& segment : source.segments) {
        switch (segment.verb) {
        case svg::PathVerb::move:
            result.move_to(normalized(segment.to, bounds));
            break;
        case svg::PathVerb::line:
            result.line_to(normalized(segment.to, bounds));
            break;
        case svg::PathVerb::cubic:
            result.cubic_to(
                normalized(segment.control_a, bounds),
                normalized(segment.control_b, bounds),
                normalized(segment.to, bounds)
            );
            break;
        case svg::PathVerb::close:
            result.close();
            break;
        }
    }
    return result;
}

[[nodiscard]] PathCap path_cap(const svg::LineCap value) noexcept {
    switch (value) {
    case svg::LineCap::butt: return PathCap::butt;
    case svg::LineCap::round: return PathCap::round;
    case svg::LineCap::square: return PathCap::square;
    }
    return PathCap::butt;
}

[[nodiscard]] PathJoin path_join(const svg::LineJoin value) noexcept {
    switch (value) {
    case svg::LineJoin::miter: return PathJoin::miter;
    case svg::LineJoin::round: return PathJoin::round;
    case svg::LineJoin::bevel: return PathJoin::bevel;
    }
    return PathJoin::miter;
}

} // namespace

void validate_svg_image_geometry(const svg::Document& document) {
    const std::size_t expected_paths = static_cast<std::size_t>(std::ranges::count_if(
        document.commands,
        [](const svg::DrawCommand& command) {
            return !command.path.empty() &&
                (command.paint.has_fill ||
                 (command.paint.has_stroke && command.paint.stroke_width > 0.0));
        }
    ));
    std::vector<RenderCommand> commands;
    append_svg_image(
        commands,
        document,
        Rect{0.0, 0.0, document.width, document.height},
        TextureRegion{},
        RenderColor{255U, 255U, 255U, 255U},
        1.0
    );
    std::size_t validated_paths = 0U;
    for (const RenderCommand& command : commands) {
        const auto* path_command = std::get_if<PathRenderCommand>(&command);
        if (path_command == nullptr) continue;
        ++validated_paths;
        static_cast<void>(tessellate_shape(
            path_command->shape,
            Size{path_command->bounds.width, path_command->bounds.height},
            100.0
        ));
    }
    if (validated_paths != expected_paths) {
        throw std::invalid_argument("SVG viewport or geometry exceeds the finite UI range");
    }
}

void append_svg_image(
    std::vector<RenderCommand>& output,
    const svg::Document& document,
    const Rect bounds,
    const TextureRegion source,
    const RenderColor tint,
    const double opacity
) {
    if (!finite(bounds) || bounds.width <= 0.0 || bounds.height <= 0.0 ||
        !std::isfinite(source.u) || !std::isfinite(source.v) ||
        !std::isfinite(source.width) || !std::isfinite(source.height) ||
        source.width <= 0.0 || source.height <= 0.0 || !std::isfinite(opacity)) {
        return;
    }
    const Rect full_viewport{
        bounds.x - source.u * bounds.width / source.width,
        bounds.y - source.v * bounds.height / source.height,
        bounds.width / source.width,
        bounds.height / source.height,
    };
    if (!finite(full_viewport)) return;
    const svg::AffineTransform viewport = viewport_transform(document, full_viewport);
    if (!finite(viewport)) return;
    output.emplace_back(ClipPushRenderCommand{bounds});
    for (const svg::DrawCommand& command : document.commands) {
        if (command.path.empty() || (!command.paint.has_fill && !command.paint.has_stroke)) {
            continue;
        }
        const svg::AffineTransform transform = multiply(viewport, command.transform);
        const Rect local_bounds = path_bounds(command.path, command.paint);
        if (!finite(transform) || !finite(local_bounds) || local_bounds.empty()) continue;
        PathShape shape;
        shape.path = path(command.path, local_bounds);
        if (command.paint.has_fill) {
            shape.fill = Paint(color(
                command.paint.fill,
                command.paint.fill_uses_current_color,
                tint,
                opacity * command.paint.opacity * command.paint.fill_opacity
            ));
        }
        if (command.paint.has_stroke && command.paint.stroke_width > 0.0) {
            shape.stroke = Paint(color(
                command.paint.stroke,
                command.paint.stroke_uses_current_color,
                tint,
                opacity * command.paint.opacity * command.paint.stroke_opacity
            ));
            shape.stroke_style = StrokeStyle{
                command.paint.stroke_width,
                path_cap(command.paint.line_cap),
                path_join(command.paint.line_join),
                command.paint.miter_limit,
            };
        }
        shape.fill_rule = command.paint.fill_rule == svg::FillRule::evenodd
            ? PathFillRule::evenodd
            : PathFillRule::nonzero;
        if (shape.fill.has_value() || shape.stroke.has_value()) {
            output.emplace_back(TransformPushRenderCommand{
                transform.a,
                transform.c,
                transform.e,
                transform.b,
                transform.d,
                transform.f,
            });
            output.emplace_back(PathRenderCommand{local_bounds, std::move(shape)});
            output.emplace_back(TransformPopRenderCommand{});
        }
    }
    output.emplace_back(ClipPopRenderCommand{});
}

} // namespace strata::ui
