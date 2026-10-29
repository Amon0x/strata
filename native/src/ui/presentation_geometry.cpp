#include "ui/presentation_geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>

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

const PresentationStatics& presentation_statics(const RetainedNode& node) noexcept {
    std::shared_ptr<const PresentationStatics>& cached = node.presentation_statics_cache();
    if (cached != nullptr)
        return *cached;
    PresentationStatics statics;
    statics.scale = visual_number(node, "scale");
    statics.scale_x = visual_number(node, "scaleX");
    statics.scale_y = visual_number(node, "scaleY");
    statics.translate_x = visual_number(node, "translateX");
    statics.translate_y = visual_number(node, "translateY");
    statics.rotate = visual_number(node, "rotate");
    statics.rotate_x = visual_number(node, "rotateX");
    statics.rotate_y = visual_number(node, "rotateY");
    statics.perspective = visual_number(node, "perspective");
    statics.opacity = visual_number(node, "opacity");
    statics.tone = visual_number(node, "tone");
    const runtime::Value* origin = visual_value(node, "transformOrigin");
    const auto origin_fraction = [origin](const std::string_view axis) {
        const runtime::Value* value = origin != nullptr ? origin->field(axis) : nullptr;
        return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
                   ? *value->number()
                   : 0.5;
    };
    statics.origin_x = origin_fraction("x");
    statics.origin_y = origin_fraction("y");
    const std::optional<ContentSizeMotionSpec> content_motion =
        content_size_motion(node.description());
    statics.content_motion_clip = content_motion.has_value() && content_motion->clip;
    cached = std::make_shared<const PresentationStatics>(statics);
    return *cached;
}

bool motion_presentation_group(const RetainedNode& node, const MotionRuntime& motion) noexcept {
    const MotionComputedValues* computed = motion.computed_values(node.identity());
    // Only while the motion runs: a settled node bakes its final presentation back into its
    // commands, so it keeps visibility culling and costs nothing per frame.
    if (computed == nullptr || !motion.running(node.identity()))
        return false;
    for (const MotionProperty property :
         {MotionProperty::opacity, MotionProperty::scale, MotionProperty::scale_x,
          MotionProperty::scale_y, MotionProperty::x, MotionProperty::y,
          MotionProperty::translate_x, MotionProperty::translate_y, MotionProperty::rotate,
          MotionProperty::rotate_x, MotionProperty::rotate_y, MotionProperty::perspective}) {
        if (computed->number(property).has_value())
            return true;
    }
    return false;
}

bool presentation_in_depth(const RetainedNode& node, const MotionRuntime& motion) noexcept {
    const MotionComputedValues* computed = motion.computed_values(node.identity());
    const PresentationStatics& statics = presentation_statics(node);
    const auto turned = [&](const MotionProperty property, const std::optional<double> base) {
        const std::optional<double> value =
            computed != nullptr ? computed->number(property) : std::nullopt;
        const double degrees = value.value_or(base.value_or(0.0));
        return std::isfinite(degrees) && std::fmod(degrees, 360.0) != 0.0;
    };
    return turned(MotionProperty::rotate_x, statics.rotate_x) ||
           turned(MotionProperty::rotate_y, statics.rotate_y);
}

std::optional<double> local_presentation_tone(const RetainedNode& node,
                                              const MotionRuntime& motion) noexcept {
    const MotionComputedValues* computed = motion.computed_values(node.identity());
    std::optional<double> tone =
        computed != nullptr ? computed->number(MotionProperty::tone) : std::nullopt;
    if (!tone.has_value())
        tone = presentation_statics(node).tone;
    return tone.has_value() ? std::optional<double>(std::clamp(*tone, 0.0, 1.0)) : std::nullopt;
}

