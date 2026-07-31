#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <string_view>
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
enum class GradientKind : std::uint32_t { linear = 0U, radial = 1U };

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
    /** Radial centre and its per-axis radii, both normalized to the filled bounds. */
    Point center{0.5, 0.5};
    double radius_x = 0.5;
    double radius_y = 0.5;
    /** Ascending, in [0, 1]. At least two stops. */
    std::vector<GradientStop> stops;

    /** Throws std::invalid_argument describing the first violated rule. */
    void validate() const;
    [[nodiscard]] runtime::ColorValue sample(double offset) const noexcept;
    /** The linear axis for a shape of this size, resolving an authored angle when present. */
    [[nodiscard]] std::pair<Point, Point> axis(Size shape) const noexcept;
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
    [[nodiscard]] Paint with_alpha_multiplied(double opacity) const;

    [[nodiscard]] friend bool operator==(const Paint&, const Paint&) = default;

private:
    std::variant<runtime::ColorValue, Gradient> value_{runtime::ColorValue{0U, 0U, 0U, 0U}};
};

/**
 * Reads an authored paint value: a colour, or an object of the form
 * `{ kind: "linear" | "radial", angle | from/to | center/radius, extend, stops: [...] }`.
 * Returns nothing when the value is not a paint; throws when it is a malformed one.
 */
[[nodiscard]] std::optional<Paint> paint_from_value(const runtime::Value* value);

/**
 * The gradient's own parameter at a point in the filled shape's normalized space: the projection
 * onto a linear axis, or the elliptical distance from a radial centre.
 */
[[nodiscard]] double gradient_parameter(const Gradient& gradient, Point normalized, Size shape) noexcept;

/** The authored spelling of an extend mode. */
[[nodiscard]] std::string_view gradient_extend_name(GradientExtend extend) noexcept;

} // namespace strata::ui
