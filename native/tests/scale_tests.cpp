#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <strata/strata.h>

#include "ui/scale.hpp"

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void near(const double actual, const double expected, const std::string_view message) {
    check(std::abs(actual - expected) <= 1.0e-9, message);
}

void test_portable_policies() {
    using namespace strata::ui;
    const ScaleContext integer = resolve_scale(
        FramebufferSize{2560, 1440}, AutoFitScalePolicy{}
    );
    near(integer.scale, 2.0, "AutoFit did not prefer the exact integer scale");
    near(integer.logical_width, 1280.0, "AutoFit integer logical width is wrong");
    near(integer.logical_height, 720.0, "AutoFit integer logical height is wrong");
    check(integer.integer_scale, "AutoFit integer scale was not classified as integer");

    const ScaleContext representative = resolve_scale(
        FramebufferSize{2576, 1408}, AutoFitScalePolicy{}
    );
    near(representative.scale, 1.75, "AutoFit did not quantize to the largest fitting quarter-step");
    near(representative.logical_width, 2576.0 / 1.75, "AutoFit representative logical width is wrong");
    near(representative.logical_height, 1408.0 / 1.75, "AutoFit representative logical height is wrong");
    check(!representative.integer_scale, "fractional AutoFit scale was classified as integer");

    const ScaleContext minimum = resolve_scale(
        FramebufferSize{854, 480}, AutoFitScalePolicy{}
    );
    near(minimum.scale, 0.5, "AutoFit minimum scale clamp diverged from the core policy");

    near(
        resolve_scale(FramebufferSize{1000, 500}, ManualScalePolicy{9.0, 0.5, 4.0}).scale,
        4.0,
        "Manual scale did not clamp to its maximum"
    );
    const ScaleContext pixels = resolve_scale(
        FramebufferSize{1000, 700}, PixelPerfectScalePolicy{}
    );
    near(pixels.scale, 3.0, "PixelPerfect did not choose the largest fitting integer");
    check(pixels.integer_scale, "PixelPerfect did not report an integer scale");
    near(
        resolve_scale(FramebufferSize{800, 450}, FluidScalePolicy{2.0}).scale,
        1.25,
        "Fluid did not preserve minimum logical space"
    );
}

void test_c_abi_policy_boundary() {
    strata_scale_policy_config policy{sizeof(strata_scale_policy_config)};
    check(
        strata_scale_policy_defaults(STRATA_SCALE_POLICY_AUTO_FIT, &policy).status ==
            STRATA_STATUS_OK,
        "C ABI did not produce AutoFit defaults"
    );
    strata_scale_context context{sizeof(strata_scale_context)};
    check(
        strata_resolve_scale_context(&policy, 2576, 1408, &context).status ==
            STRATA_STATUS_OK,
        "C ABI did not resolve a valid AutoFit context"
    );
    near(context.scale, 1.75, "C ABI AutoFit scale differs from the portable core");
    check(
        context.framebuffer_width == 2576 && context.framebuffer_height == 1408 &&
            context.point_snapping == STRATA_POINT_SNAP_NEAREST &&
            context.rectangle_snapping == STRATA_RECTANGLE_SNAP_OUTWARD,
        "C ABI scale context lost framebuffer or snapping policy"
    );

    policy.kind = STRATA_SCALE_POLICY_FLUID;
    policy.asset_scale = 2.0;
    check(
        strata_resolve_scale_context(&policy, 800, 450, &context).status == STRATA_STATUS_OK,
        "C ABI did not resolve Fluid scale"
    );
    near(context.scale, 1.25, "C ABI Fluid scale differs from the portable core");

    policy.reserved = 1U;
    check(
        strata_resolve_scale_context(&policy, 800, 450, &context).status ==
            STRATA_STATUS_INVALID_ARGUMENT,
        "C ABI accepted a non-zero reserved scale-policy field"
    );
    policy.reserved = 0U;
    check(
        strata_resolve_scale_context(&policy, 0, 450, &context).status ==
            STRATA_STATUS_INVALID_ARGUMENT,
        "C ABI accepted a non-positive framebuffer dimension"
    );
}

} // namespace

int main() {
    try {
        test_portable_policies();
        test_c_abi_policy_boundary();
        std::cout << "strata_scale_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_scale_tests: " << error.what() << '\n';
        return 1;
    }
}
