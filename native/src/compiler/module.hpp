#pragma once

#include <functional>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "compiler/ast.hpp"
#include "compiler/diagnostic.hpp"

namespace strata::compiler {

struct ModuleSource final {
    std::string source_id;
    std::string text;
};

class ModuleLoadError final : public std::runtime_error {
public:
    ModuleLoadError(std::string message, std::optional<std::string> dependency_source_id = std::nullopt);

    [[nodiscard]] const std::optional<std::string>& dependency_source_id() const noexcept;

private:
    std::optional<std::string> dependency_source_id_;
};

using ModuleLoader = std::function<ModuleSource(std::string_view importer, std::string_view path)>;

struct ModuleResult final {
    std::string entry_source_id;
    std::vector<std::string> dependencies;
    File merged_file;
    std::vector<Diagnostic> diagnostics;
};

[[nodiscard]] ModuleResult load_modules(
    const ModuleSource& entry,
    const ModuleLoader& loader,
    std::pmr::memory_resource* scratch = std::pmr::get_default_resource()
);

} // namespace strata::compiler
