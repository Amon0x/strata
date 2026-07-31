#include "compiler/security.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/source.hpp"

namespace strata::compiler {

std::vector<Diagnostic> scan_security_boundary(
    const std::string_view source_id,
    const std::string_view source
) {
    constexpr std::array terms{
        std::string_view("kotlin.script"),
        std::string_view("javax.script"),
        std::string_view("Class.forName"),
        std::string_view("getDeclaredMethod"),
        std::string_view("getDeclaredField"),
        std::string_view("MethodHandles"),
        std::string_view("Runtime.getRuntime"),
        std::string_view("ProcessBuilder"),
    };
    constexpr std::string_view expected =
        "no reflection, dynamic-class-loading, script-execution, process-access, file-access, "
        "network-access, environment-access, thread-spawning, host-object-mutation";

    if (source.empty()) {
        return {};
    }
    const SourceBuffer buffer{std::string(source_id), std::string(source)};
    std::vector<Diagnostic> diagnostics;
    for (const std::string_view term : terms) {
        std::size_t start = source.find(term);
        while (start != std::string_view::npos) {
            diagnostics.push_back(Diagnostic{
                "STRATA.DSL.SECURITY_BOUNDARY",
                DiagnosticSeverity::error,
                "DSL source contains prohibited security boundary term '" + std::string(term) + "'.",
                buffer.span(start, start + term.size()).range(),
                std::string("security"),
                std::string(expected),
            });
            start = source.find(term, start + term.size());
        }
    }
    return diagnostics;
}

} // namespace strata::compiler
