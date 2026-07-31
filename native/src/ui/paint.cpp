#include "ui/paint.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>

namespace strata::ui {
namespace {

[[nodiscard]] double channel(const double value) noexcept {
    return std::clamp(value, 0.0, 255.0);
}

[[nodiscard]] std::uint8_t quantize(const double value) noexcept {
    return static_cast<std::uint8_t>(std::lround(channel(value)));
}

[[nodiscard]] runtime::ColorValue mix(
    const runtime::ColorValue from,
    const runtime::ColorValue to,
    const double factor
) noexcept {
    const auto blend = [factor](const std::uint8_t left, const std::uint8_t right) {
        return quantize(
            static_cast<double>(left) +
            (static_cast<double>(right) - static_cast<double>(left)) * factor
        );
    };
    return runtime::ColorValue{
        blend(from.red, to.red),
        blend(from.green, to.green),
        blend(from.blue, to.blue),
        blend(from.alpha, to.alpha),
    };
}

[[nodiscard]] double wrapped(const double offset, const GradientExtend extend) noexcept {
    if (offset >= 0.0 && offset <= 1.0) return offset;
    switch (extend) {
    case GradientExtend::clamp:
        return std::clamp(offset, 0.0, 1.0);
    case GradientExtend::repeat: {
        const double wrapped_value = offset - std::floor(offset);
        return wrapped_value;
    }
    case GradientExtend::mirror: {
        const double period = std::abs(std::fmod(offset, 2.0));
        const double folded = period > 1.0 ? 2.0 - period : period;
        return std::clamp(folded, 0.0, 1.0);
    }
    }
    return std::clamp(offset, 0.0, 1.0);
}

[[nodiscard]] const runtime::Value* field(const runtime::Value& value, const std::string_view name) {
    return value.field(name);
}

[[nodiscard]] std::optional<double> number_field(
    const runtime::Value& value,
    const std::string_view name
) {
    const runtime::Value* found = field(value, name);
    if (found == nullptr || found->number() == nullptr) return std::nullopt;
    const double number = *found->number();
    if (!std::isfinite(number)) {
        throw std::invalid_argument(
            "gradient field '" + std::string(name) + "' must be a finite number"
        );
    }
    return number;
}

[[nodiscard]] std::optional<Point> point_field(
    const runtime::Value& value,
    const std::string_view name
) {
    const runtime::Value* found = field(value, name);
    if (found == nullptr || found->object() == nullptr) return std::nullopt;
    const std::optional<double> x = number_field(*found, "x");
    const std::optional<double> y = number_field(*found, "y");
    if (!x.has_value() || !y.has_value()) {
        throw std::invalid_argument(
            "gradient point '" + std::string(name) + "' requires finite x and y"
        );
    }
    return Point{*x, *y};
}

[[nodiscard]] GradientExtend extend_from_text(const std::string& text) {
    if (text == "clamp") return GradientExtend::clamp;
    if (text == "repeat") return GradientExtend::repeat;
    if (text == "mirror") return GradientExtend::mirror;
    throw std::invalid_argument("gradient extend must be clamp, repeat or mirror");
}

} // namespace

void Gradient::validate() const {
    if (stops.size() < 2U) {
        throw std::invalid_argument("a gradient requires at least two stops");
    }
    if (stops.size() > maximum_gradient_stops) {
        throw std::invalid_argument(
            "a gradient supports at most " + std::to_string(maximum_gradient_stops) + " stops"
        );
    }
    double previous = 0.0;
    for (std::size_t index = 0U; index < stops.size(); ++index) {
        const double offset = stops[index].offset;
        if (!std::isfinite(offset) || offset < 0.0 || offset > 1.0) {
            throw std::invalid_argument("gradient stop offsets must lie in [0, 1]");
        }
        if (index > 0U && offset < previous) {
            throw std::invalid_argument("gradient stop offsets must not decrease");
        }
        previous = offset;
    }
    const auto finite_point = [](const Point point) {
        return std::isfinite(point.x) && std::isfinite(point.y);
    };
    if (kind == GradientKind::linear) {
        if (angle_degrees.has_value()) {
            if (!std::isfinite(*angle_degrees)) {
                throw std::invalid_argument("a linear gradient angle must be finite");
            }
            return;
        }
        if (!finite_point(start) || !finite_point(end)) {
            throw std::invalid_argument("a linear gradient axis must be finite");
        }
        if (start == end) {
            throw std::invalid_argument("a linear gradient axis must have a non-zero length");
        }
        return;
    }
    if (!finite_point(center)) {
        throw std::invalid_argument("a radial gradient centre must be finite");
    }
    if (!std::isfinite(radius_x) || !std::isfinite(radius_y) || radius_x <= 0.0 || radius_y <= 0.0) {
        throw std::invalid_argument("radial gradient radii must be finite and positive");
    }
}

runtime::ColorValue Gradient::sample(const double offset) const noexcept {
    if (stops.empty()) return runtime::ColorValue{};
    const double position = wrapped(offset, extend);
    if (position <= stops.front().offset) return stops.front().color;
    if (position >= stops.back().offset) return stops.back().color;
    for (std::size_t index = 1U; index < stops.size(); ++index) {
        const GradientStop& previous = stops[index - 1U];
        const GradientStop& next = stops[index];
        if (position > next.offset) continue;
        const double span = next.offset - previous.offset;
        if (span <= 0.0) return next.color;
        return mix(previous.color, next.color, (position - previous.offset) / span);
    }
    return stops.back().color;
}

std::pair<Point, Point> Gradient::axis(const Size shape) const noexcept {
    if (!angle_degrees.has_value()) return {start, end};
    // 0 degrees points up and angles advance clockwise, matching how designers state them. The
    // direction is built in the shape's own pixel space and then normalized, so the painted angle
    // survives a non-square shape; the axis spans the shape's projection onto that direction.
    const double width = shape.width > 0.0 ? shape.width : 1.0;
    const double height = shape.height > 0.0 ? shape.height : 1.0;
    const double radians = *angle_degrees * std::numbers::pi / 180.0;
    const double dx = std::sin(radians);
    const double dy = -std::cos(radians);
    const double extent = (std::abs(dx) * width + std::abs(dy) * height) * 0.5;
    const Point half{dx * extent / width, dy * extent / height};
    return {Point{0.5 - half.x, 0.5 - half.y}, Point{0.5 + half.x, 0.5 + half.y}};
}

Paint::Paint(const runtime::ColorValue color) noexcept : value_(color) {}

Paint::Paint(Gradient gradient) : value_(std::move(gradient)) {
    std::get<Gradient>(value_).validate();
}

bool Paint::is_gradient() const noexcept {
    return std::holds_alternative<Gradient>(value_);
}

const runtime::ColorValue* Paint::color() const noexcept {
    return std::get_if<runtime::ColorValue>(&value_);
}

const Gradient* Paint::gradient() const noexcept {
    return std::get_if<Gradient>(&value_);
}

runtime::ColorValue Paint::representative() const noexcept {
    if (const runtime::ColorValue* solid = color(); solid != nullptr) return *solid;
    return std::get<Gradient>(value_).sample(0.5);
}

bool Paint::transparent() const noexcept {
    if (const runtime::ColorValue* solid = color(); solid != nullptr) return solid->alpha == 0U;
    const Gradient& value = std::get<Gradient>(value_);
    return std::ranges::all_of(value.stops, [](const GradientStop& stop) {
        return stop.color.alpha == 0U;
    });
}

void Paint::multiply_alpha(const double opacity) noexcept {
    const double factor = std::clamp(opacity, 0.0, 1.0);
    const auto scale = [factor](runtime::ColorValue& color) {
        color.alpha = quantize(static_cast<double>(color.alpha) * factor);
    };
    if (runtime::ColorValue* solid = std::get_if<runtime::ColorValue>(&value_); solid != nullptr) {
        scale(*solid);
        return;
    }
    for (GradientStop& stop : std::get<Gradient>(value_).stops) scale(stop.color);
}

Paint Paint::with_alpha_multiplied(const double opacity) const {
    Paint result = *this;
    result.multiply_alpha(opacity);
    return result;
}

double gradient_parameter(
    const Gradient& gradient,
    const Point normalized,
    const Size shape
) noexcept {
    if (gradient.kind == GradientKind::linear) {
        const auto [start, end] = gradient.axis(shape);
        const Point direction{end.x - start.x, end.y - start.y};
        const double length_squared = direction.x * direction.x + direction.y * direction.y;
        if (length_squared <= 0.0) return 0.0;
        const Point offset{normalized.x - start.x, normalized.y - start.y};
        return (offset.x * direction.x + offset.y * direction.y) / length_squared;
    }
    const double x = (normalized.x - gradient.center.x) / gradient.radius_x;
    const double y = (normalized.y - gradient.center.y) / gradient.radius_y;
    return std::sqrt(x * x + y * y);
}

std::string_view gradient_extend_name(const GradientExtend extend) noexcept {
    switch (extend) {
    case GradientExtend::clamp: return "clamp";
    case GradientExtend::repeat: return "repeat";
    case GradientExtend::mirror: return "mirror";
    }
    return "clamp";
}

std::optional<Paint> paint_from_value(const runtime::Value* const value) {
    if (value == nullptr) return std::nullopt;
    if (const runtime::ColorValue* color = value->color(); color != nullptr) return Paint(*color);
    if (value->object() == nullptr) return std::nullopt;
    const runtime::Value* kind_value = field(*value, "kind");
    if (kind_value == nullptr || kind_value->string() == nullptr) return std::nullopt;
    const std::string& kind = *kind_value->string();
    Gradient gradient;
    if (kind == "linear") {
        gradient.kind = GradientKind::linear;
    } else if (kind == "radial") {
        gradient.kind = GradientKind::radial;
    } else {
        return std::nullopt;
    }
    if (const runtime::Value* extend = field(*value, "extend");
        extend != nullptr && extend->string() != nullptr) {
        gradient.extend = extend_from_text(*extend->string());
    }
    if (gradient.kind == GradientKind::linear) {
        const std::optional<Point> start = point_field(*value, "from");
        const std::optional<Point> end = point_field(*value, "to");
        const std::optional<double> angle = number_field(*value, "angle");
        if (start.has_value() != end.has_value()) {
            throw std::invalid_argument("a linear gradient axis requires both 'from' and 'to'");
        }
        if (start.has_value() && angle.has_value()) {
            throw std::invalid_argument(
                "a linear gradient uses either 'angle' or 'from'/'to', not both"
            );
        }
        if (start.has_value()) {
            gradient.start = *start;
            gradient.end = *end;
        } else {
            gradient.angle_degrees = angle.value_or(180.0);
        }
    } else {
        gradient.center = point_field(*value, "center").value_or(Point{0.5, 0.5});
        const std::optional<double> radius = number_field(*value, "radius");
        gradient.radius_x = number_field(*value, "radiusX").value_or(radius.value_or(0.5));
        gradient.radius_y = number_field(*value, "radiusY").value_or(radius.value_or(0.5));
    }
    const runtime::Value* stops = field(*value, "stops");
    const runtime::ValueList* list = stops != nullptr ? stops->list() : nullptr;
    if (list == nullptr) throw std::invalid_argument("a gradient requires a 'stops' list");
    gradient.stops.reserve(list->values.size());
    for (std::size_t index = 0U; index < list->values.size(); ++index) {
        const runtime::Value& entry = list->values[index];
        const runtime::Value* color = field(entry, "color");
        if (color == nullptr || color->color() == nullptr) {
            throw std::invalid_argument("every gradient stop requires a colour");
        }
        const std::optional<double> offset = number_field(entry, "offset");
        gradient.stops.push_back(GradientStop{
            offset.value_or(
                list->values.size() < 2U
                    ? 0.0
                    : static_cast<double>(index) / static_cast<double>(list->values.size() - 1U)
            ),
            *color->color(),
        });
    }
    return Paint(std::move(gradient));
}

} // namespace strata::ui
