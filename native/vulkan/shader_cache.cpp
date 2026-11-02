#include "shader_cache.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>

#include <shaderc/shaderc.h>
#include <spirv-tools/libspirv.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <strata/vulkan.hpp>

namespace strata::vulkan {
namespace {

/** Compilers copy configuration under the lock; admission survives detached work at process exit. */
struct CacheSettings final {
    std::mutex mutex;
    std::filesystem::path directory;
};

[[nodiscard]] CacheSettings& cache_settings() {
    static CacheSettings& settings = *new CacheSettings;
    return settings;
}

std::atomic<std::uint64_t> partial_serial{0U};
constexpr std::uint64_t hash_seed = 14695981039346656037ULL;
constexpr std::size_t maximum_bytes = 16U * 1024U * 1024U;
// Bump when shader.cpp's fixed compilation options change, not when shader source changes.
constexpr std::uint64_t options_revision = 1U;
constexpr std::uint64_t signature = 0x3156435053525453ULL;

void mix(std::uint64_t& hash, const std::string_view bytes) noexcept {
    for (const char byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
}

std::string_view bytes(const std::span<const std::uint32_t> words) noexcept {
    return {reinterpret_cast<const char*>(words.data()), words.size_bytes()};
}

/** Hashes the loaded compiler module once, including a statically linked compiler's executable. */
std::optional<std::uint64_t> compiler_identity() {
    static const std::optional<std::uint64_t> identity = []() -> std::optional<std::uint64_t> {
        std::filesystem::path path;
#ifdef _WIN32
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&shaderc_compiler_initialize), &module))
            return std::nullopt;
        std::array<wchar_t, 32768U> name;
        const DWORD length = GetModuleFileNameW(module, name.data(), static_cast<DWORD>(name.size()));
        if (length == 0U || length >= name.size())
            return std::nullopt;
        path = std::wstring_view(name.data(), length);
#else
        Dl_info module{};
        if (dladdr(reinterpret_cast<const void*>(&shaderc_compiler_initialize), &module) == 0 ||
            module.dli_fname == nullptr)
            return std::nullopt;
        path = module.dli_fname;
#endif
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return std::nullopt;
        std::uint64_t hash = hash_seed;
        std::array<char, 65536U> buffer;
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            mix(hash, {buffer.data(), static_cast<std::size_t>(input.gcount())});
        }
        return input.eof() ? std::optional(hash) : std::nullopt;
    }();
    return identity;
}

bool matches(std::ifstream& input, std::string_view expected) {
    std::array<char, 4096U> buffer;
    while (!expected.empty()) {
        const std::size_t count = std::min(buffer.size(), expected.size());
        if (!input.read(buffer.data(), static_cast<std::streamsize>(count)) ||
            std::memcmp(buffer.data(), expected.data(), count) != 0)
            return false;
        expected.remove_prefix(count);
    }
    return true;
}

} // namespace

void set_shader_cache_directory(std::filesystem::path directory) {
    auto& settings = cache_settings();
    const std::lock_guard lock(settings.mutex);
    settings.directory = std::move(directory);
}

