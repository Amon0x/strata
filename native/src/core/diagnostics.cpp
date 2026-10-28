#include "core/diagnostics.hpp"

#include <limits>
#include <optional>
#include <tuple>
#include <utility>

namespace strata::core {

void deliver(const DiagnosticSink& sink, const DiagnosticEvent& event) noexcept {
    if (!sink) return;
    try {
        sink(event);
    } catch (...) {
        /* A foreign callback cannot be allowed to unwind through the engine. */
    }
}

Diagnostics::Diagnostics(DiagnosticSink sink) : sink_(std::move(sink)) {}

bool Diagnostics::Fingerprint::operator<(const Fingerprint& other) const noexcept {
    return std::tie(
               severity,
               code,
               message,
               source_id,
               component_path,
               expected,
               has_range,
               range.byte_start,
               range.byte_end,
               range.line_start,
               range.column_start,
               range.line_end,
               range.column_end
           ) <
           std::tie(
               other.severity,
               other.code,
               other.message,
               other.source_id,
               other.component_path,
               other.expected,
               other.has_range,
               other.range.byte_start,
               other.range.byte_end,
               other.range.line_start,
               other.range.column_start,
               other.range.line_end,
               other.range.column_end
           );
}

base::Result Diagnostics::emit(
    const base::Status status,
    const DiagnosticSeverity severity,
    const std::string_view code,
    const std::string_view message,
    const std::string_view source_id,
    const SourceRange* const range,
    const std::string_view component_path,
    const std::string_view expected
) noexcept {
    std::uint64_t id = next_id_;
    std::uint64_t occurrence_count = 1U;
    try {
        Fingerprint fingerprint{
            severity,
            std::string(code),
            std::string(message),
            std::string(source_id),
            std::string(component_path),
            std::string(expected),
            range != nullptr,
            range != nullptr ? *range : SourceRange{},
        };
        const auto existing = occurrences_.find(fingerprint);
        if (existing != occurrences_.end()) {
            id = existing->second.id;
            if (existing->second.count != std::numeric_limits<std::uint64_t>::max()) {
                ++existing->second.count;
            }
            occurrence_count = existing->second.count;
        } else {
            occurrences_.emplace(std::move(fingerprint), Occurrence{id, 1U});
            if (next_id_ != std::numeric_limits<std::uint64_t>::max()) ++next_id_;
        }
    } catch (...) {
        if (next_id_ != std::numeric_limits<std::uint64_t>::max()) ++next_id_;
    }
    deliver(sink_, DiagnosticEvent{
        id,
        severity,
        code,
        message,
        source_id,
        range != nullptr ? *range : SourceRange{},
        occurrence_count,
        id,
        0U,
        0U,
        0U,
        component_path,
        expected,
    });
    return base::result(status, id);
}

base::Result Diagnostics::publish(
    const runtime::RuntimeDiagnosticRecord& record,
    const std::uint64_t dropped_count
) noexcept {
    const runtime::RuntimeDiagnostic& value = record.diagnostic;
    const SourceRange range = value.range.has_value()
        ? SourceRange{
              value.range->start.offset.value_or(0U),
              value.range->end.offset.value_or(0U),
              value.range->start.line,
              value.range->start.column,
              value.range->end.line,
              value.range->end.column,
          }
        : SourceRange{};
    const DiagnosticSeverity severity = [&value] {
        switch (value.severity) {
        case runtime::DiagnosticSeverity::info: return DiagnosticSeverity::info;
        case runtime::DiagnosticSeverity::warning: return DiagnosticSeverity::warning;
        case runtime::DiagnosticSeverity::error: return DiagnosticSeverity::error;
        }
        return DiagnosticSeverity::error;
    }();
    deliver(sink_, DiagnosticEvent{
        record.sequence,
        severity,
        value.code,
        value.message,
        value.range.has_value() ? std::string_view(value.range->source_id) : std::string_view{},
        range,
        static_cast<std::uint64_t>(record.occurrence_count),
        record.sequence,
        record.first_frame_index,
        record.frame_index,
        dropped_count,
        value.path,
        value.expected.has_value() ? std::string_view(*value.expected) : std::string_view{},
    });
    return base::result(base::Status::ok, record.sequence);
}

void Diagnostics::clear() noexcept {
    occurrences_.clear();
    next_id_ = 1U;
}

} // namespace strata::core
