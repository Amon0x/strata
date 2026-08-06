#ifndef STRATA_D3D11_H
#define STRATA_D3D11_H

#include <strata/adapter.h>

#if defined(_WIN32) && defined(STRATA_D3D11_SHARED)
#if defined(STRATA_D3D11_BUILD)
#define STRATA_D3D11_API __declspec(dllexport)
#else
#define STRATA_D3D11_API __declspec(dllimport)
#endif
#else
#define STRATA_D3D11_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct strata_d3d11_presenter strata_d3d11_presenter;

typedef uint32_t strata_d3d11_context_state_policy;
#define STRATA_D3D11_CONTEXT_STATE_PRESERVE UINT32_C(0)
#define STRATA_D3D11_CONTEXT_STATE_HOST_MANAGED UINT32_C(1)

typedef uint32_t strata_d3d11_target_load_action;
#define STRATA_D3D11_TARGET_PRESERVE UINT32_C(0)
#define STRATA_D3D11_TARGET_CLEAR UINT32_C(1)

typedef strata_status (*strata_d3d11_program_source_fn)(
    void* user_data,
    strata_string_view resource_id,
    strata_bytes_view* out_source
);

typedef struct strata_d3d11_presenter_options {
    size_t struct_size;
    strata_d3d11_context_state_policy context_state;
    uint32_t asynchronous_shader_compilation;
    uint32_t reserved;
    void* program_source_user_data;
    strata_d3d11_program_source_fn load_program_source;
} strata_d3d11_presenter_options;

typedef struct strata_d3d11_render_target {
    size_t struct_size;
    void* texture;
    void* render_target_view;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    double logical_width;
    double logical_height;
} strata_d3d11_render_target;

typedef struct strata_d3d11_frame_options {
    size_t struct_size;
    strata_d3d11_target_load_action load_action;
    uint32_t reserved;
    float clear_color[4];
} strata_d3d11_frame_options;

typedef struct strata_d3d11_render_telemetry {
    uint64_t blur_passes;
    uint32_t blur_target_width;
    uint32_t blur_target_height;
    uint64_t blur_nanoseconds;
    uint64_t effect_passes;
    uint32_t effect_target_width;
    uint32_t effect_target_height;
    uint64_t effect_nanoseconds;
} strata_d3d11_render_telemetry;

typedef struct strata_d3d11_presented_frame {
    size_t struct_size;
    strata_surface_frame_info surface;
    size_t packet_bytes;
    strata_d3d11_render_telemetry rendering;
} strata_d3d11_presented_frame;

STRATA_D3D11_API strata_adapter_result strata_d3d11_presenter_create(
    strata_runtime* runtime,
    void* device,
    void* immediate_context,
    const strata_d3d11_presenter_options* options,
    strata_d3d11_presenter** out_presenter
);
STRATA_D3D11_API void strata_d3d11_presenter_destroy(
    strata_d3d11_presenter* presenter
);
STRATA_D3D11_API strata_adapter_result strata_d3d11_presenter_synchronize_programs(
    strata_d3d11_presenter* presenter
);
STRATA_D3D11_API strata_adapter_result strata_d3d11_presenter_reload_program_source(
    strata_d3d11_presenter* presenter,
    strata_string_view resource_id,
    strata_bytes_view source,
    uint32_t* out_matched
);
STRATA_D3D11_API strata_adapter_result strata_d3d11_presenter_attach(
    strata_d3d11_presenter* presenter,
    strata_string_view layer_id,
    strata_surface* surface
);
STRATA_D3D11_API strata_adapter_result strata_d3d11_presenter_present(
    strata_d3d11_presenter* presenter,
    strata_string_view layer_id,
    strata_surface* surface,
    const strata_d3d11_render_target* target,
    int64_t time_nanoseconds,
    const strata_d3d11_frame_options* options,
    strata_d3d11_presented_frame* out_frame
);
STRATA_D3D11_API strata_adapter_result strata_d3d11_presenter_detach(
    strata_d3d11_presenter* presenter,
    strata_string_view layer_id
);
STRATA_D3D11_API void strata_d3d11_presenter_discard(
    strata_d3d11_presenter* presenter,
    strata_string_view layer_id
);
STRATA_D3D11_API strata_adapter_result strata_d3d11_presenter_attached(
    const strata_d3d11_presenter* presenter,
    strata_string_view layer_id,
    uint32_t* out_attached
);
STRATA_D3D11_API strata_adapter_result strata_d3d11_presenter_release_target(
    strata_d3d11_presenter* presenter
);

#ifdef __cplusplus
}
#endif

#endif
