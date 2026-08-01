#include <strata/extension.hpp>
#include <strata/strata.h>

#include "host/extensions.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace strata::extension;

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Callable>
[[nodiscard]] std::string rejection(Callable&& callable) {
    try {
        callable();
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open extension fixture " + path.string());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

[[nodiscard]] strata_string_view view(const std::string_view value) {
    return strata_string_view{value.data(), value.size()};
}

struct Clock final {
    std::int64_t now = 0;
};

std::int64_t clock_now(void* const user_data) {
    return static_cast<Clock*>(user_data)->now;
}

struct DiagnosticLog final {
    std::string last_code;
    std::string last_message;
    std::uint64_t count = 0U;
};

void emit_diagnostic(void* const user_data, const strata_diagnostic* const diagnostic) {
    auto& log = *static_cast<DiagnosticLog*>(user_data);
    ++log.count;
    log.last_code.assign(diagnostic->code.data, diagnostic->code.size);
    log.last_message.assign(diagnostic->message.data, diagnostic->message.size);
}

struct FileResources final {
    std::filesystem::path root;
    std::string loaded;
};

strata_status load_resource(
    void* const user_data,
    const strata_string_view id,
    strata_bytes_view* const out_bytes
) {
    auto& resources = *static_cast<FileResources*>(user_data);
    const std::filesystem::path path = resources.root / std::string(id.data, id.size);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return STRATA_STATUS_NOT_FOUND;
    resources.loaded = read_file(path);
    *out_bytes = strata_bytes_view{
        reinterpret_cast<const std::uint8_t*>(resources.loaded.data()),
        resources.loaded.size(),
    };
    return STRATA_STATUS_OK;
}

struct ActionLog final {
    std::uint64_t calls = 0U;
    std::string last_id;
    std::string last_payload;
};

strata_action_handler_result record_action(
    void* const user_data,
    const strata_action_call* const call
) {
    auto& log = *static_cast<ActionLog*>(user_data);
    ++log.calls;
    log.last_id.assign(call->action_id.data, call->action_id.size);
    log.last_payload.assign(call->payload_json.data, call->payload_json.size);
    return STRATA_ACTION_HANDLER_HANDLED;
}

void capture_json(void* const user_data, const strata_string_view value) {
    static_cast<std::string*>(user_data)->assign(value.data, value.size);
}

/* Harness package exercising every publicly supported widget lifecycle phase. */

struct HarnessCounters final {
    std::uint64_t activations = 0U;
    std::uint64_t keys = 0U;
    std::uint64_t presentations = 0U;
    std::uint64_t overlays = 0U;
    std::uint64_t semantics = 0U;
    std::uint64_t hit_bounds = 0U;
    std::uint64_t behavior_events = 0U;
    bool declared_write_accepted = false;
    bool undeclared_write_rejected = false;
    bool text_write_accepted = false;
    bool retained_text_round_tripped = false;
    bool parameter_default_visible = false;
    bool measured_text = false;
    bool behavior_emit_accepted = false;
    bool pointer_focus_hidden = false;
    bool keyboard_focus_visible = false;
    std::string last_key;
};

HarnessCounters counters;

constexpr auto harness_count = retained<number>("harness.count");
constexpr auto harness_open = retained<boolean>("harness.open");
constexpr auto harness_label = retained<text>("harness.label", "idle", Invalidation::text);
constexpr auto harness_step = parameter<number>("step", 4.0);
constexpr auto harness_caption = parameter<text>("caption");
/* Never declared by the widget: writes through it must fail instead of creating hidden state. */
constexpr auto harness_undeclared = retained<number>("harness.undeclared");

bool harness_activate(Input& input) {
    ++counters.activations;
    counters.parameter_default_visible = input.get(harness_step) == 4.0;
    counters.declared_write_accepted = input.set(harness_count, input.get(harness_count) + input.get(harness_step));
    counters.undeclared_write_rejected = !input.set(harness_undeclared, 1.0);
    counters.text_write_accepted = input.set(harness_label, "pressed");
    counters.retained_text_round_tripped = input.get(harness_label) == "pressed";
    input.set(harness_open, true);
    input.emit("harness.activated", R"({"value":1})");
    return true;
}

bool harness_key(Input& input, const Key& key) {
    ++counters.keys;
    counters.last_key = key.name;
    if (key.name != "right") return false;
    input.set(harness_count, input.get(harness_count) + 10.0);
    return true;
}

constexpr std::array<MeshVertex, 3U> harness_vertices{
    MeshVertex{0.0, 0.0, 0.0, 0.0, 0.0, rgba(255U, 0U, 0U)},
    MeshVertex{1.0, 0.0, 0.0, 1.0, 0.0, rgba(0U, 255U, 0U)},
    MeshVertex{0.5, 1.0, 0.0, 0.5, 1.0, rgba(0U, 0U, 255U)},
};
constexpr std::array<std::uint32_t, 3U> harness_indices{0U, 1U, 2U};
/* Out-of-domain geometry must drop its own draw instead of failing the whole frame. */
constexpr std::array<MeshVertex, 3U> invalid_vertices{
    MeshVertex{0.0, 0.0, 0.0, 0.0, 0.0, rgba(255U, 0U, 0U)},
    MeshVertex{4.0, 0.0, 0.0, 1.0, 0.0, rgba(0U, 255U, 0U)},
    MeshVertex{0.5, 1.0, 0.0, 0.5, 1.0, rgba(0U, 0U, 255U)},
};

void harness_present(Present& present) {
    ++counters.presentations;
    const Rect bounds = present.bounds();
    present.shadow(bounds, corners(4.0), rgba(0U, 0U, 0U, 90U), 6.0, 1.0);
    present.rounded_rect(bounds, 4.0, rgba(30U, 40U, 60U), stroke(1.0, rgba(90U, 110U, 140U)));
    present.blur(bounds, 3.0);
    if (present.measure("harness").has_value()) counters.measured_text = true;
    {
        const ClipScope clip = present.clip(bounds);
        present.text("harness", bounds.x + 4.0, bounds.y + 12.0, rgba(240U, 240U, 240U));
        const Material material{
            "strata:custom_mesh",
            {},
            {},
            1.0,
        };
        present.mesh(
            bounds,
            "harness.mesh",
            Mesh{harness_vertices, harness_indices},
            {},
            material
        );
        present.mesh(bounds, "harness.invalid", Mesh{invalid_vertices, harness_indices});
    }
    counters.pointer_focus_hidden = counters.pointer_focus_hidden ||
        (present.focused() && !present.focus_visible());
    counters.keyboard_focus_visible = counters.keyboard_focus_visible || present.focus_visible();
    if (present.focus_visible()) {
        present.border(bounds, 4.0, stroke(2.0, rgba(120U, 190U, 255U)));
    }
}

void harness_overlay(Present& present) {
    if (!present.get(harness_open)) return;
    ++counters.overlays;
    const Rect anchor = present.bounds();
    present.rounded_rect(
        Rect{anchor.x, anchor.y + anchor.height, 120.0, 24.0},
        4.0,
        rgba(20U, 26U, 36U, 250U)
    );
}

void harness_semantics(Semantics& semantics) {
    ++counters.semantics;
    semantics.name("Harness widget");
    semantics.value_text("value");
    semantics.expanded(semantics.get(harness_open));
    semantics.add_action("activate");
}

Rect harness_hit_bounds(Inspect& inspect) {
    ++counters.hit_bounds;
    const Rect bounds = inspect.layout_bounds();
    return Rect{bounds.x, bounds.y, bounds.width, bounds.height * 0.5};
}

bool harness_pointer(BehaviorInput& input, const Pointer& pointer) {
    ++counters.behavior_events;
    if (pointer.kind != Pointer::Kind::release) return false;
    counters.behavior_emit_accepted = input.emit("harness.observed", R"({"x":1})");
    return false;
}

[[nodiscard]] std::unique_ptr<Package> harness_package() {
    auto harness = widget("HarnessWidget")
        .parameter(harness_step)
        .parameter(harness_caption)
        .no_children()
        .focusable()
        .intrinsic_size(160.0, 40.0)
        .retained(harness_count)
        .retained(harness_open)
        .retained(harness_label)
        .semantics_role("button")
        .semantics_actions({"activate"})
        .depends_on_motion()
        .depends_on_status()
        .emits(ActionContract{
            "harness.activated",
            "Harness widget activation",
            "HarnessActivation",
            "optional",
            {ActionArgument{"value", "number"}},
        })
        .on_activate(&harness_activate)
        .on_key(&harness_key)
        .on_semantics(&harness_semantics)
        .present(&harness_present)
        .detached_overlay(&harness_overlay, harness_open)
        .hit_bounds(&harness_hit_bounds);

    auto observer = behavior("harness.observe")
        .on_pointer(&harness_pointer)
        .emits(ActionContract{
            "harness.observed",
            "Harness behavior observation",
            "HarnessPoint",
            "optional",
            {ActionArgument{"x", "number"}},
        });

    auto created = package("strata.harness.v1");
    created->widget(std::move(harness)).behavior(std::move(observer));
    return created;
}

void test_definition_diagnostics() {
    check(
        rejection([] { static_cast<void>(widget("")); }).find("must not be empty") !=
            std::string::npos,
        "an unnamed extension widget was accepted"
    );
    check(
        rejection([] {
            widget("Duplicate")
                .parameter(parameter<number>("step", 1.0))
                .parameter(parameter<boolean>("step"));
        }).find("declares parameter 'step' twice") != std::string::npos,
        "a duplicate extension parameter was accepted"
    );
    check(
        rejection([] {
            widget("Duplicate")
                .retained(retained<number>("field"))
                .retained(retained<number>("field"));
        })
                .find("declares retained field 'field' twice") != std::string::npos,
        "a duplicate retained field was accepted"
    );
    check(
        rejection([] {
            widget("Popup").detached_overlay(&harness_overlay, retained<boolean>(""));
        })
                .find("without a retained field") != std::string::npos,
        "a detached overlay without retained state was accepted"
    );
    check(
        rejection([] {
            auto duplicated = package("strata.duplicate.v1");
            duplicated->widget(widget("Same")).widget(widget("Same"));
        }).find("declares widget 'Same' twice") != std::string::npos,
        "a duplicate widget declaration was accepted inside one package"
    );
    check(
        rejection([] {
            auto sealed = package("strata.sealed.v1");
            static_cast<void>(sealed->bundle());
            sealed->widget(widget("Late"));
        }).find("already in use") != std::string::npos,
        "a package accepted a widget after its bundle was taken"
    );
}

void test_external_package_and_schema_projection() {
    strata::host::SelectedExtensions selected =
        strata::host::select_extensions({"strata.demo.v1"});
    check(
        selected.packages.size() == 1U && selected.packages.front().id() == "strata.demo.v1",
        "the external demo package was not loaded"
    );
    check(
        selected.widgets.size() == 2U && selected.behaviors.size() == 1U,
        "the external demo package did not project its runtime descriptors"
    );
    const std::string unknown = rejection([] {
        static_cast<void>(strata::host::select_extensions({"strata.missing.v1"}));
    });
    check(
        unknown.find("strata.missing.v1") != std::string::npos &&
            unknown.find("strata-extension-strata.missing.v1") != std::string::npos,
        "an unknown external package did not report its id and expected library"
    );
    check(
        rejection([] {
            static_cast<void>(strata::host::select_extensions(
                {"strata.demo.v1", "strata.demo.v1"}
            ));
        }).find("must be unique") != std::string::npos,
        "duplicate external package selection was accepted"
    );

    const std::string schema = selected.packages.front().schema_json();
    for (const std::string_view expected : {
             std::string_view(R"("name":"DemoPulse")"),
             std::string_view(R"("name":"DemoDisclosure")"),
             std::string_view(R"("allowsChildren":true)"),
             std::string_view(R"("id":"demo.custom.pulse")"),
             std::string_view(R"("payloadContract":"DemoPulseActivation")"),
             std::string_view(R"("id":"demo.inspect.pointer")"),
             std::string_view(R"("name":"step")"),
             std::string_view(R"("ids":["demo.inspector-pick"])"),
         }) {
        check(
            schema.find(expected) != std::string::npos,
            "projected package schema is missing a declaration"
        );
    }
}

[[nodiscard]] strata_runtime_config runtime_config(Clock& clock, DiagnosticLog& diagnostics) {
    strata_runtime_config config{};
    config.struct_size = sizeof(strata_runtime_config);
    config.abi_version = STRATA_ABI_VERSION_CURRENT;
    config.required_capabilities = STRATA_CAPABILITY_CORE_LIFECYCLE |
        STRATA_CAPABILITY_CALLER_CLOCK | STRATA_CAPABILITY_APPLICATION_LIFECYCLE |
        STRATA_CAPABILITY_COMPILER_ACTIVATION | STRATA_CAPABILITY_ACTION_DISPATCH |
        STRATA_CAPABILITY_RESOURCE_ADAPTER | STRATA_CAPABILITY_SURFACE_RUNTIME;
    config.clock = strata_clock{sizeof(strata_clock), &clock, &clock_now};
    config.diagnostics = strata_diagnostic_sink{
        sizeof(strata_diagnostic_sink), &diagnostics, &emit_diagnostic,
    };
    return config;
}

void test_surface_lifecycle(const std::filesystem::path& registry_path) {
    counters = HarnessCounters{};
    const std::unique_ptr<Package> harness = harness_package();
    const std::string package_schema = harness->schema_json();

    Clock clock{8'000};
    DiagnosticLog diagnostics;
    strata_runtime_config config = runtime_config(clock, diagnostics);
    strata_runtime* runtime = nullptr;
    check(
        strata_runtime_create(&config, &runtime).status == STRATA_STATUS_OK,
        "extension harness runtime creation failed"
    );

    FileResources resources{registry_path.parent_path().parent_path(), {}};
    const strata_resource_adapter adapter{
        sizeof(strata_resource_adapter), &resources, 1U, &load_resource,
    };
    check(
        strata_runtime_set_resource_adapter(runtime, &adapter).status == STRATA_STATUS_OK,
        "extension harness resource adapter installation failed"
    );

    const std::string registry = read_file(registry_path);
    const strata_string_view extension_schemas[]{view(package_schema)};
    const strata_application_config application{
        sizeof(strata_application_config),
        view("strata.extension.harness"),
        view(registry),
        strata_string_view{nullptr, 0U},
        extension_schemas,
        std::size(extension_schemas),
    };
    check(
        strata_runtime_configure_application(runtime, &application).status == STRATA_STATUS_OK,
        "package declarations were rejected as an application schema source"
    );

    ActionLog actions;
    strata_action_registration* activation_registration = nullptr;
    strata_action_registration* observation_registration = nullptr;
    const strata_action_handler_config activation_handler{
        sizeof(strata_action_handler_config), view("harness.activated"), view("strata.harness"),
        &actions, &record_action,
    };
    const strata_action_handler_config observation_handler{
        sizeof(strata_action_handler_config), view("harness.observed"), view("strata.harness"),
        &actions, &record_action,
    };
    check(
        strata_runtime_register_action_handler(runtime, &activation_handler, &activation_registration)
                .status == STRATA_STATUS_OK &&
            strata_runtime_register_action_handler(
                runtime, &observation_handler, &observation_registration
            ).status == STRATA_STATUS_OK,
        "package-declared action contracts were not registrable"
    );

    const std::string source = R"(
screen Main {
  root Panel(key: "harness.root", layout: { kind: "COLUMN" }, behaviors: [{ id: "harness.observe" }]) {
    HarnessWidget(key: "harness.widget", layout: { width: 160, height: 40 })
  }
}
)";
    const strata_activation_config activation{
        sizeof(strata_activation_config),
        1U,
        view("harness/main.strata"),
        view(source),
        nullptr,
        nullptr,
    };
    strata_activation_info activation_info{};
    activation_info.struct_size = sizeof(activation_info);
    check(
        strata_runtime_compile_and_activate(runtime, &activation, &activation_info).status ==
                STRATA_STATUS_OK &&
            activation_info.status == STRATA_ACTIVATION_ACTIVATED,
        "a module using only package-declared widgets did not compile against the projected schema"
    );

    const strata_surface_environment environment{
        sizeof(strata_surface_environment),
        1U,
        480,
        320,
        480.0,
        320.0,
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
    const strata_surface_font_resource fonts[]{
        {view("strata:fonts/default-medium"), view(control_font)},
        {view("strata:fonts/default"), view(regular_font)},
    };
    strata_surface_config surface_config{};
    surface_config.struct_size = sizeof(surface_config);
    surface_config.id = view("harness.surface");
    surface_config.root_role = STRATA_SURFACE_ROOT_SCREEN;
    surface_config.root_name = view("Main");
    surface_config.environment = environment;
    surface_config.fonts = fonts;
    surface_config.font_count = std::size(fonts);
    surface_config.extensions = &harness->bundle();
    strata_surface* surface = nullptr;
    check(
        strata_runtime_create_surface(runtime, &surface_config, &surface).status ==
                STRATA_STATUS_OK,
        "the projected extension bundle was rejected by surface creation"
    );

    strata_surface_frame_info frame_info{};
    frame_info.struct_size = sizeof(frame_info);
    const strata_result first_frame = strata_surface_frame(surface, 8'000, &frame_info);
    check(
        first_frame.status == STRATA_STATUS_OK && frame_info.render_command_count >= 2U &&
            frame_info.semantics_generation != 0U,
        "the first extension frame produced no render or semantics output: status=" +
            std::to_string(first_frame.status) +
            " commands=" + std::to_string(frame_info.render_command_count) +
            " diagnostic=" + diagnostics.last_code + " " + diagnostics.last_message
    );
    check(counters.presentations != 0U, "the presentation hook did not run");
    check(counters.measured_text, "text measurement was unavailable during presentation");

    std::string frame_json;
    const strata_value_json_sink frame_sink{
        sizeof(strata_value_json_sink), &frame_json, &capture_json,
    };
    check(
        strata_surface_read_frame_json(surface, &frame_sink).status == STRATA_STATUS_OK &&
            frame_json.find("Harness widget") != std::string::npos,
        "extension semantics did not reach the published frame"
    );
    for (const std::string_view primitive : {
             std::string_view("custom_mesh"),
             std::string_view("shadow"),
             std::string_view("blur"),
             std::string_view("clip"),
         }) {
        check(
            frame_json.find(primitive) != std::string::npos,
            "an extension render primitive did not reach the published frame"
        );
    }
    check(counters.semantics != 0U, "the semantics hook did not run");

    strata_input_event click[2]{};
    click[0].struct_size = sizeof(strata_input_event);
    click[0].version = STRATA_INPUT_EVENT_VERSION_2;
    click[0].kind = STRATA_INPUT_POINTER_PRESS;
    click[0].x = 8.0;
    click[0].y = 8.0;
    click[1] = click[0];
    click[1].kind = STRATA_INPUT_POINTER_RELEASE;
    strata_surface_input_batch_info batch{};
    batch.struct_size = sizeof(batch);
    check(
        strata_surface_enqueue_input(surface, click, 2U, &batch).status == STRATA_STATUS_OK &&
            strata_surface_frame(surface, 9'000, &frame_info).status == STRATA_STATUS_OK,
        "pointer input was not accepted by the extension surface"
    );
    check(counters.activations == 1U, "the activation hook did not run exactly once");
    check(counters.parameter_default_visible, "a declared parameter default was not readable");
    check(counters.declared_write_accepted, "a declared retained write was rejected");
    check(counters.undeclared_write_rejected, "an undeclared retained write silently succeeded");
    check(counters.text_write_accepted, "a declared retained text write was rejected");
    check(counters.retained_text_round_tripped, "retained text did not round-trip");
    check(counters.behavior_events != 0U, "the behavior pointer hook did not run");
    check(counters.hit_bounds != 0U, "the inspection hook did not narrow pointer hit testing");
    check(
        actions.calls != 0U && counters.behavior_emit_accepted,
        "package-declared actions did not dispatch through the runtime registry"
    );
    check(counters.overlays != 0U, "the detached overlay did not paint once its retained flag was set");
    check(
        counters.pointer_focus_hidden && !counters.keyboard_focus_visible,
        "pointer-focused extension did not preserve semantic focus while hiding focus paint"
    );

    strata_input_event key{};
    key.struct_size = sizeof(strata_input_event);
    key.version = STRATA_INPUT_EVENT_VERSION_2;
    key.kind = STRATA_INPUT_KEY;
    key.key_action = STRATA_KEY_PRESS;
    key.text = view("Right");
    check(
        strata_surface_enqueue_input(surface, &key, 1U, &batch).status == STRATA_STATUS_OK &&
            strata_surface_frame(surface, 10'000, &frame_info).status == STRATA_STATUS_OK,
        "key input was not accepted by the extension surface"
    );
    check(
        counters.keys != 0U && counters.last_key == "right",
        "the key hook did not receive the focused key press"
    );
    check(
        counters.keyboard_focus_visible,
        "keyboard input did not expose focus-visible state to the extension presenter"
    );

    strata_surface_release(surface);
    strata_action_registration_release(activation_registration);
    strata_action_registration_release(observation_registration);
    strata_runtime_release(runtime);
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    try {
        test_definition_diagnostics();
        test_external_package_and_schema_projection();
        if (argument_count == 2) test_surface_lifecycle(std::filesystem::path(arguments[1]));
        std::cout << "strata_extension_tests: OK\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "strata_extension_tests: " << exception.what() << '\n';
        return 1;
    }
}
