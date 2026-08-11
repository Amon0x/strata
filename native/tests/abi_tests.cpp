#include <strata/strata.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sstream>

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

struct Clock final {
    std::int64_t now = 0;
};

std::int64_t clock_now(void* const user_data) {
    return static_cast<Clock*>(user_data)->now;
}

struct DiagnosticCapture final {
    std::uint64_t count = 0U;
    std::uint64_t last_id = 0U;
    std::uint64_t last_occurrence_count = 0U;
    std::uint64_t last_sequence = 0U;
    std::uint64_t last_frame_index = 0U;
    std::uint64_t last_dropped_count = 0U;
    std::uint32_t last_version = 0U;
    std::string last_code;
    std::string last_source_id;
    std::string last_component_path;
    std::string last_expected;
    strata_source_range last_range{};
};

void capture_diagnostic(void* const user_data, const strata_diagnostic* const diagnostic) {
    auto& capture = *static_cast<DiagnosticCapture*>(user_data);
    ++capture.count;
    capture.last_id = diagnostic->id;
    capture.last_occurrence_count = diagnostic->occurrence_count;
    capture.last_sequence = diagnostic->sequence;
    capture.last_frame_index = diagnostic->frame_index;
    capture.last_dropped_count = diagnostic->dropped_count;
    capture.last_version = diagnostic->version;
    capture.last_code.assign(
        diagnostic->code.data != nullptr ? diagnostic->code.data : "", diagnostic->code.size
    );
    capture.last_source_id.assign(
        diagnostic->source_id.data != nullptr ? diagnostic->source_id.data : "",
        diagnostic->source_id.size
    );
    capture.last_component_path.assign(
        diagnostic->component_path.data != nullptr ? diagnostic->component_path.data : "",
        diagnostic->component_path.size
    );
    capture.last_expected.assign(
        diagnostic->expected.data != nullptr ? diagnostic->expected.data : "",
        diagnostic->expected.size
    );
    capture.last_range = diagnostic->range;
}

struct DiagnosticSnapshotCapture final {
    std::uint32_t version = 0U;
    std::uint64_t frame_index = 0U;
    std::uint64_t dropped_count = 0U;
    std::size_t record_count = 0U;
};

void capture_diagnostic_snapshot(
    void* const user_data,
    const strata_diagnostics_snapshot* const snapshot
) {
    auto& capture = *static_cast<DiagnosticSnapshotCapture*>(user_data);
    capture.version = snapshot->version;
    capture.frame_index = snapshot->frame_index;
    capture.dropped_count = snapshot->dropped_count;
    capture.record_count = snapshot->record_count;
}

void capture_json(void* const user_data, const strata_string_view value) {
    static_cast<std::string*>(user_data)->assign(value.data, value.size);
}

void capture_bytes(void* const user_data, const strata_bytes_view value) {
    static_cast<std::string*>(user_data)->assign(
        reinterpret_cast<const char*>(value.data),
        value.size
    );
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path);

struct FileResources final {
    std::filesystem::path root;
    std::string loaded;
    strata_runtime* reentrant_runtime = nullptr;
    std::size_t reentrant_swap_on_load = 0U;
    std::size_t load_count = 0U;
    std::uint64_t reentrant_generation = 0U;
    strata_status reentrant_status = STRATA_STATUS_OK;
};

