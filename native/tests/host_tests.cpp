#include <strata/host.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void check(const bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::int64_t clock(void* const user_data) noexcept {
    return *static_cast<std::int64_t*>(user_data);
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("could not open host test registry");
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void structured_values_round_trip() {
    using strata::host::Value;
    const Value source = Value::object({
        {"active", true},
        {"count", 7},
        {"items", Value::array({"alpha", "beta"})},
        {"nested", Value::object({{"ratio", 0.25}, {"whole", 1.0}, {"value", nullptr}})},
    });
    const std::string json = source.json();
    const Value decoded = Value::parse(json);
    check(decoded == source, "typed host value did not round-trip through canonical JSON");
    check(decoded.require("items").array() != nullptr &&
              decoded.require("items").array()->size() == 2U,
          "typed host array changed shape during round-trip");
}

void drag_events_are_typed_once() {
    using namespace strata::host;
    const ActionEvent action{
        "demo.reorder",
        Value::object({}),
        "drag",
        std::string("target"),
        Value::parse(R"({
          "phase":"drop",
          "payload":{"type":"demo.item","value":"beta"},
          "targetKey":"list",
          "operation":"move",
          "placement":"before",
          "beforeKey":"alpha",
          "afterKey":null,
          "insertionIndex":0
        })"),
    };
    const std::optional<DragEvent> drag = DragEvent::from(action);
    check(drag.has_value() && drag->dropped() && drag->payload_type == "demo.item" &&
              drag->payload.string() != nullptr && *drag->payload.string() == "beta" &&
              drag->target_key == std::optional<std::string>("list") &&
              drag->placement == DropPlacement::before &&
              drag->before_key == std::optional<std::string>("alpha") &&
              drag->insertion_index == std::optional<std::size_t>(0U),
          "shared drag event did not decode into its typed host projection");
}

void list_reorder_uses_stable_neighbors() {
    struct Item final {
        explicit Item(std::string key) : key(std::move(key)) {}
        Item(const Item&) = delete;
        Item& operator=(const Item&) = delete;
        Item(Item&&) noexcept = default;
        Item& operator=(Item&&) noexcept = default;
        std::string key;
    };
    std::vector<Item> values;
    values.emplace_back("alpha");
    values.emplace_back("beta");
    values.emplace_back("gamma");
    check(strata::host::reorder_before_after(values, "gamma", std::optional<std::string>("alpha"),
                                             std::nullopt, &Item::key) &&
              values[0].key == "gamma" && values[1].key == "alpha" && values[2].key == "beta",
          "stable-neighbor list reorder produced the wrong order");
}

void action_bindings_decode_at_the_boundary(const std::filesystem::path& registry_path) {
    std::int64_t now = 1;
    strata_runtime_config config{};
    config.struct_size = sizeof(config);
    config.abi_version = STRATA_ABI_VERSION_CURRENT;
    config.required_capabilities =
        STRATA_CAPABILITY_CORE_LIFECYCLE | STRATA_CAPABILITY_CALLER_CLOCK |
        STRATA_CAPABILITY_APPLICATION_LIFECYCLE | STRATA_CAPABILITY_ACTION_DISPATCH;
    config.clock = strata_clock{sizeof(strata_clock), &now, &clock};
    strata::Runtime runtime(config);
    const std::string registry = read_file(registry_path);
    const std::string schemas = R"({
      "widgets":{"registry":"host-tests.v1","required":[],"definitions":[]},
      "actions":{"registry":"host-tests.v1","required":["host.test"],"definitions":[{
        "id":"host.test","dispatchPolicy":"required","summary":"typed host test",
        "payloadContract":"no payload","arguments":[]
      }]},
      "host":[]
    })";
    const strata_application_config application{
        sizeof(strata_application_config),
        strata::view("host-tests.actions"),
        strata::view(registry),
        strata::view(schemas),
        nullptr,
        0U,
    };
    runtime.configure_application(application);

    std::optional<strata::host::ActionEvent> captured;
    strata::host::Bindings bindings(runtime, "strata.host-tests");
    bindings.on("host.test", [&captured](const strata::host::ActionEvent& event) {
        captured = event;
        return strata::host::ActionResult::handled;
    });
    strata::Runtime active_runtime = std::move(runtime);
    const strata::ActionDispatchInfo info = active_runtime.dispatch(strata::ActionDispatch{
        .action_id = "host.test",
        .payload_json = "null",
        .event_kind = "host-test-event",
        .source_key = "host.source",
        .event_value_json = R"({"name":"alpha","count":3})",
        .state_scope = std::nullopt,
        .dynamic = false,
    });
    check(info.status == strata::ActionDispatchStatus::handled && captured.has_value() &&
              captured->id == "host.test" && captured->kind == "host-test-event" &&
              captured->source_key == std::optional<std::string>("host.source") &&
              captured->value.require_string("name") == "alpha" &&
              captured->value.optional_integer("count") == std::optional<std::int64_t>(3),
          "typed action binding leaked or mistranslated the ABI JSON call");
}

