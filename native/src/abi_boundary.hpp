#pragma once

#include <cstdint>
#include <exception>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <strata/strata.h>

#include "base/status.hpp"
#include "core/diagnostics.hpp"
#include "data/json.hpp"

/*
 * The one exception firewall of the C ABI. No C++ exception may unwind through an exported
 * function: every export either cannot throw or runs its body through these helpers, which map the
 * escaped exception to a status (and, when a runtime is reachable, one diagnostic).
 */
namespace strata::abi_detail {

// The engine reports base::Status and core::DiagnosticSeverity; their values are the ABI codes.
static_assert(static_cast<strata_status>(base::Status::ok) == STRATA_STATUS_OK);
static_assert(static_cast<strata_status>(base::Status::invalid_argument) == STRATA_STATUS_INVALID_ARGUMENT);
static_assert(static_cast<strata_status>(base::Status::unsupported_abi) == STRATA_STATUS_UNSUPPORTED_ABI);
static_assert(static_cast<strata_status>(base::Status::unsupported_capability) == STRATA_STATUS_UNSUPPORTED_CAPABILITY);
static_assert(static_cast<strata_status>(base::Status::invalid_utf8) == STRATA_STATUS_INVALID_UTF8);
static_assert(static_cast<strata_status>(base::Status::clock_regression) == STRATA_STATUS_CLOCK_REGRESSION);
static_assert(static_cast<strata_status>(base::Status::out_of_memory) == STRATA_STATUS_OUT_OF_MEMORY);
static_assert(static_cast<strata_status>(base::Status::invariant_failure) == STRATA_STATUS_INVARIANT_FAILURE);
static_assert(static_cast<strata_status>(base::Status::internal_error) == STRATA_STATUS_INTERNAL_ERROR);
static_assert(static_cast<strata_status>(base::Status::not_found) == STRATA_STATUS_NOT_FOUND);
static_assert(static_cast<strata_status>(base::Status::compile_failed) == STRATA_STATUS_COMPILE_FAILED);
static_assert(static_cast<strata_status>(base::Status::activation_rejected) == STRATA_STATUS_ACTIVATION_REJECTED);
static_assert(static_cast<strata_status>(base::Status::service_unavailable) == STRATA_STATUS_SERVICE_UNAVAILABLE);
static_assert(static_cast<strata_status>(base::Status::input_queue_full) == STRATA_STATUS_INPUT_QUEUE_FULL);
static_assert(static_cast<strata_status>(base::Status::reentrant_call) == STRATA_STATUS_REENTRANT_CALL);
static_assert(static_cast<strata_status>(base::Status::wrong_thread) == STRATA_STATUS_WRONG_THREAD);
static_assert(static_cast<strata_diagnostic_severity>(core::DiagnosticSeverity::info) ==
              STRATA_DIAGNOSTIC_INFO);
static_assert(static_cast<strata_diagnostic_severity>(core::DiagnosticSeverity::warning) ==
              STRATA_DIAGNOSTIC_WARNING);
static_assert(static_cast<strata_diagnostic_severity>(core::DiagnosticSeverity::error) ==
              STRATA_DIAGNOSTIC_ERROR);
static_assert(static_cast<strata_diagnostic_severity>(core::DiagnosticSeverity::fatal) ==
              STRATA_DIAGNOSTIC_FATAL);

[[nodiscard]] constexpr strata_result result(const strata_status status,
                                             const std::uint64_t diagnostic_id = 0U) noexcept {
    return strata_result{status, 0U, diagnostic_id};
}

[[nodiscard]] constexpr strata_result to_abi(const base::Result value) noexcept {
    return result(static_cast<strata_status>(value.status), value.diagnostic_id);
}

[[nodiscard]] constexpr base::Status to_status(const strata_status status) noexcept {
    return static_cast<base::Status>(status);
}

[[nodiscard]] constexpr strata_string_view to_view(const std::string_view value) noexcept {
    return strata_string_view{value.data(), value.size()};
}

/** Hands one engine diagnostic to a host sink, containing whatever the callback throws. */
inline void deliver(const strata_diagnostic_sink& sink,
                    const core::DiagnosticEvent& event) noexcept {
    if (sink.emit == nullptr || sink.struct_size < sizeof(strata_diagnostic_sink))
        return;
    const strata_diagnostic diagnostic{
        sizeof(strata_diagnostic),
        event.id,
        static_cast<strata_diagnostic_severity>(event.severity),
        STRATA_DIAGNOSTIC_VERSION_CURRENT,
        to_view(event.code),
        to_view(event.message),
        to_view(event.source_id),
        strata_source_range{event.range.byte_start, event.range.byte_end, event.range.line_start,
                            event.range.column_start, event.range.line_end,
                            event.range.column_end},
        event.occurrence_count,
        event.sequence,
        event.first_frame_index,
        event.frame_index,
        event.dropped_count,
        to_view(event.component_path),
        to_view(event.expected),
    };
    try {
        sink.emit(sink.user_data, &diagnostic);
    } catch (...) {
        /* A foreign callback cannot be allowed to unwind through the C ABI. */
    }
}

/** The engine-side sink publishing to a host's C callback. */
[[nodiscard]] inline core::DiagnosticSink diagnostic_sink(const strata_diagnostic_sink& sink) {
    if (sink.emit == nullptr || sink.struct_size < sizeof(strata_diagnostic_sink))
        return {};
    return [sink](const core::DiagnosticEvent& event) { deliver(sink, event); };
}

/** Emits one diagnostic through a runtime's store with ABI status and severity codes. */
[[nodiscard]] inline strata_result
emit(core::Diagnostics& diagnostics, const strata_status status,
     const strata_diagnostic_severity severity, const std::string_view code,
     const std::string_view message, const std::string_view source_id = {},
     const strata_source_range* const range = nullptr, const std::string_view component_path = {},
     const std::string_view expected = {}) noexcept {
    core::SourceRange converted;
    if (range != nullptr) {
        converted = core::SourceRange{range->byte_start, range->byte_end,   range->line_start,
                                      range->column_start, range->line_end, range->column_end};
    }
    return to_abi(diagnostics.emit(to_status(status),
                                   static_cast<core::DiagnosticSeverity>(severity), code, message,
                                   source_id, range != nullptr ? &converted : nullptr,
                                   component_path, expected));
}

/** Reports a failure before any runtime, and so any diagnostic store, exists. */
[[nodiscard]] inline strata_result emit_without_runtime(const strata_diagnostic_sink& sink,
                                                        const strata_status status,
                                                        const std::string_view code,
                                                        const std::string_view message) noexcept {
    constexpr std::uint64_t diagnostic_id = 1U;
    core::DiagnosticEvent event;
    event.id = diagnostic_id;
    event.severity = core::DiagnosticSeverity::error;
    event.code = code;
    event.message = message;
    event.occurrence_count = 1U;
    event.sequence = diagnostic_id;
    deliver(sink, event);
    return result(status, diagnostic_id);
}

/** Exception carrying the exact status and diagnostic code an ABI call reports. */
class StatusError final : public std::runtime_error {
  public:
    StatusError(const strata_status status, const std::string_view code, std::string message)
        : std::runtime_error(std::move(message)), status_(status), code_(code) {}

