#pragma once

#include "data/json.hpp"
#include "runtime/diagnostic.hpp"

namespace strata::runtime {

[[nodiscard]] data::JsonValue diagnostic_json(const RuntimeDiagnostic& diagnostic);
[[nodiscard]] data::JsonValue diagnostics_json(const RuntimeDiagnosticsSnapshot& diagnostics);

} // namespace strata::runtime
