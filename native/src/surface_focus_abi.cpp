#include <strata/strata.h>

#include <optional>
#include <string>
#include <string_view>

#include "abi_internal.hpp"
#include "abi_support.hpp"
#include "core/utf8.hpp"

namespace {

[[nodiscard]] strata_result surface_failure(
    const strata_surface& surface,
    const strata_status status,
    const char* const code,
    const char* const message
) noexcept {
    return strata::abi_detail::runtime_failure(*surface.owner, status, code, message);
}

} // namespace

extern "C" {

strata_result strata_surface_set_focus_containment(
    strata_surface* const surface,
    const strata_string_view key,
    uint32_t* const out_contained
) {
    if (surface == nullptr) return strata::abi_detail::invalid_argument();
    if (surface->release_packet_prepared) {
        return strata::abi_detail::terminal_surface_failure(*surface);
    }
    if (out_contained != nullptr) *out_contained = 0U;
    if (out_contained == nullptr || !strata::abi_detail::valid_view(key, true)) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_FOCUS_CONTAINMENT",
            "Focus containment requires an optional key and output pointer."
        );
    }
    try {
        if (key.size == 0U) {
            static_cast<void>(surface->core.set_focus_containment(std::nullopt));
            return strata::core::result(STRATA_STATUS_OK);
        }
        const std::string value = strata::abi_detail::copied_string(key);
        if (!strata::core::valid_utf8(value)) {
            return surface_failure(
                *surface,
                STRATA_STATUS_INVALID_ARGUMENT,
                "STRATA.FOCUS.INVALID_CONTAINMENT_KEY",
                "Focus containment key is not valid UTF-8."
            );
        }
        const bool contained = surface->core.set_focus_containment(std::string_view(value));
        if (!contained) {
            return surface_failure(
                *surface,
                STRATA_STATUS_NOT_FOUND,
                "STRATA.FOCUS.CONTAINMENT_TARGET_NOT_FOUND",
                "Focus containment target is not retained on this surface."
            );
        }
        *out_contained = 1U;
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.UNCAUGHT_EXCEPTION",
            "Focus containment failed inside the C ABI boundary."
        );
    }
}

} // extern "C"
