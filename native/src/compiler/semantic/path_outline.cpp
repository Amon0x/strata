#include <exception>

#include "compiler/semantic/analysis.hpp"
#include "geometry/path.hpp"

namespace strata::compiler::semantic {

// The outline is parsed by the geometry library the runtime draws with, so a malformed one is a compile error
// pointing at the literal instead of a shape missing at runtime. This is the analysis's only use
// of the path parser.
std::optional<std::string> path_outline_error(const std::string& commands) {
    try {
        static_cast<void>(geometry::Path::parse(commands));
    } catch (const std::exception& error) {
        return std::string(error.what());
    }
    return std::nullopt;
}

} // namespace strata::compiler::semantic
