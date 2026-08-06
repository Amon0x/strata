#include <strata/win32.h>

#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include <strata/strata.hpp>
#include <strata/win32.hpp>

namespace {

thread_local std::string last_error;

[[nodiscard]] strata_adapter_result result(
    const strata_status status,
    std::string message = {}
) noexcept {
    last_error = std::move(message);
    return {
        status,
        0U,
        strata_string_view{last_error.data(), last_error.size()},
    };
}

[[nodiscard]] strata_adapter_result ok() noexcept {
    return result(STRATA_STATUS_OK);
}

template <typename Operation>
[[nodiscard]] strata_adapter_result guarded(Operation&& operation) noexcept {
    try {
        std::forward<Operation>(operation)();
        return ok();
    } catch (const strata::AbiError& error) {
        return result(error.status(), error.what());
    } catch (const std::invalid_argument& error) {
        return result(STRATA_STATUS_INVALID_ARGUMENT, error.what());
    } catch (const std::bad_alloc& error) {
        return result(STRATA_STATUS_OUT_OF_MEMORY, error.what());
    } catch (const std::logic_error& error) {
        return result(STRATA_STATUS_INVARIANT_FAILURE, error.what());
    } catch (const std::exception& error) {
        return result(STRATA_STATUS_INTERNAL_ERROR, error.what());
    } catch (...) {
        return result(STRATA_STATUS_INTERNAL_ERROR, "unknown Win32 adapter failure");
    }
}

} // namespace

struct strata_win32_input_adapter final {
    std::unique_ptr<strata::win32::InputAdapter> value;
};

extern "C" {

strata_adapter_result strata_win32_input_adapter_create(
    const strata_win32_input_options* const options,
    strata_win32_input_adapter** const out_adapter
) {
    if (out_adapter != nullptr) *out_adapter = nullptr;
    return guarded([&] {
        if (out_adapter == nullptr) {
            throw std::invalid_argument("Win32 input adapter output is null");
        }
        strata::win32::InputAdapterOptions native_options;
        if (options != nullptr) {
            if (options->struct_size < sizeof(strata_win32_input_options)) {
                throw std::invalid_argument("Win32 input adapter options are incomplete");
            }
            native_options.coordinate_scale = options->coordinate_scale;
            native_options.manage_pointer_capture = options->manage_pointer_capture != 0U;
            native_options.focus_on_pointer_press = options->focus_on_pointer_press != 0U;
            native_options.consume_system_keys = options->consume_system_keys != 0U;
            if (options->clock.now_nanoseconds != nullptr) {
                if (options->clock.struct_size < sizeof(strata_clock)) {
                    throw std::invalid_argument("Win32 input adapter clock is incomplete");
                }
                const strata_clock clock = options->clock;
                native_options.clock = [clock] {
                    return clock.now_nanoseconds(clock.user_data);
                };
            }
        }
        auto adapter = std::make_unique<strata_win32_input_adapter>();
        adapter->value = std::make_unique<strata::win32::InputAdapter>(
            std::move(native_options)
        );
        *out_adapter = adapter.release();
    });
}

void strata_win32_input_adapter_destroy(strata_win32_input_adapter* const adapter) {
    delete adapter;
}

strata_adapter_result strata_win32_input_adapter_handle(
    strata_win32_input_adapter* const adapter,
    strata_surface* const surface,
    void* const window,
    const uint32_t message,
    const uintptr_t word_parameter,
    const intptr_t long_parameter,
    strata_win32_message_result* const out_result
) {
    if (out_result != nullptr) *out_result = strata_win32_message_result{};
    return guarded([&] {
        if (adapter == nullptr || out_result == nullptr) {
            throw std::invalid_argument("Win32 input handling arguments are incomplete");
        }
        const std::optional<std::intptr_t> handled = adapter->value->handle(
            surface,
            window,
            message,
            word_parameter,
            long_parameter
        );
        out_result->handled = handled.has_value() ? 1U : 0U;
        out_result->result = handled.value_or(0);
    });
}

strata_adapter_result strata_win32_input_adapter_set_coordinate_scale(
    strata_win32_input_adapter* const adapter,
    const double scale
) {
    return guarded([&] {
        if (adapter == nullptr) throw std::invalid_argument("Win32 input adapter is null");
        adapter->value->set_coordinate_scale(scale);
    });
}

strata_adapter_result strata_win32_input_adapter_get_coordinate_scale(
    const strata_win32_input_adapter* const adapter,
    double* const out_scale
) {
    if (out_scale != nullptr) *out_scale = 0.0;
    return guarded([&] {
        if (adapter == nullptr || out_scale == nullptr) {
            throw std::invalid_argument("Win32 input scale query arguments are incomplete");
        }
        *out_scale = adapter->value->coordinate_scale();
    });
}

void strata_win32_input_adapter_reset(
    strata_win32_input_adapter* const adapter,
    void* const window
) {
    if (adapter != nullptr) adapter->value->reset(window);
}

} // extern "C"
