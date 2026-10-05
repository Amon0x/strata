#include "host/system_fonts.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace strata::host {
namespace {

[[nodiscard]] std::string lowered(const std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](const unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
    });
    return result;
}

#ifdef _WIN32
[[nodiscard]] std::filesystem::path environment_path(const wchar_t* const name) {
    std::wstring value(32'768U, L'\0');
    const DWORD size = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (size == 0U || size >= value.size())
        return {};
    value.resize(size);
    return std::filesystem::path(value);
}
#endif

/** Where the platform keeps installed fonts, most specific first. Missing entries are harmless. */
[[nodiscard]] std::vector<std::filesystem::path> font_directories() {
    std::vector<std::filesystem::path> result;
#ifdef _WIN32
    if (const std::filesystem::path local = environment_path(L"LOCALAPPDATA"); !local.empty())
        result.push_back(local / "Microsoft" / "Windows" / "Fonts");
    if (const std::filesystem::path windows = environment_path(L"WINDIR"); !windows.empty())
        result.push_back(windows / "Fonts");
#else
    if (const char* const home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        const std::filesystem::path root(home);
#ifdef __APPLE__
        result.push_back(root / "Library" / "Fonts");
#else
        result.push_back(root / ".local" / "share" / "fonts");
        result.push_back(root / ".fonts");
#endif
    }
#ifdef __APPLE__
    result.emplace_back("/Library/Fonts");
    result.emplace_back("/System/Library/Fonts");
#else
    result.emplace_back("/usr/local/share/fonts");
    result.emplace_back("/usr/share/fonts");
#endif
#endif
    return result;
}

} // namespace

std::filesystem::path find_system_font(const std::string_view file_name) {
    if (file_name.empty() || file_name.find_first_of("/\\:") != std::string_view::npos ||
        file_name == "." || file_name == "..") {
        return {};
    }
    const std::string wanted = lowered(file_name);
    for (const std::filesystem::path& directory : font_directories()) {
        std::error_code error;
        // Distributions nest fonts by family, so the directories are searched recursively.
        std::filesystem::recursive_directory_iterator entry(
            directory, std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        for (; !error && entry != end; entry.increment(error)) {
            if (lowered(entry->path().filename().string()) != wanted)
                continue;
            std::error_code status_error;
            if (entry->is_regular_file(status_error) && !status_error)
                return entry->path();
        }
    }
    return {};
}

std::optional<std::string> select_system_font(const std::span<const std::string> candidates) {
    for (const std::string& candidate : candidates) {
        if (!find_system_font(candidate).empty())
            return std::string(system_font_scheme) + candidate;
    }
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> read_system_font(const std::string_view resource_id) {
    if (!resource_id.starts_with(system_font_scheme))
        return std::nullopt;
    const std::filesystem::path path =
        find_system_font(resource_id.substr(system_font_scheme.size()));
    std::vector<std::uint8_t> bytes;
    if (path.empty())
        return bytes;
    std::ifstream input(path, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return bytes;
}

} // namespace strata::host
