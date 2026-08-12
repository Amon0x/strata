#include "launch.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "data/json.hpp"

namespace strata::desktop {
namespace {

using data::JsonValue;

[[nodiscard]] const JsonValue& required(const JsonValue& object, const std::string_view field) {
    const JsonValue* const value = object.find(field);
    if (value == nullptr) {
        throw std::invalid_argument("Strata application manifest is missing '" +
                                    std::string(field) + "'");
    }
    return *value;
}

[[nodiscard]] const JsonValue* optional(const JsonValue& object,
                                        const std::string_view field) noexcept {
    return object.find(field);
}

[[nodiscard]] const JsonValue::Object& object(const JsonValue& value,
                                              const std::string_view label) {
    const JsonValue::Object* const result = value.object();
    if (result == nullptr) {
        throw std::invalid_argument(std::string(label) + " must be a JSON object");
    }
    return *result;
}

[[nodiscard]] std::string text(const JsonValue& value, const std::string_view label) {
    const std::string* const result = value.string();
    if (result == nullptr || result->empty()) {
        throw std::invalid_argument(std::string(label) + " must be a non-empty JSON string");
    }
    return *result;
}

[[nodiscard]] bool boolean(const JsonValue& value, const std::string_view label) {
    const bool* const result = value.boolean();
    if (result == nullptr) {
        throw std::invalid_argument(std::string(label) + " must be a JSON boolean");
    }
    return *result;
}

[[nodiscard]] std::filesystem::path resolve_path(const std::filesystem::path& manifest,
                                                 const std::string& configured) {
    const std::filesystem::path value(configured);
    return std::filesystem::absolute(value.is_absolute() ? value : manifest.parent_path() / value)
        .lexically_normal();
}

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

LaunchManifest load_launch_manifest(const std::filesystem::path& source_path) {
    const std::filesystem::path manifest_path =
        std::filesystem::absolute(source_path).lexically_normal();
    if (!manifest_path.filename().wstring().ends_with(L".strata-app.json")) {
        throw std::invalid_argument("application manifest name must end with .strata-app.json");
    }
    std::ifstream input(manifest_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open Strata application manifest: " +
                                 manifest_path.string());
    }
    const std::string source{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    const JsonValue document = data::parse_json(source);
    static_cast<void>(object(document, "Strata application manifest"));
    const std::int64_t* const version = required(document, "version").integer();
    if (version == nullptr || *version != 1) {
        throw std::invalid_argument("Strata application manifest version must be 1");
    }
    const JsonValue& launch = required(document, "launch");
    static_cast<void>(object(launch, "launch"));

    LaunchManifest result;
    result.path = manifest_path;
    const std::string kind = text(required(launch, "kind"), "launch.kind");
    if (const JsonValue* const title = optional(launch, "title"); title != nullptr) {
        result.title = text(*title, "launch.title");
    }
    if (const JsonValue* const watch = optional(launch, "watch"); watch != nullptr) {
        result.watch = boolean(*watch, "launch.watch");
    }

    if (kind == "generic") {
        result.kind = LaunchKind::generic;
        result.resource_root = resolve_path(
            manifest_path, text(required(launch, "resourceRoot"), "launch.resourceRoot"));
        result.application = headless::load_scenario(manifest_path);
        if (result.title.empty())
            result.title = result.application.application_id;
        return result;
    }
    if (kind != "custom") {
        throw std::invalid_argument("launch.kind must be 'generic' or 'custom'");
    }

    result.kind = LaunchKind::custom;
    result.executable =
        resolve_path(manifest_path, text(required(launch, "executable"), "launch.executable"));
    if (const JsonValue* const arguments = optional(launch, "arguments"); arguments != nullptr) {
        const JsonValue::Array* const values = arguments->array();
        if (values == nullptr) {
            throw std::invalid_argument("launch.arguments must be a JSON array");
        }
        result.arguments.reserve(values->size());
        for (const JsonValue& value : *values) {
            const std::string* const argument = value.string();
            if (argument == nullptr) {
                throw std::invalid_argument("launch.arguments entries must be JSON strings");
            }
            result.arguments.push_back(*argument);
        }
    }
    return result;
}

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
