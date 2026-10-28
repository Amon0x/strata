#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <strata/strata.h>

#include "abi_boundary.hpp"
#include "abi_internal.hpp"
#include "abi_struct.hpp"
#include "core/diagnostics.hpp"
#include "data/json.hpp"
#include "runtime/value.hpp"

namespace strata::abi_detail {

inline constexpr strata_capabilities capabilities =
    STRATA_CAPABILITY_CORE_LIFECYCLE | STRATA_CAPABILITY_CUSTOM_ALLOCATOR |
    STRATA_CAPABILITY_CALLER_CLOCK | STRATA_CAPABILITY_IMMUTABLE_SNAPSHOTS |
    STRATA_CAPABILITY_STABLE_IDENTITIES | STRATA_CAPABILITY_HOST_SNAPSHOTS |
    STRATA_CAPABILITY_VALUE_JSON | STRATA_CAPABILITY_APPLICATION_LIFECYCLE |
    STRATA_CAPABILITY_COMPILER_ACTIVATION | STRATA_CAPABILITY_ACTION_DISPATCH |
    STRATA_CAPABILITY_RESOURCE_ADAPTER | STRATA_CAPABILITY_CLIPBOARD_IME_ADAPTER |
    STRATA_CAPABILITY_EFFECT_ADAPTER | STRATA_CAPABILITY_SURFACE_RUNTIME |
    STRATA_CAPABILITY_SURFACE_RENDER_PACKET | STRATA_CAPABILITY_SURFACE_EXTENSIONS |
    STRATA_CAPABILITY_ALLOCATOR_TELEMETRY | STRATA_CAPABILITY_SOURCE_MAP_LOOKUP |
    STRATA_CAPABILITY_SURFACE_EVENT_DRAIN | STRATA_CAPABILITY_DIAGNOSTIC_SNAPSHOTS |
    STRATA_CAPABILITY_PROFILER_SNAPSHOTS | STRATA_CAPABILITY_SURFACE_THEMES |
    STRATA_CAPABILITY_COMPILED_MODULE_ACTIVATION | STRATA_CAPABILITY_DURABLE_STATE |
    STRATA_CAPABILITY_ASYNC_HOST_DATA;

[[nodiscard]] inline strata_diagnostic_sink empty_sink() noexcept {
    return strata_diagnostic_sink{sizeof(strata_diagnostic_sink), nullptr, nullptr};
}

[[nodiscard]] inline strata_result invalid_argument() noexcept {
    return strata::abi_detail::result(STRATA_STATUS_INVALID_ARGUMENT);
}

[[nodiscard]] inline strata_result runtime_failure(strata_runtime& runtime,
                                                   const strata_status status,
                                                   const char* const code,
                                                   const char* const message) noexcept {
    return strata::abi_detail::emit(runtime.core.diagnostics(), status, STRATA_DIAGNOSTIC_ERROR, code, message);
}

/** Whether an ABI call may run while a host callback of the same runtime is executing. */
enum class Reentry : bool {
    /** Structural calls: they may destroy or replace state the in-progress call is using. */
    rejected,
    /** Reads and host-service calls that leave core state unchanged. */
    allowed,
};

/**
 * Admits one ABI call into a runtime. Calls may come from any thread but never overlap: a call
 * while another thread is inside the runtime is refused with STRATA_STATUS_WRONG_THREAD, and a
 * structural call from inside a host callback is refused with STRATA_STATUS_REENTRANT_CALL.
 * Neither refusal emits a diagnostic, since the diagnostic sink is itself a callback and the
 * diagnostic store belongs to the thread inside the runtime. A null runtime is admitted so the
 * export can apply its own null-handle contract.
 */
class EntryGuard final {
  public:
    EntryGuard(const strata_runtime* const runtime, const Reentry reentry) noexcept
        : runtime_(runtime) {
        if (runtime_ == nullptr)
            return;
        const std::thread::id self = std::this_thread::get_id();
        std::thread::id idle{};
        if (runtime_->active_thread.compare_exchange_strong(idle, self,
                                                            std::memory_order_acq_rel)) {
            runtime_->call_depth = 1U;
            return;
        }
        if (idle != self) {
            status_ = STRATA_STATUS_WRONG_THREAD;
        } else if (reentry == Reentry::rejected) {
            status_ = STRATA_STATUS_REENTRANT_CALL;
        } else {
            ++runtime_->call_depth;
            return;
        }
        runtime_ = nullptr;
    }

    EntryGuard(const strata_surface* const surface, const Reentry reentry) noexcept
        : EntryGuard(surface != nullptr ? surface->owner : nullptr, reentry) {}

    ~EntryGuard() {
        if (runtime_ != nullptr && --runtime_->call_depth == 0U)
            runtime_->active_thread.store(std::thread::id{}, std::memory_order_release);
    }

    EntryGuard(const EntryGuard&) = delete;
    EntryGuard& operator=(const EntryGuard&) = delete;

    [[nodiscard]] bool refused() const noexcept {
        return status_ != STRATA_STATUS_OK;
    }

    [[nodiscard]] strata_result refusal() const noexcept {
        return strata::abi_detail::result(status_);
    }

