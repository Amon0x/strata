#include <strata/strata.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct owned_file {
    char* data;
    size_t size;
};

struct packet_capture {
    uint8_t header[12];
    size_t size;
};

static int64_t smoke_clock(void* user_data) {
    return *(const int64_t*)user_data;
}

static struct owned_file read_file(const char* path) {
    struct owned_file result = {NULL, 0U};
    FILE* input = fopen(path, "rb");
    long length;
    if (input == NULL || fseek(input, 0L, SEEK_END) != 0) goto fail;
    length = ftell(input);
    if (length <= 0L || fseek(input, 0L, SEEK_SET) != 0) goto fail;
    result.size = (size_t)length;
    if (result.size == SIZE_MAX) goto fail;
    result.data = (char*)malloc(result.size + 1U);
    if (result.data == NULL || fread(result.data, 1U, result.size, input) != result.size) goto fail;
    result.data[result.size] = '\0';
    fclose(input);
    return result;
fail:
    if (input != NULL) fclose(input);
    free(result.data);
    result.data = NULL;
    result.size = 0U;
    return result;
}

static void capture_packet(void* user_data, strata_bytes_view bytes) {
    struct packet_capture* capture = (struct packet_capture*)user_data;
    capture->size = bytes.size;
    if (bytes.size >= sizeof(capture->header)) {
        memcpy(capture->header, bytes.data, sizeof(capture->header));
    }
}

static int failed(strata_result result) {
    return result.status != STRATA_STATUS_OK;
}