    [[nodiscard]] strata_status status() const noexcept {
        return status_;
    }

    [[nodiscard]] std::string_view code() const noexcept {
        return code_;
    }

  private:
    strata_status status_;
    std::string_view code_;
};

/** Names one ABI operation for the diagnostic its exception firewall emits. */
struct AbiCall final {
    /** Sentence subject such as "Surface framing"; used in out-of-memory and internal messages. */
    std::string_view operation;
    /** Code for argument errors the core reports by throwing `std::invalid_argument`. */
    std::string_view invalid_code = "STRATA.ABI.INVALID_ARGUMENT";
    /** Code for strict-JSON parse failures; empty reuses `invalid_code`. */
    std::string_view json_code = {};
};

/** The status, diagnostic code, and detail one escaped exception maps to. */
struct CaughtException final {
    strata_status status = STRATA_STATUS_INTERNAL_ERROR;
    std::string_view code = "STRATA.ABI.UNCAUGHT_EXCEPTION";
    /** `what()` of the handled exception; valid while the enclosing handler is active. */
    const char* detail = nullptr;
};

/**
 * Classifies the exception currently being handled. Call only from inside a `catch` handler.
 * Allocation failure is out-of-memory, argument and range errors are invalid arguments, a
 * StatusError carries its own status, and everything else is an internal error.
 */
[[nodiscard]] inline CaughtException classify_current_exception(const AbiCall& call) noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        return CaughtException{STRATA_STATUS_OUT_OF_MEMORY, "STRATA.CORE.OUT_OF_MEMORY", nullptr};
    } catch (const StatusError& error) {
        return CaughtException{error.status(), error.code(), error.what()};
    } catch (const data::JsonError& error) {
        return CaughtException{STRATA_STATUS_INVALID_ARGUMENT,
                               call.json_code.empty() ? call.invalid_code : call.json_code,
                               error.what()};
    } catch (const std::invalid_argument& error) {
        return CaughtException{STRATA_STATUS_INVALID_ARGUMENT, call.invalid_code, error.what()};
    } catch (const std::out_of_range& error) {
        return CaughtException{STRATA_STATUS_INVALID_ARGUMENT, call.invalid_code, error.what()};
    } catch (const std::length_error& error) {
        return CaughtException{STRATA_STATUS_INVALID_ARGUMENT, call.invalid_code, error.what()};
    } catch (const std::domain_error& error) {
        return CaughtException{STRATA_STATUS_INVALID_ARGUMENT, call.invalid_code, error.what()};
    } catch (const std::exception& error) {
        return CaughtException{STRATA_STATUS_INTERNAL_ERROR, "STRATA.ABI.UNCAUGHT_EXCEPTION",
                               error.what()};
    } catch (...) {
        return CaughtException{};
    }
}

