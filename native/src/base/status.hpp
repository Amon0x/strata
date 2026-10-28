#pragma once

#include <cstdint>

namespace strata::base {

/**
 * Outcome of an engine operation. The numeric values are the C ABI's `strata_status` codes, which
 * the ABI layer checks at compile time, so hosts see the same code the engine reports.
 */
enum class Status : std::uint32_t {
    ok = 0U,
    invalid_argument = 1U,
    unsupported_abi = 2U,
    unsupported_capability = 3U,
    invalid_utf8 = 4U,
    clock_regression = 5U,
    out_of_memory = 6U,
    invariant_failure = 7U,
    internal_error = 8U,
    not_found = 9U,
    compile_failed = 10U,
    activation_rejected = 11U,
    service_unavailable = 12U,
    input_queue_full = 13U,
    reentrant_call = 14U,
    wrong_thread = 15U,
};

/** A status and the diagnostic that explains it, or zero when none was published. */
struct Result final {
    Status status = Status::ok;
    std::uint64_t diagnostic_id = 0U;

    [[nodiscard]] constexpr bool ok() const noexcept { return status == Status::ok; }
    [[nodiscard]] friend constexpr bool operator==(const Result&, const Result&) = default;
};

[[nodiscard]] constexpr Result result(const Status status, const std::uint64_t diagnostic_id = 0U) noexcept {
    return Result{status, diagnostic_id};
}

} // namespace strata::base
