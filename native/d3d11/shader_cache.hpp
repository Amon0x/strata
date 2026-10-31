#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3dcommon.h>
#include <wrl/client.h>

#include <string_view>

namespace strata::d3d11 {

/**
 * HLSL compiled to bytecode, read from the shader cache (set_shader_cache_directory) when the same
 * source compiled before with the same entry, target and flags, and compiled and kept there
 * otherwise. Compiling the backend's shaders takes most of a second; a later run reads them in a
 * moment. Throws, with the compiler's diagnostics after `what`, when the source does not compile.
 */
[[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob>
compile_hlsl(std::string_view source, const char* name, const char* entry, const char* target,
             unsigned int flags, std::string_view what);

/** The cached bytecode compile_hlsl would read for these, if the cache holds it; null otherwise. */
[[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob>
cached_hlsl(std::string_view source, const char* entry, const char* target, unsigned int flags);

} // namespace strata::d3d11
