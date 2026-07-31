#include "runtime/diagnostic_json.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace strata::runtime {
namespace {

using data::JsonValue;

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> entries) {
    return JsonValue(JsonValue::Object(entries));
}

[[nodiscard]] JsonValue array(std::vector<JsonValue> values = {}) {
    return JsonValue(JsonValue::Array(std::move(values)));
}

[[nodiscard]] std::string_view severity_name(const DiagnosticSeverity severity) noexcept {
    switch (severity) {
    case DiagnosticSeverity::info: return "info";
    case DiagnosticSeverity::warning: return "warning";
    case DiagnosticSeverity::error: return "error";
    }
    return "error";
}

[[nodiscard]] JsonValue position(const DiagnosticPosition& value) {
    return object({
        {"column", JsonValue(static_cast<std::int64_t>(value.column))},
        {"line", JsonValue(static_cast<std::int64_t>(value.line))},
        {"offset", value.offset.has_value()
                       ? JsonValue(static_cast<std::int64_t>(*value.offset))
                       : JsonValue{}},
    });
}

} // namespace

data::JsonValue diagnostic_json(const RuntimeDiagnostic& value) {
    JsonValue range;
    if (value.range.has_value()) {
        range = object({
            {"end", position(value.range->end)},
            {"sourceId", JsonValue(value.range->source_id)},
            {"start", position(value.range->start)},
        });
    }
    return object({
        {"code", JsonValue(value.code)},
        {"componentPath", value.path.empty() ? JsonValue{} : JsonValue(value.path)},
        {"expected", value.expected.has_value() ? JsonValue(*value.expected) : JsonValue{}},
        {"message", JsonValue(value.message)},
        {"range", std::move(range)},
        {"severity", JsonValue(std::string(severity_name(value.severity)))},
    });
}

data::JsonValue diagnostics_json(const RuntimeDiagnosticsSnapshot& value) {
    std::vector<JsonValue> records;
    records.reserve(value.records.size());
    for (const RuntimeDiagnosticRecord& record : value.records) {
        records.push_back(object({
            {"diagnostic", diagnostic_json(record.diagnostic)},
            {"firstFrameIndex", JsonValue(static_cast<std::int64_t>(record.first_frame_index))},
            {"frameIndex", JsonValue(static_cast<std::int64_t>(record.frame_index))},
            {"occurrenceCount", JsonValue(static_cast<std::int64_t>(record.occurrence_count))},
            {"sequence", JsonValue(static_cast<std::int64_t>(record.sequence))},
        }));
    }
    return object({
        {"droppedCount", JsonValue(static_cast<std::int64_t>(value.dropped_count))},
        {"frameIndex", JsonValue(static_cast<std::int64_t>(value.frame_index))},
        {"records", array(std::move(records))},
    });
}

} // namespace strata::runtime
