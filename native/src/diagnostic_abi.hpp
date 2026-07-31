#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <strata/strata.h>

#include "runtime/diagnostic.hpp"

namespace strata::abi_detail {

[[nodiscard]] inline strata_string_view diagnostic_view(const std::string_view value) noexcept {
    return strata_string_view{value.data(), value.size()};
}

[[nodiscard]] inline strata_diagnostic_severity diagnostic_severity(
    const runtime::DiagnosticSeverity severity
) noexcept {
    switch (severity) {
    case runtime::DiagnosticSeverity::info: return STRATA_DIAGNOSTIC_INFO;
    case runtime::DiagnosticSeverity::warning: return STRATA_DIAGNOSTIC_WARNING;
    case runtime::DiagnosticSeverity::error: return STRATA_DIAGNOSTIC_ERROR;
    }
    return STRATA_DIAGNOSTIC_ERROR;
}

[[nodiscard]] inline strata_diagnostic transported_diagnostic(
    const runtime::RuntimeDiagnosticRecord& record,
    const std::uint64_t dropped_count
) noexcept {
    const runtime::RuntimeDiagnostic& value = record.diagnostic;
    const runtime::DiagnosticRange* const range = value.range.has_value() ? &*value.range : nullptr;
    return strata_diagnostic{
        sizeof(strata_diagnostic),
        record.sequence,
        diagnostic_severity(value.severity),
        STRATA_DIAGNOSTIC_VERSION_CURRENT,
        diagnostic_view(value.code),
        diagnostic_view(value.message),
        range != nullptr ? diagnostic_view(range->source_id) : strata_string_view{},
        range != nullptr
            ? strata_source_range{
                  range->start.offset.value_or(0U),
                  range->end.offset.value_or(0U),
                  range->start.line,
                  range->start.column,
                  range->end.line,
                  range->end.column,
              }
            : strata_source_range{},
        static_cast<std::uint64_t>(record.occurrence_count),
        record.sequence,
        record.first_frame_index,
        record.frame_index,
        dropped_count,
        diagnostic_view(value.path),
        value.expected.has_value() ? diagnostic_view(*value.expected) : strata_string_view{},
    };
}

inline void emit_diagnostics_snapshot(
    const runtime::RuntimeDiagnosticsSnapshot& source,
    const strata_diagnostics_snapshot_sink& sink
) {
    std::vector<strata_diagnostic> records;
    records.reserve(source.records.size());
    for (const runtime::RuntimeDiagnosticRecord& record : source.records) {
        records.push_back(transported_diagnostic(record, source.dropped_count));
    }
    const strata_diagnostics_snapshot snapshot{
        sizeof(strata_diagnostics_snapshot),
        STRATA_DIAGNOSTICS_SNAPSHOT_VERSION_CURRENT,
        0U,
        source.frame_index,
        source.dropped_count,
        records.data(),
        records.size(),
    };
    sink.emit(sink.user_data, &snapshot);
}

} // namespace strata::abi_detail
