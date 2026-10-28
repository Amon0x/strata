#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "base/diagnostic.hpp"
#include "base/status.hpp"

namespace strata::core {

/** Severity of a published diagnostic. Values match the C ABI's `strata_diagnostic_severity`. */
enum class DiagnosticSeverity : std::uint32_t { info = 0U, warning = 1U, error = 2U, fatal = 3U };

/** Byte and line/column extent of a diagnostic in its source. */
struct SourceRange final {
    std::uint64_t byte_start = 0U;
    std::uint64_t byte_end = 0U;
    std::uint32_t line_start = 0U;
    std::uint32_t column_start = 0U;
    std::uint32_t line_end = 0U;
    std::uint32_t column_end = 0U;

    [[nodiscard]] friend bool operator==(const SourceRange&, const SourceRange&) = default;
};

/** One diagnostic as published to the host. Its views are valid only for the sink call. */
struct DiagnosticEvent final {
    std::uint64_t id = 0U;
    DiagnosticSeverity severity = DiagnosticSeverity::error;
    std::string_view code;
    std::string_view message;
    std::string_view source_id;
    /** All zero when the diagnostic has no source location. */
    SourceRange range;
    std::uint64_t occurrence_count = 0U;
    std::uint64_t sequence = 0U;
    std::uint64_t first_frame_index = 0U;
    std::uint64_t frame_index = 0U;
    std::uint64_t dropped_count = 0U;
    std::string_view component_path;
    std::string_view expected;
};

/** Receives published diagnostics. Whatever it throws is contained by the publisher. */
using DiagnosticSink = std::function<void(const DiagnosticEvent&)>;

/** Delivers one event, containing any exception the sink raises. */
void deliver(const DiagnosticSink& sink, const DiagnosticEvent& event) noexcept;

class Diagnostics final {
public:
    explicit Diagnostics(DiagnosticSink sink);

    /**
     * Publishes one diagnostic and returns `status` with the diagnostic's id. Repeating an
     * identical diagnostic reuses its id and counts the occurrence.
     */
    [[nodiscard]] base::Result emit(
        base::Status status,
        DiagnosticSeverity severity,
        std::string_view code,
        std::string_view message,
        std::string_view source_id = {},
        const SourceRange* range = nullptr,
        std::string_view component_path = {},
        std::string_view expected = {}
    ) noexcept;

    /** Publishes a record already canonicalized by RuntimeDiagnosticStore without re-aggregating. */
    [[nodiscard]] base::Result publish(
        const base::RuntimeDiagnosticRecord& record,
        std::uint64_t dropped_count
    ) noexcept;
    void clear() noexcept;

private:
    struct Fingerprint final {
        DiagnosticSeverity severity;
        std::string code;
        std::string message;
        std::string source_id;
        std::string component_path;
        std::string expected;
        bool has_range;
        SourceRange range;
        [[nodiscard]] bool operator<(const Fingerprint& other) const noexcept;
    };

    struct Occurrence final {
        std::uint64_t id;
        std::uint64_t count;
    };

    DiagnosticSink sink_;
    std::uint64_t next_id_ = 1U;
    std::map<Fingerprint, Occurrence> occurrences_;
};

} // namespace strata::core
