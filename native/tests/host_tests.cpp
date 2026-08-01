#include <strata/host.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
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
    const std::string event_json = R"({"name":"alpha","count":3})";
    const strata_action_dispatch_config dispatch{
        sizeof(strata_action_dispatch_config),
        strata::view("host.test"),
        {},
        strata::view("host-test-event"),
        strata::view("host.source"),
        strata::view(event_json),
        {},
        0U,
        0U,
    };
    strata_action_dispatch_info info{};
    info.struct_size = sizeof(info);
    strata::require_ok(
        strata_runtime_dispatch_action_json(active_runtime.native_handle(), &dispatch, &info),
        "typed host test action dispatch");
    check(info.status == STRATA_ACTION_DISPATCH_HANDLED && captured.has_value() &&
              captured->id == "host.test" && captured->kind == "host-test-event" &&
              captured->source_key == std::optional<std::string>("host.source") &&
              captured->value.require_string("name") == "alpha" &&
              captured->value.optional_integer("count") == std::optional<std::int64_t>(3),
          "typed action binding leaked or mistranslated the ABI JSON call");
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
    strata_host_snapshot_info info{};
    info.struct_size = sizeof(info);
    strata::require_ok(strata_runtime_get_host_snapshot_info(active_runtime.native_handle(), &info),
                       "host test snapshot info");
    check(info.has_snapshot != 0U && info.generation == 41U,
          "initial watched snapshot did not advance the retained producer generation");

    bindings.synchronize();
    strata::require_ok(strata_runtime_get_host_snapshot_info(active_runtime.native_handle(), &info),
                       "host test unchanged snapshot info");
    check(info.generation == 41U, "unchanged model revision republished its snapshot");

    count = 2;
    revision.changed();
    bindings.synchronize();
    strata::require_ok(strata_runtime_get_host_snapshot_info(active_runtime.native_handle(), &info),
                       "host test changed snapshot info");
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
        std::cout << "strata_host_tests: typed values, events, models, and publications OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_host_tests: " << error.what() << '\n';
        return 1;
    }
}