namespace detail {

ShaderCacheEntry::ShaderCacheEntry(const std::string_view source, const std::string_view entry,
                                   const std::string_view name, const ShaderStage stage,
                                   const bool optimized)
    : key_{source, entry, name} {
    std::filesystem::path directory;
    {
        auto& settings = cache_settings();
        const std::lock_guard lock(settings.mutex);
        directory = settings.directory;
    }
    if (directory.empty())
        return;
    std::size_t remaining = maximum_bytes - sizeof(header_);
    for (const std::string_view part : key_) {
        if (part.size() > remaining)
            return;
        remaining -= part.size();
    }
    const std::optional<std::uint64_t> compiler = compiler_identity();
    if (!compiler.has_value())
        return;
    std::uint64_t validator = hash_seed;
    mix(validator, spvSoftwareVersionDetailsString());
    header_ = {signature, options_revision, *compiler, validator,
               static_cast<std::uint64_t>(stage), optimized ? 1U : 0U,
               source.size(), entry.size(), name.size(), 0U, 0U};
    std::uint64_t hash = hash_seed;
    mix(hash, {reinterpret_cast<const char*>(header_.data()), 9U * sizeof(header_[0])});
    for (const std::string_view part : key_)
        mix(hash, part);
    char filename[32];
    std::snprintf(filename, sizeof(filename), "%016llx.spv", static_cast<unsigned long long>(hash));
    path_ = directory / filename;
}

std::optional<std::vector<std::uint32_t>> ShaderCacheEntry::load() const {
    if (path_.empty())
        return std::nullopt;
    std::ifstream input(path_, std::ios::binary | std::ios::ate);
    if (!input)
        return std::nullopt;
    const std::streamoff length = input.tellg();
    if (length < static_cast<std::streamoff>(sizeof(header_)) ||
        length > static_cast<std::streamoff>(maximum_bytes))
        return std::nullopt;
    input.seekg(0);
    std::array<std::uint64_t, 11U> header{};
    if (!input.read(reinterpret_cast<char*>(header.data()), sizeof(header)) ||
        !std::equal(header.begin(), header.begin() + 9, header_.begin()) ||
        header[9] < 5U || header[9] > maximum_bytes / sizeof(std::uint32_t))
        return std::nullopt;
    const std::size_t words = static_cast<std::size_t>(header[9]);
    std::size_t expected_length = sizeof(header) + words * sizeof(std::uint32_t);
    for (const std::string_view part : key_)
        expected_length += part.size();
    if (expected_length != static_cast<std::size_t>(length))
        return std::nullopt;
    // The filename is only an index: exact source/name comparisons also cover hash collisions.
    for (const std::string_view part : key_) {
        if (!matches(input, part))
            return std::nullopt;
    }
    std::vector<std::uint32_t> result(words);
    if (!input.read(reinterpret_cast<char*>(result.data()),
                    static_cast<std::streamsize>(words * sizeof(std::uint32_t))))
        return std::nullopt;
    std::uint64_t checksum = hash_seed;
    mix(checksum, bytes(result));
    if (checksum != header[10] || result[0] != 0x07230203U || result[3] == 0U ||
        result[3] > std::min<std::size_t>(1U << 20U, words * 4U))
        return std::nullopt;
    struct Context final {
        spv_context value = spvContextCreate(SPV_ENV_VULKAN_1_1);
        ~Context() { spvContextDestroy(value); }
    } context;
    if (context.value == nullptr)
        return std::nullopt;
    spv_diagnostic diagnostic = nullptr;
    const spv_result_t valid = spvValidateBinary(context.value, result.data(), result.size(), &diagnostic);
    spvDiagnosticDestroy(diagnostic);
    if (valid != SPV_SUCCESS)
        return std::nullopt;
    const std::uint32_t model = header_[4] == static_cast<std::uint64_t>(ShaderStage::vertex) ? 0U :
        header_[4] == static_cast<std::uint64_t>(ShaderStage::fragment) ? 4U : 5U;
    for (std::size_t at = 5U; at < result.size(); at += result[at] >> 16U) {
        const std::uint32_t count = result[at] >> 16U;
        if ((result[at] & 0xffffU) != 15U || count < 4U || result[at + 1U] != model)
            continue;
        const char* entry = reinterpret_cast<const char*>(result.data() + at + 3U);
        const auto* end = static_cast<const char*>(std::memchr(entry, '\0', (count - 3U) * 4U));
        if (end != nullptr && std::string_view(entry, static_cast<std::size_t>(end - entry)) == key_[1])
            return result;
    }
    return std::nullopt;
}

void ShaderCacheEntry::store(const std::span<const std::uint32_t> words) const noexcept {
    if (path_.empty() || words.size() < 5U || words.size_bytes() > maximum_bytes)
        return;
    std::filesystem::path partial;
    try {
        std::size_t size = sizeof(header_) + words.size_bytes();
        for (const std::string_view part : key_)
            size += part.size();
        if (size > maximum_bytes)
            return;
        std::error_code error;
        std::filesystem::create_directories(path_.parent_path(), error);
        if (error)
            return;
#ifdef _WIN32
        const auto process = GetCurrentProcessId();
#else
        const auto process = getpid();
#endif
        partial = path_;
        partial += "." + std::to_string(process) + "." +
                   std::to_string(partial_serial.fetch_add(1U, std::memory_order_relaxed)) + ".partial";
        auto header = header_;
        header[9] = words.size();
        header[10] = hash_seed;
        mix(header[10], bytes(words));
        std::ofstream output(partial, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(header.data()), sizeof(header));
        for (const std::string_view part : key_)
            output.write(part.data(), static_cast<std::streamsize>(part.size()));
        output.write(reinterpret_cast<const char*>(words.data()),
                     static_cast<std::streamsize>(words.size_bytes()));
        output.close();
        if (output) {
#ifdef _WIN32
            if (MoveFileExW(partial.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING))
                return;
#else
            std::filesystem::rename(partial, path_, error);
            if (!error)
                return;
#endif
        }
    } catch (...) {
        // Persistence is optional; the caller still owns successfully compiled shader code.
    }
    if (!partial.empty()) {
        std::error_code error;
        std::filesystem::remove(partial, error);
    }
}

} // namespace detail
} // namespace strata::vulkan
