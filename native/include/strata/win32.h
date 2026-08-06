#ifndef STRATA_WIN32_H
#define STRATA_WIN32_H

#include <strata/adapter.h>

#if defined(_WIN32) && defined(STRATA_WIN32_SHARED)
#if defined(STRATA_WIN32_BUILD)
#define STRATA_WIN32_API __declspec(dllexport)
#else
#define STRATA_WIN32_API __declspec(dllimport)
#endif
#else
#define STRATA_WIN32_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct strata_win32_input_adapter strata_win32_input_adapter;

typedef struct strata_win32_input_options {
    size_t struct_size;
    strata_clock clock;
    double coordinate_scale;
    uint32_t manage_pointer_capture;
    uint32_t focus_on_pointer_press;
    uint32_t consume_system_keys;
    uint32_t reserved;
} strata_win32_input_options;

typedef struct strata_win32_message_result {
    uint32_t handled;
    uint32_t reserved;
    intptr_t result;
} strata_win32_message_result;

STRATA_WIN32_API strata_adapter_result strata_win32_input_adapter_create(
    const strata_win32_input_options* options,
    strata_win32_input_adapter** out_adapter
);
STRATA_WIN32_API void strata_win32_input_adapter_destroy(
    strata_win32_input_adapter* adapter
);
STRATA_WIN32_API strata_adapter_result strata_win32_input_adapter_handle(
    strata_win32_input_adapter* adapter,
    strata_surface* surface,
    void* window,
    uint32_t message,
    uintptr_t word_parameter,
    intptr_t long_parameter,
    strata_win32_message_result* out_result
);
STRATA_WIN32_API strata_adapter_result strata_win32_input_adapter_set_coordinate_scale(
    strata_win32_input_adapter* adapter,
    double scale
);
STRATA_WIN32_API strata_adapter_result strata_win32_input_adapter_get_coordinate_scale(
    const strata_win32_input_adapter* adapter,
    double* out_scale
);
STRATA_WIN32_API void strata_win32_input_adapter_reset(
    strata_win32_input_adapter* adapter,
    void* window
);

#ifdef __cplusplus
}
#endif

#endif