MotionTransform local_presentation_transform(
    const RetainedNode& node,
    const MotionRuntime& motion,
    const Rect& bounds
) noexcept {
    const MotionComputedValues* computed = motion.computed_values(node.identity());
    const PresentationStatics& statics = presentation_statics(node);
    const auto animated = [computed](const MotionProperty property) -> std::optional<double> {
        return computed != nullptr ? computed->number(property) : std::nullopt;
    };
    const double uniform = animated(MotionProperty::scale).value_or(statics.scale.value_or(1.0));
    const auto axis_scale = [&](const MotionProperty property, const std::optional<double> base) {
        if (const std::optional<double> value = animated(property); value.has_value()) return *value;
        return base.has_value() ? *base * uniform : uniform;
    };
    const runtime::Value* movement = node.retained_value("strata.movement.offset");
    const auto movement_axis = [movement](const std::string_view axis) {
        const runtime::Value* value = movement != nullptr ? movement->field(axis) : nullptr;
        return value != nullptr && value->number() != nullptr ? *value->number() : 0.0;
    };
    const double scale_x = axis_scale(MotionProperty::scale_x, statics.scale_x);
    const double scale_y = axis_scale(MotionProperty::scale_y, statics.scale_y);
    const double translation_x = animated(MotionProperty::x)
        .value_or(animated(MotionProperty::translate_x)
                      .value_or(statics.translate_x.value_or(0.0))) + movement_axis("x");
    const double translation_y = animated(MotionProperty::y)
        .value_or(animated(MotionProperty::translate_y)
                      .value_or(statics.translate_y.value_or(0.0))) + movement_axis("y");
    const double degrees =
        animated(MotionProperty::rotate).value_or(statics.rotate.value_or(0.0));
    const Point center{
        bounds.x + bounds.width * statics.origin_x,
        bounds.y + bounds.height * statics.origin_y,
    };
    const double tilt_x =
        animated(MotionProperty::rotate_x).value_or(statics.rotate_x.value_or(0.0));
    const double tilt_y =
        animated(MotionProperty::rotate_y).value_or(statics.rotate_y.value_or(0.0));
    const bool in_depth = (std::isfinite(tilt_x) && std::fmod(tilt_x, 360.0) != 0.0) ||
                          (std::isfinite(tilt_y) && std::fmod(tilt_y, 360.0) != 0.0);
    if (in_depth) {
        // The plane turned in depth about the origin and seen from `perspective` away, as CSS
        // does: rotateY first, then rotateX. A point (x, y) about the origin lands at
        // (X d / (d - Z), Y d / (d - Z)), which is a homography of (x, y) on the plane z = 0.
        const double distance = std::max(
            1.0,
            animated(MotionProperty::perspective).value_or(statics.perspective.value_or(800.0)));
        const double a = std::isfinite(tilt_x) ? tilt_x * std::numbers::pi / 180.0 : 0.0;
        const double b = std::isfinite(tilt_y) ? tilt_y * std::numbers::pi / 180.0 : 0.0;
        const double planar = std::isfinite(degrees) ? degrees * std::numbers::pi / 180.0 : 0.0;
        // Scale, then turn in the plane, as for 2D presentation.
        const double pc = std::cos(planar);
        const double ps = std::sin(planar);
        const std::array<double, 9U> plane{
            pc * scale_x, -ps * scale_y, 0.0, ps * scale_x, pc * scale_y, 0.0, 0.0, 0.0, 1.0};
        const std::array<double, 9U> depth{
            std::cos(b),
            0.0,
            0.0,
            std::sin(a) * std::sin(b),
            std::cos(a),
            0.0,
            std::sin(b) * std::cos(a) / distance,
            -std::sin(a) / distance,
            1.0,
        };
        const auto multiply = [](const std::array<double, 9U>& left,
                                 const std::array<double, 9U>& right) {
            std::array<double, 9U> product{};
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    for (int k = 0; k < 3; ++k)
                        product[row * 3 + column] += left[row * 3 + k] * right[k * 3 + column];
                }
            }
            return product;
        };
        const std::array<double, 9U> to_origin{1.0,       0.0, -center.x, 0.0, 1.0,
                                               -center.y, 0.0, 0.0,       1.0};
        const std::array<double, 9U> back{
            1.0, 0.0, center.x + translation_x, 0.0, 1.0, center.y + translation_y, 0.0, 0.0, 1.0};
        const std::array<double, 9U> h =
            multiply(back, multiply(depth, multiply(plane, to_origin)));
        const double w = h[8] != 0.0 ? h[8] : 1.0;
        return MotionTransform{h[2] / w, h[5] / w, h[0] / w, h[4] / w,
                               h[1] / w, h[3] / w, h[6] / w, h[7] / w};
    }
    if (!std::isfinite(degrees) || std::fmod(degrees, 360.0) == 0.0) {
        return MotionTransform{
            translation_x + center.x * (1.0 - scale_x),
            translation_y + center.y * (1.0 - scale_y),
            scale_x,
            scale_y,
        };
    }
    // Scale, then rotate clockwise on screen (y grows downward), both about the origin.
    const double radians = degrees * std::numbers::pi / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const double a = cosine * scale_x;
    const double b = -sine * scale_y;
    const double c = sine * scale_x;
    const double d = cosine * scale_y;
    return MotionTransform{
        translation_x + center.x - (a * center.x + b * center.y),
        translation_y + center.y - (c * center.x + d * center.y),
        a,
        d,
        b,
        c,
    };
}

