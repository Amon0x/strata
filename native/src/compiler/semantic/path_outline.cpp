#include <exception>

#include "compiler/semantic/analysis.hpp"
#include "ui/path.hpp"

namespace strata::compiler::semantic {

// The outline is parsed by the same code the runtime uses, so a malformed one is a compile error
// pointing at the literal instead of a shape missing at runtime. This is the analysis's only use
// of the path parser.
std::optional<std::string> path_outline_error(const std::string& commands) {
    try {
        static_cast<void>(ui::Path::parse(commands));
    } catch (const std::exception& error) {
        return std::string(error.what());
    }
    return std::nullopt;
}

} // namespace strata::compiler::semantic
