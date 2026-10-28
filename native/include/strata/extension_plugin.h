#ifndef STRATA_EXTENSION_PLUGIN_H
#define STRATA_EXTENSION_PLUGIN_H

#include <strata/strata.h>
#include <strata/widget_api.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Plugin ABI 1 (deprecated): the package exports `strata_extension_plugin_query` and calls Strata
 * through the strata_widget_* exports it binds by name, so it links strata_c and shares its core
 * ABI version. Hosts still load such packages.
 *
 * Plugin ABI 2: the package exports `strata_extension_plugin_query_v2`, which receives the host's
 * strata_widget_api table and calls Strata only through it; the package links nothing from
 * strata_c. Hosts look for the version 2 entry point first.
 */
#define STRATA_EXTENSION_PLUGIN_ABI_VERSION_1 UINT32_C(1)
#define STRATA_EXTENSION_PLUGIN_ABI_VERSION_2 UINT32_C(2)
#define STRATA_EXTENSION_PLUGIN_ABI_VERSION_CURRENT STRATA_EXTENSION_PLUGIN_ABI_VERSION_2
#define STRATA_EXTENSION_PLUGIN_ENTRY_NAME "strata_extension_plugin_query"
#define STRATA_EXTENSION_PLUGIN_ENTRY_V2_NAME "strata_extension_plugin_query_v2"

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

/**
 * What a host hands a plugin ABI 2 package. `widget_api` is borrowed for as long as the package
 * stays loaded; it is null when the host only reads package schemas and never runs widgets.
 */
typedef struct strata_extension_host {
    size_t struct_size;
    uint32_t plugin_abi_version;
    uint32_t reserved;
    const strata_widget_api* widget_api;
} strata_extension_host;

/** Fills `out_plugin` with plugin_abi_version STRATA_EXTENSION_PLUGIN_ABI_VERSION_2. */
typedef strata_status (*strata_extension_plugin_entry_v2)(
    const strata_extension_host* host,
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
