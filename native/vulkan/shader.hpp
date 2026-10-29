#pragma once
#include <cstdint>
#include <string_view>
#include <vector>
namespace strata::vulkan::detail {
std::vector<std::uint32_t> compile_hlsl(std::string_view source, bool vertex,
                                        std::string_view name);
/** A compute kernel: `entry` of `source`, compiled with STRATA_VULKAN defined. */
std::vector<std::uint32_t> compile_hlsl_compute(std::string_view source, std::string_view entry,
                                                std::string_view name);
}
