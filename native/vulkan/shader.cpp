#include "shader.hpp"
#include <shaderc/shaderc.hpp>
#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <string>
namespace strata::vulkan::detail {
std::vector<std::uint32_t> compile_hlsl(std::string_view source, bool vertex,
                                        std::string_view name) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetSourceLanguage(shaderc_source_language_hlsl);
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
    options.SetHlslIoMapping(true);
    options.SetHlslOffsets(true);
    options.SetAutoMapLocations(true);
    options.SetBindingBase(shaderc_uniform_kind_buffer, 0);
    // b0 frame, b1 rounded clips, b2 presentation groups; t0/t1; s0.
    options.SetBindingBase(shaderc_uniform_kind_texture, 3);
    options.SetBindingBase(shaderc_uniform_kind_sampler, 5);
    options.SetInvertY(vertex);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    const auto result = compiler.CompileGlslToSpv(
        source.data(), source.size(), vertex ? shaderc_vertex_shader : shaderc_fragment_shader,
        std::string(name).c_str(), "main", options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success)
        throw std::runtime_error("Vulkan HLSL compilation failed: " + result.GetErrorMessage());
    return {result.cbegin(), result.cend()};
}
void route_second_output_to_dual_source(std::vector<std::uint32_t>& spirv) {
    constexpr std::uint32_t op_decorate = 71U;
    constexpr std::uint32_t op_variable = 59U;
    constexpr std::uint32_t storage_output = 3U;
    constexpr std::uint32_t decoration_location = 30U;
    constexpr std::uint32_t decoration_index = 32U;
    constexpr std::size_t header_words = 5U;
    if (spirv.size() < header_words)
        throw std::runtime_error("SPIR-V module is truncated");
    // The output variables, then the Location 1 decoration among them.
    std::vector<std::uint32_t> outputs;
    for (std::size_t at = header_words; at < spirv.size();) {
        const std::uint32_t count = spirv[at] >> 16U;
        if (count == 0U || at + count > spirv.size())
            throw std::runtime_error("SPIR-V module is malformed");
        if ((spirv[at] & 0xFFFFU) == op_variable && count >= 4U && spirv[at + 3U] == storage_output)
            outputs.push_back(spirv[at + 2U]);
        at += count;
    }
    for (std::size_t at = header_words; at < spirv.size();) {
        const std::uint32_t count = spirv[at] >> 16U;
        if ((spirv[at] & 0xFFFFU) == op_decorate && count == 4U &&
            spirv[at + 2U] == decoration_location && spirv[at + 3U] == 1U &&
            std::ranges::find(outputs, spirv[at + 1U]) != outputs.end()) {
            const std::uint32_t target = spirv[at + 1U];
            spirv[at + 3U] = 0U;
            const std::uint32_t index[]{(4U << 16U) | op_decorate, target, decoration_index, 1U};
            spirv.insert(spirv.begin() + static_cast<std::ptrdiff_t>(at + count),
                         std::begin(index), std::end(index));
            return;
        }
        at += count;
    }
    throw std::runtime_error("fragment shader has no second output to route to dual-source");
}

std::vector<std::uint32_t> compile_hlsl_compute(std::string_view source, std::string_view entry,
                                                std::string_view name, bool optimize) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetSourceLanguage(shaderc_source_language_hlsl);
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
    options.SetHlslIoMapping(true);
    options.SetHlslOffsets(true);
    options.AddMacroDefinition("STRATA_VULKAN");
    options.SetOptimizationLevel(optimize ? shaderc_optimization_level_performance
                                          : shaderc_optimization_level_zero);
    const auto result = compiler.CompileGlslToSpv(source.data(), source.size(),
                                                  shaderc_compute_shader, std::string(name).c_str(),
                                                  std::string(entry).c_str(), options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success)
        throw std::runtime_error("Vulkan HLSL compilation failed: " + result.GetErrorMessage());
    return {result.cbegin(), result.cend()};
}
} // namespace strata::vulkan::detail
