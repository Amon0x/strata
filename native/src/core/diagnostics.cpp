#include "core/diagnostics.hpp"

#include <limits>
#include <tuple>
#include <utility>

namespace strata::core {
namespace {

[[nodiscard]] strata_string_view view(const std::string_view value) noexcept {
    return strata_string_view{value.data(), value.size()};
}

void dispatch(
    const strata_diagnostic_sink& sink,
    const std::uint64_t id,
    const strata_diagnostic_severity severity,
    const std::string_view code,
    const std::string_view message,
    const std::string_view source_id,
    const strata_source_range* const range,
    const std::uint64_t occurrence_count,
    const std::uint64_t sequence,
    const std::uint64_t first_frame_index,
    const std::uint64_t frame_index,
    const std::uint64_t dropped_count,
    const std::string_view component_path,
    const std::string_view expected
) noexcept {
    if (sink.emit == nullptr || sink.struct_size < sizeof(strata_diagnostic_sink)) {
        return;
    }
    const strata_diagnostic diagnostic{
        sizeof(strata_diagnostic),
        id,
        severity,
        STRATA_DIAGNOSTIC_VERSION_CURRENT,
        view(code),
        view(message),
        view(source_id),
        range != nullptr ? *range : strata_source_range{0U, 0U, 0U, 0U, 0U, 0U},
        occurrence_count,
        sequence,
        first_frame_index,
        frame_index,
        dropped_count,
        view(component_path),
        view(expected),
    };
    try {
        sink.emit(sink.user_data, &diagnostic);
    } catch (...) {
        /* A foreign callback cannot be allowed to unwind through the C ABI. */
    }
}

} // namespace

Diagnostics::Diagnostics(const strata_diagnostic_sink sink) noexcept : sink_(sink) {}

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

strata_result Diagnostics::emit(
    const strata_status status,
    const strata_diagnostic_severity severity,
    const std::string_view code,
    const std::string_view message,
    const std::string_view source_id,
    const strata_source_range* const range,
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
            range != nullptr ? *range : strata_source_range{0U, 0U, 0U, 0U, 0U, 0U},
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
    dispatch(
        sink_, id, severity, code, message, source_id, range, occurrence_count,
        id, 0U, 0U, 0U, component_path, expected
    );
    return result(status, id);
}

strata_result Diagnostics::publish(
    const runtime::RuntimeDiagnosticRecord& record,
    const std::uint64_t dropped_count
) noexcept {
    const runtime::RuntimeDiagnostic& value = record.diagnostic;
    const std::optional<strata_source_range> range = value.range.has_value()
        ? std::optional(strata_source_range{
              value.range->start.offset.value_or(0U),
              value.range->end.offset.value_or(0U),
              value.range->start.line,
              value.range->start.column,
              value.range->end.line,
              value.range->end.column,
          })
        : std::nullopt;
    const strata_diagnostic_severity severity = [&value] {
        switch (value.severity) {
        case runtime::DiagnosticSeverity::info: return STRATA_DIAGNOSTIC_INFO;
        case runtime::DiagnosticSeverity::warning: return STRATA_DIAGNOSTIC_WARNING;
        case runtime::DiagnosticSeverity::error: return STRATA_DIAGNOSTIC_ERROR;
        }
        return STRATA_DIAGNOSTIC_ERROR;
    }();
    dispatch(
        sink_,
        record.sequence,
        severity,
        value.code,
        value.message,
        value.range.has_value() ? std::string_view(value.range->source_id) : std::string_view{},
        range.has_value() ? &*range : nullptr,
        static_cast<std::uint64_t>(record.occurrence_count),
        record.sequence,
        record.first_frame_index,
        record.frame_index,
        dropped_count,
        value.path,
        value.expected.has_value() ? std::string_view(*value.expected) : std::string_view{}
    );
    return result(STRATA_STATUS_OK, record.sequence);
}

void Diagnostics::clear() noexcept {
    occurrences_.clear();
    next_id_ = 1U;
}

strata_result emit_without_runtime(
    const strata_diagnostic_sink& sink,
    const strata_status status,
    const std::string_view code,
    const std::string_view message
) noexcept {
    constexpr std::uint64_t diagnostic_id = 1U;
    dispatch(
        sink, diagnostic_id, STRATA_DIAGNOSTIC_ERROR, code, message, {}, nullptr, 1U,
        diagnostic_id, 0U, 0U, 0U, {}, {}
    );
    return result(status, diagnostic_id);
}

} // namespace strata::core
