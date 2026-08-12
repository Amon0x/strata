#include "extensions.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include "core/utf8.hpp"
#include "data/json.hpp"

namespace strata::host {
namespace {

[[nodiscard]] std::string copied(const strata_string_view value, const std::string_view label) {
    if (value.size != 0U && value.data == nullptr) {
        throw std::runtime_error("external extension returned a null " + std::string(label));
    }
    std::string result(value.data == nullptr ? "" : value.data, value.size);
    if (!core::valid_utf8(result)) {
        throw std::runtime_error("external extension returned invalid UTF-8 in " +
                                 std::string(label));
    }
    return result;
}

void require_package_id(const std::string_view id) {
    if (id.empty() || id.front() == '.' || id.contains("..") ||
        !std::ranges::all_of(id, [](const char character) {
            return (character >= 'A' && character <= 'Z') ||
                   (character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9') || character == '.' || character == '-' ||
                   character == '_';
        })) {
        throw std::invalid_argument(
            "native extension package ids may contain only letters, digits, '.', '-', and '_'");
    }
}

[[nodiscard]] std::string library_name(const std::string_view id) {
#if defined(_WIN32)
    return "strata-extension-" + std::string(id) + ".dll";
#elif defined(__APPLE__)
    return "libstrata-extension-" + std::string(id) + ".dylib";
#else
    return "libstrata-extension-" + std::string(id) + ".so";
#endif
}

[[nodiscard]] std::filesystem::path executable_directory() {
#if defined(_WIN32)
    std::wstring buffer(512U, L'\0');
    while (true) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0U)
            return {};
        if (length < buffer.size() - 1U) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2U);
    }
#else
    std::error_code error;
    const std::filesystem::path executable = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path{} : executable.parent_path();
#endif
}

void append_environment_paths(std::vector<std::filesystem::path>& paths) {
#if defined(_WIN32)
    char* buffer = nullptr;
    std::size_t length = 0U;
    if (_dupenv_s(&buffer, &length, "STRATA_EXTENSION_PATH") != 0 || buffer == nullptr)
        return;
    const std::string environment(buffer, length == 0U ? 0U : length - 1U);
    std::free(buffer);
    constexpr char separator = ';';
#else
    const char* const value = std::getenv("STRATA_EXTENSION_PATH");
    if (value == nullptr)
        return;
    const std::string environment(value);
    constexpr char separator = ':';
#endif
    std::string_view remaining(environment);
    while (!remaining.empty()) {
        const std::size_t split = remaining.find(separator);
        const std::string_view value = remaining.substr(0U, split);
        if (!value.empty())
            paths.emplace_back(value);
        if (split == std::string_view::npos)
            break;
        remaining.remove_prefix(split + 1U);
    }
}

[[nodiscard]] std::vector<std::filesystem::path>
search_paths(const std::vector<std::filesystem::path>& explicit_paths) {
    std::vector<std::filesystem::path> result = explicit_paths;
    append_environment_paths(result);
    const std::filesystem::path executable = executable_directory();
    if (!executable.empty()) {
        result.push_back(executable);
        result.push_back(executable / "strata" / "extensions");
        result.push_back(executable / ".." / "lib" / "strata" / "extensions");
    }
    std::vector<std::filesystem::path> normalized;
    std::set<std::filesystem::path> seen;
    for (const std::filesystem::path& candidate : result) {
        std::error_code error;
        const std::filesystem::path value = std::filesystem::weakly_canonical(candidate, error);
        const std::filesystem::path key = error ? candidate.lexically_normal() : value;
        if (seen.insert(key).second)
            normalized.push_back(key);
    }
    return normalized;
}

[[nodiscard]] std::filesystem::path
resolve_library(const std::string_view id, const std::vector<std::filesystem::path>& directories) {
    const std::string name = library_name(id);
    std::string searched;
    for (const std::filesystem::path& directory : search_paths(directories)) {
        const std::filesystem::path candidate = directory / name;
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return std::filesystem::canonical(candidate, error);
        }
        if (!searched.empty())
            searched += ", ";
        searched += directory.generic_string();
    }
    throw std::runtime_error(
        "could not find external extension package '" + std::string(id) + "' as " + name +
        "; searched: " + (searched.empty() ? std::string("no directories") : searched));
}

[[nodiscard]] void* open_library(const std::filesystem::path& path) {
#if defined(_WIN32)
    HMODULE library = LoadLibraryW(path.c_str());
    if (library == nullptr) {
        throw std::runtime_error(
            std::format("could not load external extension '{}' (Win32 error {})",
                        path.generic_string(), GetLastError()));
    }
    return library;
#else
    void* const library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        const char* const detail = dlerror();
        throw std::runtime_error("could not load external extension '" + path.generic_string() +
                                 "': " +
                                 (detail == nullptr ? std::string("unknown dynamic-loader error")
                                                    : std::string(detail)));
    }
    return library;
