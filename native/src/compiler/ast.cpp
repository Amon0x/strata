#include "compiler/ast.hpp"

#include <cstddef>

namespace strata::compiler {

std::string CallTarget::qualified_name() const {
    std::string result;
    for (std::size_t index = 0U; index < parts.size(); ++index) {
        if (index != 0U) {
            result.push_back('.');
        }
        result.append(parts[index]);
    }
    return result;
}

} // namespace strata::compiler
