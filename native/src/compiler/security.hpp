#pragma once

#include <string_view>
#include <vector>

#include "compiler/diagnostic.hpp"

namespace strata::compiler {

[[nodiscard]] std::vector<Diagnostic> scan_security_boundary(
    std::string_view source_id,
    std::string_view source
);

} // namespace strata::compiler