/** Emits the one diagnostic for the exception currently being handled and returns its result. */
[[nodiscard]] inline strata_result report_current_exception(core::Diagnostics& diagnostics,
                                                            const AbiCall& call) noexcept {
    const CaughtException caught = classify_current_exception(call);
    try {
        std::string message;
        if (caught.status == STRATA_STATUS_OUT_OF_MEMORY) {
            message = std::string(call.operation) + " exhausted memory.";
        } else if (caught.status == STRATA_STATUS_INTERNAL_ERROR &&
                   caught.code == "STRATA.ABI.UNCAUGHT_EXCEPTION") {
            message = std::string(call.operation) +
                      (caught.detail != nullptr
                           ? std::string(" failed inside the C ABI exception boundary: ") +
                                 caught.detail
                           : std::string(" failed inside the C ABI exception boundary with a "
                                         "non-standard exception."));
        } else {
            message = caught.detail != nullptr ? caught.detail : std::string(call.operation);
        }
        return emit(diagnostics, caught.status, STRATA_DIAGNOSTIC_ERROR, caught.code, message);
    } catch (...) {
        // Formatting the message itself failed; report the classification without detail.
        return emit(diagnostics, caught.status, STRATA_DIAGNOSTIC_ERROR, caught.code,
                    call.operation);
    }
}

/** Firewall for calls with no diagnostic channel: the escaped exception becomes a bare status. */
template <typename Body>
[[nodiscard]] strata_result guarded(Body&& body, const AbiCall& call = AbiCall{}) noexcept {
    try {
        return std::forward<Body>(body)();
    } catch (...) {
        return result(classify_current_exception(call).status);
    }
}

/** Firewall for value-returning calls whose contract reports failure as the caller's fallback. */
template <typename Value, typename Body>
[[nodiscard]] Value guarded_value(const Value fallback, Body&& body) noexcept {
    try {
        return std::forward<Body>(body)();
    } catch (...) {
        return fallback;
    }
}

} // namespace strata::abi_detail
