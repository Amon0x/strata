#include "ui/presentation_geometry.hpp"

#include <algorithm>
#include <cmath>

#include "ui/motion.hpp"
#include "ui/tree.hpp"

namespace strata::ui {

const runtime::Value* visual_value(
    const RetainedNode& node,
    const std::string_view name
) noexcept {
    // Presence is significant: an explicit null removes themed/style chrome.
    const auto direct = node.description().properties.find(name);
    if (direct != node.description().properties.end()) {
        return direct->second.value();
    }
    const auto layout = node.description().properties.find("$layout");
    const runtime::Value* style = layout != node.description().properties.end()
                                      ? layout->second.value()
                                      : nullptr;
    const runtime::Value* nested = style != nullptr ? style->field(name) : nullptr;
    return nested;
}

std::optional<double> visual_number(
    const RetainedNode& node,
    const std::string_view name
) noexcept {
    const runtime::Value* value = visual_value(node, name);
    return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
               ? std::optional<double>(*value->number())
               : std::nullopt;
}

MotionTransform local_presentation_transform(
    const RetainedNode& node,
    const MotionRuntime& motion
) noexcept {
    const MotionComputedValues* computed = motion.computed_values(node.identity());
    const auto animated = [computed](const MotionProperty property) -> std::optional<double> {
        return computed != nullptr ? computed->number(property) : std::nullopt;
    };
    const double uniform = animated(MotionProperty::scale)
                               .value_or(visual_number(node, "scale").value_or(1.0));
    const auto axis_scale = [&](const MotionProperty property, const std::string_view name) {
        if (const std::optional<double> value = animated(property); value.has_value()) return *value;
        if (const std::optional<double> base = visual_number(node, name); base.has_value()) {
            return *base * uniform;
        }
        return uniform;
    };
    const runtime::Value* movement = node.retained_value("strata.movement.offset");
    const double movement_x = movement != nullptr && movement->field("x") != nullptr &&
        movement->field("x")->number() != nullptr
        ? *movement->field("x")->number()
        : 0.0;
    const double movement_y = movement != nullptr && movement->field("y") != nullptr &&
        movement->field("y")->number() != nullptr
        ? *movement->field("y")->number()
        : 0.0;
    return MotionTransform{
        animated(MotionProperty::x)
            .value_or(animated(MotionProperty::translate_x)
                          .value_or(visual_number(node, "translateX").value_or(0.0))) + movement_x,
        animated(MotionProperty::y)
            .value_or(animated(MotionProperty::translate_y)
                          .value_or(visual_number(node, "translateY").value_or(0.0))) + movement_y,
        axis_scale(MotionProperty::scale_x, "scaleX"),
        axis_scale(MotionProperty::scale_y, "scaleY"),
    };
}

MotionTransform concatenate_presentation_transform(
    const MotionTransform& parent,
    const MotionTransform& local
) noexcept {
    return MotionTransform{
        parent.scale_x * local.translate_x + parent.translate_x,
        parent.scale_y * local.translate_y + parent.translate_y,
        parent.scale_x * local.scale_x,
        parent.scale_y * local.scale_y,
    };
}

Rect transform_presentation_bounds(
    const Rect& bounds,
    const MotionTransform& transform
) noexcept {
    const double first_x = transform.scale_x * bounds.x + transform.translate_x;
    const double second_x = transform.scale_x * bounds.right() + transform.translate_x;
    const double first_y = transform.scale_y * bounds.y + transform.translate_y;
    const double second_y = transform.scale_y * bounds.bottom() + transform.translate_y;
    return Rect{
        std::min(first_x, second_x),
        std::min(first_y, second_y),
        std::abs(second_x - first_x),
        std::abs(second_y - first_y),
    };
}

Rect inverse_presentation_bounds(
    const Rect& bounds,
    const MotionTransform& transform
) noexcept {
    if (transform.identity() || transform.scale_x == 0.0 || transform.scale_y == 0.0) {
        return bounds;
    }
    // Preserve the full affine inverse operation order. Algebraically reducing this to
    // (position - translation) / scale changes canonical IEEE-754 output by a few ulps.
    const double inverse_determinant = 1.0 / (transform.scale_x * transform.scale_y);
    const double inverse_scale_x = transform.scale_y * inverse_determinant;
    const double inverse_scale_y = transform.scale_x * inverse_determinant;
    const MotionTransform inverse{
        -(inverse_scale_x * transform.translate_x),
        -(inverse_scale_y * transform.translate_y),
        inverse_scale_x,
        inverse_scale_y,
    };
    return transform_presentation_bounds(bounds, inverse);
}

Point inverse_presentation_point(
    const Point point,
    const MotionTransform& transform
) noexcept {
    if (transform.identity() || transform.scale_x == 0.0 || transform.scale_y == 0.0) {
        return point;
    }
    return Point{
        (point.x - transform.translate_x) / transform.scale_x,
        (point.y - transform.translate_y) / transform.scale_y,
    };
}

} // namespace strata::ui
