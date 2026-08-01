#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace strata::runtime {

enum class DiagnosticSeverity { info, warning, error };

struct DiagnosticPosition final {
    std::uint32_t line = 1U;
    std::uint32_t column = 1U;
    std::optional<std::uint64_t> offset;

    [[nodiscard]] friend bool operator==(
        const DiagnosticPosition&,
        const DiagnosticPosition&
    ) = default;
};

struct DiagnosticRange final {
    std::string source_id;
    DiagnosticPosition start;
    DiagnosticPosition end;

    [[nodiscard]] friend bool operator==(const DiagnosticRange&, const DiagnosticRange&) = default;
};

/** Language-neutral diagnostic retained by a runtime instance. */
struct RuntimeDiagnostic final {
    std::string code;
    std::string message;
    std::string path;
    std::optional<std::string> expected;
    DiagnosticSeverity severity = DiagnosticSeverity::error;
    std::optional<DiagnosticRange> range = std::nullopt;

    [[nodiscard]] friend bool operator==(
        const RuntimeDiagnostic&,
        const RuntimeDiagnostic&
    ) = default;
};

struct RuntimeDiagnosticRecord final {
    std::uint64_t sequence = 0U;
    std::uint64_t first_frame_index = 0U;
    std::uint64_t frame_index = 0U;
    std::size_t occurrence_count = 0U;
    RuntimeDiagnostic diagnostic;
};

struct RuntimeDiagnosticsSnapshot final {
    std::uint64_t frame_index = 0U;
    std::vector<RuntimeDiagnosticRecord> records;
    std::uint64_t dropped_count = 0U;
};

using DiagnosticReporter = std::function<void(RuntimeDiagnostic)>;

/**
 * Frame-thread diagnostic history with bounded retention and nearby-occurrence aggregation.
 * Producers decide at which frame boundary a diagnostic becomes observable; the store owns only
 * canonical ordering, aggregation, and bounded history.
 */
class RuntimeDiagnosticStore final {
public:
    static constexpr std::size_t default_max_records = 256U;
    static constexpr std::uint64_t aggregation_frame_window = 300U;

    explicit RuntimeDiagnosticStore(std::size_t max_records = default_max_records);

    /** Appends or aggregates and returns the canonical retained record after the mutation. */
    [[nodiscard]] const RuntimeDiagnosticRecord& append(
        std::uint64_t frame_index,
        RuntimeDiagnostic diagnostic
    );
    [[nodiscard]] RuntimeDiagnosticsSnapshot snapshot(std::uint64_t frame_index) const;
    [[nodiscard]] std::uint64_t dropped_count() const noexcept;
    void clear() noexcept;

private:
    std::size_t max_records_;
    std::deque<RuntimeDiagnosticRecord> records_;
    std::uint64_t next_sequence_ = 1U;
    std::uint64_t dropped_count_ = 0U;
};

} // namespace strata::runtime
