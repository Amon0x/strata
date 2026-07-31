#pragma once

#include <cstdint>

namespace strata::ui {

struct FramebufferSize final {
    std::int64_t width = 0;
    std::int64_t height = 0;
};

struct ScaleContext final {
    FramebufferSize framebuffer;
    double logical_width = 0.0;
    double logical_height = 0.0;
    double scale = 1.0;
    bool integer_scale = true;
};

struct AutoFitScalePolicy final {
    double preferred_logical_width = 1280.0;
    double preferred_logical_height = 720.0;
    double min_scale = 0.5;
    double max_scale = 4.0;
    double rational_step = 0.25;
    bool prefer_integer_scale = true;
    double integer_preference_tolerance = 1.0e-6;
};

struct ManualScalePolicy final {
    double multiplier = 1.0;
    double min_scale = 0.5;
    double max_scale = 4.0;
};

struct PixelPerfectScalePolicy final {
    double preferred_logical_width = 320.0;
    double preferred_logical_height = 180.0;
    std::int64_t min_scale = 1;
    std::int64_t max_scale = 8;
};

struct FluidScalePolicy final {
    double asset_scale = 1.0;
    double min_logical_width = 640.0;
    double min_logical_height = 360.0;
    double min_scale = 0.5;
    double max_scale = 4.0;
};

/** Quality-preserving policies produce one uniform logical/framebuffer mapping; never stretch. */
[[nodiscard]] ScaleContext resolve_scale(
    FramebufferSize framebuffer,
    const AutoFitScalePolicy& policy
);
[[nodiscard]] ScaleContext resolve_scale(
    FramebufferSize framebuffer,
    const ManualScalePolicy& policy
);
[[nodiscard]] ScaleContext resolve_scale(
    FramebufferSize framebuffer,
    const PixelPerfectScalePolicy& policy
);
[[nodiscard]] ScaleContext resolve_scale(
    FramebufferSize framebuffer,
    const FluidScalePolicy& policy
);

} // namespace strata::ui
