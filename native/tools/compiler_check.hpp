#pragma once

#include <filesystem>
#include <vector>

namespace strata::tools {

/*
 * Imports resolve inside one root, the same boundary the hosts load modules within: the resource
 * root when one is given (always for artifacts), otherwise the entry module's own folder, which is
 * never more permissive than a host.
 */

/** Compiles an ordinary .strata module with the production native compiler. */
[[nodiscard]] int check_module(
    const std::filesystem::path& entry_path,
    const std::filesystem::path& schemas_path,
    const std::vector<std::filesystem::path>& extension_paths = {},
    const std::filesystem::path& resource_root = {}
);

/** Compiles a module and emits one machine-readable diagnostics document to stdout. */
[[nodiscard]] int check_module_json(
    const std::filesystem::path& entry_path,
    const std::filesystem::path& schemas_path,
    const std::vector<std::filesystem::path>& extension_paths = {},
    const std::filesystem::path& resource_root = {}
);

/** Emits or verifies the canonical portable artifact for one bundled application module. */
[[nodiscard]] int write_module_artifact(
    const std::filesystem::path& entry_path,
    const std::filesystem::path& schemas_path,
    const std::filesystem::path& resource_root,
    const std::filesystem::path& artifact_path,
    bool check_only,
    const std::vector<std::filesystem::path>& extension_paths = {}
);

} // namespace strata::tools