double local_presentation_opacity(
    const RetainedNode& node,
    const MotionRuntime* const motion
) noexcept {
    const MotionComputedValues* computed =
        motion != nullptr ? motion->computed_values(node.identity()) : nullptr;
    const std::optional<double> base = presentation_statics(node).opacity;
    return std::clamp(
        computed != nullptr ? computed->number(MotionProperty::opacity).value_or(base.value_or(1.0))
                            : base.value_or(1.0),
        0.0,
        1.0
    );
}

Point transform_presentation_point(const Point point, const MotionTransform& transform) noexcept {
    const double x =
        transform.scale_x * point.x + transform.shear_x * point.y + transform.translate_x;
    const double y =
        transform.shear_y * point.x + transform.scale_y * point.y + transform.translate_y;
    if (transform.affine())
        return Point{x, y};
    // A point at or behind the viewer would flip through infinity: keep it just in front.
    const double w =
        std::max(transform.project_x * point.x + transform.project_y * point.y + 1.0, 1.0e-6);
    return Point{x / w, y / w};
}

std::optional<MotionTransform> inverse_presentation_transform(
    const MotionTransform& transform
) noexcept {
    if (!transform.affine()) {
        // The inverse of the 3x3 homography, by cofactors, normalised so its last term is one.
        const double m00 = transform.scale_x, m01 = transform.shear_x, m02 = transform.translate_x;
        const double m10 = transform.shear_y, m11 = transform.scale_y, m12 = transform.translate_y;
        const double m20 = transform.project_x, m21 = transform.project_y, m22 = 1.0;
        const double c00 = m11 * m22 - m12 * m21;
        const double c01 = m02 * m21 - m01 * m22;
        const double c02 = m01 * m12 - m02 * m11;
        const double c10 = m12 * m20 - m10 * m22;
        const double c11 = m00 * m22 - m02 * m20;
        const double c12 = m02 * m10 - m00 * m12;
        const double c20 = m10 * m21 - m11 * m20;
        const double c21 = m01 * m20 - m00 * m21;
        const double c22 = m00 * m11 - m01 * m10;
        const double determinant = m00 * c00 + m01 * c10 + m02 * c20;
        if (determinant == 0.0 || !std::isfinite(determinant) || c22 == 0.0)
            return std::nullopt;
        return MotionTransform{c02 / c22, c12 / c22, c00 / c22, c11 / c22,
                               c01 / c22, c10 / c22, c20 / c22, c21 / c22};
    }
    const double determinant =
        transform.scale_x * transform.scale_y - transform.shear_x * transform.shear_y;
    if (determinant == 0.0 || !std::isfinite(determinant))
        return std::nullopt;
    const double a = transform.scale_y / determinant;
    const double b = -transform.shear_x / determinant;
    const double c = -transform.shear_y / determinant;
    const double d = transform.scale_x / determinant;
    return MotionTransform{
        -(a * transform.translate_x + b * transform.translate_y),
        -(c * transform.translate_x + d * transform.translate_y),
        a,
        d,
        b,
        c,
    };
}

