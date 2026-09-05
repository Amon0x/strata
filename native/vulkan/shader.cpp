#include "shader.hpp"
#include <shaderc/shaderc.hpp>
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
    options.SetBindingBase(shaderc_uniform_kind_texture, 2);
    options.SetBindingBase(shaderc_uniform_kind_sampler, 4);
    options.SetInvertY(vertex);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    const auto result = compiler.CompileGlslToSpv(
        source.data(), source.size(), vertex ? shaderc_vertex_shader : shaderc_fragment_shader,
        std::string(name).c_str(), "main", options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success)
        throw std::runtime_error("Vulkan HLSL compilation failed: " + result.GetErrorMessage());
    return {result.cbegin(), result.cend()};
}
} // namespace strata::vulkan::detail