#endif
}

[[nodiscard]] strata_extension_plugin_entry entry_point(void* const library,
                                                        const std::filesystem::path& path) {
    strata_extension_plugin_entry entry = nullptr;
#if defined(_WIN32)
    entry = reinterpret_cast<strata_extension_plugin_entry>(
        GetProcAddress(static_cast<HMODULE>(library), STRATA_EXTENSION_PLUGIN_ENTRY_NAME));
#else
    void* const symbol = dlsym(library, STRATA_EXTENSION_PLUGIN_ENTRY_NAME);
    static_assert(sizeof(entry) == sizeof(symbol));
    std::memcpy(&entry, &symbol, sizeof(entry));
#endif
    if (entry == nullptr) {
        throw std::runtime_error("external extension '" + path.generic_string() +
                                 "' does not export " + STRATA_EXTENSION_PLUGIN_ENTRY_NAME);
    }
    return entry;
}

} // namespace

LoadedExtension::LoadedExtension(LoadedExtension&& other) noexcept
    : library_(std::exchange(other.library_, nullptr)), path_(std::move(other.path_)),
      id_(std::move(other.id_)), schema_json_(std::move(other.schema_json_)),
      bundle_(std::exchange(other.bundle_, nullptr)) {}

LoadedExtension& LoadedExtension::operator=(LoadedExtension&& other) noexcept {
    if (this == &other)
        return *this;
    close();
    library_ = std::exchange(other.library_, nullptr);
    path_ = std::move(other.path_);
    id_ = std::move(other.id_);
    schema_json_ = std::move(other.schema_json_);
    bundle_ = std::exchange(other.bundle_, nullptr);
    return *this;
}

LoadedExtension::~LoadedExtension() {
    close();
}

void LoadedExtension::close() noexcept {
    if (library_ == nullptr)
        return;
#if defined(_WIN32)
    static_cast<void>(FreeLibrary(static_cast<HMODULE>(library_)));
#else
    static_cast<void>(dlclose(library_));
#endif
    library_ = nullptr;
    bundle_ = nullptr;
}

LoadedExtension load_extension(const std::string_view expected_id,
                               const std::vector<std::filesystem::path>& search_directories) {
    require_package_id(expected_id);
    LoadedExtension result;
    result.path_ = resolve_library(expected_id, search_directories);
    result.library_ = open_library(result.path_);
    const strata_extension_plugin_entry query = entry_point(result.library_, result.path_);
    strata_extension_plugin plugin{};
    plugin.struct_size = sizeof(plugin);
    const strata_status status = query(STRATA_EXTENSION_PLUGIN_ABI_VERSION_CURRENT, &plugin);
    if (status != STRATA_STATUS_OK) {
        throw std::runtime_error("external extension '" + result.path_.generic_string() +
                                 "' rejected its package query with status " +
                                 std::to_string(status));
    }
    if (plugin.struct_size < sizeof(strata_extension_plugin) ||
        plugin.plugin_abi_version != STRATA_EXTENSION_PLUGIN_ABI_VERSION_CURRENT ||
        plugin.core_abi_version < STRATA_ABI_VERSION_MINIMUM ||
        plugin.core_abi_version > STRATA_ABI_VERSION_CURRENT || plugin.extensions == nullptr ||
        plugin.extensions->struct_size < STRATA_SURFACE_EXTENSION_BUNDLE_VERSION_1_SIZE ||
        (plugin.extensions->widget_count != 0U && plugin.extensions->widgets == nullptr) ||
        (plugin.extensions->behavior_count != 0U && plugin.extensions->behaviors == nullptr) ||
        (plugin.extensions->struct_size >= STRATA_SURFACE_EXTENSION_BUNDLE_VERSION_2_SIZE &&
         plugin.extensions->widget_input_count != 0U &&
         plugin.extensions->widget_inputs == nullptr) ||
        (plugin.extensions->struct_size >= STRATA_SURFACE_EXTENSION_BUNDLE_VERSION_3_SIZE &&
         plugin.extensions->widget_scroll_count != 0U &&
         plugin.extensions->widget_scrolls == nullptr) ||
        (plugin.extensions->struct_size >= sizeof(strata_surface_extension_bundle) &&
         plugin.extensions->behavior_input_count != 0U &&
         plugin.extensions->behavior_inputs == nullptr)) {
        throw std::runtime_error("external extension '" + result.path_.generic_string() +
                                 "' returned an incompatible package descriptor");
    }
    result.id_ = copied(plugin.package_id, "package id");
    result.schema_json_ = copied(plugin.schema_json, "schema document");
    if (result.id_ != expected_id) {
        throw std::runtime_error("external extension '" + result.path_.generic_string() +
                                 "' exports package '" + result.id_ + "', expected '" +
                                 std::string(expected_id) + "'");
    }
    if (result.schema_json_.empty()) {
        throw std::runtime_error("external extension package '" + result.id_ + "' has no schema");
    }
    const data::JsonValue schema = data::parse_json(result.schema_json_);
    const data::JsonValue* const schema_package = schema.find("package");
    if (schema_package == nullptr || schema_package->string() == nullptr ||
        *schema_package->string() != result.id_) {
        throw std::runtime_error("external extension package '" + result.id_ +
                                 "' schema has a mismatched package id");
    }
    result.bundle_ = plugin.extensions;
    return result;
}

