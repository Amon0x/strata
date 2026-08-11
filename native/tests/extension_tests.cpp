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
    if (!condition)
        throw std::runtime_error(std::string(message));
}

template <typename Callable> [[nodiscard]] std::string rejection(Callable&& callable) {
    try {
        callable();
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("could not open extension fixture " + path.string());
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

strata_status load_resource(void* const user_data, const strata_string_view id,
                            strata_bytes_view* const out_bytes) {
    auto& resources = *static_cast<FileResources*>(user_data);
    const std::filesystem::path path = resources.root / std::string(id.data, id.size);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return STRATA_STATUS_NOT_FOUND;
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

strata_action_handler_result record_action(void* const user_data,
                                           const strata_action_call* const call) {
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
    std::uint64_t drag_presentations = 0U;
    std::uint64_t overlays = 0U;
    std::uint64_t semantics = 0U;
    std::uint64_t drag_semantics = 0U;
    std::uint64_t hit_bounds = 0U;
    std::uint64_t behavior_events = 0U;
    std::uint64_t widget_pointer_events = 0U;
    std::uint64_t widget_scroll_events = 0U;
    std::uint64_t commit_events = 0U;
    std::uint64_t compound_subtarget_projections = 0U;
    std::uint64_t frame_callbacks = 0U;
    std::uint64_t frame_presentations = 0U;
    std::uint64_t frame_requests = 0U;
    bool declared_write_accepted = false;
    bool undeclared_write_rejected = false;
    bool text_write_accepted = false;
    bool retained_text_round_tripped = false;
    bool parameter_default_visible = false;
    bool parameter_presence_visible = false;
    bool input_scale_visible = false;
    bool presentation_context_visible = false;
    bool semantics_parameter_visible = false;
    bool input_state_round_tripped = false;
    bool scroll_local_position = false;
    bool measured_text = false;
    bool behavior_emit_accepted = false;
    bool pointer_focus_hidden = false;
    bool keyboard_focus_visible = false;
    bool pointer_claimed = false;
    bool pointer_local_position = false;
    bool pointer_cancelled = false;
    bool live_event_emitted = false;
    bool commit_event_emitted = false;
    bool structured_state_round_tripped = false;
    bool structured_property_visible = false;
    bool typed_color_visible = false;
    bool compound_subtarget_routed = false;
    bool reduced_motion_frame = false;
    std::int64_t last_frame_time = 0;
    std::int64_t last_frame_delta = 0;
    double last_local_x = 0.0;
    double last_input_value = 0.0;
    double last_presented_value = 0.0;
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
constexpr auto drag_value = retained<number>("harness.drag", 0.0, Invalidation::paint);
constexpr auto drag_session = retained<number>("harness.drag-session", 0.0, Invalidation::input);

struct CompoundState final {
    std::uint32_t selected = 7U;
    double position = 0.25;
};

constexpr auto compound_state =
    retained<structured<CompoundState>>("harness.compound", CompoundState{}, Invalidation::paint);
constexpr auto compound_points = parameter<any>("points");
constexpr auto compound_accent = parameter<color>("accent");

struct FrameHarnessState final {
    std::uint32_t ticks = 0U;
    std::uint32_t target_ticks = 0U;
    FrameCost cost = FrameCost::paint;
};

constexpr auto frame_state = retained<structured<FrameHarnessState>>(
    "harness.frame", FrameHarnessState{}, Invalidation::input);

bool harness_activate(Input& input) {
    ++counters.activations;
    counters.parameter_default_visible = input.get(harness_step) == 4.0;
    counters.parameter_presence_visible = input.has(harness_step) && !input.has(harness_caption);
    counters.input_scale_visible = input.scale() == 1.25;
    counters.declared_write_accepted =
        input.set(harness_count, input.get(harness_count) + input.get(harness_step));
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
    if (key.name != "right")
        return false;
    input.set(harness_count, input.get(harness_count) + 10.0);
    return true;
}
bool drag_pointer(Input& input, const Pointer& pointer) {
    ++counters.widget_pointer_events;
    counters.pointer_local_position = counters.pointer_local_position || pointer.has_local_position;
    counters.last_local_x = pointer.local_x;
    if (pointer.kind == Pointer::Kind::press) {
        counters.pointer_claimed = input.claim_gesture();
        counters.compound_subtarget_routed =
            pointer.subtarget_id == "thumb.top" && pointer.subtarget_index == 7U;
        CompoundState state = input.get(compound_state);
        state.position = pointer.local_x / 160.0;
        counters.structured_state_round_tripped =
            input.set(compound_state, state) &&
            input.get(compound_state).position == state.position;
        input.set(drag_value, pointer.local_x);
        counters.last_input_value = input.get(drag_value);
        return true;
    }
    if (pointer.kind == Pointer::Kind::move) {
        input.set(drag_value, pointer.local_x);
        CompoundState state = input.get(compound_state);
        state.position = std::clamp(pointer.local_x / 160.0, 0.0, 1.0);
        static_cast<void>(input.set(compound_state, state));
        counters.last_input_value = input.get(drag_value);
        if (!counters.live_event_emitted) {
            counters.live_event_emitted = input.live(pointer.local_x);
        }
        return true;
    }
    if (pointer.kind == Pointer::Kind::release) {
        input.set(drag_value, pointer.local_x);
        counters.last_input_value = input.get(drag_value);
        ++counters.commit_events;
        counters.commit_event_emitted = input.commit(pointer.local_x);
        return true;
    }
    counters.pointer_cancelled = true;
    static_cast<void>(input.cancel_gesture());
    return true;
}
bool drag_scroll(Input& input, const Scroll& scroll) {
    ++counters.widget_scroll_events;
    const double value = input.get(drag_session) + scroll.delta_y;
    counters.input_state_round_tripped =
        input.set(drag_session, value) && input.get(drag_session) == value;
    counters.scroll_local_position =
        scroll.on_target && scroll.local_x == 8.0 && scroll.local_y == 8.0;
    return true;
}

void drag_present(Present& present) {
    ++counters.drag_presentations;
    counters.last_presented_value = present.get(drag_value);
    const Color accent = present.get(compound_accent).value_or(rgba(80U, 180U, 150U));
    counters.typed_color_visible = accent.green == 187U;
    const Rect bounds = present.bounds();
    present.rounded_rect(bounds, 4.0, rgba(22U, 28U, 38U), stroke(1.0, rgba(80U, 98U, 120U)));
    const CompoundState compound = present.get(compound_state);
    const double x = compound.position * bounds.width;
    present.rounded_rect(Rect{bounds.x + x - 3.0, bounds.y + 4.0, 6.0, bounds.height - 8.0}, 3.0,
                         accent);
}

void drag_semantics(Semantics& semantics) {
    ++counters.drag_semantics;
    const double value = semantics.get(drag_value);
    const CompoundState compound = semantics.get(compound_state);
    semantics.name("Drag harness");
    semantics.value_range(value, 0.0, 160.0);
    semantics.add_action("decrement");
    semantics.add_action("focus");
    semantics.add_action("increment");
    static_cast<void>(semantics.child(SemanticChild{
        compound.selected,
        "slider",
        "Compound thumb",
        "25%",
        compound.position,
        0.0,
        1.0,
        true,
        false,
    }));
}

void drag_subtargets(Subtargets& subtargets) {
    ++counters.compound_subtarget_projections;
    const CompoundState compound = subtargets.get(compound_state);
    const ValueView points = subtargets.get(compound_points);
    counters.structured_property_visible = points.kind() == ValueView::Kind::list &&
                                           points.at(0U).field("position").number(-1.0) == 0.25;
    const double x = compound.position * subtargets.bounds().width;
    static_cast<void>(subtargets.add(Subtarget{
        "thumb.under",
        6U,
        Rect{x - 12.0, 0.0, 32.0, 40.0},
        1,
        true,
    }));
    static_cast<void>(subtargets.add(Subtarget{
        "thumb.top",
        7U,
        Rect{x - 12.0, 0.0, 32.0, 40.0},
        2,
        true,
        true,
    }));
}

bool frame_pointer(Input& input, const Pointer& pointer) {
    if (pointer.kind == Pointer::Kind::cancel) {
        input.cancel_frame();
        return true;
    }
    if (pointer.kind != Pointer::Kind::press)
        return false;
    FrameHarnessState state;
    const double fraction = pointer.local_x / std::max(1.0, input.bounds().width);
    if (fraction < 0.34) {
        state.target_ticks = 2U;
        state.cost = FrameCost::paint;
    } else if (fraction < 0.67) {
        state.target_ticks = 1U;
        state.cost = FrameCost::layout;
    } else {
        state.target_ticks = 100U;
        state.cost = FrameCost::paint;
    }
    static_cast<void>(input.set(frame_state, state));
    if (input.request_frame(state.cost))
        ++counters.frame_requests;
    return true;
}

void frame_advance(Input& input, const Frame& frame) {
    ++counters.frame_callbacks;
    counters.last_frame_time = frame.time_nanoseconds;
    counters.last_frame_delta = frame.delta_nanoseconds;
    counters.reduced_motion_frame = counters.reduced_motion_frame || frame.reduced_motion;
    FrameHarnessState state = input.get(frame_state);
    ++state.ticks;
    static_cast<void>(input.set(frame_state, state));
    if (state.ticks < state.target_ticks) {
        if (input.request_frame(state.cost))
            ++counters.frame_requests;
    } else {
        input.cancel_frame();
    }
}

void frame_present(Present& present) {
    ++counters.frame_presentations;
    const FrameHarnessState state = present.get(frame_state);
    const Rect bounds = present.bounds();
    present.rounded_rect(bounds, 4.0, rgba(18U, 36U, 34U), stroke(1.0, rgba(72U, 198U, 177U)));
    present.rect(
        Rect{bounds.x, bounds.y, bounds.width * std::min(1.0, state.ticks / 2.0), bounds.height},
        rgba(72U, 198U, 177U, 96U));
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
    counters.presentation_context_visible =
        counters.presentation_context_visible ||
        (present.has(harness_step) && !present.has(harness_caption) && present.scale() == 1.25);
    const Rect bounds = present.bounds();
    present.shadow(bounds, corners(4.0), rgba(0U, 0U, 0U, 90U), 6.0, 1.0);
    present.rounded_rect(bounds, 4.0, rgba(30U, 40U, 60U), stroke(1.0, rgba(90U, 110U, 140U)));
    present.blur(bounds, 3.0);
    if (present.measure("harness").has_value())
        counters.measured_text = true;
    {
        const ClipScope clip = present.clip(bounds);
        present.text("harness", bounds, rgba(240U, 240U, 240U), TextAlignment::center,
                     TextAlignment::center);
        const Material material{
            "strata:custom_mesh",
            {},
            {},
            1.0,
        };
        present.mesh(bounds, "harness.mesh", Mesh{harness_vertices, harness_indices}, {}, material);
        present.mesh(bounds, "harness.invalid", Mesh{invalid_vertices, harness_indices});
    }
    counters.pointer_focus_hidden =
        counters.pointer_focus_hidden || (present.focused() && !present.focus_visible());
    counters.keyboard_focus_visible = counters.keyboard_focus_visible || present.focus_visible();
    if (present.focus_visible()) {
        present.border(bounds, 4.0, stroke(2.0, rgba(120U, 190U, 255U)));
    }
}

void harness_overlay(Present& present) {
    if (!present.get(harness_open))
        return;
    ++counters.overlays;
    const Rect anchor = present.bounds();
    present.rounded_rect(Rect{anchor.x, anchor.y + anchor.height, 120.0, 24.0}, 4.0,
                         rgba(20U, 26U, 36U, 250U));
}

void harness_semantics(Semantics& semantics) {
    ++counters.semantics;
    counters.semantics_parameter_visible =
        counters.semantics_parameter_visible ||
        (semantics.has(harness_step) && !semantics.has(harness_caption) &&
         semantics.get(harness_step) == 4.0);
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
    if (pointer.kind != Pointer::Kind::release)
        return false;
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
    auto drag = widget("DragHarnessWidget")
                    .no_children()
                    .focusable()
                    .intrinsic_size(160.0, 40.0)
                    .parameter(compound_points)
                    .parameter(compound_accent)
                    .retained(drag_value)
                    .retained(drag_session)
                    .retained(compound_state)
                    .semantics_role("slider")
                    .on_pointer(&drag_pointer)
                    .on_scroll(&drag_scroll)
                    .on_semantics(&drag_semantics)
                    .subtargets(&drag_subtargets)
                    .present(&drag_present);
    auto frame = widget("FrameHarnessWidget")
                     .no_children()
                     .intrinsic_size(160.0, 40.0)
                     .retained(frame_state)
                     .on_pointer(&frame_pointer)
                     .on_frame(&frame_advance)
                     .present(&frame_present);

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
    created->widget(std::move(harness))
        .widget(std::move(drag))
        .widget(std::move(frame))
        .behavior(std::move(observer));
    return created;
}

void test_definition_diagnostics() {
    check(rejection([] { static_cast<void>(widget("")); }).find("must not be empty") !=
              std::string::npos,
          "an unnamed extension widget was accepted");
    check(rejection([] {
              widget("Duplicate")
                  .parameter(parameter<number>("step", 1.0))
                  .parameter(parameter<boolean>("step"));
          }).find("declares parameter 'step' twice") != std::string::npos,
          "a duplicate extension parameter was accepted");
    check(rejection([] {
              widget("Duplicate")
                  .retained(retained<number>("field"))
                  .retained(retained<number>("field"));
          }).find("declares retained field 'field' twice") != std::string::npos,
          "a duplicate retained field was accepted");
    check(rejection([] {
              widget("Popup").detached_overlay(&harness_overlay, retained<boolean>(""));
          }).find("without a retained field") != std::string::npos,
          "a detached overlay without retained state was accepted");
    check(rejection([] {
              auto duplicated = package("strata.duplicate.v1");
              duplicated->widget(widget("Same")).widget(widget("Same"));
          }).find("declares widget 'Same' twice") != std::string::npos,
          "a duplicate widget declaration was accepted inside one package");
    check(rejection([] {
              auto sealed = package("strata.sealed.v1");
              static_cast<void>(sealed->bundle());
              sealed->widget(widget("Late"));
          }).find("already in use") != std::string::npos,
          "a package accepted a widget after its bundle was taken");
}

void test_external_package_and_schema_projection() {
    strata::host::SelectedExtensions selected = strata::host::select_extensions({"strata.demo.v1"});
    check(selected.packages.size() == 1U && selected.packages.front().id() == "strata.demo.v1",
          "the external demo package was not loaded");
    check(selected.widgets.size() == 2U && selected.behaviors.size() == 1U,
          "the external demo package did not project its runtime descriptors");
    const std::string unknown = rejection(
        [] { static_cast<void>(strata::host::select_extensions({"strata.missing.v1"})); });
    check(unknown.find("strata.missing.v1") != std::string::npos &&
              unknown.find("strata-extension-strata.missing.v1") != std::string::npos,
          "an unknown external package did not report its id and expected library");
    check(rejection([] {
              static_cast<void>(
                  strata::host::select_extensions({"strata.demo.v1", "strata.demo.v1"}));
          }).find("must be unique") != std::string::npos,
          "duplicate external package selection was accepted");

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
        check(schema.find(expected) != std::string::npos,
              "projected package schema is missing a declaration");
    }
    const std::vector<std::string> declared = strata::host::declared_extension_packages(
        R"({"extensionPackages":["strata.control-deck.v1"]})");
    check(declared == std::vector<std::string>{"strata.control-deck.v1"},
          "application schema package discovery did not preserve its single declaration");
    check(rejection([] {
              static_cast<void>(strata::host::declared_extension_packages(
                  R"({"extensionPackages":["duplicate","duplicate"]})"));
          }).find("must be unique") != std::string::npos,
          "duplicate schema package declarations were accepted");

    strata::host::SelectedExtensions control = strata::host::select_extensions(declared);
    check(control.widgets.size() == 3U && control.widget_inputs.size() == 3U &&
              control.behaviors.empty(),
          "the Control Deck package did not project widget pointer input separately");
    const std::string control_schema = control.packages.front().schema_json();
    check(control_schema.find(R"("name":"DeckColorPicker")") != std::string::npos &&
              control_schema.find(R"("name":"DeckGradientEditor")") != std::string::npos &&
              control_schema.find(R"("name":"DeckInertialScrubber")") != std::string::npos &&
              control_schema.find(R"("id":"control-deck.color.commit")") != std::string::npos &&
              control_schema.find(R"("id":"control-deck.gradient.commit")") != std::string::npos &&
              control_schema.find(R"("id":"control-deck.motion.commit")") != std::string::npos,
          "the Control Deck package schema is missing a public widget or value contract");
}

[[nodiscard]] strata_runtime_config runtime_config(Clock& clock, DiagnosticLog& diagnostics) {
    strata_runtime_config config{};
    config.struct_size = sizeof(strata_runtime_config);
    config.abi_version = STRATA_ABI_VERSION_CURRENT;
    config.required_capabilities =
        STRATA_CAPABILITY_CORE_LIFECYCLE | STRATA_CAPABILITY_CALLER_CLOCK |
        STRATA_CAPABILITY_APPLICATION_LIFECYCLE | STRATA_CAPABILITY_COMPILER_ACTIVATION |
        STRATA_CAPABILITY_ACTION_DISPATCH | STRATA_CAPABILITY_RESOURCE_ADAPTER |
        STRATA_CAPABILITY_SURFACE_RUNTIME;
    config.clock = strata_clock{sizeof(strata_clock), &clock, &clock_now};
    config.diagnostics = strata_diagnostic_sink{
        sizeof(strata_diagnostic_sink),
        &diagnostics,
        &emit_diagnostic,
    };
    return config;
}

void test_surface_lifecycle(const std::filesystem::path& resource_root) {
    counters = HarnessCounters{};
    const std::unique_ptr<Package> harness = harness_package();
    const std::string package_schema = harness->schema_json();

    Clock clock{8'000};
    DiagnosticLog diagnostics;
    strata_runtime_config config = runtime_config(clock, diagnostics);
    strata_runtime* runtime = nullptr;
    check(strata_runtime_create(&config, &runtime).status == STRATA_STATUS_OK,
          "extension harness runtime creation failed");

    FileResources resources{resource_root, {}};
    const strata_resource_adapter adapter{
        sizeof(strata_resource_adapter),
        &resources,
        1U,
        &load_resource,
    };
    check(strata_runtime_set_resource_adapter(runtime, &adapter).status == STRATA_STATUS_OK,
          "extension harness resource adapter installation failed");

    const strata_string_view extension_schemas[]{view(package_schema)};
    const strata_application_config application{
        sizeof(strata_application_config), view("strata.extension.harness"),
        strata_string_view{nullptr, 0U},   extension_schemas,
        std::size(extension_schemas),
    };
    check(strata_runtime_configure_application(runtime, &application).status == STRATA_STATUS_OK,
          "package declarations were rejected as an application schema source");

    ActionLog actions;
    strata_action_registration* activation_registration = nullptr;
    strata_action_registration* observation_registration = nullptr;
    const strata_action_handler_config activation_handler{
        sizeof(strata_action_handler_config),
        view("harness.activated"),
        view("strata.harness"),
        &actions,
        &record_action,
    };
    const strata_action_handler_config observation_handler{
        sizeof(strata_action_handler_config),
        view("harness.observed"),
        view("strata.harness"),
        &actions,
        &record_action,
    };
    check(strata_runtime_register_action_handler(runtime, &activation_handler,
                                                 &activation_registration)
                      .status == STRATA_STATUS_OK &&
              strata_runtime_register_action_handler(runtime, &observation_handler,
                                                     &observation_registration)
                      .status == STRATA_STATUS_OK,
          "package-declared action contracts were not registrable");

    const std::string source = R"(
screen Main {
  root Panel(key: "harness.root", layout: { kind: "COLUMN" }, behaviors: [{ id: "harness.observe" }]) {
    HarnessWidget(key: "harness.widget", layout: { width: 160, height: 40 })
    DragHarnessWidget(
      key: "harness.drag",
      points: [{ position: 0.25 }],
      accent: #44BB99FF,
      layout: { width: 160, height: 40 }
    )
    FrameHarnessWidget(key: "harness.frame", layout: { width: 160, height: 40 })
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
    check(strata_runtime_compile_and_activate(runtime, &activation, &activation_info).status ==
                  STRATA_STATUS_OK &&
              activation_info.status == STRATA_ACTIVATION_ACTIVATED,
          "a module using only package-declared widgets did not compile against the projected "
          "schema");

    strata_surface_environment environment{
        sizeof(strata_surface_environment),
        1U,
        480,
        320,
        480.0,
        320.0,
        1.25,
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
    check(strata_runtime_create_surface(runtime, &surface_config, &surface).status ==
              STRATA_STATUS_OK,
          "the projected extension bundle was rejected by surface creation");

    strata_surface_frame_info frame_info{};
    frame_info.struct_size = sizeof(frame_info);
    const strata_result first_frame = strata_surface_frame(surface, 8'000, &frame_info);
    check(first_frame.status == STRATA_STATUS_OK && frame_info.render_command_count >= 2U &&
              frame_info.semantics_generation != 0U,
          "the first extension frame produced no render or semantics output: status=" +
              std::to_string(first_frame.status) +
              " commands=" + std::to_string(frame_info.render_command_count) +
              " diagnostic=" + diagnostics.last_code + " " + diagnostics.last_message);
    check(counters.presentations != 0U, "the presentation hook did not run");
    check(counters.measured_text, "text measurement was unavailable during presentation");

    std::string frame_json;
    const strata_value_json_sink frame_sink{
        sizeof(strata_value_json_sink),
        &frame_json,
        &capture_json,
    };
    check(strata_surface_read_frame_json(surface, &frame_sink).status == STRATA_STATUS_OK &&
              frame_json.find("Harness widget") != std::string::npos,
          "extension semantics did not reach the published frame");
    for (const std::string_view primitive : {
             std::string_view("custom_mesh"),
             std::string_view("shadow"),
             std::string_view("blur"),
             std::string_view("clip"),
         }) {
        check(frame_json.find(primitive) != std::string::npos,
              "an extension render primitive did not reach the published frame");
    }
    check(counters.typed_color_visible && frame_json.find("Compound thumb") != std::string::npos,
          "typed color access or compound semantic child projection was unavailable");
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
    check(strata_surface_enqueue_input(surface, click, 2U, &batch).status == STRATA_STATUS_OK &&
              strata_surface_frame(surface, 9'000, &frame_info).status == STRATA_STATUS_OK,
          "pointer input was not accepted by the extension surface");
    check(counters.activations == 1U, "the activation hook did not run exactly once");
    check(counters.parameter_default_visible, "a declared parameter default was not readable");
    check(counters.declared_write_accepted, "a declared retained write was rejected");
    check(counters.undeclared_write_rejected, "an undeclared retained write silently succeeded");
    check(counters.text_write_accepted, "a declared retained text write was rejected");
    check(counters.retained_text_round_tripped, "retained text did not round-trip");
    check(counters.parameter_presence_visible && counters.input_scale_visible &&
              counters.presentation_context_visible && counters.semantics_parameter_visible,
          "property presence or display scale was not exposed consistently to extension hooks");
    check(counters.behavior_events != 0U, "the behavior pointer hook did not run");
    check(counters.hit_bounds != 0U, "the inspection hook did not narrow pointer hit testing");
    check(actions.calls != 0U && counters.behavior_emit_accepted,
          "package-declared actions did not dispatch through the runtime registry");
    check(counters.overlays != 0U,
          "the detached overlay did not paint once its retained flag was set");
    check(counters.pointer_focus_hidden && !counters.keyboard_focus_visible,
          "pointer-focused extension did not preserve semantic focus while hiding focus paint");

    strata_input_event key{};
    key.struct_size = sizeof(strata_input_event);
    key.version = STRATA_INPUT_EVENT_VERSION_2;
    key.kind = STRATA_INPUT_KEY;
    key.key_action = STRATA_KEY_PRESS;
    key.text = view("Right");
    check(strata_surface_enqueue_input(surface, &key, 1U, &batch).status == STRATA_STATUS_OK &&
              strata_surface_frame(surface, 10'000, &frame_info).status == STRATA_STATUS_OK,
          "key input was not accepted by the extension surface");
    check(counters.keys != 0U && counters.last_key == "right",
          "the key hook did not receive the focused key press");
    check(counters.keyboard_focus_visible,
          "keyboard input did not expose focus-visible state to the extension presenter");
    strata_input_event focus_drag[2]{};
    focus_drag[0].struct_size = sizeof(strata_input_event);
    focus_drag[0].version = STRATA_INPUT_EVENT_VERSION_2;
    focus_drag[0].kind = STRATA_INPUT_POINTER_PRESS;
    focus_drag[0].x = 40.0;
    focus_drag[0].y = 48.0;
    focus_drag[1] = focus_drag[0];
    focus_drag[1].kind = STRATA_INPUT_POINTER_RELEASE;
    focus_drag[1].x = 240.0;
    check(strata_surface_enqueue_input(surface, focus_drag, 2U, &batch).status ==
                  STRATA_STATUS_OK &&
              strata_surface_frame(surface, 11'000, &frame_info).status == STRATA_STATUS_OK &&
              counters.last_local_x > 160.0,
          "widget pointer capture did not retain release outside its bounds");
    check(counters.compound_subtarget_routed && counters.structured_state_round_tripped &&
              counters.structured_property_visible,
          "compound subtarget/state/property: routed=" +
              std::to_string(counters.compound_subtarget_routed) +
              " state=" + std::to_string(counters.structured_state_round_tripped) +
              " property=" + std::to_string(counters.structured_property_visible));
    strata_input_event hover_drag = focus_drag[0];
    hover_drag.kind = STRATA_INPUT_POINTER_MOVE;
    hover_drag.x = 8.0;
    check(strata_surface_enqueue_input(surface, &hover_drag, 1U, &batch).status ==
                  STRATA_STATUS_OK &&
              strata_surface_frame(surface, 11'250, &frame_info).status == STRATA_STATUS_OK,
          "pointer hover could not return to the extension before scroll");
    const std::uint64_t scroll_presentation_baseline = counters.drag_presentations;
    const std::uint64_t scroll_semantics_baseline = counters.drag_semantics;
    strata_input_event scroll{};
    scroll.struct_size = sizeof(strata_input_event);
    scroll.version = STRATA_INPUT_EVENT_VERSION_2;
    scroll.kind = STRATA_INPUT_SCROLL;
    scroll.x = 8.0;
    scroll.y = 48.0;
    scroll.delta_y = 2.0;
    check(strata_surface_enqueue_input(surface, &scroll, 1U, &batch).status == STRATA_STATUS_OK &&
              strata_surface_frame(surface, 11'500, &frame_info).status == STRATA_STATUS_OK,
          "scroll input was not accepted by the extension surface");
    check(counters.widget_scroll_events == 1U && counters.scroll_local_position &&
              counters.input_state_round_tripped,
          "the extension scroll lifecycle lost target geometry or retained input state");
    check(counters.drag_presentations == scroll_presentation_baseline &&
              counters.drag_semantics == scroll_semantics_baseline,
          "input-only retained state escaped into presentation or semantic work");

    strata_input_event drag_press = focus_drag[0];
    check(strata_surface_enqueue_input(surface, &drag_press, 1U, &batch).status ==
                  STRATA_STATUS_OK &&
              strata_surface_frame(surface, 11'750, &frame_info).status == STRATA_STATUS_OK,
          "the extension drag did not establish pointer capture");
    strata_input_event drag_warmup = focus_drag[0];
    drag_warmup.kind = STRATA_INPUT_POINTER_MOVE;
    check(strata_surface_enqueue_input(surface, &drag_warmup, 1U, &batch).status ==
                  STRATA_STATUS_OK &&
              strata_surface_frame(surface, 11'875, &frame_info).status == STRATA_STATUS_OK,
          "the captured drag did not warm its post-press hit geometry");
    const std::uint64_t semantic_baseline = counters.drag_semantics;
    const std::uint64_t semantic_generation_baseline = frame_info.semantics_generation;
    const std::uint64_t presentation_baseline = counters.drag_presentations;
    const std::uint64_t commit_baseline = counters.commit_events;
    const std::uint64_t hit_bounds_baseline = counters.hit_bounds;
    const std::uint64_t subtarget_projection_baseline = counters.compound_subtarget_projections;
    strata_runtime_memory_info memory_before{};
    memory_before.struct_size = sizeof(memory_before);
    check(strata_runtime_get_memory_info(runtime, &memory_before).status == STRATA_STATUS_OK,
          "runtime allocation telemetry was unavailable before the drag");
    constexpr std::size_t move_count = 120U;
    std::array<strata_input_event, move_count> drag{};
    for (std::size_t index = 0U; index < move_count; ++index) {
        drag[index] = focus_drag[0];
        drag[index].kind = STRATA_INPUT_POINTER_MOVE;
        drag[index].x =
            8.0 + 144.0 * static_cast<double>(index + 1U) / static_cast<double>(move_count);
    }
    check(strata_surface_enqueue_input(surface, drag.data(), drag.size(), &batch).status ==
                  STRATA_STATUS_OK &&
              batch.accepted_event_count == drag.size() && batch.queued_event_count <= 1U &&
              strata_surface_frame(surface, 12'000, &frame_info).status == STRATA_STATUS_OK,
          "continuous pointer input was not accepted or coalesced before the extension frame");
    check(counters.pointer_claimed && counters.pointer_local_position &&
              counters.widget_pointer_events >= 4U && counters.last_local_x > 150.0,
          "widget pointer capture did not retain the coalesced movement");
    check(counters.live_event_emitted && counters.commit_events == commit_baseline,
          "live movement emitted no typed event or committed before release");
    check(counters.drag_presentations == presentation_baseline + 1U &&
              counters.drag_semantics == semantic_baseline &&
              counters.hit_bounds == hit_bounds_baseline &&
              counters.compound_subtarget_projections == subtarget_projection_baseline &&
              frame_info.semantics_generation == semantic_generation_baseline,
          "paint-only move counts: presentation " +
              std::to_string(counters.drag_presentations - presentation_baseline) +
              ", semantics hooks " + std::to_string(counters.drag_semantics - semantic_baseline) +
              ", semantics generation " +
              std::to_string(frame_info.semantics_generation - semantic_generation_baseline) +
              ", hit bounds " + std::to_string(counters.hit_bounds - hit_bounds_baseline) +
              ", subtarget projections " +
              std::to_string(counters.compound_subtarget_projections -
                             subtarget_projection_baseline) +
              ", input " + std::to_string(counters.last_input_value) + ", presented " +
              std::to_string(counters.last_presented_value));
    strata_runtime_memory_info memory_after{};
    memory_after.struct_size = sizeof(memory_after);
    check(strata_runtime_get_memory_info(runtime, &memory_after).status == STRATA_STATUS_OK &&
              memory_after.routed_allocation_count == memory_before.routed_allocation_count,
          "the coalesced drag allocated through the runtime ABI allocator");

    strata_input_event release = drag.back();
    release.kind = STRATA_INPUT_POINTER_RELEASE;
    check(strata_surface_enqueue_input(surface, &release, 1U, &batch).status == STRATA_STATUS_OK &&
              strata_surface_frame(surface, 12'500, &frame_info).status == STRATA_STATUS_OK &&
              counters.commit_event_emitted && counters.commit_events == commit_baseline + 1U &&
              frame_info.semantics_generation == semantic_generation_baseline + 1U,
          "release did not emit one typed commit and schedule its semantic projection");

    strata_input_event cancel[2]{};
    cancel[0] = focus_drag[0];
    cancel[1] = cancel[0];
    cancel[1].kind = STRATA_INPUT_POINTER_CANCEL;
    check(strata_surface_enqueue_input(surface, cancel, 2U, &batch).status == STRATA_STATUS_OK &&
              strata_surface_frame(surface, 13'000, &frame_info).status == STRATA_STATUS_OK &&
              counters.pointer_cancelled,
          "pointer cancellation did not reach the captured extension widget");

    strata_input_event frame_click[2]{};
    frame_click[0].struct_size = sizeof(strata_input_event);
    frame_click[0].version = STRATA_INPUT_EVENT_VERSION_2;
    frame_click[0].kind = STRATA_INPUT_POINTER_PRESS;
    frame_click[0].x = 20.0;
    frame_click[0].y = 100.0;
    frame_click[0].timestamp_nanoseconds = 20'000'000;
    frame_click[1] = frame_click[0];
    frame_click[1].kind = STRATA_INPUT_POINTER_RELEASE;
    const std::uint64_t paint_callback_baseline = counters.frame_callbacks;
    check(strata_surface_enqueue_input(surface, frame_click, 2U, &batch).status ==
                  STRATA_STATUS_OK &&
              strata_surface_frame(surface, 20'000'000, &frame_info).status == STRATA_STATUS_OK &&
              counters.frame_callbacks == paint_callback_baseline,
          "a paint frame request ran in the input frame instead of the requested successor");
    const std::uint64_t paint_presentation_baseline = counters.frame_presentations;
    const std::uint64_t frame_semantics_baseline = frame_info.semantics_generation;
    check(strata_surface_frame(surface, 36'000'000, &frame_info).status == STRATA_STATUS_OK &&
              counters.frame_callbacks == paint_callback_baseline + 1U &&
              counters.frame_presentations == paint_presentation_baseline + 1U &&
              counters.last_frame_time == 36'000'000 && counters.last_frame_delta == 16'000'000 &&
              frame_info.semantics_generation == frame_semantics_baseline,
          "the requested paint frame lost its time, delta, or paint-only cost");
    frame_json.clear();
    const strata_result paint_snapshot = strata_surface_read_frame_json(surface, &frame_sink);
    check(paint_snapshot.status == STRATA_STATUS_OK &&
              frame_json.find(R"("layoutWork": 0)") != std::string::npos,
          "a paint-cost extension frame escaped into layout work: status=" +
              std::to_string(paint_snapshot.status) + " frame=" + frame_json);
    check(strata_surface_frame(surface, 52'000'000, &frame_info).status == STRATA_STATUS_OK &&
              counters.frame_callbacks == paint_callback_baseline + 2U,
          "the frame callback did not explicitly schedule its bounded successor");
    const std::uint64_t settled_presentations = counters.frame_presentations;
    check(strata_surface_frame(surface, 68'000'000, &frame_info).status == STRATA_STATUS_OK &&
              counters.frame_callbacks == paint_callback_baseline + 2U &&
              counters.frame_presentations == settled_presentations,
          "a settled extension widget kept pumping idle frames");

    frame_click[0].x = 80.0;
    frame_click[0].timestamp_nanoseconds = 80'000'000;
    frame_click[1] = frame_click[0];
    frame_click[1].kind = STRATA_INPUT_POINTER_RELEASE;
    check(strata_surface_enqueue_input(surface, frame_click, 2U, &batch).status ==
                  STRATA_STATUS_OK &&
              strata_surface_frame(surface, 80'000'000, &frame_info).status == STRATA_STATUS_OK &&
              strata_surface_frame(surface, 96'000'000, &frame_info).status == STRATA_STATUS_OK,
          "a layout-cost extension frame did not run");
    frame_json.clear();
    check(strata_surface_read_frame_json(surface, &frame_sink).status == STRATA_STATUS_OK &&
              frame_json.find(R"("layoutWork": 1)") != std::string::npos,
          "a layout-cost extension frame was incorrectly collapsed into paint work");

    environment.generation = 2U;
    environment.reduced_motion = 1U;
    std::uint32_t adopted = 0U;
    check(strata_surface_adopt_environment(surface, &environment, &adopted).status ==
                  STRATA_STATUS_OK &&
              adopted == 1U,
          "the reduced-motion environment was not adopted");
    frame_click[0].x = 20.0;
    frame_click[0].timestamp_nanoseconds = 110'000'000;
    frame_click[1] = frame_click[0];
    frame_click[1].kind = STRATA_INPUT_POINTER_RELEASE;
    const std::uint64_t reduced_baseline = counters.frame_callbacks;
    check(strata_surface_enqueue_input(surface, frame_click, 2U, &batch).status ==
                  STRATA_STATUS_OK &&
              strata_surface_frame(surface, 110'000'000, &frame_info).status == STRATA_STATUS_OK &&
              strata_surface_frame(surface, 126'000'000, &frame_info).status == STRATA_STATUS_OK &&
              counters.frame_callbacks == reduced_baseline + 1U && counters.reduced_motion_frame &&
              strata_surface_frame(surface, 142'000'000, &frame_info).status == STRATA_STATUS_OK &&
              counters.frame_callbacks == reduced_baseline + 1U,
          "reduced motion did not bound a re-requesting extension to one snap frame");

    environment.generation = 3U;
    environment.reduced_motion = 0U;
    adopted = 0U;
    check(strata_surface_adopt_environment(surface, &environment, &adopted).status ==
                  STRATA_STATUS_OK &&
              adopted == 1U,
          "the ordinary-motion environment was not restored");
    frame_click[0].x = 145.0;
    frame_click[0].timestamp_nanoseconds = 160'000'000;
    frame_click[1] = frame_click[0];
    frame_click[1].kind = STRATA_INPUT_POINTER_CANCEL;
    const std::uint64_t cancel_baseline = counters.frame_callbacks;
    check(strata_surface_enqueue_input(surface, frame_click, 2U, &batch).status ==
                  STRATA_STATUS_OK &&
              strata_surface_frame(surface, 160'000'000, &frame_info).status == STRATA_STATUS_OK &&
              strata_surface_frame(surface, 176'000'000, &frame_info).status == STRATA_STATUS_OK &&
              counters.frame_callbacks == cancel_baseline,
          "explicit frame cancellation left a callback pending");

    frame_click[0].timestamp_nanoseconds = 190'000'000;
    frame_click[1] = frame_click[0];
    frame_click[1].kind = STRATA_INPUT_POINTER_RELEASE;
    check(strata_surface_enqueue_input(surface, frame_click, 2U, &batch).status ==
                  STRATA_STATUS_OK &&
              strata_surface_frame(surface, 190'000'000, &frame_info).status == STRATA_STATUS_OK,
          "the detachment frame request was not accepted");
    std::string detached_source = source;
    const std::string frame_declaration =
        "    FrameHarnessWidget(key: \"harness.frame\", layout: { width: 160, height: 40 })\n";
    const std::size_t frame_declaration_offset = detached_source.find(frame_declaration);
    check(frame_declaration_offset != std::string::npos,
          "the frame harness declaration could not be removed for detachment");
    detached_source.erase(frame_declaration_offset, frame_declaration.size());
    const strata_activation_config detached_activation{
        sizeof(strata_activation_config),
        2U,
        view("harness/main.strata"),
        view(detached_source),
        nullptr,
        nullptr,
    };
    activation_info = {};
    activation_info.struct_size = sizeof(strata_activation_info);
    check(strata_runtime_compile_and_activate(runtime, &detached_activation, &activation_info)
                      .status == STRATA_STATUS_OK &&
              activation_info.status == STRATA_ACTIVATION_ACTIVATED,
          "the frame harness could not be detached through ordinary reconciliation");
    const std::uint64_t detach_baseline = counters.frame_callbacks;
    check(strata_surface_frame(surface, 206'000'000, &frame_info).status == STRATA_STATUS_OK &&
              strata_surface_frame(surface, 222'000'000, &frame_info).status == STRATA_STATUS_OK &&
              counters.frame_callbacks == detach_baseline,
          "a detached extension widget retained its requested frame callback");

    strata_surface_release(surface);
    strata_surface_extension_bundle version_2_widget_extensions = harness->bundle();
    std::vector<strata_widget_extension> version_2_widgets(
        version_2_widget_extensions.widgets,
        version_2_widget_extensions.widgets + version_2_widget_extensions.widget_count);
    for (strata_widget_extension& descriptor : version_2_widgets) {
        descriptor.struct_size = STRATA_WIDGET_EXTENSION_VERSION_2_SIZE;
    }
    version_2_widget_extensions.widgets = version_2_widgets.data();
    surface_config.id = view("harness.version-2-widget");
    surface_config.extensions = &version_2_widget_extensions;
    surface = nullptr;
    check(strata_runtime_create_surface(runtime, &surface_config, &surface).status ==
                  STRATA_STATUS_OK &&
              strata_surface_frame(surface, 230'000'000, &frame_info).status == STRATA_STATUS_OK,
          "a version-2 widget descriptor prefix was rejected after frame-hook growth");
    strata_surface_release(surface);
    strata_surface_extension_bundle legacy_widget_extensions = harness->bundle();
    std::vector<strata_widget_extension> legacy_widgets(legacy_widget_extensions.widgets,
                                                        legacy_widget_extensions.widgets +
                                                            legacy_widget_extensions.widget_count);
    legacy_widgets.front().struct_size = STRATA_WIDGET_EXTENSION_VERSION_1_SIZE;
    legacy_widget_extensions.widgets = legacy_widgets.data();
    surface_config.id = view("harness.legacy-widget");
    surface_config.extensions = &legacy_widget_extensions;
    surface = nullptr;
    check(strata_runtime_create_surface(runtime, &surface_config, &surface).status ==
                  STRATA_STATUS_OK &&
              strata_surface_frame(surface, 13'500, &frame_info).status == STRATA_STATUS_OK,
          "a version-1 widget descriptor prefix was rejected after subtarget growth");
    strata_surface_release(surface);
    strata_surface_extension_bundle legacy_extensions = harness->bundle();
    legacy_extensions.struct_size = STRATA_SURFACE_EXTENSION_BUNDLE_VERSION_1_SIZE;
    surface_config.id = view("harness.legacy-surface");
    surface_config.extensions = &legacy_extensions;
    surface = nullptr;
    check(strata_runtime_create_surface(runtime, &surface_config, &surface).status ==
                  STRATA_STATUS_OK &&
              strata_surface_frame(surface, 14'000, &frame_info).status == STRATA_STATUS_OK,
          "a version-1 extension bundle prefix was rejected after input-table growth");
    strata_surface_release(surface);
    legacy_extensions.struct_size = STRATA_SURFACE_EXTENSION_BUNDLE_VERSION_2_SIZE;
    surface_config.id = view("harness.version-2-surface");
    surface = nullptr;
    check(strata_runtime_create_surface(runtime, &surface_config, &surface).status ==
                  STRATA_STATUS_OK &&
              strata_surface_frame(surface, 15'000, &frame_info).status == STRATA_STATUS_OK,
          "a version-2 extension bundle prefix was rejected after scroll-table growth");
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
        if (argument_count == 2)
            test_surface_lifecycle(std::filesystem::path(arguments[1]));
        std::cout << "strata_extension_tests: OK\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "strata_extension_tests: " << exception.what() << '\n';
        return 1;
    }
}
