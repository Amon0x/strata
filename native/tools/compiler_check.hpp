#pragma once

#include <filesystem>
#include <vector>

namespace strata::tools {

/** Compiles an ordinary .strata module with the production native compiler. */
[[nodiscard]] int check_module(
    const std::filesystem::path& entry_path,
    const std::filesystem::path& registry_path,
    const std::filesystem::path& schemas_path,
    const std::vector<std::filesystem::path>& extension_paths = {}
);

/** Compiles a module and emits one machine-readable diagnostics document to stdout. */
[[nodiscard]] int check_module_json(
    const std::filesystem::path& entry_path,
    const std::filesystem::path& registry_path,
    const std::filesystem::path& schemas_path,
    const std::vector<std::filesystem::path>& extension_paths = {}
);

/** Emits or verifies the canonical portable artifact for one bundled application module. */
[[nodiscard]] int write_module_artifact(
    const std::filesystem::path& entry_path,
    const std::filesystem::path& registry_path,
    const std::filesystem::path& schemas_path,
    const std::filesystem::path& resource_root,
    const std::filesystem::path& artifact_path,
    bool check_only,
    const std::vector<std::filesystem::path>& extension_paths = {}
);

} // namespace strata::tools
