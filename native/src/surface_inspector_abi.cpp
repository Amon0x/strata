#include <strata/strata.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "abi_internal.hpp"
#include "abi_support.hpp"
#include "core/utf8.hpp"
#include "data/json.hpp"
#include "ui/inspection.hpp"

using strata::abi_detail::AbiCall;
using strata::abi_detail::EntryGuard;
using strata::abi_detail::has_struct;
using strata::abi_detail::Reentry;
using strata::abi_detail::surface_call;

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

strata_result strata_surface_inspector_select(
    strata_surface* const surface,
    const strata_string_view key,
    uint32_t* const out_selected
) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr) return strata::abi_detail::invalid_argument();
    if (surface->release_packet_prepared) {
        return strata::abi_detail::terminal_surface_failure(*surface);
    }
    if (out_selected != nullptr) *out_selected = 0U;
    if (out_selected == nullptr || !strata::abi_detail::valid_view(key, false)) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_INSPECTOR_SELECTION",
            "Inspector selection requires a complete keyed target and output pointer."
        );
    }
    constexpr AbiCall call{"Inspector selection", "STRATA.INSPECT.INVALID_TARGET"};
    return surface_call(surface, call, [&] {
        const std::string value = strata::abi_detail::copied_string(key);
        if (!strata::core::valid_utf8(value)) throw std::invalid_argument("inspector key is not valid UTF-8");
        *out_selected = surface->core.inspect_select(value) ? 1U : 0U;
        return strata::core::result(STRATA_STATUS_OK);
    });
}

strata_result strata_surface_inspector_pick(
    strata_surface* const surface,
    const double x,
    const double y,
    uint32_t* const out_selected
) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr) return strata::abi_detail::invalid_argument();
    if (surface->release_packet_prepared) {
        return strata::abi_detail::terminal_surface_failure(*surface);
    }
    if (out_selected != nullptr) *out_selected = 0U;
    if (out_selected == nullptr || !std::isfinite(x) || !std::isfinite(y)) {
        return surface_failure(*surface, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.ABI.INVALID_INSPECTOR_PICK", "Inspector picking requires finite coordinates and an output pointer.");
    }
    return surface_call(surface, AbiCall{"Inspector picking"}, [&] {
        *out_selected = surface->core.inspect_pick(strata::ui::Point{x, y}) ? 1U : 0U;
        return strata::core::result(STRATA_STATUS_OK);
    });
}

strata_result strata_surface_inspector_clear(strata_surface* const surface) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr) return strata::abi_detail::invalid_argument();
    if (surface->release_packet_prepared) {
        return strata::abi_detail::terminal_surface_failure(*surface);
    }
    surface->core.inspect_clear();
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_surface_read_inspector_selection_json(
    const strata_surface* const surface,
    const strata_value_json_sink* const sink
) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr) return strata::abi_detail::invalid_argument();
    if (sink == nullptr || !has_struct(sink) || sink->emit == nullptr) {
        return surface_failure(*surface, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.ABI.INVALID_VALUE_SINK", "Reading inspector selection requires a complete JSON sink.");
    }
    return surface_call(surface, AbiCall{"Inspector selection delivery"}, [&] {
        const std::string json = strata::data::encode_canonical_json(
            strata::ui::inspect_selection(surface->core)
        );
        sink->emit(sink->user_data, strata_string_view{json.data(), json.size()});
        return strata::core::result(STRATA_STATUS_OK);
    });
}

strata_result strata_surface_read_inspection_node_json(
    const strata_surface* const surface,
    const strata_string_view key,
    const uint32_t depth,
    const strata_value_json_sink* const sink
) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr) return strata::abi_detail::invalid_argument();
    if (sink == nullptr || !has_struct(sink) || sink->emit == nullptr ||
        !strata::abi_detail::valid_view(key, false)) {
        return surface_failure(*surface, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.ABI.INVALID_INSPECTION_NODE", "Reading an inspection node requires a complete key and JSON sink.");
    }
    if (!surface->frame_snapshot_available) {
        return surface_failure(*surface, STRATA_STATUS_NOT_FOUND, "STRATA.SURFACE.FRAME_UNAVAILABLE", "Frame the surface before reading an inspection node.");
    }
    constexpr AbiCall call{"Inspection node delivery", "STRATA.INSPECT.INVALID_TARGET"};
    return surface_call(surface, call, [&] {
        const std::string value = strata::abi_detail::copied_string(key);
        if (!strata::core::valid_utf8(value)) throw std::invalid_argument("inspection key is not valid UTF-8");
        const std::size_t levels = depth == std::numeric_limits<uint32_t>::max()
            ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(depth);
        const std::string json = strata::data::encode_json_line(
            strata::ui::inspect_node(surface->core, value, levels)
        );
        sink->emit(sink->user_data, strata_string_view{json.data(), json.size()});
        return strata::core::result(STRATA_STATUS_OK);
    });
}

} // extern "C"
