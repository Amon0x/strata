#include <strata/extension_plugin.h>

#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

int main(const int argument_count, const char* const* const arguments) {
    if (argument_count != 2) return 2;
#if defined(_WIN32)
    HMODULE library = LoadLibraryW(std::filesystem::path(arguments[1]).c_str());
    if (library == nullptr) return 3;
    const auto query = reinterpret_cast<strata_extension_plugin_entry>(
        GetProcAddress(library, STRATA_EXTENSION_PLUGIN_ENTRY_NAME)
    );
#else
    void* library = dlopen(arguments[1], RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) return 3;
    void* symbol = dlsym(library, STRATA_EXTENSION_PLUGIN_ENTRY_NAME);
    strata_extension_plugin_entry query = nullptr;
    static_assert(sizeof(query) == sizeof(symbol));
    std::memcpy(&query, &symbol, sizeof(query));
#endif
    if (query == nullptr) return 4;
    strata_extension_plugin incompatible{};
    incompatible.struct_size = sizeof(incompatible);
    if (query(STRATA_EXTENSION_PLUGIN_ABI_VERSION_CURRENT + 1U, &incompatible) !=
        STRATA_STATUS_UNSUPPORTED_ABI) {
        return 5;
    }
    strata_extension_plugin plugin{};
    plugin.struct_size = sizeof(plugin);
    const strata_status status = query(STRATA_EXTENSION_PLUGIN_ABI_VERSION_CURRENT, &plugin);
    const std::string id(
        plugin.package_id.data == nullptr ? "" : plugin.package_id.data,
        plugin.package_id.size
    );
    const std::string schema(
        plugin.schema_json.data == nullptr ? "" : plugin.schema_json.data,
        plugin.schema_json.size
    );
    const bool accepted = status == STRATA_STATUS_OK &&
        plugin.plugin_abi_version == STRATA_EXTENSION_PLUGIN_ABI_VERSION_CURRENT &&
        plugin.core_abi_version == STRATA_ABI_VERSION_CURRENT && id == "installed.smoke.v1" &&
        plugin.extensions != nullptr && plugin.extensions->widget_count == 1U &&
        schema.find("InstalledExternalWidget") != std::string::npos;
#if defined(_WIN32)
    static_cast<void>(FreeLibrary(library));
#else
    static_cast<void>(dlclose(library));
#endif
    if (!accepted) return 6;
    std::cout << "strata_installed_extension: OK\n";
    return 0;
}
