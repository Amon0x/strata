#pragma once

#include <string>

#include "data/json.hpp"

namespace strata::tools {

[[nodiscard]] std::string render_reference(const data::JsonValue& registry);
[[nodiscard]] std::string render_completions(const data::JsonValue& registry);

} // namespace strata::tools
