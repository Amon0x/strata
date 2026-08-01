#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <strata/strata.h>

namespace strata {

enum class DiagnosticSeverity { info, warning, error, fatal };

struct SourceRange final {
    std::uint64_t byte_start = 0U;
    std::uint64_t byte_end = 0U;
    std::uint32_t line_start = 0U;
    std::uint32_t column_start = 0U;
    std::uint32_t line_end = 0U;
    std::uint32_t column_end = 0U;
};

/** Owned diagnostic copied out of the borrowed C ABI callback. */
struct Diagnostic final {
    std::uint64_t id = 0U;
    DiagnosticSeverity severity = DiagnosticSeverity::error;
    std::string code;
    std::string message;
    std::optional<std::string> source_id;
    std::optional<SourceRange> range;
    std::uint64_t occurrence_count = 0U;
    std::uint64_t sequence = 0U;
    std::uint64_t first_frame_index = 0U;
    std::uint64_t frame_index = 0U;
    std::uint64_t dropped_count = 0U;
    std::optional<std::string> component_path;
    std::optional<std::string> expected;
};

struct DiagnosticsSnapshot final {
    std::uint64_t frame_index = 0U;
    std::uint64_t dropped_count = 0U;
    std::vector<Diagnostic> records;
};

[[nodiscard]] constexpr const char* status_name(const strata_status status) noexcept {
    switch (status) {
    case STRATA_STATUS_OK: return "ok";
    case STRATA_STATUS_INVALID_ARGUMENT: return "invalid argument";
    case STRATA_STATUS_UNSUPPORTED_ABI: return "unsupported ABI";
    case STRATA_STATUS_UNSUPPORTED_CAPABILITY: return "unsupported capability";
    case STRATA_STATUS_INVALID_UTF8: return "invalid UTF-8";
    case STRATA_STATUS_CLOCK_REGRESSION: return "clock regression";
    case STRATA_STATUS_OUT_OF_MEMORY: return "out of memory";
    case STRATA_STATUS_INVARIANT_FAILURE: return "invariant failure";
    case STRATA_STATUS_INTERNAL_ERROR: return "internal error";
    case STRATA_STATUS_NOT_FOUND: return "not found";
    case STRATA_STATUS_COMPILE_FAILED: return "compile failed";
    case STRATA_STATUS_ACTIVATION_REJECTED: return "activation rejected";
    case STRATA_STATUS_SERVICE_UNAVAILABLE: return "service unavailable";
    case STRATA_STATUS_INPUT_QUEUE_FULL: return "input queue full";
    default: return "unknown status";
    }
}

} // namespace strata
