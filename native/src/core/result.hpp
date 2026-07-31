#pragma once

#include <cstdint>

#include <strata/strata.h>

namespace strata::core {

[[nodiscard]] constexpr strata_result result(
    const strata_status status,
    const std::uint64_t diagnostic_id = 0U
) noexcept {
    return strata_result{status, 0U, diagnostic_id};
}

} // namespace strata::core
