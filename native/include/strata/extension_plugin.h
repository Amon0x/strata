#ifndef STRATA_EXTENSION_PLUGIN_H
#define STRATA_EXTENSION_PLUGIN_H

#include <strata/strata.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STRATA_EXTENSION_PLUGIN_ABI_VERSION_1 UINT32_C(1)
#define STRATA_EXTENSION_PLUGIN_ABI_VERSION_CURRENT STRATA_EXTENSION_PLUGIN_ABI_VERSION_1
#define STRATA_EXTENSION_PLUGIN_ENTRY_NAME "strata_extension_plugin_query"

/**
 * Borrowed package projection owned by a loaded extension library.
 *
 * Every pointer remains valid until the host unloads the library. The host must keep the library
 * loaded until every Surface using `extensions` has been released.
 */
typedef struct strata_extension_plugin {
    size_t struct_size;
    uint32_t plugin_abi_version;
    uint32_t core_abi_version;
    strata_string_view package_id;
    strata_string_view schema_json;
    const strata_surface_extension_bundle* extensions;
} strata_extension_plugin;

typedef strata_status (*strata_extension_plugin_entry)(
    uint32_t requested_plugin_abi,
    strata_extension_plugin* out_plugin
);

#if defined(_WIN32)
#define STRATA_EXTENSION_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define STRATA_EXTENSION_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define STRATA_EXTENSION_PLUGIN_EXPORT
#endif

#ifdef __cplusplus
}
#endif

#endif