int main(int argument_count, char** arguments) {
    static const char application_id[] = "installed.c.application";
    static const char source_id[] = "installed/c/main.strata";
    static const char source[] =
        "style Root { width: { weight: 1 }; height: { weight: 1 }; background: #334155FF; } "
        "screen Main { root Panel(key: \"embedded.panel\", style: Root) }";
    static const char surface_id[] = "installed.c.surface";
    static const char root_name[] = "Main";
    struct owned_file registry = {NULL, 0U};
    strata_runtime* runtime = NULL;
    strata_surface* surface = NULL;
    int exit_code = 1;
    int64_t now = INT64_C(123456);
    strata_runtime_config runtime_config = {0};
    strata_application_config application = {0};
    strata_activation_config activation = {0};
    strata_activation_info activation_info = {0};
    strata_surface_config surface_config = {0};
    strata_surface_frame_info frame_info = {0};
    strata_runtime_memory_info memory_info = {0};
    struct packet_capture packet = {{0}, 0U};
    strata_bytes_sink packet_sink = {0};
    strata_theme theme_contract = {0};
    strata_scroll_animation_request scroll_contract = {0};
    strata_theme_visual_style theme_visual = {0};
    strata_theme_layout_style theme_layout = {0};

    theme_contract.struct_size = sizeof(theme_contract);
    theme_contract.model_version = STRATA_THEME_MODEL_VERSION_CURRENT;
    scroll_contract.struct_size = sizeof(scroll_contract);
    theme_visual.struct_size = sizeof(theme_visual);
    theme_layout.struct_size = sizeof(theme_layout);
    (void)theme_contract;
    (void)scroll_contract;

    if (argument_count != 2) return 64;
    if (failed(strata_theme_visual_style_defaults(&theme_visual)) ||
        failed(strata_theme_layout_style_defaults(&theme_layout))) return 66;
    registry = read_file(arguments[1]);
    if (registry.data == NULL) return 65;

    runtime_config.struct_size = sizeof(runtime_config);
    runtime_config.abi_version = STRATA_ABI_VERSION_CURRENT;
    runtime_config.required_capabilities = STRATA_CAPABILITY_CORE_LIFECYCLE |
        STRATA_CAPABILITY_APPLICATION_LIFECYCLE |
        STRATA_CAPABILITY_COMPILER_ACTIVATION |
        STRATA_CAPABILITY_SURFACE_RUNTIME |
        STRATA_CAPABILITY_SURFACE_RENDER_PACKET |
        STRATA_CAPABILITY_ALLOCATOR_TELEMETRY |
        STRATA_CAPABILITY_SURFACE_RESOURCE_RELOAD;
    runtime_config.clock.struct_size = sizeof(runtime_config.clock);
    runtime_config.clock.user_data = &now;
    runtime_config.clock.now_nanoseconds = smoke_clock;
    if (failed(strata_runtime_create(&runtime_config, &runtime))) goto cleanup;

    application.struct_size = sizeof(application);
    application.id.data = application_id;
    application.id.size = sizeof(application_id) - 1U;
    application.registry_json.data = registry.data;
    application.registry_json.size = registry.size;
    if (failed(strata_runtime_configure_application(runtime, &application))) goto cleanup;

    activation.struct_size = sizeof(activation);
    activation.generation = 1U;
    activation.entry_source_id.data = source_id;
    activation.entry_source_id.size = sizeof(source_id) - 1U;
    activation.entry_text.data = source;
    activation.entry_text.size = sizeof(source) - 1U;
    activation_info.struct_size = sizeof(activation_info);
    if (failed(strata_runtime_compile_and_activate(runtime, &activation, &activation_info)) ||
        activation_info.status != STRATA_ACTIVATION_ACTIVATED) goto cleanup;

    surface_config.struct_size = sizeof(surface_config);
    surface_config.id.data = surface_id;
    surface_config.id.size = sizeof(surface_id) - 1U;
    surface_config.root_role = STRATA_SURFACE_ROOT_SCREEN;
    surface_config.root_name.data = root_name;
    surface_config.root_name.size = sizeof(root_name) - 1U;
    surface_config.environment.struct_size = sizeof(surface_config.environment);
    surface_config.environment.generation = 1U;
    surface_config.environment.framebuffer_width = 640;
    surface_config.environment.framebuffer_height = 480;
    surface_config.environment.logical_width = 640.0;
    surface_config.environment.logical_height = 480.0;
    surface_config.environment.scale = 1.0;
    surface_config.environment.point_snapping = STRATA_POINT_SNAP_NEAREST;
    surface_config.environment.rectangle_snapping = STRATA_RECTANGLE_SNAP_OUTWARD;
    surface_config.environment.density = STRATA_SURFACE_DENSITY_COMFORTABLE;
    surface_config.environment.pointer_precision = STRATA_POINTER_PRECISION_FINE;
    surface_config.environment.input_capabilities =
        STRATA_SURFACE_INPUT_POINTER | STRATA_SURFACE_INPUT_KEYBOARD;
    if (failed(strata_runtime_create_surface(runtime, &surface_config, &surface))) goto cleanup;

    frame_info.struct_size = sizeof(frame_info);
    if (failed(strata_surface_frame(surface, now, &frame_info)) ||
        frame_info.frame_index != 1U || frame_info.render_command_count == 0U) goto cleanup;
    packet_sink.struct_size = sizeof(packet_sink);
    packet_sink.user_data = &packet;
    packet_sink.emit = capture_packet;
    if (failed(strata_surface_read_render_packet(surface, &packet_sink)) ||
        packet.size < sizeof(packet.header) || memcmp(packet.header, "STRATARP", 8U) != 0) goto cleanup;
    if (failed(strata_surface_reload_resources(surface))) goto cleanup;
    memory_info.struct_size = sizeof(memory_info);
    if (failed(strata_runtime_get_memory_info(runtime, &memory_info)) ||
        memory_info.routed_current_bytes == 0U ||
        memory_info.routed_peak_bytes < memory_info.routed_current_bytes) goto cleanup;
    memset(&packet, 0, sizeof(packet));
    if (failed(strata_surface_prepare_release_packet(surface, &packet_sink)) ||
        packet.size < sizeof(packet.header) || memcmp(packet.header, "STRATARP", 8U) != 0) goto cleanup;
    if (failed(strata_surface_acknowledge_release_packet(surface))) goto cleanup;

    exit_code = 0;
cleanup:
    if (surface != NULL) {
        strata_result released = exit_code == 0
            ? strata_surface_release(surface)
            : strata_surface_abandon(surface);
        if (failed(released)) exit_code = 1;
    }
    if (failed(strata_runtime_release(runtime))) exit_code = 1;
    free(registry.data);
    if (exit_code == 0) puts("strata_c_smoke: compiled, activated, framed, emitted a packet, and read allocator telemetry");
    return exit_code;
}