  private:
    const strata_runtime* runtime_;
    strata_status status_ = STRATA_STATUS_OK;
};

/**
 * Runtime-attached firewall: a null runtime is an invalid argument, and an exception escaping the
 * body is reported as one diagnostic on the runtime instead of unwinding through the C ABI.
 */
template <typename Runtime, typename Body>
[[nodiscard]] strata_result runtime_call(Runtime* const runtime, const AbiCall& call,
                                         Body&& body) noexcept {
    static_assert(std::is_same_v<std::remove_const_t<Runtime>, strata_runtime>);
    if (runtime == nullptr)
        return invalid_argument();
    try {
        return std::forward<Body>(body)();
    } catch (...) {
        return report_current_exception(
            const_cast<strata_runtime*>(runtime)->core.diagnostics(), call);
    }
}

/** Surface-attached firewall; failures are reported on the owning runtime. */
template <typename Surface, typename Body>
[[nodiscard]] strata_result surface_call(Surface* const surface, const AbiCall& call,
                                         Body&& body) noexcept {
    static_assert(std::is_same_v<std::remove_const_t<Surface>, strata_surface>);
    if (surface == nullptr || surface->owner == nullptr)
        return invalid_argument();
    return runtime_call(surface->owner, call, std::forward<Body>(body));
}

[[nodiscard]] inline strata_result
terminal_surface_failure(const strata_surface& surface) noexcept {
    return runtime_failure(*surface.owner, STRATA_STATUS_INVALID_ARGUMENT,
                           "STRATA.SURFACE.TERMINAL_RELEASE_PREPARED",
                           "The Surface has prepared its terminal release packet; only that packet "
                           "may be read again before release.");
}

[[nodiscard]] inline bool valid_view(const strata_string_view value,
                                     const bool allow_empty) noexcept {
    return (value.data != nullptr || value.size == 0U) && (allow_empty || value.size != 0U);
}

[[nodiscard]] inline std::string copied_string(const strata_string_view value) {
    return value.size == 0U ? std::string{} : std::string(value.data, value.size);
}

[[nodiscard]] inline strata_activation_status
activation_status(const runtime::ActivationStatus status) noexcept {
    using runtime::ActivationStatus;
    switch (status) {
    case ActivationStatus::activated:
        return STRATA_ACTIVATION_ACTIVATED;
    case ActivationStatus::rejected_generation:
        return STRATA_ACTIVATION_REJECTED_GENERATION;
    case ActivationStatus::rejected_compile:
        return STRATA_ACTIVATION_REJECTED_COMPILE;
    case ActivationStatus::rejected_unit:
        return STRATA_ACTIVATION_REJECTED_UNIT;
    case ActivationStatus::rejected_capability:
        return STRATA_ACTIVATION_REJECTED_CAPABILITY;
    }
    return STRATA_ACTIVATION_REJECTED_UNIT;
}

[[nodiscard]] inline strata_action_dispatch_status
dispatch_status(const runtime::ActionDispatchStatus status) noexcept {
    using runtime::ActionDispatchStatus;
    switch (status) {
    case ActionDispatchStatus::no_action:
        return STRATA_ACTION_DISPATCH_NO_ACTION;
    case ActionDispatchStatus::handled:
        return STRATA_ACTION_DISPATCH_HANDLED;
    case ActionDispatchStatus::forwarded:
        return STRATA_ACTION_DISPATCH_FORWARDED;
    case ActionDispatchStatus::ignored:
        return STRATA_ACTION_DISPATCH_IGNORED;
    case ActionDispatchStatus::unhandled:
        return STRATA_ACTION_DISPATCH_UNHANDLED;
    case ActionDispatchStatus::failed:
        return STRATA_ACTION_DISPATCH_FAILED;
    }
    return STRATA_ACTION_DISPATCH_FAILED;
}

[[nodiscard]] inline runtime::Value decoded_value(const strata_string_view json,
                                                  const bool empty_is_null) {
    if (json.size == 0U && empty_is_null)
        return runtime::Value{};
    if (!valid_view(json, false))
        throw std::invalid_argument("JSON value view is invalid");
    return runtime::value_from_json(data::parse_json(copied_string(json)));
}

[[nodiscard]] inline bool
valid_action_dispatch_config(const strata_action_dispatch_config* const provided) noexcept {
    const std::optional<strata_action_dispatch_config> config = read_struct(provided);
    if (!config.has_value() || !valid_view(config->action_id, false) ||
        !valid_view(config->payload_json, true) || !valid_view(config->event_kind, false) ||
        !valid_view(config->source_key, true) || !valid_view(config->event_value_json, true) ||
        !valid_view(config->state_scope, true)) {
        return false;
    }
    return config->dynamic <= 1U && config->reserved == 0U;
}

/** Whether a dispatch is dynamic; a caller layout that predates the flag dispatches statically. */
[[nodiscard]] inline bool
dynamic_action_dispatch(const strata_action_dispatch_config& provided) noexcept {
    const std::optional<strata_action_dispatch_config> config = read_struct(&provided);
    return config.has_value() && config->dynamic != 0U;
}

} // namespace strata::abi_detail
