#include "compiler/module.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compiler/parser.hpp"

namespace strata::compiler {

ModuleLoadError::ModuleLoadError(
    std::string message,
    std::optional<std::string> dependency_source_id
)
    : std::runtime_error(std::move(message)),
      dependency_source_id_(std::move(dependency_source_id)) {}

const std::optional<std::string>& ModuleLoadError::dependency_source_id() const noexcept {
    return dependency_source_id_;
}

ModuleResult load_modules(
    const ModuleSource& entry,
    const ModuleLoader& loader,
    std::pmr::memory_resource* const scratch
) {
    std::vector<std::pair<std::string, ParseResult>> modules;
    std::vector<std::string> dependencies;
    std::vector<Diagnostic> diagnostics;
    std::vector<std::string> visiting;

    const auto add_dependency = [&dependencies](const std::string& source_id) {
        if (std::ranges::find(dependencies, source_id) == dependencies.end()) {
            dependencies.push_back(source_id);
        }
    };
    std::function<void(const ModuleSource&, const std::optional<SourceSpan>&)> visit;
    visit = [&](const ModuleSource& source, const std::optional<SourceSpan>& import_span) {
        if (std::ranges::find(modules, source.source_id, &decltype(modules)::value_type::first) !=
            modules.end()) {
            return;
        }
        const auto cycle_start = std::ranges::find(visiting, source.source_id);
        if (cycle_start != visiting.end()) {
            std::string cycle;
            for (auto current = cycle_start; current != visiting.end(); ++current) {
                if (!cycle.empty()) cycle.append(" -> ");
                cycle.append(*current);
            }
            cycle.append(" -> ").append(source.source_id);
            diagnostics.push_back(Diagnostic{
                "STRATA.DSL.IMPORT_CYCLE",
                DiagnosticSeverity::error,
                "DSL import cycle detected: " + cycle + ".",
                import_span.has_value() ? std::optional(import_span->range()) : std::nullopt,
                std::string("import"),
                std::string("acyclic import graph"),
            });
            return;
        }

        visiting.push_back(source.source_id);
        ParseResult parsed = parse_source(source.source_id, source.text, {}, scratch);
        diagnostics.insert(
            diagnostics.end(),
            std::make_move_iterator(parsed.diagnostics.begin()),
            std::make_move_iterator(parsed.diagnostics.end())
        );
        parsed.diagnostics.clear();
        for (const Import& import : parsed.file.imports) {
            try {
                ModuleSource imported = loader(source.source_id, import.path);
                add_dependency(imported.source_id);
                visit(imported, import.span);
            } catch (const ModuleLoadError& failure) {
                if (failure.dependency_source_id().has_value()) {
                    add_dependency(*failure.dependency_source_id());
                }
                diagnostics.push_back(Diagnostic{
                    "STRATA.DSL.IMPORT_LOAD_FAILED",
                    DiagnosticSeverity::error,
                    failure.what(),
                    import.span.range(),
                    "import " + import.path,
                    std::string("readable DSL module within configured roots"),
                });
            } catch (const std::exception& failure) {
                diagnostics.push_back(Diagnostic{
                    "STRATA.DSL.IMPORT_LOAD_FAILED",
                    DiagnosticSeverity::error,
                    "DSL import '" + import.path + "' could not be loaded: " + failure.what() + ".",
                    import.span.range(),
                    "import " + import.path,
                    std::string("readable DSL module within configured roots"),
                });
            }
        }
        visiting.pop_back();
        modules.emplace_back(source.source_id, std::move(parsed));
    };

    visit(entry, std::nullopt);
    auto entry_module = std::ranges::find(modules, entry.source_id, &decltype(modules)::value_type::first);
    if (entry_module == modules.end()) {
        modules.emplace_back(entry.source_id, parse_source(entry.source_id, entry.text, {}, scratch));
        entry_module = std::prev(modules.end());
    }
    File merged = entry_module->second.file;
    merged.imports.clear();
    merged.declarations.clear();
    for (const auto& [source_id, module] : modules) {
        static_cast<void>(source_id);
        merged.declarations.insert(
            merged.declarations.end(),
            module.file.declarations.begin(),
            module.file.declarations.end()
        );
    }
    std::ranges::sort(dependencies);
    return ModuleResult{
        entry.source_id,
        std::move(dependencies),
        std::move(merged),
        std::move(diagnostics),
    };
}

} // namespace strata::compiler
