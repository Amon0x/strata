#include "shader_cache.hpp"

#include <strata/d3d11.hpp>

#include <d3dcompiler.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace strata::d3d11 {
namespace {

using Microsoft::WRL::ComPtr;

/** set_shader_cache_directory's directory, read by every thread that compiles. */
std::mutex cache_mutex;
std::filesystem::path cache_directory;
/** Tells apart the partial files of threads writing the same entry at once. */
std::atomic<std::uint64_t> partial_serial{0U};

/**
 * The file an entry is kept in: named by a hash of everything its bytecode depends on (the whole
 * source, entry, target, flags and compiler), so a changed shader or compiler never reads a stale
 * one. Empty when no cache is set.
 */
[[nodiscard]] std::filesystem::path cache_file(const std::string_view source,
                                               const char* const entry, const char* const target,
                                               const unsigned int flags) {
    std::filesystem::path directory;
    {
        const std::lock_guard lock(cache_mutex);
        directory = cache_directory;
    }
    if (directory.empty())
        return {};
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](const std::string_view part) {
        for (const char byte : part) {
            hash ^= static_cast<unsigned char>(byte);
            hash *= 1099511628211ULL;
        }
        hash ^= 0xffU;
        hash *= 1099511628211ULL;
    };
    mix(source);
    mix(entry);
    mix(target);
    mix(std::to_string(flags));
    mix("d3dcompiler-" + std::to_string(D3D_COMPILER_VERSION));
    char name[40];
    std::snprintf(name, sizeof(name), "%016llx.dxbc", static_cast<unsigned long long>(hash));
    return directory / name;
}

[[nodiscard]] ComPtr<ID3DBlob> read(const std::filesystem::path& file) {
    if (file.empty())
        return nullptr;
    std::ifstream input(file, std::ios::binary | std::ios::ate);
    if (!input)
        return nullptr;
    const std::streamsize size = input.tellg();
    if (size <= 0)
        return nullptr;
    ComPtr<ID3DBlob> blob;
    if (FAILED(D3DCreateBlob(static_cast<SIZE_T>(size), &blob)))
        return nullptr;
    input.seekg(0);
    if (!input.read(static_cast<char*>(blob->GetBufferPointer()), size))
        return nullptr;
    return blob;
}

/** Keeps compiled bytecode for later runs; a cache that cannot be written is skipped. */
void keep(const std::filesystem::path& file, ID3DBlob* const code) noexcept {
    if (file.empty() || code == nullptr)
        return;
    try {
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        // Written aside and moved into place, so a reader never sees a partial entry.
        const std::filesystem::path partial =
            file.string() + "." + std::to_string(partial_serial.fetch_add(1U)) + ".partial";
        {
            std::ofstream output(partial, std::ios::binary | std::ios::trunc);
            output.write(static_cast<const char*>(code->GetBufferPointer()),
                         static_cast<std::streamsize>(code->GetBufferSize()));
            if (!output)
                return;
        }
        std::filesystem::rename(partial, file, error);
        if (error)
            std::filesystem::remove(partial, error);
    } catch (...) {
    }
}

} // namespace

ComPtr<ID3DBlob> cached_hlsl(const std::string_view source, const char* const entry,
                             const char* const target, const unsigned int flags) {
    return read(cache_file(source, entry, target, flags));
}

ComPtr<ID3DBlob> compile_hlsl(const std::string_view source, const char* const name,
                              const char* const entry, const char* const target,
                              const unsigned int flags, const std::string_view what) {
    const std::filesystem::path file = cache_file(source, entry, target, flags);
    if (ComPtr<ID3DBlob> cached = read(file); cached != nullptr)
        return cached;
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> diagnostics;
    const HRESULT status = D3DCompile(source.data(), source.size(), name, nullptr, nullptr, entry,
                                      target, flags, 0U, &code, &diagnostics);
    if (FAILED(status)) {
        const std::string detail =
            diagnostics == nullptr
                ? std::string("unknown compiler error")
                : std::string(static_cast<const char*>(diagnostics->GetBufferPointer()),
                              diagnostics->GetBufferSize());
        throw std::runtime_error(std::string(what) + ": " + detail);
    }
    keep(file, code.Get());
    return code;
}

void set_shader_cache_directory(std::filesystem::path directory) {
    const std::lock_guard lock(cache_mutex);
    cache_directory = std::move(directory);
}

} // namespace strata::d3d11