MotionTransform concatenate_presentation_transform(
    const MotionTransform& parent,
    const MotionTransform& local
) noexcept {
    if (parent.axis_aligned() && local.axis_aligned()) {
        return MotionTransform{
            parent.scale_x * local.translate_x + parent.translate_x,
            parent.scale_y * local.translate_y + parent.translate_y,
            parent.scale_x * local.scale_x,
            parent.scale_y * local.scale_y,
        };
    }
    if (!parent.affine() || !local.affine()) {
        // The product of two homographies, normalised so its last term is one.
        const std::array<double, 9U> p{parent.scale_x,   parent.shear_x,   parent.translate_x,
                                       parent.shear_y,   parent.scale_y,   parent.translate_y,
                                       parent.project_x, parent.project_y, 1.0};
        const std::array<double, 9U> l{local.scale_x,   local.shear_x,   local.translate_x,
                                       local.shear_y,   local.scale_y,   local.translate_y,
                                       local.project_x, local.project_y, 1.0};
        std::array<double, 9U> h{};
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                for (int k = 0; k < 3; ++k)
                    h[row * 3 + column] += p[row * 3 + k] * l[k * 3 + column];
            }
        }
        const double w = h[8] != 0.0 ? h[8] : 1.0;
        return MotionTransform{h[2] / w, h[5] / w, h[0] / w, h[4] / w,
                               h[1] / w, h[3] / w, h[6] / w, h[7] / w};
    }
    return MotionTransform{
        parent.scale_x * local.translate_x + parent.shear_x * local.translate_y +
            parent.translate_x,
        parent.shear_y * local.translate_x + parent.scale_y * local.translate_y +
            parent.translate_y,
        parent.scale_x * local.scale_x + parent.shear_x * local.shear_y,
        parent.shear_y * local.shear_x + parent.scale_y * local.scale_y,
        parent.scale_x * local.shear_x + parent.shear_x * local.scale_y,
        parent.shear_y * local.scale_x + parent.scale_y * local.shear_y,
    };
}

Rect transform_presentation_bounds(
    const Rect& bounds,
    const MotionTransform& transform
) noexcept {
    if (!transform.axis_aligned()) {
        // The axis-aligned box around the four transformed corners.
        double left = std::numeric_limits<double>::infinity();
        double top = left;
        double right = -left;
        double bottom = -left;
        for (const Point corner : {Point{bounds.x, bounds.y}, Point{bounds.right(), bounds.y},
                                   Point{bounds.x, bounds.bottom()},
                                   Point{bounds.right(), bounds.bottom()}}) {
            const Point mapped = transform_presentation_point(corner, transform);
            left = std::min(left, mapped.x);
            top = std::min(top, mapped.y);
            right = std::max(right, mapped.x);
            bottom = std::max(bottom, mapped.y);
        }
        return Rect{left, top, right - left, bottom - top};
    }
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
    if (!transform.axis_aligned()) {
        const std::optional<MotionTransform> inverse = inverse_presentation_transform(transform);
        return inverse.has_value() ? transform_presentation_bounds(bounds, *inverse) : bounds;
    }
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
    if (!transform.axis_aligned()) {
        const std::optional<MotionTransform> inverse = inverse_presentation_transform(transform);
        return inverse.has_value() ? transform_presentation_point(point, *inverse) : point;
    }
    if (transform.identity() || transform.scale_x == 0.0 || transform.scale_y == 0.0) {
        return point;
    }
    return Point{
        (point.x - transform.translate_x) / transform.scale_x,
        (point.y - transform.translate_y) / transform.scale_y,
    };
}

} // namespace strata::ui
