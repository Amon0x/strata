#include <strata/strata.h>

#include <optional>
#include <string>
#include <string_view>

#include "abi_internal.hpp"
#include "abi_support.hpp"
#include "base/utf8.hpp"
#include "ui/input.hpp"

using strata::abi_detail::AbiCall;
using strata::abi_detail::EntryGuard;
using strata::abi_detail::Reentry;
using strata::abi_detail::surface_call;

namespace {

static_assert(static_cast<uint32_t>(strata::ui::PointerCursor::standard) ==
              STRATA_POINTER_CURSOR_DEFAULT);
static_assert(static_cast<uint32_t>(strata::ui::PointerCursor::text) == STRATA_POINTER_CURSOR_TEXT);
static_assert(static_cast<uint32_t>(strata::ui::PointerCursor::resize_nwse) ==
              STRATA_POINTER_CURSOR_RESIZE_NWSE);
static_assert(static_cast<uint32_t>(strata::ui::PointerCursor::none) == STRATA_POINTER_CURSOR_NONE);

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
) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr) return strata::abi_detail::invalid_argument();
    if (surface->packets.release_prepared()) {
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
    return surface_call(surface, AbiCall{"Focus containment"}, [&] {
        if (key.size == 0U) {
            static_cast<void>(surface->core.set_focus_containment(std::nullopt));
            return strata::abi_detail::result(STRATA_STATUS_OK);
        }
        const std::string value = strata::abi_detail::copied_string(key);
        if (!strata::base::valid_utf8(value)) {
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
        return strata::abi_detail::result(STRATA_STATUS_OK);
    });
}

strata_result strata_surface_pointer_cursor(
    strata_surface* const surface,
    uint32_t* const out_cursor
) noexcept {
    const EntryGuard entry(surface, Reentry::allowed);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr || out_cursor == nullptr) return strata::abi_detail::invalid_argument();
    *out_cursor = STRATA_POINTER_CURSOR_DEFAULT;
    return surface_call(surface, AbiCall{"Pointer cursor"}, [&] {
        *out_cursor = static_cast<uint32_t>(surface->core.input().cursor());
        return strata::abi_detail::result(STRATA_STATUS_OK);
    });
}

} // extern "C"
