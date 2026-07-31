#pragma once

#include <filesystem>

namespace strata::tools {

/** Compiles an ordinary .strata module with the production native compiler. */
[[nodiscard]] int check_module(
    const std::filesystem::path& entry_path,
    const std::filesystem::path& registry_path,
    const std::filesystem::path& schemas_path
);

/** Emits or verifies the canonical portable artifact for one bundled application module. */
[[nodiscard]] int write_module_artifact(
    const std::filesystem::path& entry_path,
    const std::filesystem::path& registry_path,
    const std::filesystem::path& schemas_path,
    const std::filesystem::path& resource_root,
    const std::filesystem::path& artifact_path,
    bool check_only
);

} // namespace strata::tools
