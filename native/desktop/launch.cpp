#include "launch.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace strata::desktop {
namespace {

[[nodiscard]] std::wstring wide(const std::string_view value) {
    if (value.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0)
        throw std::invalid_argument("custom host argument is not valid UTF-8");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), size) != size) {
        throw std::runtime_error("custom host argument conversion was incomplete");
    }
    return result;
}

[[nodiscard]] std::wstring quote(const std::wstring_view value) {
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(value);
    }
    std::wstring result(1U, L'\"');
    std::size_t backslashes = 0U;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            result.append(backslashes * 2U + 1U, L'\\');
            result.push_back(L'\"');
        } else {
            result.append(backslashes, L'\\');
            result.push_back(character);
        }
        backslashes = 0U;
    }
    result.append(backslashes * 2U, L'\\');
    result.push_back(L'\"');
    return result;
}

} // namespace
int run_custom_host(const LaunchManifest& manifest) {
    if (manifest.kind != LaunchKind::custom) {
        throw std::invalid_argument("custom host launch requires a custom application manifest");
    }
    if (!std::filesystem::is_regular_file(manifest.executable)) {
        throw std::runtime_error(
            "custom host executable does not exist: " + manifest.executable.string() +
            "; build the application target or update launch.executable");
    }

    const std::wstring executable = manifest.executable.wstring();
    std::wstring command_line = quote(executable);
    for (const std::string& argument : manifest.arguments) {
        command_line.push_back(L' ');
        command_line += quote(wide(argument));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(STARTUPINFOW);
    PROCESS_INFORMATION process{};
    const std::wstring working_directory = manifest.path.parent_path().wstring();
    if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0U,
                        nullptr, working_directory.c_str(), &startup, &process)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "could not start custom host " + manifest.executable.string());
    }
    CloseHandle(process.hThread);
    if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0) {
        const DWORD error = GetLastError();
        CloseHandle(process.hProcess);
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "could not wait for custom host");
    }
    DWORD exit_code = 1U;
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
        const DWORD error = GetLastError();
        CloseHandle(process.hProcess);
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "could not read custom host exit status");
    }
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
}

} // namespace strata::desktop
