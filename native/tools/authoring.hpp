#pragma once

#include <filesystem>

namespace strata::tools {

[[nodiscard]] int check_authoring(const std::filesystem::path& project_root);
[[nodiscard]] int write_authoring(const std::filesystem::path& project_root);

} // namespace strata::tools