const strata_surface_extension_bundle* SelectedExtensions::pointer() noexcept {
    if (widgets.empty() && widget_inputs.empty() && widget_scrolls.empty() &&
        behavior_inputs.empty() && behaviors.empty()) {
        return nullptr;
    }
    bundle = strata_surface_extension_bundle{
        sizeof(strata_surface_extension_bundle),
        widgets.empty() ? nullptr : widgets.data(),
        widgets.size(),
        behaviors.empty() ? nullptr : behaviors.data(),
        behaviors.size(),
        widget_inputs.empty() ? nullptr : widget_inputs.data(),
        widget_inputs.size(),
        widget_scrolls.empty() ? nullptr : widget_scrolls.data(),
        widget_scrolls.size(),
        behavior_inputs.empty() ? nullptr : behavior_inputs.data(),
        behavior_inputs.size(),
    };
    return &bundle;
}

std::vector<std::string> SelectedExtensions::schemas() const {
    std::vector<std::string> documents;
    documents.reserve(packages.size());
    for (const LoadedExtension& package : packages)
        documents.push_back(package.schema_json());
    return documents;
}
std::vector<std::string> declared_extension_packages(const std::string_view schemas_json) {
    if (schemas_json.empty())
        return {};
    const data::JsonValue schemas = data::parse_json(schemas_json);
    const data::JsonValue* const packages = schemas.find("extensionPackages");
    if (packages == nullptr)
        return {};
    if (packages->array() == nullptr) {
        throw std::runtime_error("extensionPackages must be an array of package ids");
    }
    std::vector<std::string> result;
    result.reserve(packages->array()->size());
    std::set<std::string, std::less<>> seen;
    for (const data::JsonValue& package : *packages->array()) {
        if (package.string() == nullptr) {
            throw std::runtime_error("extensionPackages entries must be package id strings");
        }
        require_package_id(*package.string());
        if (!seen.insert(*package.string()).second) {
            throw std::runtime_error("extensionPackages package ids must be unique");
        }
        result.push_back(*package.string());
    }
    return result;
}

SelectedExtensions select_extensions(const std::vector<std::string>& package_ids,
                                     const std::vector<std::filesystem::path>& search_directories) {
    SelectedExtensions result;
    result.packages.reserve(package_ids.size());
    std::set<std::string, std::less<>> seen;
    for (const std::string& id : package_ids) {
        if (!seen.insert(id).second) {
            throw std::invalid_argument("native extension package ids must be unique");
        }
        result.packages.push_back(load_extension(id, search_directories));
        const strata_surface_extension_bundle& package = result.packages.back().bundle();
        if (package.widget_count != 0U) {
            result.widgets.insert(result.widgets.end(), package.widgets,
                                  package.widgets + package.widget_count);
        }
        if (package.struct_size >= STRATA_SURFACE_EXTENSION_BUNDLE_VERSION_2_SIZE &&
            package.widget_input_count != 0U) {
            result.widget_inputs.insert(result.widget_inputs.end(), package.widget_inputs,
                                        package.widget_inputs + package.widget_input_count);
        }
        if (package.struct_size >= STRATA_SURFACE_EXTENSION_BUNDLE_VERSION_3_SIZE &&
            package.widget_scroll_count != 0U) {
            result.widget_scrolls.insert(result.widget_scrolls.end(), package.widget_scrolls,
                                         package.widget_scrolls + package.widget_scroll_count);
        }
        if (package.struct_size >= sizeof(strata_surface_extension_bundle) &&
            package.behavior_input_count != 0U) {
            result.behavior_inputs.insert(result.behavior_inputs.end(), package.behavior_inputs,
                                          package.behavior_inputs + package.behavior_input_count);
        }
        if (package.behavior_count != 0U) {
            result.behaviors.insert(result.behaviors.end(), package.behaviors,
                                    package.behaviors + package.behavior_count);
        }
    }
    return result;
}

} // namespace strata::host
