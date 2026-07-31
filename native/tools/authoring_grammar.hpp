#pragma once

#include <string>

#include "data/json.hpp"

namespace strata::tools {

[[nodiscard]] std::string render_grammar(const data::JsonValue& lexical);

} // namespace strata::tools
