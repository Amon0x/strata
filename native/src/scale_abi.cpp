#include <strata/strata.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>

#include "core/diagnostics.hpp"
#include "ui/scale.hpp"

namespace {

[[nodiscard]] bool valid_policy_kind(const strata_scale_policy_kind kind) noexcept {
    return kind == STRATA_SCALE_POLICY_AUTO_FIT || kind == STRATA_SCALE_POLICY_MANUAL ||
           kind == STRATA_SCALE_POLICY_PIXEL_PERFECT || kind == STRATA_SCALE_POLICY_FLUID;
}

[[nodiscard]] bool valid_snapping(const strata_scale_policy_config& policy) noexcept {
    return (policy.point_snapping == STRATA_POINT_SNAP_NONE ||
            policy.point_snapping == STRATA_POINT_SNAP_NEAREST) &&
           (policy.rectangle_snapping == STRATA_RECTANGLE_SNAP_NONE ||
            policy.rectangle_snapping == STRATA_RECTANGLE_SNAP_NEAREST ||
            policy.rectangle_snapping == STRATA_RECTANGLE_SNAP_OUTWARD);
}

[[nodiscard]] strata_scale_policy_config defaults(const strata_scale_policy_kind kind) {
    if (!valid_policy_kind(kind)) throw std::invalid_argument("unknown scale policy kind");
    return strata_scale_policy_config{
        sizeof(strata_scale_policy_config),
        kind,
        1U,
        kind == STRATA_SCALE_POLICY_PIXEL_PERFECT ? 320.0 : 1280.0,
        kind == STRATA_SCALE_POLICY_PIXEL_PERFECT ? 180.0 : 720.0,
        kind == STRATA_SCALE_POLICY_PIXEL_PERFECT ? 1.0 : 0.5,
        kind == STRATA_SCALE_POLICY_PIXEL_PERFECT ? 8.0 : 4.0,
        0.25,
        1.0e-6,
        1.0,
        1.0,
        640.0,
        360.0,
        1,
        8,
        STRATA_POINT_SNAP_NEAREST,
        STRATA_RECTANGLE_SNAP_OUTWARD,
        0U,
    };
}

[[nodiscard]] strata::ui::ScaleContext resolve(
    const strata_scale_policy_config& policy,
    const strata::ui::FramebufferSize framebuffer
) {
    switch (policy.kind) {
    case STRATA_SCALE_POLICY_AUTO_FIT:
        return strata::ui::resolve_scale(framebuffer, strata::ui::AutoFitScalePolicy{
            policy.preferred_logical_width,
            policy.preferred_logical_height,
            policy.min_scale,
            policy.max_scale,
            policy.rational_step,
            policy.prefer_integer_scale != 0U,
            policy.integer_preference_tolerance,
        });
    case STRATA_SCALE_POLICY_MANUAL:
        return strata::ui::resolve_scale(framebuffer, strata::ui::ManualScalePolicy{
            policy.manual_multiplier,
            policy.min_scale,
            policy.max_scale,
        });
    case STRATA_SCALE_POLICY_PIXEL_PERFECT:
        return strata::ui::resolve_scale(framebuffer, strata::ui::PixelPerfectScalePolicy{
            policy.preferred_logical_width,
            policy.preferred_logical_height,
            policy.minimum_integer_scale,
            policy.maximum_integer_scale,
        });
    case STRATA_SCALE_POLICY_FLUID:
        return strata::ui::resolve_scale(framebuffer, strata::ui::FluidScalePolicy{
            policy.asset_scale,
            policy.minimum_logical_width,
            policy.minimum_logical_height,
            policy.min_scale,
            policy.max_scale,
        });
    default: throw std::invalid_argument("unknown scale policy kind");
    }
}

} // namespace

extern "C" {

strata_result strata_scale_policy_defaults(
    const strata_scale_policy_kind kind,
    strata_scale_policy_config* const out_policy
) {
    if (out_policy == nullptr || out_policy->struct_size < sizeof(strata_scale_policy_config)) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        *out_policy = defaults(kind);
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::invalid_argument&) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    } catch (const std::bad_alloc&) {
        return strata::core::result(STRATA_STATUS_OUT_OF_MEMORY);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INTERNAL_ERROR);
    }
}

strata_result strata_resolve_scale_context(
    const strata_scale_policy_config* const policy,
    const std::int64_t framebuffer_width,
    const std::int64_t framebuffer_height,
    strata_scale_context* const out_context
) {
    if (policy == nullptr || out_context == nullptr ||
        policy->struct_size < sizeof(strata_scale_policy_config) ||
        out_context->struct_size < sizeof(strata_scale_context) ||
        policy->reserved != 0U || policy->prefer_integer_scale > 1U ||
        !valid_policy_kind(policy->kind) || !valid_snapping(*policy)) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        const strata::ui::ScaleContext resolved = resolve(
            *policy,
            strata::ui::FramebufferSize{framebuffer_width, framebuffer_height}
        );
        *out_context = strata_scale_context{
            sizeof(strata_scale_context),
            resolved.framebuffer.width,
            resolved.framebuffer.height,
            resolved.logical_width,
            resolved.logical_height,
            resolved.scale,
            resolved.integer_scale ? 1U : 0U,
            policy->point_snapping,
            policy->rectangle_snapping,
            0U,
        };
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::invalid_argument&) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    } catch (const std::bad_alloc&) {
        return strata::core::result(STRATA_STATUS_OUT_OF_MEMORY);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INTERNAL_ERROR);
    }
}

} // extern "C"
