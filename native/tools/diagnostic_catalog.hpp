#pragma once

#include <filesystem>
#include <string>

namespace strata::tools {

[[nodiscard]] std::string render_diagnostic_catalog(const std::filesystem::path& project_root);

} // namespace strata::tools