void public_runtime_facade_owns_host_boundaries(const std::filesystem::path& registry_path) {
    std::int64_t now = 100;
    std::vector<strata::Diagnostic> observed_diagnostics;
    strata::RuntimeOptions options;
    options.clock = [&now] { return now; };
    options.diagnostic = [&observed_diagnostics](const strata::Diagnostic& diagnostic) {
        observed_diagnostics.push_back(diagnostic);
    };
    strata::Runtime runtime(std::move(options));

    runtime.set_resource_adapter(strata::ResourceAdapter{
        .generation = 1U,
        .load = [](const std::string_view id)
            -> std::optional<std::vector<std::uint8_t>> {
            if (id != "host-tests.resource") return std::nullopt;
            return std::vector<std::uint8_t>{1U, 2U, 3U};
        },
    });
    const auto resource = runtime.resource("host-tests.resource");
    check(resource.has_value() && *resource == std::vector<std::uint8_t>({1U, 2U, 3U}),
          "owned resource adapter did not cross the public C++ facade");

    std::string clipboard;
    runtime.set_clipboard_adapter(strata::ClipboardAdapter{
        .read = [&clipboard] { return std::optional<std::string>(clipboard); },
        .write = [&clipboard](const std::string_view value) { clipboard = value; },
    });
    runtime.set_clipboard_text("facade clipboard");
    check(runtime.clipboard_text() == std::optional<std::string>("facade clipboard"),
          "owned clipboard adapter did not round-trip text");

    bool ime_active = false;
    strata::Rect ime_rect;
    runtime.set_ime_adapter(strata::ImeAdapter{
        .set_active = [&ime_active](const bool active) { ime_active = active; },
        .set_cursor_rect = [&ime_rect](const strata::Rect rect) { ime_rect = rect; },
    });
    runtime.set_ime_active(true);
    runtime.set_ime_cursor_rect(strata::Rect{1.0, 2.0, 3.0, 4.0});
    check(ime_active && ime_rect.width == 3.0 && ime_rect.height == 4.0,
          "owned IME adapter did not receive state and geometry");

    std::optional<strata::EffectRequest> effect;
    runtime.set_effect_adapter(strata::EffectAdapter{
        .emit = [&effect](const strata::EffectRequest& request) { effect = request; },
    });
    runtime.emit_effect(strata::EffectRequest{"host-tests.effect", R"({"value":1})"});
    check(effect.has_value() && effect->id == "host-tests.effect",
          "owned effect adapter did not receive the effect");

    runtime.configure_application(strata::ApplicationOptions{
        .id = "host-tests.facade",
        .registry_json = read_file(registry_path),
    });
    runtime.set_profiler_capture(true);
    runtime.set_durable_store(strata::DurableStoreAdapter{
        .load = [](std::string_view) -> std::optional<std::vector<std::uint8_t>> {
            return std::nullopt;
        },
        .write = [](std::string_view, std::span<const std::uint8_t>) {},
    });
    runtime.set_async_host(strata::AsyncHostAdapter{
        .begin = [](const strata::AsyncRequest&) {},
        .cancel = [](std::uint64_t) {},
    });
    constexpr std::string_view source =
        "overlay Main { root Panel(key: \"facade.root\") }";
    check(runtime.activate(strata::SourceActivation{
              .generation = 1U,
              .entry_source_id = "host-tests/facade.strata",
              .entry_text = std::string(source),
          }).activated(),
          "owned source activation facade rejected valid source");

    const strata::Snapshot snapshot = runtime.snapshot();
    check(snapshot.info().time_nanoseconds == now,
          "RAII runtime snapshot did not retain the caller clock");
    const strata::ApplicationStateSnapshot state = runtime.application_state();

    strata::SurfaceOptions surface_options;
    surface_options.id = "host-tests.facade.surface";
    surface_options.root_role = strata::SurfaceRootRole::overlay;
    surface_options.root_name = "Main";
    surface_options.environment.framebuffer_width = 320;
    surface_options.environment.framebuffer_height = 200;
    surface_options.environment.logical_width = 320.0;
    surface_options.environment.logical_height = 200.0;
    strata::Surface surface = runtime.create_surface(surface_options);
    surface.set_profiler_capture(true);
    check(surface.enqueue(strata::InputEvent::pointer(
              strata::InputKind::pointer_move, strata::Point{10.0, 20.0}
          )).accepted_event_count == 1U,
          "owned input event did not enqueue");
    ++now;
    check(surface.frame(now).processed_input_event_count == 1U,
          "owned input event did not reach the Surface frame");
    strata::ProfilerHostFrame host_frame;
    host_frame.draw_calls = 1U;
    surface.record_host_frame(host_frame);
    check(surface.profiler().scope == strata::ProfilerScope::surface &&
              runtime.profiler().scope == strata::ProfilerScope::runtime,
          "owned profiler snapshots did not preserve their scope");

    strata::SurfaceEnvironment resized = surface_options.environment;
    resized.generation = 2U;
    resized.framebuffer_width = 640;
    resized.logical_width = 640.0;
    check(surface.adopt_environment(resized),
          "owned Surface environment was not adopted");

    bool enriched = false;
    try {
        surface.close();
    } catch (const strata::AbiError& error) {
        enriched = error.diagnostic().has_value() &&
            error.diagnostic()->code == "STRATA.SURFACE.RELEASE_BARRIER_INCOMPLETE" &&
            std::string_view(error.what()).find("prepared, synchronously consumed") !=
                std::string_view::npos;
    }
    check(enriched, "AbiError did not include the retained diagnostic code and message");
    check(surface.diagnostics().frame_index == 1U,
          "owned Surface diagnostic snapshot lost its frame identity");

    static_cast<void>(surface.prepare_release_packet());
    surface.acknowledge_release_packet();
    surface.close();
    check(!runtime.restore_application_state(state),
          "unchanged application state snapshot unexpectedly mutated the runtime");

    {
        strata::Surface abandoned = runtime.create_surface(surface_options);
        static_cast<void>(abandoned);
    }
    check(std::ranges::any_of(observed_diagnostics, [](const strata::Diagnostic& diagnostic) {
              return diagnostic.code == "STRATA.SURFACE.RELEASE_ABANDONED";
          }),
          "forgotten Surface did not auto-abandon with a diagnostic");
    runtime.close();
}

