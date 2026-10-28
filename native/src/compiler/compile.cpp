#include "compiler/compile.hpp"

#include <iterator>
#include <utility>

#include "compiler/portable_ir.hpp"
#include "compiler/semantic.hpp"

namespace strata::compiler {

bool CompileResult::succeeded() const noexcept {
    return unit.has_value() && diagnostics.empty();
}

CompileResult compile_program(
    const ModuleSource& entry,
    const ModuleLoader& loader,
    const SchemaRegistry& registry,
    std::pmr::memory_resource* const scratch
) {
    ModuleResult modules = load_modules(entry, loader, scratch);
    // Analysis runs on what parsed even when a module or import failed, so tools see every problem
    // at once; what a missing declaration would have resolved is not reported again.
    SemanticResult semantic = validate_semantics(
        modules.merged_file,
        registry,
        modules.diagnostics.empty() ? AnalysisInput::complete : AnalysisInput::incomplete
    );
    CompileResult result{
        modules.entry_source_id,
        std::move(modules.dependencies),
        build_compiled_source_map(modules.merged_file, semantic.model),
        std::nullopt,
        std::move(modules.diagnostics),
    };
    result.diagnostics.insert(
        result.diagnostics.end(),
        std::make_move_iterator(semantic.diagnostics.begin()),
        std::make_move_iterator(semantic.diagnostics.end())
    );
    if (!result.diagnostics.empty()) return result;

    PortableIrResult lowered = lower_portable_ir(modules.merged_file, registry, semantic);
    result.diagnostics.insert(
        result.diagnostics.end(),
        std::make_move_iterator(lowered.diagnostics.begin()),
        std::make_move_iterator(lowered.diagnostics.end())
    );
    if (result.diagnostics.empty()) result.unit = std::move(lowered.unit);
    return result;
}

} // namespace strata::compiler
