#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include <strata/strata.h>

#include "core/result.hpp"
#include "runtime/diagnostic.hpp"

namespace strata::core {

class Diagnostics final {
public:
    explicit Diagnostics(strata_diagnostic_sink sink) noexcept;

    [[nodiscard]] strata_result emit(
        strata_status status,
        strata_diagnostic_severity severity,
        std::string_view code,
        std::string_view message,
        std::string_view source_id = {},
        const strata_source_range* range = nullptr,
        std::string_view component_path = {},
        std::string_view expected = {}
    ) noexcept;

    /** Publishes a record already canonicalized by RuntimeDiagnosticStore without re-aggregating. */
    [[nodiscard]] strata_result publish(
        const runtime::RuntimeDiagnosticRecord& record,
        std::uint64_t dropped_count
    ) noexcept;
    void clear() noexcept;

private:
    struct Fingerprint final {
        strata_diagnostic_severity severity;
        std::string code;
        std::string message;
        std::string source_id;
        std::string component_path;
        std::string expected;
        bool has_range;
        strata_source_range range;
        [[nodiscard]] bool operator<(const Fingerprint& other) const noexcept;
    };

    struct Occurrence final {
        std::uint64_t id;
        std::uint64_t count;
    };

    strata_diagnostic_sink sink_;
    std::uint64_t next_id_ = 1U;
    std::map<Fingerprint, Occurrence> occurrences_;
};

[[nodiscard]] strata_result emit_without_runtime(
    const strata_diagnostic_sink& sink,
    strata_status status,
    std::string_view code,
    std::string_view message
) noexcept;

} // namespace strata::core
