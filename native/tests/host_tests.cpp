#include <strata/contracts/debug_overlay.hpp>
#include <strata/contracts/demo_surface.hpp>
#include <strata/contracts/settings_app.hpp>
#include <strata/host.hpp>
#include <strata/render_packet.hpp>

#include "host/resource_root.hpp"
#include "host/session.hpp"
#include "host/system_fonts.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "durable_writer.hpp"
#endif

namespace {

void check(const bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::int64_t clock(void* const user_data) noexcept {
    return *static_cast<std::int64_t*>(user_data);
}

struct HostTestAction final {
    static constexpr std::string_view id = "host.test";
    std::string kind;
    std::optional<std::string> source_key;
    std::string name;
    std::optional<std::int64_t> count;

    [[nodiscard]] static HostTestAction decode(const strata::host::ActionEvent& event) {
        return {
            event.kind,
            event.source_key,
            std::string(event.value.require_string("name")),
            event.value.optional_integer("count"),
        };
    }
};

void generated_contracts_round_trip() {
    namespace settings = strata::contracts::settings_app;
    const settings::Settings source{
        .saved_message = "generated",
        .profile_tree =
            {
                settings::SettingsProfileTreeItem{
                    .key = "profile.generated",
                    .label = "Generated profile",
                    .may_have_children = false,
                    .children_loaded = true,
                },
            },
    };
    const strata::host::Value snapshot = settings::encode_settings(source);
    const settings::Settings decoded = settings::decode_settings(snapshot.require("settings"));
    check(decoded.saved_message == "generated" && decoded.profile_tree.size() == 1U &&
              decoded.profile_tree.front().key == "profile.generated",
          "generated host snapshot contract did not round-trip");

    namespace demo = strata::contracts::demo_surface;
    const demo::DemoHostMessageAction action =
        demo::DemoHostMessageAction::decode(strata::host::ActionEvent{
            std::string(demo::DemoHostMessageAction::id),
            strata::host::Value::object({{"message", "typed"}}),
            "activate",
            std::nullopt,
            {},
        });
    check(action.message == "typed" && action.priority == 0.0,
          "generated action decoder did not apply its schema default");

    namespace debug = strata::contracts::debug_overlay;
    const auto mode = debug::StrataDebugSelectModeAction::decode(strata::host::ActionEvent{
        std::string(debug::StrataDebugSelectModeAction::id),
        strata::host::Value::object({{"mode", "HOT_TREE"}}),
        "activate",
        std::nullopt,
        {},
    });
    check(debug::wire_name(mode.mode) == "HOT_TREE",
          "generated enum action contract changed its wire value");
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

void large_host_documents_parse() {
    using strata::host::Value;
    // Frame snapshots of large surfaces exceed 16 MiB of canonical, indented JSON.
    std::string json = "{\"value\":";
    json.append(17U * 1024U * 1024U, ' ');
    json += "7}";
    check(Value::parse(json).require("value") == Value(7), "host could not parse a large document");
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

void action_bindings_decode_at_the_boundary() {
    std::int64_t now = 1;
    strata_runtime_config config{};
    config.struct_size = sizeof(config);
    config.abi_version = STRATA_ABI_VERSION_CURRENT;
    config.required_capabilities =
        STRATA_CAPABILITY_CORE_LIFECYCLE | STRATA_CAPABILITY_CALLER_CLOCK |
        STRATA_CAPABILITY_APPLICATION_LIFECYCLE | STRATA_CAPABILITY_ACTION_DISPATCH;
    config.clock = strata_clock{sizeof(strata_clock), &now, &clock};
    strata::Runtime runtime(config);
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
        strata::view(schemas),
        nullptr,
        0U,
    };
    runtime.configure_application(application);

    std::optional<HostTestAction> captured;
    strata::host::Bindings bindings(runtime, "strata.host-tests");
    bindings.on<HostTestAction>([&captured](HostTestAction action) {
        captured = std::move(action);
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
              captured->kind == "host-test-event" &&
              captured->source_key == std::optional<std::string>("host.source") &&
              captured->name == "alpha" && captured->count == std::optional<std::int64_t>(3),
          "typed action binding leaked or mistranslated the ABI JSON call");
}

void raw_runtime_config_retains_activation_diagnostics() {
    std::int64_t now = 1;
    strata_runtime_config config{};
    config.struct_size = sizeof(config);
    config.abi_version = STRATA_ABI_VERSION_CURRENT;
    config.required_capabilities =
        STRATA_CAPABILITY_CORE_LIFECYCLE | STRATA_CAPABILITY_CALLER_CLOCK |
        STRATA_CAPABILITY_APPLICATION_LIFECYCLE | STRATA_CAPABILITY_COMPILER_ACTIVATION |
        STRATA_CAPABILITY_ACTION_DISPATCH;
    config.clock = strata_clock{sizeof(strata_clock), &now, &clock};
    strata::Runtime runtime(config);
    runtime.configure_application(strata::ApplicationOptions{
        .id = "host-tests.activation-diagnostics",
        .schemas_json = R"({
          "widgets":{"registry":"host-tests.diagnostics.v1","required":[],"definitions":[]},
          "actions":{
            "registry":"host-tests.diagnostics.v1",
            "required":["host.required"],
            "definitions":[{
              "id":"host.required",
              "dispatchPolicy":"required",
              "summary":"required host action",
              "payloadContract":"no payload",
              "arguments":[]
            }]
          },
          "host":[]
        })",
    });

    bool enriched = false;
    try {
        static_cast<void>(runtime.activate(strata::SourceActivation{
            .generation = 1U,
            .entry_source_id = "host-tests/activation-diagnostics.strata",
            .entry_text = R"(
              overlay Main {
                root Button(
                  key: "diagnostic.button",
                  label: "Save",
                  onClick: action("host.required")
                )
              }
            )",
        }));
    } catch (const strata::AbiError& error) {
        enriched = error.diagnostic().has_value() &&
                   error.diagnostic()->code == "STRATA.DSL.ACTION_HANDLER_MISSING" &&
                   std::string_view(error.what()).find("host.required") != std::string_view::npos;
    }
    check(enriched, "raw-config Runtime activation lost its diagnostic code and message");
    runtime.close();
}

void public_runtime_facade_owns_host_boundaries() {
    std::int64_t now = 100;
    std::vector<strata::Diagnostic> observed_diagnostics;
    strata::RuntimeOptions options;
    options.clock = [&now] { return now; };
    options.diagnostic = [&observed_diagnostics](const strata::Diagnostic& diagnostic) {
        observed_diagnostics.push_back(diagnostic);
    };
    strata::Runtime runtime(std::move(options));

    runtime.set_resource_adapter(strata::ResourceAdapter{
        .load = [](const std::string_view id) -> std::optional<std::vector<std::uint8_t>> {
            if (id != "host-tests.resource")
                return std::nullopt;
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
    constexpr std::string_view source = "overlay Main { root Panel(key: \"facade.root\") }";
    check(runtime
              .activate(strata::SourceActivation{
                  .generation = 1U,
                  .entry_source_id = "host-tests/facade.strata",
                  .entry_text = std::string(source),
              })
              .activated(),
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
    check(surface.enqueue(strata::InputEvent::pointer(strata::InputKind::pointer_move,
                                                      strata::Point{10.0, 20.0}))
                  .accepted_event_count == 1U,
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
    check(surface.adopt_environment(resized), "owned Surface environment was not adopted");

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
    check(std::ranges::any_of(observed_diagnostics,
                              [](const strata::Diagnostic& diagnostic) {
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

void generated_root_models_publish_changed_fields_only() {
    std::int64_t now = 1;
    strata_runtime_config config{};
    config.struct_size = sizeof(config);
    config.abi_version = STRATA_ABI_VERSION_CURRENT;
    config.required_capabilities = STRATA_CAPABILITY_CORE_LIFECYCLE |
                                   STRATA_CAPABILITY_CALLER_CLOCK |
                                   STRATA_CAPABILITY_HOST_SNAPSHOTS;
    config.clock = strata_clock{sizeof(strata_clock), &now, &clock};
    strata::Runtime runtime(config);
    strata::Runtime active_runtime = std::move(runtime);

    namespace settings = strata::contracts::settings_app;
    settings::SettingsModel model;
    strata::host::Bindings bindings(active_runtime, "strata.host-tests.generated");
    model.bind(bindings, "host-tests.settings");
    static_cast<void>(model.set(settings::Settings{}));
    bindings.synchronize();
    const std::uint64_t initial_generation = active_runtime.host_snapshot_info().generation;

    check(model.set_saved_message("changed"), "generated root model ignored a changed field");
    bindings.synchronize();
    const std::uint64_t changed_generation = active_runtime.host_snapshot_info().generation;
    check(changed_generation == initial_generation + 1U,
          "generated root model published more than the changed field");

    check(!model.set_saved_message("changed"), "generated root model accepted an unchanged field");
    bindings.synchronize();
    check(active_runtime.host_snapshot_info().generation == changed_generation,
          "generated root model republished an unchanged field");
    active_runtime.close();
}

void runtime_images_upload_only_when_sampled() {
    std::int64_t now = 100;
    strata::RuntimeOptions options;
    options.clock = [&now] { return now; };
    strata::Runtime runtime(std::move(options));
    runtime.configure_application(strata::ApplicationOptions{.id = "host-tests.images"});
    check(runtime
              .activate(strata::SourceActivation{
                  .generation = 1U,
                  .entry_source_id = "host-tests/images.strata",
                  .entry_text = R"(
                    overlay Main {
                      root Panel(key: "images.root", layout: { kind: "COLUMN" }) {
                        Image(key: "images.photo", image: "test:photo",
                          layout: { width: 32, height: 32 })
                        Draw(key: "images.face", layout: { width: 40, height: 40 }, shapes: [
                          { kind: "image", image: "test:photo", tint: #808080FF,
                            source: { u: 0.5, v: 0, width: 0.5, height: 1 },
                            points: [{ x: 0.1, y: 0 }, { x: 1, y: 0.2 }, { x: 0, y: 1 }] }
                        ])
                      }
                    }
                  )",
              })
              .activated(),
          "runtime image fixture did not activate");
    strata::SurfaceOptions surface_options;
    surface_options.id = "host-tests.images.surface";
    surface_options.root_role = strata::SurfaceRootRole::overlay;
    surface_options.root_name = "Main";
    surface_options.environment.framebuffer_width = 320;
    surface_options.environment.framebuffer_height = 200;
    surface_options.environment.logical_width = 320.0;
    surface_options.environment.logical_height = 200.0;
    strata::Surface surface = runtime.create_surface(surface_options);
    strata::host::RenderPacketDecoder decoder;
    const auto frame = [&]() -> const strata::host::RenderPacket& {
        now += 16'000'000;
        static_cast<void>(surface.frame(now));
        return decoder.decode(surface.render_packet());
    };
    const auto sampled = [](const strata::host::RenderPacket& packet) {
        return std::ranges::count_if(packet.batches, [](const auto& batch) {
            const auto* draw = std::get_if<strata::host::DrawBatch>(&batch);
            return draw != nullptr && draw->texture.has_value() &&
                   draw->texture->find("test:photo") != std::string::npos;
        });
    };

    const strata::host::RenderPacket& missing = frame();
    check(missing.resources.empty() && sampled(missing) == 0,
          "an unpublished runtime image was uploaded or drawn");

    std::vector<std::uint8_t> pixels(2U * 2U * 4U, 200U);
    runtime.publish_image("test:photo", 2U, 2U, pixels, strata::ImageSampling::nearest);
    runtime.publish_image("test:unused", 1U, 1U, std::vector<std::uint8_t>(4U, 1U));
    const strata::host::RenderPacket& published = frame();
    check(published.resources.size() == 1U &&
              published.resources.front().kind == strata::host::resource_image &&
              published.resources.front().texture.ends_with("/image/test:photo") &&
              published.resources.front().format == strata::host::texture_format_rgba8 &&
              published.resources.front().sampling == strata::host::texture_sampling_nearest &&
              published.resources.front().width == 2U && published.resources.front().bytes == pixels,
          "a published runtime image was not uploaded exactly once as RGBA8 when sampled");
    check(sampled(published) >= 1, "a published runtime image was not drawn");
    const std::string host_id = published.resources.front().texture;

    const strata::host::RenderPacket& settled = frame();
    check(settled.resources.empty() && !settled.full_geometry_payload,
          "a settled Surface re-sent a resident runtime image or its geometry");

    std::ranges::fill(pixels, std::uint8_t{40U});
    runtime.publish_image("test:photo", 2U, 2U, pixels, strata::ImageSampling::nearest);
    const strata::host::RenderPacket& replaced = frame();
    check(replaced.resources.size() == 1U && replaced.resources.front().texture == host_id &&
              replaced.resources.front().bytes == pixels && !replaced.full_geometry_payload,
          "replacing a runtime image did not re-upload only its pixels");

    runtime.release_image("test:photo");
    const strata::host::RenderPacket& released = frame();
    check(released.resources.size() == 1U &&
              released.resources.front().kind == strata::host::resource_release &&
              released.resources.front().texture == host_id && sampled(released) == 0,
          "a released runtime image was not released from the host and dropped from drawing");

    const std::vector<std::uint8_t> teardown = surface.prepare_release_packet();
    const strata::host::RenderPacket& closing = decoder.decode(teardown);
    check(std::ranges::none_of(closing.resources,
                               [&host_id](const auto& resource) { return resource.texture == host_id; }),
          "Surface teardown released a runtime image the host no longer held");
    surface.acknowledge_release_packet();
    surface.close();
    runtime.close();
}

/**
 * Resource loads report exact statuses, never cache a failure, read UTF-8 ids without the narrow
 * code page, and cannot leave the root lexically or through a junction inside it.
 */
void resource_root_maps_failures_and_confines_links() {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / "strata-resource-root-test";
    fs::remove_all(base);
    const fs::path root = base / "root";
    const fs::path outside = base / "outside";
    fs::create_directories(root);
    fs::create_directories(outside);
    {
        std::ofstream secret(outside / "secret.txt", std::ios::binary);
        secret << "secret";
    }
    strata::host::ResourceRoot resources(root);
    const auto load = [&resources](const std::string_view id) {
        strata_bytes_view bytes{};
        const strata_status status = resources.load(id, bytes);
        return std::pair{status, std::string(reinterpret_cast<const char*>(bytes.data),
                                             bytes.data != nullptr ? bytes.size : 0U)};
    };

    check(load("late.txt").first == STRATA_STATUS_NOT_FOUND,
          "a missing resource was not reported as missing");
    {
        std::ofstream late(root / "late.txt", std::ios::binary);
        late << "late";
    }
    check(load("late.txt") == std::pair{STRATA_STATUS_OK, std::string("late")},
          "a resource that appeared after a failed load stayed missing");
    check(load("../outside/secret.txt").first == STRATA_STATUS_INVALID_ARGUMENT &&
              load("/outside/secret.txt").first == STRATA_STATUS_INVALID_ARGUMENT,
          "an escaping resource id was not rejected as an invalid argument");

    const std::string unicode_id = "\xC3\xBC" "ber.txt"; // "über.txt" in UTF-8
    {
        std::ofstream unicode(root / strata::host::utf8_path(unicode_id), std::ios::binary);
        unicode << "unicode";
    }
    check(load(unicode_id) == std::pair{STRATA_STATUS_OK, std::string("unicode")},
          "a UTF-8 resource id was decoded through the narrow code page");

#if defined(_WIN32)
    const fs::path link = root / "linked";
    const std::wstring command =
        L"mklink /J \"" + link.wstring() + L"\" \"" + outside.wstring() + L"\" >NUL";
    check(_wsystem(command.c_str()) == 0 && fs::exists(link / "secret.txt"),
          "could not create the resource junction fixture");
    check(load("linked/secret.txt").first == STRATA_STATUS_INVALID_ARGUMENT,
          "a junction inside the resource root escaped it");
    fs::remove(link);
#endif
    fs::remove_all(base);
}

/** A font directory holding names outside the narrow code page is still searchable. */
void system_font_search_tolerates_unicode_names() {
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / "strata-system-font-test";
    fs::remove_all(directory);
    fs::create_directories(directory);
    // U+5B57 and U+00FC: no single-byte code page holds both, so a narrow conversion throws.
    for (const std::string_view name : {std::string_view("\xE5\xAD\x97\xC3\xBC.ttf"),
                                        std::string_view("Target.TTF")}) {
        std::ofstream font(directory / strata::host::utf8_path(name), std::ios::binary);
        font << "font";
    }
    const std::vector<fs::path> directories{directory};
    check(strata::host::find_system_font("target.ttf", directories) == directory / "Target.TTF",
          "a system font search failed on a directory entry outside the narrow code page");
    check(strata::host::find_system_font("\xE5\xAD\x97\xC3\xBC.ttf", directories).filename() ==
              strata::host::utf8_path("\xE5\xAD\x97\xC3\xBC.ttf"),
          "a system font with a Unicode file name was not found by its UTF-8 name");
    fs::remove_all(directory);
}

#if defined(_WIN32)
[[nodiscard]] std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

/**
 * One unwritable path parks with its payload and is reported; other paths keep writing, later
 * enqueues never rethrow, and a briefly locked destination is written once the lock is gone.
 */
void durable_writer_isolates_and_retries_failures() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
                          ("strata-durable-writer-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(root);
    fs::create_directories(root);
    std::mutex reports_mutex;
    std::vector<std::string> reports;
    {
        strata::desktop::DurableWriter writer(
            [&](const fs::path& path, const std::string_view message) {
                const std::scoped_lock lock(reports_mutex);
                reports.push_back(path.filename().string() + ": " + std::string(message));
            },
            strata::desktop::DurableWriter::Options{6U, std::chrono::milliseconds(5)});

        // A directory where the temporary file belongs makes this path permanently unwritable.
        const fs::path blocked = root / "blocked.json";
        fs::create_directories(fs::path(blocked.wstring() + L".tmp"));
        const fs::path healthy = root / "healthy.json";
        writer.enqueue(blocked, "blocked");
        writer.enqueue(healthy, "healthy");
        const std::vector<fs::path> parked = writer.flush();
        check(parked == std::vector<fs::path>{blocked},
              "a failing durable path was not parked on its own");
        check(read_text_file(healthy) == "healthy",
              "a failing durable path stopped an unrelated write");
        check(writer.pending(blocked) == std::optional<std::string>("blocked"),
              "a parked durable payload was not served to readers");
        {
            const std::scoped_lock lock(reports_mutex);
            check(reports.size() == 1U && reports.front().starts_with("blocked.json: "),
                  "a durable write failure was not reported exactly once");
        }

        writer.enqueue(healthy, "healthy again");
        check(writer.flush() == std::vector<fs::path>{blocked} &&
                  read_text_file(healthy) == "healthy again",
              "the durable writer stopped after one path failed");

        // A destination another process holds without sharing is retried until it is released.
        const fs::path locked = root / "locked.json";
        {
            std::ofstream initial(locked, std::ios::binary);
            initial << "initial";
        }
        const HANDLE holder = CreateFileW(locked.c_str(), GENERIC_READ, 0U, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(holder != INVALID_HANDLE_VALUE, "could not lock the durable test file");
        std::jthread release([holder] {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            CloseHandle(holder);
        });
        writer.enqueue(locked, "unlocked");
        release.join();
        check(writer.flush() == std::vector<fs::path>{blocked} &&
                  read_text_file(locked) == "unlocked",
              "a briefly locked durable file was not retried");

        // The next payload for a parked path retries it.
        fs::remove(fs::path(blocked.wstring() + L".tmp"));
        writer.enqueue(blocked, "recovered");
        check(writer.flush().empty() && read_text_file(blocked) == "recovered" &&
                  !writer.pending(blocked).has_value(),
              "a parked durable path did not recover on its next payload");
    }
    fs::remove_all(root);
}
#endif

// The portable Session every host shell uses: imports resolve through the host reader, the Surface
// frames into decoded packets on the Session's clock, and the release barrier hands the terminal
// packet to the backend before the Surface and runtime close.
void portable_session_runs_the_application_lifecycle() {
    std::vector<std::string> codes;
    std::vector<std::string> reads;
    strata::host::SessionOptions options;
    options.application_id = "host-tests.session";
    options.stable_identity_seed = 7U;
    options.read_text = [&reads](const std::string_view id) -> std::string {
        reads.emplace_back(id);
        if (id == "session/main.strata")
            return "import \"part.strata\";\noverlay Main { root Part() }";
        if (id == "session/part.strata")
            return "component Part() { Panel(key: \"part\") }";
        if (id == "session/broken.strata")
            return "import \"session/missing.strata\";\noverlay Main { root Panel() }";
        throw strata::host::ResourceNotFound("no module " + std::string(id));
    };
    options.on_diagnostic = [&codes](const strata_diagnostic& diagnostic) {
        codes.emplace_back(diagnostic.code.data, diagnostic.code.size);
    };
    strata::host::Session session(std::move(options), strata::host::SessionAdapters{});

    bool rejected = false;
    try {
        static_cast<void>(session.activate_source("session/broken.strata", 1U));
    } catch (const strata::AbiError&) {
        rejected = true;
    }
    check(rejected && std::ranges::find(codes, "STRATA.DSL.IMPORT_LOAD_FAILED") != codes.end(),
          "a Session activated a module whose import its reader could not find");
    reads.clear();
    const strata_activation_info activated = session.activate_source("session/main.strata", 2U);
    check(activated.status == STRATA_ACTIVATION_ACTIVATED,
          "a Session did not activate a module with a resolved import");
    check(reads == std::vector<std::string>{"session/main.strata", "session/part.strata"},
          "a Session did not read the entry module and its import through the host reader");
    check(std::ranges::find(session.module_resources(), "session/part.strata") !=
              session.module_resources().end(),
          "a Session did not remember the imported module");

    strata::host::SessionSurface surface;
    surface.root_name = "Main";
    surface.environment.struct_size = sizeof(surface.environment);
    surface.environment.generation = 1U;
    surface.environment.framebuffer_width = 320;
    surface.environment.framebuffer_height = 200;
    surface.environment.logical_width = 320.0;
    surface.environment.logical_height = 200.0;
    surface.environment.scale = 1.0;
    session.create_surface(surface);
    check(session.has_surface(), "a Session did not create its Surface");

    const strata::host::SurfacePacketFrame first = session.frame(5'000'000);
    check(session.time() == 5'000'000 && first.packet != nullptr &&
              first.packet->frame_index == first.surface.frame_index &&
              first.packet_bytes == first.surface.render_packet_size,
          "a Session frame did not decode the packet of the frame it produced");
    session.record_host_frame(*first.packet, 1'000);
    check(session.runtime().snapshot().info().time_nanoseconds == 5'000'000,
          "the Session clock did not drive the runtime clock");

    strata_surface_environment resized = surface.environment;
    resized.generation = 2U;
    resized.framebuffer_width = 640;
    resized.logical_width = 640.0;
    check(session.adopt_environment(resized), "a Session did not adopt a new environment");

    bool consumed = false;
    session.close([&consumed](const strata::host::RenderPacket&) { consumed = true; });
    check(consumed && !session.has_surface(),
          "a Session did not hand the terminal packet to its backend before closing");
    check(std::ranges::find(codes, "STRATA.SURFACE.RELEASE_ABANDONED") == codes.end(),
          "an ordered Session close abandoned its Surface");
}

} // namespace

int strata_test_host() {
    try {
        generated_contracts_round_trip();
        structured_values_round_trip();
        large_host_documents_parse();
        drag_events_are_typed_once();
        list_reorder_uses_stable_neighbors();
        action_bindings_decode_at_the_boundary();
        raw_runtime_config_retains_activation_diagnostics();
        snapshot_bindings_publish_only_changed_revisions();
        generated_root_models_publish_changed_fields_only();
        public_runtime_facade_owns_host_boundaries();
        runtime_images_upload_only_when_sampled();
        resource_root_maps_failures_and_confines_links();
        system_font_search_tolerates_unicode_names();
        portable_session_runs_the_application_lifecycle();
#if defined(_WIN32)
        durable_writer_isolates_and_retries_failures();
#endif
        std::cout << "strata_host_tests: typed values, models, and complete host facade OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_host_tests: " << error.what() << '\n';
        return 1;
    }
}
