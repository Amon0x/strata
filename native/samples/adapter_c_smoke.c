#include <strata/d3d11.h>
#include <strata/win32.h>

#include <stddef.h>

int main(void) {
    strata_d3d11_presenter_options presenter_options = {0};
    presenter_options.struct_size = sizeof(presenter_options);
    presenter_options.context_state = STRATA_D3D11_CONTEXT_STATE_PRESERVE;

    strata_d3d11_render_target target = {0};
    target.struct_size = sizeof(target);

    strata_win32_input_options input_options = {0};
    input_options.struct_size = sizeof(input_options);
    input_options.coordinate_scale = 1.0;

    void (*destroy_presenter)(strata_d3d11_presenter*) =
        &strata_d3d11_presenter_destroy;
    void (*destroy_input)(strata_win32_input_adapter*) =
        &strata_win32_input_adapter_destroy;

    return presenter_options.struct_size == 0U || target.struct_size == 0U ||
            input_options.struct_size == 0U || destroy_presenter == NULL ||
            destroy_input == NULL
        ? 1
        : 0;
}
