#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "runtime/value.hpp"
#include "ui/layout.hpp"

namespace strata::ui {

/**
 * Gradient geometry is authored in the filled shape's normalized space: (0,0) is its top-left
 * corner and (1,1) its bottom-right one. A paint therefore survives layout changes, motion and
 * fragment reuse without being re-authored per size.
 */
enum class GradientKind : std::uint32_t { linear = 0U, radial = 1U, conic = 2U };

/** Behaviour outside the authored stop range. */
enum class GradientExtend : std::uint32_t { clamp = 0U, repeat = 1U, mirror = 2U };

struct GradientStop final {
    double offset = 0.0;
    runtime::ColorValue color{0U, 0U, 0U, 0U};
    [[nodiscard]] friend bool operator==(const GradientStop&, const GradientStop&) = default;
};

/** The largest authored stop count. Bounded so tessellation cost stays predictable. */
inline constexpr std::size_t maximum_gradient_stops = 16U;

struct Gradient final {
    GradientKind kind = GradientKind::linear;
    GradientExtend extend = GradientExtend::clamp;
    /**
     * Linear axis, running from `start` (offset 0) to `end` (offset 1). An authored angle wins
     * over the axis and is resolved against the filled shape's aspect ratio, so the painted angle
     * is the visual one rather than one skewed by a non-square shape.
     */
    Point start{0.5, 0.0};
    Point end{0.5, 1.0};
    std::optional<double> angle_degrees;
    /**
     * Radial and conic centre, and the radial per-axis radii, all normalized to the filled bounds.
     * A conic gradient sweeps its stops once around the centre, clockwise, starting from
     * `angle_degrees` (0 points up, as for a linear angle).
     */
    Point center{0.5, 0.5};
    double radius_x = 0.5;
    double radius_y = 0.5;
    /**
     * Turns the whole paint clockwise by this many degrees about the filled shape's centre, in the
     * shape's own pixel space (motion's `paintRotate`). A linear angle and a conic start advance by
     * it; an explicit linear axis, a radial ellipse and an off-centre conic centre turn with it.
     */
    double rotation_degrees = 0.0;
    /** Ascending, in [0, 1]. At least two stops. */
    std::vector<GradientStop> stops;

    /** Throws std::invalid_argument describing the first violated rule. */
    void validate() const;
    [[nodiscard]] runtime::ColorValue sample(double offset) const noexcept;
    /**
     * The linear axis for a shape of this size, resolving an authored angle when present and
     * turning it by the paint's rotation.
     */
    [[nodiscard]] std::pair<Point, Point> axis(Size shape) const noexcept;
    /**
     * The affine map from the filled shape's normalized space to the space the gradient parameter
     * is measured in, as (m[0] x + m[1] y + m[2], m[3] x + m[4] y + m[5]):
     *
     * - linear: the first row is the parameter itself (0 at the axis start, 1 at its end);
     * - radial: the ellipse's unit space, where the parameter is the length of the point;
     * - conic: logical pixels about the centre, turned so the start direction points up (-y), where
     *   the parameter is the clockwise turn from up, `atan2(x, -y) / 2pi` wrapped into [0, 1).
     *
     * Every backend evaluates gradients through this map, so they agree on every kind and rotation.
     */
    [[nodiscard]] std::array<double, 6U> frame(Size shape) const noexcept;
    [[nodiscard]] friend bool operator==(const Gradient&, const Gradient&) = default;
};

/**
 * What fills a shape: one colour or one gradient. Every fill in the render vocabulary carries a
 * Paint, so a gradient is available anywhere a colour is without a parallel command family.
 */
class Paint final {
public:
    Paint() noexcept = default;
    Paint(runtime::ColorValue color) noexcept;
    explicit Paint(Gradient gradient);

    [[nodiscard]] bool is_gradient() const noexcept;
    /** The solid colour, or null when this paint is a gradient. */
    [[nodiscard]] const runtime::ColorValue* color() const noexcept;
    [[nodiscard]] const Gradient* gradient() const noexcept;
    /**
     * One representative colour: the solid colour, or the gradient's midpoint. Used where a single
     * colour is structurally required (host contracts, overlays derived from a fill).
     */
    [[nodiscard]] runtime::ColorValue representative() const noexcept;
    /** True when nothing this paint can produce is visible. */
    [[nodiscard]] bool transparent() const noexcept;
    void multiply_alpha(double opacity) noexcept;

    [[nodiscard]] friend bool operator==(const Paint&, const Paint&) = default;

private:
    std::variant<runtime::ColorValue, Gradient> value_{runtime::ColorValue{0U, 0U, 0U, 0U}};
};

/** A paint turned clockwise by `degrees` about its shape's centre (Gradient::rotation_degrees). */
[[nodiscard]] Paint rotated_paint(Paint paint, double degrees);

/**
 * Reads an authored paint value: a colour, or an object of the form
 * `{ kind: "linear" | "radial" | "conic", angle | from/to | center/radius, extend, stops: [...] }`.
 * Returns nothing when the value is not a paint; throws when it is a malformed one.
 */
[[nodiscard]] std::optional<Paint> paint_from_value(const runtime::Value* value);

/**
 * The gradient's own parameter at a point in the filled shape's normalized space: the projection
 * onto a linear axis, the elliptical distance from a radial centre, or the clockwise turn about a
 * conic centre, before the extend mode applies.
 */
[[nodiscard]] double gradient_parameter(const Gradient& gradient, Point normalized, Size shape) noexcept;

/**
 * A conic gradient's parameter at a point of its frame (Gradient::frame): the clockwise turn from
 * up, in [0, 1). Shared by every CPU path; the GPU stages compute the same expression.
 */
[[nodiscard]] double conic_parameter(double x, double y) noexcept;

/** The authored spelling of a gradient kind. */
[[nodiscard]] std::string_view gradient_kind_name(GradientKind kind) noexcept;

/** The authored spelling of an extend mode. */
[[nodiscard]] std::string_view gradient_extend_name(GradientExtend extend) noexcept;

} // namespace strata::ui
