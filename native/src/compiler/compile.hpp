#pragma once

#include <optional>
#include <memory_resource>
#include <vector>

#include "compiler/diagnostic.hpp"
#include "compiler/module.hpp"
#include "compiler/schema.hpp"
#include "compiler/source_map.hpp"
#include "data/json.hpp"

namespace strata::compiler {

struct CompileResult final {
    std::string entry_source_id;
    std::vector<std::string> dependencies;
    CompiledSourceMap source_map;
    std::optional<data::JsonValue> unit;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool succeeded() const noexcept;
};

/** Shared compiler pipeline used by validation tools, explicit source reactivation, and hosts. */
[[nodiscard]] CompileResult compile_program(
    const ModuleSource& entry,
    const ModuleLoader& loader,
    const SchemaRegistry& registry,
    std::pmr::memory_resource* scratch = std::pmr::get_default_resource()
);

} // namespace strata::compiler
