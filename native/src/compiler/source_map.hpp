#pragma once

#include <optional>
#include <string>
#include <vector>

#include "compiler/ast.hpp"
#include "compiler/semantic/model.hpp"
#include "compiler/source.hpp"

namespace strata::compiler {

struct CompiledSourceMapEntry final {
    std::string path;
    std::string kind;
    std::optional<std::string> name;
    SourceSpan span;
    std::string runtime_component_path;
};

struct CompiledSourceMap final {
    std::string source_id;
    std::vector<CompiledSourceMapEntry> entries;
};

/** The source map of an analysed file, projected from the placements analysis assigned. */
[[nodiscard]] CompiledSourceMap build_compiled_source_map(
    const File& file,
    const SemanticModel& model
);

} // namespace strata::compiler
