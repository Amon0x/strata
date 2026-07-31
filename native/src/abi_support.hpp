#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <strata/strata.h>

#include "abi_internal.hpp"
#include "core/diagnostics.hpp"
#include "data/json.hpp"
#include "runtime/value.hpp"

namespace strata::abi_detail {

inline constexpr strata_capabilities capabilities =
    STRATA_CAPABILITY_CORE_LIFECYCLE |
    STRATA_CAPABILITY_CUSTOM_ALLOCATOR |
    STRATA_CAPABILITY_CALLER_CLOCK |
    STRATA_CAPABILITY_IMMUTABLE_SNAPSHOTS |
    STRATA_CAPABILITY_STABLE_IDENTITIES |
    STRATA_CAPABILITY_HOST_SNAPSHOTS |
    STRATA_CAPABILITY_VALUE_JSON |
    STRATA_CAPABILITY_APPLICATION_LIFECYCLE |
    STRATA_CAPABILITY_COMPILER_ACTIVATION |
    STRATA_CAPABILITY_ACTION_DISPATCH |
    STRATA_CAPABILITY_RESOURCE_ADAPTER |
    STRATA_CAPABILITY_CLIPBOARD_IME_ADAPTER |
    STRATA_CAPABILITY_EFFECT_ADAPTER |
    STRATA_CAPABILITY_SURFACE_RUNTIME |
    STRATA_CAPABILITY_SURFACE_RENDER_PACKET |
    STRATA_CAPABILITY_SURFACE_EXTENSIONS |
    STRATA_CAPABILITY_ALLOCATOR_TELEMETRY |
    STRATA_CAPABILITY_SURFACE_RESOURCE_RELOAD |
    STRATA_CAPABILITY_SOURCE_MAP_LOOKUP |
    STRATA_CAPABILITY_SURFACE_EVENT_DRAIN |
    STRATA_CAPABILITY_DIAGNOSTIC_SNAPSHOTS |
    STRATA_CAPABILITY_PROFILER_SNAPSHOTS |
    STRATA_CAPABILITY_SURFACE_THEMES |
    STRATA_CAPABILITY_COMPILED_MODULE_ACTIVATION |
    STRATA_CAPABILITY_DURABLE_STATE |
    STRATA_CAPABILITY_ASYNC_HOST_DATA;

[[nodiscard]] inline strata_diagnostic_sink empty_sink() noexcept {
    return strata_diagnostic_sink{sizeof(strata_diagnostic_sink), nullptr, nullptr};
}

[[nodiscard]] inline strata_result invalid_argument() noexcept {
    return core::result(STRATA_STATUS_INVALID_ARGUMENT);
}

[[nodiscard]] inline strata_result runtime_failure(
    strata_runtime& runtime,
    const strata_status status,
    const char* const code,
    const char* const message
) noexcept {
    return runtime.core.diagnostics().emit(status, STRATA_DIAGNOSTIC_ERROR, code, message);
}

[[nodiscard]] inline strata_result terminal_surface_failure(
    const strata_surface& surface
) noexcept {
    return runtime_failure(
        *surface.owner,
        STRATA_STATUS_INVALID_ARGUMENT,
        "STRATA.SURFACE.TERMINAL_RELEASE_PREPARED",
        "The Surface has prepared its terminal release packet; only that packet may be read again before release."
    );
}

[[nodiscard]] inline bool valid_view(
    const strata_string_view value,
    const bool allow_empty
) noexcept {
    return (value.data != nullptr || value.size == 0U) && (allow_empty || value.size != 0U);
}

[[nodiscard]] inline std::string copied_string(const strata_string_view value) {
    return value.size == 0U ? std::string{} : std::string(value.data, value.size);
}

[[nodiscard]] inline strata_activation_status activation_status(
    const runtime::ActivationStatus status
) noexcept {
    using runtime::ActivationStatus;
    switch (status) {
    case ActivationStatus::activated: return STRATA_ACTIVATION_ACTIVATED;
    case ActivationStatus::rejected_generation: return STRATA_ACTIVATION_REJECTED_GENERATION;
    case ActivationStatus::rejected_compile: return STRATA_ACTIVATION_REJECTED_COMPILE;
    case ActivationStatus::rejected_unit: return STRATA_ACTIVATION_REJECTED_UNIT;
    case ActivationStatus::rejected_capability: return STRATA_ACTIVATION_REJECTED_CAPABILITY;
    }
    return STRATA_ACTIVATION_REJECTED_UNIT;
}

[[nodiscard]] inline strata_action_dispatch_status dispatch_status(
    const runtime::ActionDispatchStatus status
) noexcept {
    using runtime::ActionDispatchStatus;
    switch (status) {
    case ActionDispatchStatus::no_action: return STRATA_ACTION_DISPATCH_NO_ACTION;
    case ActionDispatchStatus::handled: return STRATA_ACTION_DISPATCH_HANDLED;
    case ActionDispatchStatus::forwarded: return STRATA_ACTION_DISPATCH_FORWARDED;
    case ActionDispatchStatus::ignored: return STRATA_ACTION_DISPATCH_IGNORED;
    case ActionDispatchStatus::unhandled: return STRATA_ACTION_DISPATCH_UNHANDLED;
    case ActionDispatchStatus::failed: return STRATA_ACTION_DISPATCH_FAILED;
    }
    return STRATA_ACTION_DISPATCH_FAILED;
}

[[nodiscard]] inline runtime::Value decoded_value(
    const strata_string_view json,
    const bool empty_is_null
) {
    if (json.size == 0U && empty_is_null) return runtime::Value{};
    if (!valid_view(json, false)) throw std::invalid_argument("JSON value view is invalid");
    return runtime::value_from_json(data::parse_json(copied_string(json)));
}

[[nodiscard]] inline bool valid_action_dispatch_config(
    const strata_action_dispatch_config* const config
) noexcept {
    constexpr std::size_t version_one_size = offsetof(strata_action_dispatch_config, dynamic);
    if (config == nullptr || config->struct_size < version_one_size ||
        !valid_view(config->action_id, false) || !valid_view(config->payload_json, true) ||
        !valid_view(config->event_kind, false) || !valid_view(config->source_key, true) ||
        !valid_view(config->event_value_json, true) || !valid_view(config->state_scope, true)) {
        return false;
    }
    return config->struct_size < sizeof(strata_action_dispatch_config) ||
        (config->dynamic <= 1U && config->reserved == 0U);
}

[[nodiscard]] inline bool dynamic_action_dispatch(
    const strata_action_dispatch_config& config
) noexcept {
    return config.struct_size >= sizeof(strata_action_dispatch_config) && config.dynamic != 0U;
}

} // namespace strata::abi_detail
