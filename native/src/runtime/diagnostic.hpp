#pragma once

#include "base/diagnostic.hpp"

/** The runtime reports diagnostics with the base vocabulary every layer shares. */
namespace strata::runtime {

using base::DiagnosticPosition;
using base::DiagnosticRange;
using base::DiagnosticReporter;
using base::DiagnosticSeverity;
using base::RuntimeDiagnostic;
using base::RuntimeDiagnosticRecord;
using base::RuntimeDiagnosticStore;
using base::RuntimeDiagnosticsSnapshot;

} // namespace strata::runtime
