#pragma once
#include <cstdint>
#include <string_view>
#include <vector>
namespace strata::vulkan::detail {
/**
 * Makes a fragment shader's output at location 1 the second source of location 0 (index 1), as
 * HLSL's [[vk::index(1)]] would: glslang's HLSL front end ignores that attribute. The pixel stage
 * writes per-channel alpha there for dual-source blending (gpu::shaders::pixel_output).
 */
void route_second_output_to_dual_source(std::vector<std::uint32_t>& spirv);
std::vector<std::uint32_t> compile_hlsl(std::string_view source, bool vertex,
                                        std::string_view name);
/**
 * A compute kernel: `entry` of `source`, compiled with STRATA_VULKAN defined. `optimize` false
 * skips the SPIR-V optimizer, for kernels compiled while something waits on them.
 */
std::vector<std::uint32_t> compile_hlsl_compute(std::string_view source, std::string_view entry,
                                                std::string_view name, bool optimize = true);
}