strata_status load_file_resource(
    void* const user_data,
    const strata_string_view id,
    strata_bytes_view* const out_bytes
) {
    auto& resources = *static_cast<FileResources*>(user_data);
    try {
        ++resources.load_count;
        resources.loaded = read_file(
            resources.root / std::string_view(id.data, id.size)
        );
        *out_bytes = strata_bytes_view{
            reinterpret_cast<const std::uint8_t*>(resources.loaded.data()),
            resources.loaded.size(),
        };
        if (resources.reentrant_runtime != nullptr &&
            resources.load_count == resources.reentrant_swap_on_load) {
            const strata_resource_adapter replacement{
                sizeof(strata_resource_adapter),
                &resources,
                resources.reentrant_generation,
                &load_file_resource,
            };
            resources.reentrant_status = strata_runtime_set_resource_adapter(
                resources.reentrant_runtime,
                &replacement
            ).status;
        }
        return STRATA_STATUS_OK;
    } catch (...) {
        return STRATA_STATUS_NOT_FOUND;
    }
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open ABI fixture " + path.string());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

[[nodiscard]] strata_string_view view(const std::string_view value) {
    return strata_string_view{value.data(), value.size()};
}

[[nodiscard]] strata_runtime_config config(
    Clock& clock,
    DiagnosticCapture& diagnostics
) {
    return strata_runtime_config{
        sizeof(strata_runtime_config),
        STRATA_ABI_VERSION_CURRENT,
        0U,
        STRATA_CAPABILITY_CORE_LIFECYCLE |
            STRATA_CAPABILITY_CALLER_CLOCK |
            STRATA_CAPABILITY_IMMUTABLE_SNAPSHOTS,
        100U,
        strata_allocator{},
        strata_clock{sizeof(strata_clock), &clock, &clock_now},
        strata_diagnostic_sink{
            sizeof(strata_diagnostic_sink),
            &diagnostics,
            &capture_diagnostic,
        },
    };
}

void test_negotiation() {
    check(strata_abi_version() == STRATA_ABI_VERSION_CURRENT, "ABI version export changed");
    strata_api_info info{sizeof(strata_api_info), 0U, 0U, 0U};
    check(
        strata_get_api_info(STRATA_ABI_VERSION_CURRENT, &info).status == STRATA_STATUS_OK,
        "current ABI negotiation failed"
    );
    check(info.minimum_abi_version == STRATA_ABI_VERSION_MINIMUM, "minimum ABI changed");
    check(
        strata_get_api_info(STRATA_ABI_VERSION_5, &info).status ==
            STRATA_STATUS_UNSUPPORTED_ABI,
        "pre-effect-program ABI remained falsely negotiable"
    );
    check(info.capabilities == strata_capability_bits(), "capability reports disagree");
    check(
        (info.capabilities & STRATA_CAPABILITY_ALLOCATOR_TELEMETRY) != 0U,
        "allocator telemetry capability is missing"
    );
    check(
        (info.capabilities & STRATA_CAPABILITY_SURFACE_RESOURCE_RELOAD) != 0U,
        "transactional Surface resource reload capability is missing"
    );
    check(
        (info.capabilities & STRATA_CAPABILITY_DIAGNOSTIC_SNAPSHOTS) != 0U,
        "canonical diagnostic snapshot capability is missing"
    );
    check(
        (info.capabilities & STRATA_CAPABILITY_SURFACE_THEMES) != 0U,
        "typed scoped theme capability is missing"
    );
    check(
        (info.capabilities & STRATA_CAPABILITY_DURABLE_STATE) != 0U,
        "durable state capability is missing"
    );
    check(
        (info.capabilities & STRATA_CAPABILITY_ASYNC_HOST_DATA) != 0U,
        "async host data capability is missing"
    );
    check(
        strata_get_api_info(STRATA_ABI_VERSION_CURRENT + 1U, &info).status ==
            STRATA_STATUS_UNSUPPORTED_ABI,
        "future ABI request must fail explicitly"
    );
    check(
        strata_get_api_info(STRATA_ABI_VERSION_1, &info).status ==
            STRATA_STATUS_UNSUPPORTED_ABI,
        "breaking v1 release semantics remained falsely negotiable"
    );
}

void test_lifecycle_identity_snapshot_and_clock() {
    Clock clock{5000};
    DiagnosticCapture diagnostics;
    strata_runtime_config runtime_config = config(clock, diagnostics);
    strata_runtime* runtime = nullptr;
    check(
        strata_runtime_create(&runtime_config, &runtime).status == STRATA_STATUS_OK &&
            runtime != nullptr,
        "runtime creation failed"
    );

    strata_runtime_memory_info memory_info{};
    memory_info.struct_size = sizeof(memory_info);
    check(
        strata_runtime_get_memory_info(runtime, &memory_info).status == STRATA_STATUS_OK,
        "runtime memory telemetry failed"
    );
    check(memory_info.routed_current_bytes > 0U, "runtime handle bytes were not tracked");
    check(memory_info.routed_allocation_count >= 1U, "runtime allocation was not counted");
    check(memory_info.routed_peak_bytes >= memory_info.routed_current_bytes, "invalid routed peak");
    check(memory_info.arena_current_bytes == 0U, "unused runtime arena reported live bytes");
    const std::uint64_t runtime_handle_bytes = memory_info.routed_current_bytes;

    std::uint64_t identity = 0U;
    check(strata_runtime_next_identity(runtime, &identity).status == STRATA_STATUS_OK, "identity failed");
    check(identity == 101U, "identity seed was not preserved");

    strata_snapshot* first = nullptr;
    check(
        strata_runtime_create_snapshot(runtime, &first).status == STRATA_STATUS_OK && first != nullptr,
        "first snapshot failed"
    );
    memory_info = {};
    memory_info.struct_size = sizeof(memory_info);
    check(
        strata_runtime_get_memory_info(runtime, &memory_info).status == STRATA_STATUS_OK,
        "snapshot memory telemetry failed"
    );
    check(
        memory_info.routed_current_bytes > runtime_handle_bytes,
        "allocator telemetry omitted the immutable snapshot handle"
    );
    strata_snapshot_info first_info{sizeof(strata_snapshot_info), 0U, 0, 0U};
    check(strata_snapshot_get_info(first, &first_info).status == STRATA_STATUS_OK, "snapshot read failed");
    check(
        first_info.generation == 1U && first_info.time_nanoseconds == 5000 &&
            first_info.last_stable_identity == 101U,
        "snapshot contents changed"
    );

    clock.now = 4999;
    strata_snapshot* rejected = reinterpret_cast<strata_snapshot*>(UINTPTR_MAX);
    const strata_result regression = strata_runtime_create_snapshot(runtime, &rejected);
    check(regression.status == STRATA_STATUS_CLOCK_REGRESSION, "clock regression was accepted");
    check(rejected == nullptr, "failed snapshot must clear its output handle");
    check(
        diagnostics.last_code == "STRATA.RUNTIME.CLOCK_REGRESSION",
        "clock regression diagnostic changed"
    );
    const std::uint64_t regression_id = diagnostics.last_id;
    static_cast<void>(strata_runtime_create_snapshot(runtime, &rejected));
    check(diagnostics.last_id == regression_id && diagnostics.last_occurrence_count == 2U,
          "repeated runtime diagnostic was not occurrence-aggregated");
    check(
        diagnostics.last_version == STRATA_DIAGNOSTIC_VERSION_CURRENT &&
            diagnostics.last_sequence == regression_id && diagnostics.last_frame_index == 0U,
        "runtime diagnostic callback lost canonical payload metadata"
    );

    strata_runtime_release(runtime);
    strata_snapshot_info after_release{sizeof(strata_snapshot_info), 0U, 0, 0U};
    check(
        strata_snapshot_get_info(first, &after_release).status == STRATA_STATUS_OK &&
            after_release.generation == 1U,
        "immutable snapshot must outlive its runtime"
    );
    strata_snapshot_release(first);
}

void test_lazy_host_snapshot_abi() {
    Clock clock{7000};
    DiagnosticCapture diagnostics;
    strata_runtime_config runtime_config = config(clock, diagnostics);
    runtime_config.required_capabilities |=
        STRATA_CAPABILITY_HOST_SNAPSHOTS | STRATA_CAPABILITY_VALUE_JSON;
    strata_runtime* runtime = nullptr;
    check(strata_runtime_create(&runtime_config, &runtime).status == STRATA_STATUS_OK, "host-capable runtime creation failed");

    strata_host_snapshot_info info{sizeof(strata_host_snapshot_info), 0U, 0U, 0U, 0U};
    check(
        strata_runtime_get_host_snapshot_info(runtime, &info).status == STRATA_STATUS_OK &&
            info.has_snapshot == 0U,
        "empty host snapshot info changed"
    );
    std::uint64_t producer_generation = UINT64_MAX;
    check(
        strata_runtime_get_host_snapshot_generation(
            runtime, view("initial"), &producer_generation
        ).status == STRATA_STATUS_NOT_FOUND && producer_generation == 0U,
        "missing host snapshot producer generation changed"
    );

    const std::string id = "initial";
    const std::string values = R"({"app":{"generation":10,"title":"Initial"},"unused":{"value":"cold"}})";
    const strata_host_snapshot_config first{
        sizeof(strata_host_snapshot_config),
        view(id),
        10U,
        view(values),
    };
    check(
        strata_runtime_publish_host_snapshot(runtime, &first).status == STRATA_STATUS_OK,
        "host snapshot publication failed"
    );
    check(
        strata_runtime_get_host_snapshot_info(runtime, &info).status == STRATA_STATUS_OK &&
            info.has_snapshot == 1U && info.generation == 10U && info.evaluated_scalar_count == 0U,
        "host snapshot was not lazy at the ABI boundary"
    );
    check(
        strata_runtime_get_host_snapshot_generation(
            runtime, view("initial"), &producer_generation
        ).status == STRATA_STATUS_OK && producer_generation == 10U,
        "host snapshot producer generation was not queryable"
    );

    std::string encoded;
    const strata_value_json_sink sink{sizeof(strata_value_json_sink), &encoded, &capture_json};
    const std::string path = "app.title";
    const strata_result title_result = strata_runtime_read_host_value_json(runtime, view(path), &sink);
    check(
        title_result.status == STRATA_STATUS_OK && encoded == "\"Initial\"\n",
        "host value JSON read changed: status=" + std::to_string(title_result.status) +
            ", value=" + encoded
    );
    check(
        strata_runtime_get_host_snapshot_info(runtime, &info).status == STRATA_STATUS_OK &&
            info.evaluated_scalar_count == 1U,
        "host path read evaluated sibling fields"
    );
    static_cast<void>(strata_runtime_read_host_value_json(runtime, view(path), &sink));
    static_cast<void>(strata_runtime_get_host_snapshot_info(runtime, &info));
    check(info.evaluated_scalar_count == 1U, "host path read did not memoize its value");

    const std::string missing_path = "app.missing";
    check(
        strata_runtime_read_host_value_json(runtime, view(missing_path), &sink).status ==
            STRATA_STATUS_NOT_FOUND,
        "missing host path did not return NOT_FOUND"
    );
    const strata_host_snapshot_config repeated{
        sizeof(strata_host_snapshot_config),
        view("initial"),
        10U,
        view(R"({})"),
    };
    check(
        strata_runtime_publish_host_snapshot(runtime, &repeated).status == STRATA_STATUS_INVALID_ARGUMENT &&
            diagnostics.last_code == "STRATA.HOST.INVALID_SNAPSHOT",
        "repeated per-id host generation was accepted by the ABI"
    );

    const strata_host_snapshot_config updated{
        sizeof(strata_host_snapshot_config),
        view("updated"),
        11U,
        view(R"({"app":{"title":"Updated"}})"),
    };
    check(strata_runtime_publish_host_snapshot(runtime, &updated).status == STRATA_STATUS_OK, "new host generation was rejected");
    static_cast<void>(strata_runtime_get_host_snapshot_info(runtime, &info));
    check(info.generation == 11U && info.evaluated_scalar_count == 0U, "new host generation retained old field cache");
    strata_runtime_release(runtime);
}

struct ActionCapture final {
    std::uint64_t calls = 0U;
    std::string action_id;
    std::string payload;
};

struct ExtensionCapture final {
    std::uint64_t activations = 0U;
    std::uint64_t presentations = 0U;
    std::uint64_t behavior_dispatches = 0U;
};

strata_extension_input_result activate_extension(
    void* const user_data,
    strata_widget_input_context* const context
) {
    auto& capture = *static_cast<ExtensionCapture*>(user_data);
    ++capture.activations;
    const double count = strata_widget_input_retained_number(context, view("count"), 0.0) + 1.0;
    check(
        strata_widget_input_set_retained_number(context, view("count"), count).status ==
            STRATA_STATUS_OK,
        "extension retained write failed"
    );
    check(
        strata_widget_input_emit_action_json(
            context,
            view("Button"),
            strata_string_view{nullptr, 0U},
            view("activated"),
            strata_string_view{nullptr, 0U}
        ).status == STRATA_STATUS_OK,
        "extension action emission failed"
    );
    return STRATA_EXTENSION_INPUT_CONSUMED;
}

void present_extension(
    void* const user_data,
    strata_widget_render_context* const context
) {
    ++static_cast<ExtensionCapture*>(user_data)->presentations;
    const strata_rect bounds = strata_widget_render_bounds(context);
    strata_widget_render_rounded_rect(
        context,
        bounds,
        3.0,
        strata_color{20U, 80U, 140U, 255U},
        nullptr
    );
}

strata_extension_input_result observe_extension_pointer(
    void* const user_data,
    strata_behavior_input_context*,
    const strata_behavior_pointer_event* const event
) {
    if (event != nullptr) ++static_cast<ExtensionCapture*>(user_data)->behavior_dispatches;
    return STRATA_EXTENSION_INPUT_IGNORED;
}

strata_action_handler_result capture_action(
    void* const user_data,
    const strata_action_call* const call
) {
    auto& capture = *static_cast<ActionCapture*>(user_data);
    ++capture.calls;
    capture.action_id.assign(call->action_id.data, call->action_id.size);
    capture.payload.assign(call->payload_json.data, call->payload_json.size);
    return STRATA_ACTION_HANDLER_HANDLED;
}

void test_application_activation_and_action_abi(const std::filesystem::path& resource_root) {
    Clock clock{8000};
    DiagnosticCapture diagnostics;
    strata_runtime_config runtime_config = config(clock, diagnostics);
    runtime_config.required_capabilities |=
        STRATA_CAPABILITY_APPLICATION_LIFECYCLE |
        STRATA_CAPABILITY_COMPILER_ACTIVATION |
        STRATA_CAPABILITY_ACTION_DISPATCH;
    strata_runtime* runtime = nullptr;
    check(strata_runtime_create(&runtime_config, &runtime).status == STRATA_STATUS_OK, "application runtime creation failed");

    FileResources file_resources{resource_root, {}};
    const strata_resource_adapter resource_adapter{
        sizeof(strata_resource_adapter), &file_resources, 1U, &load_file_resource,
    };
    check(
        strata_runtime_set_resource_adapter(runtime, &resource_adapter).status ==
            STRATA_STATUS_OK,
        "application runtime resource adapter failed"
    );

    const std::string schemas = R"({
      "widgets": {
        "registry": "abi-extension.v1",
        "required": ["AbiExtension"],
        "definitions": [{
          "name": "AbiExtension",
          "allowsChildren": false,
          "parameters": [
            {"name":"key","type":{"kind":"key"},"required":false,"nullable":false,"aliases":[],"default":null},
            {"name":"layout","type":{"kind":"layout"},"required":false,"nullable":false,"aliases":[],"default":null}
          ]
        }]
      },
      "behaviors": {
        "registry": "abi-extension.v1",
        "required": ["abi.observe"],
        "definitions": [{
          "id": "abi.observe",
          "options": {"kind":"object","label":"ABI behavior options","allowUnknownFields":false,"valueNullable":false,"fields":[]}
        }]
      },
      "actions": {"registry":"abi-extension.v1","required":[],"definitions":[]},
      "host": []
    })";
    const strata_application_config application{
        sizeof(strata_application_config),
        view("abi-application"),
        view(schemas),
        nullptr,
        0U,
    };
    check(
        strata_runtime_configure_application(runtime, &application).status == STRATA_STATUS_OK,
        "application configuration failed"
    );
    DiagnosticSnapshotCapture initial_diagnostic_snapshot;
    const strata_diagnostics_snapshot_sink diagnostic_snapshot_sink{
        sizeof(strata_diagnostics_snapshot_sink),
        &initial_diagnostic_snapshot,
        &capture_diagnostic_snapshot,
    };
    check(
        strata_runtime_read_diagnostics(runtime, &diagnostic_snapshot_sink).status ==
                STRATA_STATUS_OK &&
            initial_diagnostic_snapshot.version == STRATA_DIAGNOSTICS_SNAPSHOT_VERSION_CURRENT &&
            initial_diagnostic_snapshot.frame_index == 0U &&
            initial_diagnostic_snapshot.record_count == 0U,
        "empty canonical runtime diagnostic snapshot changed"
    );

    const std::string source = R"(