void snapshot_bindings_publish_only_changed_revisions() {
    std::int64_t now = 1;
    strata_runtime_config config{};
    config.struct_size = sizeof(config);
    config.abi_version = STRATA_ABI_VERSION_CURRENT;
    config.required_capabilities = STRATA_CAPABILITY_CORE_LIFECYCLE |
                                   STRATA_CAPABILITY_CALLER_CLOCK |
                                   STRATA_CAPABILITY_HOST_SNAPSHOTS;
    config.clock = strata_clock{sizeof(strata_clock), &now, &clock};
    strata::Runtime runtime(config);

    strata::host::Revision revision;
    std::int64_t count = 1;
    strata::host::Bindings bindings(runtime, "strata.host-tests");
    bindings.snapshot(
        "host-tests.model", [&revision] { return revision.value(); },
        [&count] {
            return strata::host::Value::object({
                {"model", strata::host::Value::object({{"count", count}})},
            });
        });
    const std::string direct_json = R"({"model":{"count":0}})";
    const strata_host_snapshot_config direct{
        sizeof(strata_host_snapshot_config),
        strata::view("host-tests.model"),
        40U,
        strata::view(direct_json),
    };
    strata::require_ok(strata_runtime_publish_host_snapshot(runtime.native_handle(), &direct),
                       "host test direct snapshot publication");
    strata::Runtime active_runtime = std::move(runtime);
    bindings.synchronize();
    strata_host_snapshot_info info = active_runtime.host_snapshot_info();
    check(info.has_snapshot != 0U && info.generation == 41U,
          "initial watched snapshot did not advance the retained producer generation");

    bindings.synchronize();
    info = active_runtime.host_snapshot_info();
    check(info.generation == 41U, "unchanged model revision republished its snapshot");

    count = 2;
    revision.changed();
    bindings.synchronize();
    info = active_runtime.host_snapshot_info();
    check(info.generation == 42U, "changed model revision did not publish a new snapshot");
    active_runtime.close();
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count != 2)
            throw std::runtime_error("host tests require the registry path");
        structured_values_round_trip();
        drag_events_are_typed_once();
        list_reorder_uses_stable_neighbors();
        action_bindings_decode_at_the_boundary(arguments[1]);
        snapshot_bindings_publish_only_changed_revisions();
        public_runtime_facade_owns_host_boundaries(arguments[1]);
        std::cout << "strata_host_tests: typed values, models, and complete host facade OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_host_tests: " << error.what() << '\n';
        return 1;
    }
}
