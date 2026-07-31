#include "ui/scale.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace strata::ui {
namespace {

constexpr double floating_point_tolerance = 1.0e-9;

void validate_framebuffer(const FramebufferSize framebuffer) {
    if (framebuffer.width <= 0 || framebuffer.height <= 0) {
        throw std::invalid_argument("scale policy framebuffer dimensions must be positive");
    }
}

void validate_positive(const double value, const char* const name) {
    if (!std::isfinite(value) || value <= 0.0) {
        throw std::invalid_argument(std::string("scale policy ") + name + " must be finite and positive");
    }
}

void validate_bounds(const double minimum, const double maximum) {
    validate_positive(minimum, "minimum scale");
    validate_positive(maximum, "maximum scale");
    if (maximum < minimum) {
        throw std::invalid_argument("scale policy maximum scale must not be below its minimum");
    }
}

[[nodiscard]] bool is_integer_scale(const double scale) noexcept {
    return std::abs(scale - std::round(scale)) <= floating_point_tolerance;
}

[[nodiscard]] ScaleContext context(
    const FramebufferSize framebuffer,
    const double scale,
    const bool integer_scale
) {
    return ScaleContext{
        framebuffer,
        static_cast<double>(framebuffer.width) / scale,
        static_cast<double>(framebuffer.height) / scale,
        scale,
        integer_scale,
    };
}

[[nodiscard]] double maximum_fit(
    const FramebufferSize framebuffer,
    const double logical_width,
    const double logical_height
) noexcept {
    return std::min(
        static_cast<double>(framebuffer.width) / logical_width,
        static_cast<double>(framebuffer.height) / logical_height
    );
}

} // namespace

ScaleContext resolve_scale(
    const FramebufferSize framebuffer,
    const AutoFitScalePolicy& policy
) {
    validate_framebuffer(framebuffer);
    validate_positive(policy.preferred_logical_width, "preferred logical width");
    validate_positive(policy.preferred_logical_height, "preferred logical height");
    validate_bounds(policy.min_scale, policy.max_scale);
    validate_positive(policy.rational_step, "rational step");
    if (!std::isfinite(policy.integer_preference_tolerance) ||
        policy.integer_preference_tolerance < 0.0) {
        throw std::invalid_argument(
            "scale policy integer preference tolerance must be finite and non-negative"
        );
    }
    double fit = std::min(
        maximum_fit(
            framebuffer,
            policy.preferred_logical_width,
            policy.preferred_logical_height
        ),
        policy.max_scale
    );
    if (policy.prefer_integer_scale) {
        const double nearest = std::round(fit);
        if (nearest > 0.0 &&
            std::abs(fit - nearest) <= policy.integer_preference_tolerance) {
            fit = nearest;
        }
    }
    const double quantized = std::floor(
        (fit + floating_point_tolerance) / policy.rational_step
    ) * policy.rational_step;
    const double rational = quantized > 0.0 ? std::min(quantized, fit) : fit;
    const double scale = std::clamp(rational, policy.min_scale, policy.max_scale);
    return context(framebuffer, scale, is_integer_scale(scale));
}

ScaleContext resolve_scale(
    const FramebufferSize framebuffer,
    const ManualScalePolicy& policy
) {
    validate_framebuffer(framebuffer);
    validate_positive(policy.multiplier, "manual multiplier");
    validate_bounds(policy.min_scale, policy.max_scale);
    const double scale = std::clamp(policy.multiplier, policy.min_scale, policy.max_scale);
    return context(framebuffer, scale, is_integer_scale(scale));
}

ScaleContext resolve_scale(
    const FramebufferSize framebuffer,
    const PixelPerfectScalePolicy& policy
) {
    validate_framebuffer(framebuffer);
    validate_positive(policy.preferred_logical_width, "preferred logical width");
    validate_positive(policy.preferred_logical_height, "preferred logical height");
    if (policy.min_scale <= 0 || policy.max_scale < policy.min_scale) {
        throw std::invalid_argument("pixel-perfect scale bounds must be positive and ordered");
    }
    const double fit = std::floor(maximum_fit(
        framebuffer,
        policy.preferred_logical_width,
        policy.preferred_logical_height
    ));
    const auto scale = static_cast<double>(std::clamp(
        fit,
        static_cast<double>(policy.min_scale),
        static_cast<double>(policy.max_scale)
    ));
    return context(framebuffer, scale, true);
}

ScaleContext resolve_scale(
    const FramebufferSize framebuffer,
    const FluidScalePolicy& policy
) {
    validate_framebuffer(framebuffer);
    validate_positive(policy.asset_scale, "asset scale");
    validate_positive(policy.min_logical_width, "minimum logical width");
    validate_positive(policy.min_logical_height, "minimum logical height");
    validate_bounds(policy.min_scale, policy.max_scale);
    const double desired = std::clamp(policy.asset_scale, policy.min_scale, policy.max_scale);
    const double fit = maximum_fit(
        framebuffer,
        policy.min_logical_width,
        policy.min_logical_height
    );
    const double scale = std::min(
        fit >= policy.min_scale ? std::min(desired, fit) : policy.min_scale,
        policy.max_scale
    );
    return context(framebuffer, scale, is_integer_scale(scale));
}

} // namespace strata::ui