screen Main {
  root Panel(key: "abi.root", layout: { kind: "COLUMN" }, behaviors: [{ id: "abi.observe" }]) {
    Button(key: "abi.button", label: "ABI", onClick: action("Button"))
    AbiExtension(key: "abi.extension", layout: { width: 100, height: 40 })
  }
}
)";
    const strata_activation_config activation{
        sizeof(strata_activation_config),
        1U,
        view("abi/main.strata"),
        view(source),
        nullptr,
        nullptr,
    };
    strata_activation_info activation_info{};
    activation_info.struct_size = sizeof(activation_info);
    check(
        strata_runtime_compile_and_activate(runtime, &activation, &activation_info).status == STRATA_STATUS_OK &&
            activation_info.status == STRATA_ACTIVATION_ACTIVATED &&
            activation_info.has_active_generation == 1U && activation_info.active_generation == 1U,
        "application source did not activate through the C ABI"
    );
    strata_runtime_memory_info activation_memory{};
    activation_memory.struct_size = sizeof(activation_memory);
    check(
        strata_runtime_get_memory_info(runtime, &activation_memory).status == STRATA_STATUS_OK &&
            activation_memory.arena_peak_bytes > 0U &&
            activation_memory.arena_current_bytes == 0U &&
            activation_memory.arena_allocation_count ==
                activation_memory.arena_deallocation_count,
        "compiler scratch arena was not used and reclaimed after activation"
    );
    std::string active_unit;
    const strata_value_json_sink active_sink{sizeof(strata_value_json_sink), &active_unit, &capture_json};
    check(
        strata_runtime_read_active_unit_json(runtime, &active_sink).status == STRATA_STATUS_OK &&
            active_unit.find("\"Main\"") != std::string::npos,
        "active portable IR was not readable through the C ABI"
    );
    strata_application_state_snapshot* state_snapshot = nullptr;
    std::uint32_t state_changed = 1U;
    check(
        strata_runtime_create_application_state_snapshot(runtime, &state_snapshot).status ==
                STRATA_STATUS_OK &&
            state_snapshot != nullptr &&
            strata_runtime_restore_application_state(
                runtime,
                state_snapshot,
                &state_changed
            ).status == STRATA_STATUS_OK &&
            state_changed == 0U,
        "application state snapshots did not cross the C ABI"
    );
    strata_application_state_snapshot_release(state_snapshot);

    const std::string invalid_source = "screen Main { root Panel {";
    strata_activation_config rejected = activation;
    rejected.generation = 2U;
    rejected.entry_text = view(invalid_source);
    check(
        strata_runtime_compile_and_activate(runtime, &rejected, &activation_info).status ==
                STRATA_STATUS_COMPILE_FAILED &&
            activation_info.status == STRATA_ACTIVATION_REJECTED_COMPILE &&
            activation_info.active_generation == 1U,
        "failed ABI reload replaced the last-good unit"
    );
    activation_memory = {};
    activation_memory.struct_size = sizeof(activation_memory);
    check(
        strata_runtime_get_memory_info(runtime, &activation_memory).status == STRATA_STATUS_OK &&
            activation_memory.arena_current_bytes == 0U,
        "compiler scratch arena remained live after rejected activation"
    );
    check(
        diagnostics.last_source_id == "abi/main.strata" &&
            diagnostics.last_range.line_start == 1U &&
            diagnostics.last_range.column_start >= 1U &&
            diagnostics.last_range.byte_end <= invalid_source.size(),
        "compiler diagnostic source range was lost across the C ABI"
    );
    std::string after_rejection;
    const strata_value_json_sink rejected_sink{
        sizeof(strata_value_json_sink),
        &after_rejection,
        &capture_json,
    };
    check(
        strata_runtime_read_active_unit_json(runtime, &rejected_sink).status == STRATA_STATUS_OK &&
            after_rejection == active_unit,
        "last-good portable IR changed after rejected ABI reload"
    );

    ActionCapture action_capture;
    const strata_action_handler_config handler{
        sizeof(strata_action_handler_config),
        view("Button"),
        view("abi-test"),
        &action_capture,
        &capture_action,
    };
    strata_action_registration* registration = nullptr;
    check(
        strata_runtime_register_action_handler(runtime, &handler, &registration).status ==
                STRATA_STATUS_OK &&
            registration != nullptr,
        "C ABI action handler registration failed"
    );
    const strata_action_dispatch_config dispatch{
        sizeof(strata_action_dispatch_config),
        view("Button"),
        strata_string_view{nullptr, 0U},
        view("activate"),
        view("abi.button"),
        strata_string_view{nullptr, 0U},
        strata_string_view{nullptr, 0U},
        0U,
        0U,
    };
    strata_action_dispatch_info dispatch_info{};
    dispatch_info.struct_size = sizeof(dispatch_info);
    check(
        strata_runtime_dispatch_action_json(runtime, &dispatch, &dispatch_info).status ==
                STRATA_STATUS_OK &&
            dispatch_info.status == STRATA_ACTION_DISPATCH_HANDLED &&
            dispatch_info.handler_count == 1U && action_capture.calls == 1U &&
            action_capture.action_id == "Button" && action_capture.payload == "null\n",
        "typed action did not cross the C ABI exactly once"
    );

    const strata_surface_environment surface_environment{
        sizeof(strata_surface_environment),
        1U,
        320,
        180,
        320.0,
        180.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        STRATA_POINT_SNAP_NEAREST,
        STRATA_RECTANGLE_SNAP_OUTWARD,
        STRATA_SURFACE_DENSITY_COMFORTABLE,
        STRATA_POINTER_PRECISION_FINE,
        STRATA_SURFACE_INPUT_POINTER | STRATA_SURFACE_INPUT_KEYBOARD,
        0U,
        0U,
    };
    const std::string control_font = "assets/strata/fonts/medium.ttf";
    const std::string regular_font = "assets/strata/fonts/default.ttf";
    const std::string mono_font = "assets/strata/fonts/mono.ttf";
    const strata_surface_font_resource fonts[]{
        {view("strata:fonts/default-medium"), view(control_font)},
        {view("strata:fonts/default"), view(regular_font)},
        {view("strata:fonts/mono"), view(mono_font)},
    };
    const strata_surface_image_resource images[]{
        {
            view("strata:ui/icons/chevron-down"),
            view("assets/strata/images/ui/icons/chevron-down.svg"),
            STRATA_IMAGE_SAMPLING_LINEAR,
            0U,
        },
    };
    ExtensionCapture extension_capture;
    const strata_widget_retained_field extension_retained[]{
        {sizeof(strata_widget_retained_field), view("count"), STRATA_WIDGET_INVALIDATION_PROPERTIES, 0U},
    };
    strata_widget_extension widget_extension{};
    widget_extension.struct_size = sizeof(strata_widget_extension);
    widget_extension.type = view("AbiExtension");
    widget_extension.flags = STRATA_WIDGET_EXTENSION_FOCUSABLE;
    widget_extension.user_data = &extension_capture;
    widget_extension.activate = &activate_extension;
    widget_extension.present = &present_extension;
    widget_extension.retained_fields = extension_retained;
    widget_extension.retained_field_count = std::size(extension_retained);
    const strata_behavior_extension behavior_extension{
        sizeof(strata_behavior_extension),
        view("abi.observe"),
        STRATA_BEHAVIOR_EXTENSION_ACCEPTS_POINTER,
        &extension_capture,
        &observe_extension_pointer,
    };
    const strata_surface_extension_bundle extensions{
        sizeof(strata_surface_extension_bundle),
        &widget_extension,
        1U,
        &behavior_extension,
        1U,
        nullptr,
        0U,
        nullptr,
        0U,
    };
    const strata_surface_config surface_config{
        sizeof(strata_surface_config),
        view("abi.surface"),
        STRATA_SURFACE_ROOT_SCREEN,
        0U,
        view("Main"),
        surface_environment,
        fonts,
        std::size(fonts),
        &extensions,
        images,
        std::size(images),
    };
    strata_surface* surface = nullptr;
    check(
        strata_runtime_create_surface(runtime, &surface_config, &surface).status ==
                STRATA_STATUS_OK &&
            surface != nullptr,
        "portable Surface creation failed through the C ABI"
    );
    strata_surface* duplicate_surface = reinterpret_cast<strata_surface*>(UINTPTR_MAX);
    check(
        strata_runtime_create_surface(runtime, &surface_config, &duplicate_surface).status ==
                STRATA_STATUS_INVALID_ARGUMENT &&
            duplicate_surface == nullptr,
        "duplicate public Surface ids aliased runtime host-service ownership"
    );
    const strata_surface_image_resource malformed_images[]{
        {
            view("fixture:malformed-svg"),
            view("assets/strata/samples/desktop_app.strata"),
            STRATA_IMAGE_SAMPLING_LINEAR,
            0U,
        },
    };
    strata_surface_config malformed_surface_config = surface_config;
    malformed_surface_config.id = view("abi.malformed-svg");
    malformed_surface_config.images = malformed_images;
    malformed_surface_config.image_count = std::size(malformed_images);
    strata_surface* malformed_surface = reinterpret_cast<strata_surface*>(UINTPTR_MAX);
    check(
        strata_runtime_create_surface(
            runtime,
            &malformed_surface_config,
            &malformed_surface
        ).status == STRATA_STATUS_INVALID_ARGUMENT && malformed_surface == nullptr,
        "Surface image loading accepted a malformed SVG document"
    );
    std::string frame_json;
    const strata_value_json_sink frame_sink{
        sizeof(strata_value_json_sink), &frame_json, &capture_json,
    };
    check(
        strata_surface_read_frame_json(surface, &frame_sink).status == STRATA_STATUS_NOT_FOUND,
        "unframed Surface exposed a stale output snapshot"
    );
    strata_surface_frame_info frame_info{};
    frame_info.struct_size = sizeof(frame_info);
    check(
        strata_surface_frame(surface, 8'000, &frame_info).status == STRATA_STATUS_OK &&
            frame_info.frame_index == 1U && frame_info.render_command_count >= 2U &&
            frame_info.semantics_generation != 0U && frame_info.render_packet_size != 0U,
        "portable Surface did not execute its first complete frame"
    );
    check(extension_capture.presentations != 0U, "C ABI widget presentation hook did not run");
    strata_theme_tokens theme_tokens{};
    theme_tokens.struct_size = sizeof(theme_tokens);
    check(
        strata_theme_tokens_defaults(&theme_tokens).status == STRATA_STATUS_OK,
        "typed theme token defaults failed"
    );
    const auto nullable_number = [](const strata_theme_value_mode mode, const double value = 0.0) {
        return strata_theme_optional_number{mode, 0U, value};
    };
    const auto layout_size = [](
                                 const strata_theme_layout_size_kind kind,
                                 const double value
                             ) {
        return strata_theme_layout_size{
            sizeof(strata_theme_layout_size), kind, 0U, value,
            nullptr, nullptr, nullptr,
        };
    };
    strata_theme_visual_style theme_visual{};
    theme_visual.struct_size = sizeof(theme_visual);
    check(
        strata_theme_visual_style_defaults(&theme_visual).status == STRATA_STATUS_OK,
        "typed theme visual defaults failed"
    );
    theme_visual.background.mode = STRATA_THEME_VALUE_NONE;
    theme_visual.foreground = strata_color{10U, 20U, 30U, 255U};
    theme_visual.border.mode = STRATA_THEME_VALUE_SET;
    theme_visual.border.value = {1.0, strata_color{40U, 50U, 60U, 255U}, 0U, 0U};
    theme_visual.hover_overlay.mode = STRATA_THEME_VALUE_NONE;
    theme_visual.active_overlay.mode = STRATA_THEME_VALUE_NONE;
    theme_visual.focus_ring.mode = STRATA_THEME_VALUE_NONE;
    theme_visual.disabled_opacity = 0.4;
    theme_visual.opacity = 1.0;
    theme_visual.scale = 1.0;
    theme_visual.scale_x = 1.0;
    theme_visual.scale_y = 1.0;
    theme_visual.track.mode = STRATA_THEME_VALUE_NONE;
    theme_visual.fill.mode = STRATA_THEME_VALUE_NONE;
    theme_visual.thumb.mode = STRATA_THEME_VALUE_NONE;
    theme_visual.selection.mode = STRATA_THEME_VALUE_NONE;
    theme_visual.scrim.mode = STRATA_THEME_VALUE_NONE;
    theme_visual.indicator_size = nullable_number(STRATA_THEME_VALUE_NONE);
    theme_visual.indicator_inset = nullable_number(STRATA_THEME_VALUE_NONE);
    theme_visual.track_width = nullable_number(STRATA_THEME_VALUE_NONE);
    theme_visual.track_height = nullable_number(STRATA_THEME_VALUE_NONE);
    theme_visual.track_radius = nullable_number(STRATA_THEME_VALUE_NONE);
    theme_visual.thumb_size = nullable_number(STRATA_THEME_VALUE_NONE);
    theme_visual.thumb_radius = nullable_number(STRATA_THEME_VALUE_NONE);
    theme_visual.indicator_position = nullable_number(STRATA_THEME_VALUE_NONE);
    const strata_string_view fallback_fonts[]{view("strata:fonts/mono")};
    strata_theme_text_layout_style theme_text{};
    theme_text.struct_size = sizeof(theme_text);
    check(
        strata_theme_text_layout_style_defaults(&theme_text).status == STRATA_STATUS_OK,
        "typed theme text-layout defaults failed"
    );
    check(
        std::string_view(theme_text.primary_font.data, theme_text.primary_font.size) ==
            "strata:fonts/default",
        "typed theme text-layout default is not the Regular face"
    );
    theme_text.primary_font = view("strata:fonts/default");
    theme_text.fallback_fonts = fallback_fonts;
    theme_text.fallback_font_count = std::size(fallback_fonts);
    theme_text.pixel_size = 13.0;
    theme_text.style_flags = 3U;
    theme_text.line_height = nullable_number(STRATA_THEME_VALUE_NONE);
    theme_text.line_height_multiplier = 1.2;
    theme_text.letter_spacing = 0.25;
    strata_theme_layout_style theme_layout{};
    theme_layout.struct_size = sizeof(theme_layout);
    check(
        strata_theme_layout_style_defaults(&theme_layout).status == STRATA_STATUS_OK,
        "typed theme layout defaults failed"
    );
    theme_layout.participates = 1U;
    theme_layout.kind = STRATA_THEME_LAYOUT_PANEL;
    const strata_theme_layout_size clamp_minimum = layout_size(STRATA_THEME_SIZE_PERCENT, 0.25);
    strata_theme_layout_size nested_preferred = layout_size(STRATA_THEME_SIZE_CONTENT, 0.0);
    const strata_theme_layout_size nested_maximum = layout_size(STRATA_THEME_SIZE_FIXED, 160.0);
    strata_theme_layout_size preferred_clamp = layout_size(STRATA_THEME_SIZE_CLAMP, 0.0);
    preferred_clamp.preferred = &nested_preferred;
    preferred_clamp.maximum = &nested_maximum;
    const strata_theme_layout_size clamp_maximum = layout_size(STRATA_THEME_SIZE_FILL, 1.0);
    theme_layout.width = layout_size(STRATA_THEME_SIZE_CLAMP, 0.0);
    theme_layout.width.minimum = &clamp_minimum;
    theme_layout.width.preferred = &preferred_clamp;
    theme_layout.width.maximum = &clamp_maximum;
    theme_layout.height = layout_size(STRATA_THEME_SIZE_CONTENT, 0.0);
    theme_layout.aspect_ratio = nullable_number(STRATA_THEME_VALUE_NONE);
    theme_layout.align_items = STRATA_THEME_ALIGN_STRETCH;
    theme_layout.justify_content = STRATA_THEME_JUSTIFY_START;
    theme_layout.align_content = STRATA_THEME_JUSTIFY_START;
    theme_layout.align_self = STRATA_THEME_ALIGN_UNSPECIFIED;
    theme_layout.justify_self = STRATA_THEME_ALIGN_UNSPECIFIED;
    theme_layout.column_span = 1U;
    theme_layout.row_span = 1U;
    theme_layout.scroll_vertical = 1U;
    theme_layout.scroll_viewport_insets_from_inside_border = 1U;
    theme_layout.portal_target = view("root");
    theme_layout.detach_from_parent_clip = 1U;
    strata_theme_animation_set empty_motion{};
    empty_motion.struct_size = sizeof(empty_motion);
    check(
        strata_theme_animation_set_defaults(&empty_motion).status == STRATA_STATUS_OK,
        "typed theme animation-set defaults failed"
    );
    strata_theme_motion_easing keyframe_easing{};
    keyframe_easing.struct_size = sizeof(keyframe_easing);
    keyframe_easing.kind = STRATA_THEME_MOTION_EASING_CUBIC_OUT;
    strata_theme_motion_keyframe inline_frame{};
    inline_frame.offset = 0.4;
    inline_frame.value.kind = STRATA_THEME_MOTION_VALUE_NUMBER;
    inline_frame.value.number = 0.5;
    inline_frame.easing = &keyframe_easing;
    const strata_theme_motion_track inline_track{
        STRATA_THEME_MOTION_PROPERTY_OPACITY, 0U, &inline_frame, 1U,
    };
    strata_theme_declared_animation inline_animation{};
    inline_animation.struct_size = sizeof(inline_animation);
    inline_animation.trigger = STRATA_THEME_MOTION_FOCUS_VISIBLE;
    inline_animation.repeat_kind = STRATA_THEME_MOTION_REPEAT_COUNT;
    inline_animation.duration_nanoseconds = 70;
    inline_animation.delay_nanoseconds = 11;
    inline_animation.easing.struct_size = sizeof(strata_theme_motion_easing);
    inline_animation.easing.kind = STRATA_THEME_MOTION_EASING_CUBIC_BEZIER;
    inline_animation.easing.x1 = 0.2;
    inline_animation.easing.y1 = -0.3;
    inline_animation.easing.x2 = 0.8;
    inline_animation.easing.y2 = 1.3;
    inline_animation.repeat_count = 2U;
    inline_animation.reverse = 1U;
    inline_animation.fill_mode = STRATA_THEME_MOTION_FILL_FORWARDS;
    inline_animation.tracks = &inline_track;
    inline_animation.track_count = 1U;
    const strata_theme_motion_attachment inline_attachment{
        STRATA_THEME_MOTION_FOCUS_VISIBLE,
        STRATA_THEME_MOTION_FORWARD,
        strata_theme_animation_spec{
            STRATA_THEME_ANIMATION_INLINE, 0U, strata_string_view{}, &inline_animation,
        },
        1U,
        STRATA_THEME_MOTION_ENTER,
        0U,
    };
    empty_motion.attachments = &inline_attachment;
    empty_motion.attachment_count = 1U;
    strata_theme_motion_keyframe focus_visible_frame{};
    focus_visible_frame.offset = 1.0;
    focus_visible_frame.value.kind = STRATA_THEME_MOTION_VALUE_NUMBER;
    focus_visible_frame.value.number = 8.0;
    const strata_theme_motion_track focus_visible_track{
        STRATA_THEME_MOTION_PROPERTY_RADIUS, 0U, &focus_visible_frame, 1U,
    };
    strata_theme_declared_animation focus_visible_animation{};
    focus_visible_animation.struct_size = sizeof(focus_visible_animation);
    focus_visible_animation.trigger = STRATA_THEME_MOTION_ANIMATE;
    focus_visible_animation.duration_nanoseconds = 90;
    focus_visible_animation.easing.struct_size = sizeof(strata_theme_motion_easing);
    focus_visible_animation.easing.kind = STRATA_THEME_MOTION_EASING_LINEAR;
    focus_visible_animation.repeat_kind = STRATA_THEME_MOTION_REPEAT_NONE;
    focus_visible_animation.fill_mode = STRATA_THEME_MOTION_FILL_BOTH;
    focus_visible_animation.tracks = &focus_visible_track;
    focus_visible_animation.track_count = 1U;
    const strata_theme_motion_channel focus_visible_channel{
        view("focus-visible"),
        strata_theme_animation_spec{
            STRATA_THEME_ANIMATION_INLINE, 0U, strata_string_view{}, &focus_visible_animation,
        },
        STRATA_THEME_MOTION_INTERACTION_FOCUS_VISIBLE,
        STRATA_THEME_VALUE_UNSPECIFIED,
        0U,
        0U,
    };
    empty_motion.channels = &focus_visible_channel;
    empty_motion.channel_count = 1U;
    const strata_theme_widget_style theme_style{
        sizeof(strata_theme_widget_style),
        view("Panel"),
        view("CaseSensitive"),
        &theme_visual,
        nullptr,
        &theme_text,
        &theme_layout,
        STRATA_THEME_VALUE_SET,
        0U,
        &empty_motion,
    };
    const strata_theme_widget_style empty_theme_style{
        sizeof(strata_theme_widget_style),
        view("Panel"),
        view("ExplicitEmpty"),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        STRATA_THEME_VALUE_UNSPECIFIED,
        0U,
        nullptr,
    };
    const strata_theme_widget_style theme_styles[]{theme_style, empty_theme_style};
    strata_theme_motion_timing standard_theme_timing{};
    standard_theme_timing.struct_size = sizeof(standard_theme_timing);
    standard_theme_timing.name = view("standard");
    standard_theme_timing.duration_nanoseconds = 180;
    standard_theme_timing.delay_nanoseconds = 7;
    standard_theme_timing.easing.struct_size = sizeof(strata_theme_motion_easing);
    standard_theme_timing.easing.kind = STRATA_THEME_MOTION_EASING_CUBIC_IN_OUT;
    standard_theme_timing.repeat_kind = STRATA_THEME_MOTION_REPEAT_COUNT;
    standard_theme_timing.repeat_count = 3U;
    standard_theme_timing.reverse = 1U;
    standard_theme_timing.fill_mode = STRATA_THEME_MOTION_FILL_BACKWARDS;
    const strata_theme_motion_policy theme_motion_policy{
        sizeof(strata_theme_motion_policy), 0U, 0U, &standard_theme_timing, 1U,
    };
    const strata_theme typed_theme{
        sizeof(strata_theme),
        STRATA_THEME_MODEL_VERSION_CURRENT,
        0U,
        view("abi-theme"),
        strata_string_view{nullptr, 0U},
        theme_tokens,
        &theme_motion_policy,
        theme_styles,
        std::size(theme_styles),
    };
    std::uint32_t theme_changed = 0U;
    check(
        strata_surface_register_theme(surface, &typed_theme, &theme_changed).status ==
                STRATA_STATUS_OK &&
            theme_changed == 1U,
        "full typed theme ABI, including an explicit all-null style, did not register"
    );
    nested_preferred.kind = STRATA_THEME_SIZE_FIXED;
    nested_preferred.value = 77.0;
    check(
        strata_surface_register_theme(surface, &typed_theme, &theme_changed).status ==
                STRATA_STATUS_OK &&
            theme_changed == 1U,
        "recursive theme layout was not deeply copied across the borrowed ABI call"
    );
    const strata_theme_layout_size copied_width = theme_layout.width;
    strata_theme_layout_size cyclic_width = layout_size(STRATA_THEME_SIZE_CLAMP, 0.0);
    cyclic_width.preferred = &cyclic_width;
    theme_layout.width = cyclic_width;
    check(
        strata_surface_register_theme(surface, &typed_theme, &theme_changed).status ==
            STRATA_STATUS_INVALID_ARGUMENT,
        "recursive theme layout pointer cycle was accepted"
    );
    theme_layout.width = copied_width;
    check(
        strata_surface_set_scoped_theme(
            surface, view("abi.root"), &typed_theme, &theme_changed
        ).status == STRATA_STATUS_OK &&
            theme_changed == 1U,
        "typed local ThemeScope ABI did not install"
    );
    check(
        strata_surface_clear_scoped_theme(surface, view("abi.root"), &theme_changed).status ==
                STRATA_STATUS_OK &&
            theme_changed == 1U,
        "typed local ThemeScope ABI did not clear"
    );
    strata_theme invalid_theme = typed_theme;
    invalid_theme.model_version = STRATA_THEME_MODEL_VERSION_CURRENT + 1U;
    check(
        strata_surface_register_theme(surface, &invalid_theme, &theme_changed).status ==
            STRATA_STATUS_INVALID_ARGUMENT,
        "future typed theme model version was accepted"
    );
    invalid_theme.model_version = STRATA_THEME_MODEL_VERSION_2;
    check(
        strata_surface_register_theme(surface, &invalid_theme, &theme_changed).status ==
            STRATA_STATUS_INVALID_ARGUMENT,
        "pre-focus-visible typed theme model remained ambiguously accepted"
    );
    const strata_scroll_animation_request non_scroll_request{
        sizeof(strata_scroll_animation_request),
        view("abi.root"),
        0U,
        1U,
        0.0,
        10.0,
        view("standard"),
        0U,
        0U,
        0,
    };
    std::uint32_t scroll_started = 1U;
    check(
        strata_surface_animate_scroll_to(surface, &non_scroll_request, &scroll_started).status ==
                STRATA_STATUS_OK &&
            scroll_started == 0U,
        "typed scroll animation ABI mishandled a settled/non-scroll target"
    );
    const strata_action_dispatch_config dynamic_dispatch{
        sizeof(strata_action_dispatch_config),
        view("abi.dynamic.unhandled"),
        strata_string_view{nullptr, 0U},
        view("activated"),
        view("abi.button"),
        strata_string_view{nullptr, 0U},
        strata_string_view{nullptr, 0U},
        1U,
        0U,
    };
    strata_action_dispatch_info dynamic_info{};
    dynamic_info.struct_size = sizeof(dynamic_info);
    const strata_result dynamic_result =
        strata_surface_dispatch_action_json(surface, &dynamic_dispatch, &dynamic_info);
    const strata_result dynamic_frame_result = strata_surface_frame(surface, 8'500, &frame_info);
    const strata_result dynamic_snapshot_result =
        strata_surface_read_frame_json(surface, &frame_sink);
    const std::string dynamic_frame_json = frame_json;
    const strata_result diagnostic_publish_result =
        strata_surface_frame(surface, 8'750, &frame_info);
    check(
        dynamic_result.status == STRATA_STATUS_OK &&
            dynamic_info.status == STRATA_ACTION_DISPATCH_UNHANDLED &&
            dynamic_frame_result.status == STRATA_STATUS_OK &&
            dynamic_snapshot_result.status == STRATA_STATUS_OK &&
            diagnostic_publish_result.status == STRATA_STATUS_OK &&
            dynamic_frame_json.find("abi.dynamic.unhandled") != std::string::npos &&
            dynamic_frame_json.find("\"status\": \"unhandled\"") != std::string::npos &&
            diagnostics.last_code == "STRATA.UI.ACTION_UNHANDLED",
        "dynamic action did not cross the generic Surface event/diagnostic pipeline: dispatch=" +
            std::to_string(dynamic_result.status) + "/" + std::to_string(dynamic_info.status) +
            ", frame=" + std::to_string(dynamic_frame_result.status) +
            ", snapshot=" + std::to_string(dynamic_snapshot_result.status) +
            ", publish=" + std::to_string(diagnostic_publish_result.status) +
            ", diagnostic=" + diagnostics.last_code + ", json=" + dynamic_frame_json
    );
    std::uint32_t inspected = 0U;
    check(
        strata_surface_inspector_select(surface, view("abi.extension"), &inspected).status ==
                STRATA_STATUS_OK &&
            inspected == 1U,
        "C ABI inspector could not select a retained key"
    );
    std::string inspection_selection;
    const strata_value_json_sink inspection_sink{
        sizeof(strata_value_json_sink), &inspection_selection, &capture_json,
    };
    check(
        strata_surface_read_inspector_selection_json(surface, &inspection_sink).status ==
                STRATA_STATUS_OK &&
            inspection_selection.find("AbiExtension") != std::string::npos,
        "C ABI inspector selection projection lost the extension node"
    );
    check(
        strata_surface_inspector_clear(surface).status == STRATA_STATUS_OK &&
            strata_surface_inspector_pick(surface, 5.0, 50.0, &inspected).status ==
                STRATA_STATUS_OK &&
            inspected == 1U,
        "C ABI inspector could not pick by logical position"
    );
    std::string render_packet;
    const strata_bytes_sink render_sink{
        sizeof(strata_bytes_sink), &render_packet, &capture_bytes,
    };
    check(
        strata_surface_read_render_packet(surface, &render_sink).status == STRATA_STATUS_OK &&
            render_packet.size() == frame_info.render_packet_size &&
            render_packet.starts_with("STRATARP") &&
            static_cast<unsigned char>(render_packet[8]) == STRATA_RENDER_PACKET_VERSION_CURRENT,
        "Surface render output was not published as one versioned binary batch"
    );
    check(
        strata_surface_read_frame_json(surface, &frame_sink).status == STRATA_STATUS_OK,
        "canonical Surface frame snapshot was not readable"
    );
    check(
        frame_json.find("\"protocol\": \"strata.surface-frame\"") != std::string::npos,
        "canonical Surface frame snapshot lost its protocol envelope: " + frame_json
    );
    check(
        frame_json.find("\"key\": \"abi.button\"") != std::string::npos,
        "canonical Surface frame snapshot lost retained output: " + frame_json
    );
    check(
        frame_json.find("\"renderCommands\"") != std::string::npos,
        "canonical Surface frame snapshot lost render output: " + frame_json
    );
    check(
        frame_json.find("\"kind\": \"text_run\"") != std::string::npos,
        "resource-backed portable text shaping did not reach the render command batch"
    );
    check(
        strata_surface_reload_resources(surface).status == STRATA_STATUS_OK &&
            strata_surface_frame(surface, 8'900, &frame_info).status == STRATA_STATUS_OK,
        "Surface resources did not reload in place"
    );
    const std::filesystem::path valid_resource_root = file_resources.root;
    file_resources.root /= "missing-resource-root";
    check(
        strata_surface_reload_resources(surface).status != STRATA_STATUS_OK,
        "Surface resource reload accepted missing replacement fonts"
    );
    file_resources.root = valid_resource_root;
    check(
        strata_surface_frame(surface, 8'950, &frame_info).status == STRATA_STATUS_OK &&
            frame_info.render_command_count >= 2U,
        "rejected Surface resource reload did not preserve the prior renderer state"
    );
    strata_resource_adapter zero_generation_adapter = resource_adapter;
    zero_generation_adapter.generation = 0U;
    check(
        strata_runtime_set_resource_adapter(runtime, nullptr).status ==
                STRATA_STATUS_INVALID_ARGUMENT &&
            strata_runtime_set_resource_adapter(runtime, &zero_generation_adapter).status ==
                STRATA_STATUS_INVALID_ARGUMENT &&
            strata_runtime_set_resource_adapter(runtime, &resource_adapter).status ==
                STRATA_STATUS_INVALID_ARGUMENT &&
            strata_surface_frame(surface, 8'960, &frame_info).status == STRATA_STATUS_OK,
        "invalid or non-monotonic resource adapters mutated or gated the live Surface"
    );
    strata_resource_adapter replacement_resource_adapter = resource_adapter;
    replacement_resource_adapter.generation = 2U;
    check(
        strata_runtime_set_resource_adapter(runtime, &replacement_resource_adapter).status ==
                STRATA_STATUS_OK &&
            strata_surface_frame(surface, 8'975, &frame_info).status ==
                STRATA_STATUS_SERVICE_UNAVAILABLE &&
            diagnostics.last_code == "STRATA.SURFACE.RESOURCE_RELOAD_REQUIRED" &&
            strata_surface_reload_resources(surface).status == STRATA_STATUS_OK &&
            strata_surface_frame(surface, 8'975, &frame_info).status == STRATA_STATUS_OK,
        "resource-adapter replacement allowed a mixed-resource frame or could not recover"
    );

    strata_input_event click[2]{};
    click[0].struct_size = sizeof(strata_input_event);
    click[0].version = STRATA_INPUT_EVENT_VERSION_2;
    click[0].kind = STRATA_INPUT_POINTER_PRESS;
    click[0].x = 5.0;
    click[0].y = 5.0;
    click[1] = click[0];
    click[1].kind = STRATA_INPUT_POINTER_RELEASE;
    strata_surface_input_batch_info batch_info{};
    batch_info.struct_size = sizeof(batch_info);
    check(
        strata_surface_enqueue_input(surface, click, 2U, &batch_info).status ==
                STRATA_STATUS_OK &&
            batch_info.accepted_event_count == 2U && batch_info.queued_event_count == 2U,
        "atomic pointer input batch was not accepted by the Surface"
    );
    check(
        strata_surface_frame(surface, 9'000, &frame_info).status == STRATA_STATUS_OK &&
            frame_info.processed_input_event_count == 2U && action_capture.calls == 2U,
        "pointer batch did not route through retained hit testing and action dispatch exactly once"
    );

    click[0].kind = STRATA_INPUT_POINTER_PRESS;
    click[0].x = 5.0;
    click[0].y = 50.0;
    click[1] = click[0];
    click[1].kind = STRATA_INPUT_POINTER_RELEASE;
    check(
        strata_surface_enqueue_input(surface, click, 2U, &batch_info).status ==
                STRATA_STATUS_OK &&
            strata_surface_frame(surface, 9'500, &frame_info).status == STRATA_STATUS_OK &&
            extension_capture.activations == 1U && action_capture.calls == 3U &&
            extension_capture.behavior_dispatches >= 4U,
        "surface-owned C ABI widget/behavior lifecycle did not route through the native core"
    );

    click[0].kind = STRATA_INPUT_POINTER_PRESS;
    check(
        strata_surface_enqueue_input(surface, click, 1U, &batch_info).status ==
                STRATA_STATUS_OK &&
            strata_surface_frame(surface, 10'000, &frame_info).status == STRATA_STATUS_OK &&
            strata_surface_cancel_interactions(surface).status == STRATA_STATUS_OK &&
            strata_surface_frame(surface, 11'000, &frame_info).status == STRATA_STATUS_OK &&
            strata_surface_read_frame_json(surface, &frame_sink).status == STRATA_STATUS_OK &&
            frame_json.find("\"reason\": \"invalid_target\"") != std::string::npos,
        "Surface interaction cancellation did not publish focus/capture lifecycle output"
    );

    strata_surface_environment resized = surface_environment;
    resized.generation = 2U;
    resized.framebuffer_width = 640;
    resized.logical_width = 640.0;
    std::uint32_t adopted = 0U;
    check(
        strata_surface_adopt_environment(surface, &resized, &adopted).status ==
                STRATA_STATUS_OK &&
            adopted == 1U &&
            strata_surface_adopt_environment(surface, &surface_environment, &adopted).status ==
                STRATA_STATUS_OK &&
            adopted == 0U,
        "Surface environment generations did not reject stale host state"
    );
    DiagnosticSnapshotCapture surface_diagnostic_snapshot;
    const strata_diagnostics_snapshot_sink surface_diagnostic_sink{
        sizeof(strata_diagnostics_snapshot_sink),
        &surface_diagnostic_snapshot,
        &capture_diagnostic_snapshot,
    };
    check(
        strata_surface_read_diagnostics(surface, &surface_diagnostic_sink).status ==
                STRATA_STATUS_OK &&
            strata_surface_clear_diagnostics(surface).status == STRATA_STATUS_OK,
        "Surface diagnostic snapshot/clear aliases failed"
    );
    std::string release_packet;
    const strata_bytes_sink release_sink{
        sizeof(strata_bytes_sink), &release_packet, &capture_bytes,
    };
    check(
        strata_surface_release(surface).status == STRATA_STATUS_INVALID_ARGUMENT &&
            strata_runtime_release(runtime).status == STRATA_STATUS_INVALID_ARGUMENT,
        "out-of-order Surface/Runtime release destroyed recoverable live handles"
    );
    check(
        strata_surface_prepare_release_packet(surface, &release_sink).status ==
                STRATA_STATUS_OK &&
            release_packet.starts_with("STRATARP"),
        "Surface terminal release packet was not published"
    );
    const std::string first_release_packet = release_packet;
    release_packet.clear();
    check(
        strata_surface_prepare_release_packet(surface, &release_sink).status ==
                STRATA_STATUS_OK &&
            release_packet == first_release_packet &&
            strata_surface_frame(surface, 12'000, &frame_info).status ==
                STRATA_STATUS_INVALID_ARGUMENT &&
            strata_surface_reload_resources(surface).status == STRATA_STATUS_INVALID_ARGUMENT,
        "Surface release preparation was not idempotent and terminal"
    );
    check(
        strata_surface_release(surface).status == STRATA_STATUS_INVALID_ARGUMENT &&
            strata_surface_acknowledge_release_packet(surface).status == STRATA_STATUS_OK &&
            strata_surface_release(surface).status == STRATA_STATUS_OK,
        "Surface release did not enforce preparation, consumption acknowledgement, and release order"
    );

    const auto release_candidate = [&](strata_surface* const candidate) {
        release_packet.clear();
        check(
            strata_surface_prepare_release_packet(candidate, &release_sink).status ==
                    STRATA_STATUS_OK &&
                strata_surface_acknowledge_release_packet(candidate).status == STRATA_STATUS_OK &&
                strata_surface_release(candidate).status == STRATA_STATUS_OK,
            "resource-transaction test Surface did not complete its release barrier"
        );
    };
    strata_surface_config transactional_config = surface_config;
    transactional_config.id = view("abi.reentrant.first");
    file_resources.reentrant_runtime = runtime;
    file_resources.load_count = 0U;
    file_resources.reentrant_swap_on_load = 1U;
    file_resources.reentrant_generation = 3U;
    strata_surface* transactional_surface = reinterpret_cast<strata_surface*>(UINTPTR_MAX);
    check(
        strata_runtime_create_surface(runtime, &transactional_config, &transactional_surface).status ==
                STRATA_STATUS_SERVICE_UNAVAILABLE &&
            transactional_surface == nullptr &&
            file_resources.reentrant_status == STRATA_STATUS_OK,
        "reentrant first-load adapter replacement published a mixed-generation Surface"
    );
    file_resources.reentrant_swap_on_load = 0U;
    file_resources.load_count = 0U;
    check(
        strata_runtime_create_surface(runtime, &transactional_config, &transactional_surface).status ==
                STRATA_STATUS_OK &&
            transactional_surface != nullptr,
        "first-load adapter replacement was not retryable against its installed generation"
    );
    release_candidate(transactional_surface);

    const strata_resource_adapter fourth_resource_adapter{
        sizeof(strata_resource_adapter), &file_resources, 4U, &load_file_resource,
    };
    check(
        strata_runtime_set_resource_adapter(runtime, &fourth_resource_adapter).status ==
            STRATA_STATUS_OK,
        "middle-load transaction setup did not install generation four"
    );
    transactional_config.id = view("abi.reentrant.middle");
    file_resources.load_count = 0U;
    file_resources.reentrant_swap_on_load = 4U;
    file_resources.reentrant_generation = 5U;
    transactional_surface = reinterpret_cast<strata_surface*>(UINTPTR_MAX);
    check(
        strata_runtime_create_surface(runtime, &transactional_config, &transactional_surface).status ==
                STRATA_STATUS_SERVICE_UNAVAILABLE &&
            transactional_surface == nullptr &&
            file_resources.reentrant_status == STRATA_STATUS_OK,
        "reentrant middle-load adapter replacement published mixed font/static assets"
    );
    file_resources.reentrant_swap_on_load = 0U;
    file_resources.load_count = 0U;
    check(
        strata_runtime_create_surface(runtime, &transactional_config, &transactional_surface).status ==
                STRATA_STATUS_OK &&
            transactional_surface != nullptr,
        "middle-load adapter replacement was not retryable against its installed generation"
    );
    release_candidate(transactional_surface);
    file_resources.reentrant_runtime = nullptr;

    const std::string invalid_diagnostic_source = R"(
screen Broken {
  root DefinitelyMissing(key: "broken.root")
}
)";
    const strata_activation_config invalid_activation{
        sizeof(strata_activation_config),
        3U,
        view("abi/broken.strata"),
        view(invalid_diagnostic_source),
        nullptr,
        nullptr,
    };
    check(
        strata_runtime_compile_and_activate(runtime, &invalid_activation, &activation_info).status ==
            STRATA_STATUS_COMPILE_FAILED,
        "invalid activation unexpectedly succeeded"
    );
    check(
        diagnostics.last_version == STRATA_DIAGNOSTIC_VERSION_CURRENT &&
            diagnostics.last_occurrence_count == 1U &&
            !diagnostics.last_component_path.empty() && !diagnostics.last_expected.empty() &&
            diagnostics.last_source_id == "abi/broken.strata",
        "compiler diagnostic context was stripped by the canonical callback payload"
    );
    check(
        strata_runtime_clear_diagnostics(runtime).status == STRATA_STATUS_OK,
        "runtime diagnostic clear failed"
    );
    DiagnosticSnapshotCapture cleared_diagnostic_snapshot;
    const strata_diagnostics_snapshot_sink cleared_snapshot_sink{
        sizeof(strata_diagnostics_snapshot_sink),
        &cleared_diagnostic_snapshot,
        &capture_diagnostic_snapshot,
    };
    check(
        strata_runtime_read_diagnostics(runtime, &cleared_snapshot_sink).status ==
                STRATA_STATUS_OK &&
            cleared_diagnostic_snapshot.record_count == 0U &&
            cleared_diagnostic_snapshot.dropped_count == 0U,
        "runtime diagnostic clear left retained or dropped state behind"
    );

    check(
        strata_runtime_release(runtime).status == STRATA_STATUS_OK,
        "empty Runtime release failed after Surface barrier completion"
    );
    strata_action_registration_release(registration);
}

struct ServiceCapture final {
    std::string resource_id;
    std::string resource_bytes = "resource-bytes";
    std::string clipboard = "clipboard-initial";
    std::uint64_t clipboard_writes = 0U;
    std::uint64_t ime_active_calls = 0U;
    std::uint64_t ime_rect_calls = 0U;
    strata_rect ime_rect{};
    std::string effect_id;
    std::string effect_payload;
};

strata_status load_resource(
    void* const user_data,
    const strata_string_view id,
    strata_bytes_view* const out_bytes
) {
    auto& capture = *static_cast<ServiceCapture*>(user_data);
    capture.resource_id.assign(id.data, id.size);
    *out_bytes = strata_bytes_view{
        reinterpret_cast<const std::uint8_t*>(capture.resource_bytes.data()),
        capture.resource_bytes.size(),
    };
    return STRATA_STATUS_OK;
}

strata_status read_clipboard(void* const user_data, strata_string_view* const out_text) {
    auto& capture = *static_cast<ServiceCapture*>(user_data);
    *out_text = view(capture.clipboard);
    return STRATA_STATUS_OK;
}

strata_status write_clipboard(void* const user_data, const strata_string_view text) {
    auto& capture = *static_cast<ServiceCapture*>(user_data);
    capture.clipboard.assign(text.data, text.size);
    ++capture.clipboard_writes;
    return STRATA_STATUS_OK;
}

strata_status set_ime_active(void* const user_data, const std::uint32_t) {
    ++static_cast<ServiceCapture*>(user_data)->ime_active_calls;
    return STRATA_STATUS_OK;
}

strata_status set_ime_rect(void* const user_data, const strata_rect rect) {
    auto& capture = *static_cast<ServiceCapture*>(user_data);
    ++capture.ime_rect_calls;
    capture.ime_rect = rect;
    return STRATA_STATUS_OK;
}

strata_status emit_effect(
    void* const user_data,
    const strata_string_view id,
    const strata_string_view payload
) {
    auto& capture = *static_cast<ServiceCapture*>(user_data);
    capture.effect_id.assign(id.data, id.size);
    capture.effect_payload.assign(payload.data, payload.size);
    return STRATA_STATUS_OK;
}

void test_host_service_adapters() {
    Clock clock{9'000};
    DiagnosticCapture diagnostics;
    strata_runtime_config runtime_config = config(clock, diagnostics);
    runtime_config.required_capabilities |=
        STRATA_CAPABILITY_RESOURCE_ADAPTER |
        STRATA_CAPABILITY_CLIPBOARD_IME_ADAPTER |
        STRATA_CAPABILITY_EFFECT_ADAPTER;
    strata_runtime* runtime = nullptr;
    check(strata_runtime_create(&runtime_config, &runtime).status == STRATA_STATUS_OK,
          "host-service runtime creation failed");
    check(
        strata_runtime_clipboard_write(runtime, view("missing")).status ==
            STRATA_STATUS_SERVICE_UNAVAILABLE,
        "missing clipboard adapter did not fail explicitly"
    );

    ServiceCapture services;
    const strata_resource_adapter resources{
        sizeof(strata_resource_adapter), &services, 20U, &load_resource,
    };
    const strata_clipboard_adapter clipboard{
        sizeof(strata_clipboard_adapter), &services, &read_clipboard, &write_clipboard,
    };
    const strata_ime_adapter ime{
        sizeof(strata_ime_adapter), &services, &set_ime_active, &set_ime_rect,
    };
    const strata_effect_adapter effects{
        sizeof(strata_effect_adapter), &services, &emit_effect,
    };
    check(strata_runtime_set_resource_adapter(runtime, &resources).status == STRATA_STATUS_OK &&
              strata_runtime_set_clipboard_adapter(runtime, &clipboard).status == STRATA_STATUS_OK &&
              strata_runtime_set_ime_adapter(runtime, &ime).status == STRATA_STATUS_OK &&
              strata_runtime_set_effect_adapter(runtime, &effects).status == STRATA_STATUS_OK,
          "host-service adapter configuration failed");

    std::string bytes;
    const strata_bytes_sink bytes_sink{sizeof(strata_bytes_sink), &bytes, &capture_bytes};
    check(strata_runtime_read_resource(runtime, view("fixture:resource"), &bytes_sink).status ==
              STRATA_STATUS_OK && services.resource_id == "fixture:resource" &&
              bytes == "resource-bytes",
          "resource adapter transfer changed");
    std::string clipboard_text;
    const strata_string_sink text_sink{sizeof(strata_string_sink), &clipboard_text, &capture_json};
    check(strata_runtime_clipboard_read(runtime, &text_sink).status == STRATA_STATUS_OK &&
              clipboard_text == "clipboard-initial",
          "clipboard adapter read changed");
    check(strata_runtime_clipboard_write(runtime, view("clipboard-updated")).status ==
              STRATA_STATUS_OK && services.clipboard == "clipboard-updated" &&
              services.clipboard_writes == 1U,
          "clipboard adapter write changed");
    check(strata_runtime_ime_set_active(runtime, 1U).status == STRATA_STATUS_OK &&
              strata_runtime_ime_set_cursor_rect(runtime, strata_rect{1.0, 2.0, 3.0, 4.0}).status ==
                  STRATA_STATUS_OK && services.ime_active_calls == 1U &&
              services.ime_rect_calls == 1U && services.ime_rect.height == 4.0,
          "IME adapter updates changed");
    const strata_result effect_result = strata_runtime_emit_effect_json(
        runtime, view("host.open"), view("{\"z\":1,\"a\":2}")
    );
    check(effect_result.status == STRATA_STATUS_OK && services.effect_id == "host.open" &&
              services.effect_payload == "{\n  \"a\": 2,\n  \"z\": 1\n}\n",
          "effect adapter did not receive canonical JSON: status=" +
              std::to_string(effect_result.status) + ", code=" + diagnostics.last_code +
              ", payload=" + services.effect_payload);

    check(strata_runtime_set_clipboard_adapter(runtime, nullptr).status == STRATA_STATUS_OK &&
              strata_runtime_clipboard_read(runtime, &text_sink).status ==
                  STRATA_STATUS_SERVICE_UNAVAILABLE,
          "clipboard adapter detach did not sever the host callback");
    strata_runtime_release(runtime);
}

struct AllocationState final {
    std::size_t allocations = 0U;
    std::size_t deallocations = 0U;
    std::size_t outstanding = 0U;
    bool fail = false;
};

void* tracked_allocate(
    void* const user_data,
    const std::size_t size,
    const std::size_t alignment
) {
    auto& state = *static_cast<AllocationState*>(user_data);
    if (state.fail) {
        return nullptr;
    }
    void* const allocation = ::operator new(size, std::align_val_t(alignment), std::nothrow);
    if (allocation != nullptr) {
        ++state.allocations;
        ++state.outstanding;
    }
    return allocation;
}

void tracked_deallocate(
    void* const user_data,
    void* const allocation,
    const std::size_t,
    const std::size_t alignment
) {
    auto& state = *static_cast<AllocationState*>(user_data);
    ++state.deallocations;
    --state.outstanding;
    ::operator delete(allocation, std::align_val_t(alignment));
}

void test_allocator_ownership_and_validation() {
    Clock clock{1};
    DiagnosticCapture diagnostics;
    AllocationState allocation_state;
    strata_runtime_config runtime_config = config(clock, diagnostics);
    runtime_config.allocator = strata_allocator{
        sizeof(strata_allocator),
        &allocation_state,
        &tracked_allocate,
        &tracked_deallocate,
    };

    strata_runtime* runtime = nullptr;
    check(strata_runtime_create(&runtime_config, &runtime).status == STRATA_STATUS_OK, "custom allocator failed");
    strata_snapshot* snapshot = nullptr;
    check(strata_runtime_create_snapshot(runtime, &snapshot).status == STRATA_STATUS_OK, "snapshot failed");
    check(allocation_state.outstanding == 2U, "runtime and snapshot must use the host allocator");
    strata_runtime_release(runtime);
    check(allocation_state.outstanding == 1U, "snapshot ownership must be independent");
    strata_snapshot_release(snapshot);
    check(allocation_state.outstanding == 0U, "allocator-owned handles leaked");
    check(
        allocation_state.allocations == allocation_state.deallocations,
        "allocation was returned through a different lifetime path"
    );

    runtime_config.allocator = strata_allocator{sizeof(strata_allocator), nullptr, nullptr, nullptr};
    runtime = nullptr;
    check(
        strata_runtime_create(&runtime_config, &runtime).status == STRATA_STATUS_OK,
        "an explicitly initialized empty optional allocator must select the library allocator"
    );
    strata_runtime_release(runtime);

    runtime_config.allocator = strata_allocator{
        sizeof(strata_allocator),
        &allocation_state,
        &tracked_allocate,
        nullptr,
    };
    runtime = reinterpret_cast<strata_runtime*>(UINTPTR_MAX);
    const strata_result half_allocator = strata_runtime_create(&runtime_config, &runtime);
    check(half_allocator.status == STRATA_STATUS_INVALID_ARGUMENT, "half allocator was accepted");
    check(runtime == nullptr, "failed create must clear runtime output");

    runtime_config.allocator.deallocate = &tracked_deallocate;
    allocation_state.fail = true;
    const strata_result out_of_memory = strata_runtime_create(&runtime_config, &runtime);
    check(out_of_memory.status == STRATA_STATUS_OUT_OF_MEMORY, "allocator failure lost its status");
}

void test_invalid_contracts() {
    Clock clock{0};
    DiagnosticCapture diagnostics;
    strata_runtime_config runtime_config = config(clock, diagnostics);
    runtime_config.required_capabilities = UINT64_C(1) << 63U;
    strata_runtime* runtime = nullptr;
    check(
        strata_runtime_create(&runtime_config, &runtime).status ==
            STRATA_STATUS_UNSUPPORTED_CAPABILITY,
        "unsupported capability was accepted"
    );
    runtime_config.required_capabilities = 0U;
    runtime_config.clock.now_nanoseconds = nullptr;
    check(
        strata_runtime_create(&runtime_config, &runtime).status == STRATA_STATUS_INVALID_ARGUMENT,
        "missing caller clock was accepted"
    );
    strata_runtime_memory_info memory_info{};
    check(
        strata_runtime_get_memory_info(nullptr, &memory_info).status ==
            STRATA_STATUS_INVALID_ARGUMENT,
        "null runtime memory query was accepted"
    );
}

} // namespace

int strata_test_abi(const int argument_count, const char* const* const arguments) {
    try {
        test_negotiation();
        test_lifecycle_identity_snapshot_and_clock();
        test_lazy_host_snapshot_abi();
        if (argument_count == 2) test_application_activation_and_action_abi(arguments[1]);
        test_host_service_adapters();
        test_allocator_ownership_and_validation();
        test_invalid_contracts();
        std::cout << "strata_abi_tests: OK\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "strata_abi_tests: " << exception.what() << '\n';
        return 1;
    }
}
