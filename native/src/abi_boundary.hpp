#pragma once

#include <exception>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <strata/strata.h>

#include "core/diagnostics.hpp"
#include "core/result.hpp"
#include "data/json.hpp"

/*
 * The one exception firewall of the C ABI. No C++ exception may unwind through an exported
 * function: every export either cannot throw or runs its body through these helpers, which map the
 * escaped exception to a status (and, when a runtime is reachable, one diagnostic).
 */
namespace strata::abi_detail {

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
        return diagnostics.emit(caught.status, STRATA_DIAGNOSTIC_ERROR, caught.code, message);
    } catch (...) {
        // Formatting the message itself failed; report the classification without detail.
        return diagnostics.emit(caught.status, STRATA_DIAGNOSTIC_ERROR, caught.code,
                                call.operation);
    }
}

/** Firewall for calls with no diagnostic channel: the escaped exception becomes a bare status. */
template <typename Body>
[[nodiscard]] strata_result guarded(Body&& body, const AbiCall& call = AbiCall{}) noexcept {
    try {
        return std::forward<Body>(body)();
    } catch (...) {
        return core::result(classify_current_exception(call).status);
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
