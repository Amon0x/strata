# Embedding a Strata application

The stable embedding boundary is `strata/strata.h`. A C++ host may include `strata/strata.hpp` for
typed ownership and result exceptions, but both paths use the same ABI and capability model. No language VM or external build runtime is involved.

## Lifecycle

```text
negotiate ABI/capabilities
  -> create runtime + install host adapters
  -> configure application schemas
  -> publish host snapshots + register actions
  -> compile and activate source (last-good)
  -> create Surface with environment/fonts/images
  -> enqueue input -> frame -> consume packet
  -> prepare terminal packet -> consume -> acknowledge -> release Surface
  -> release registrations/snapshots/runtime
```

Every host owns its clock, environment generations, resource generations, and handle lifetime.
Separate windows should use separate runtimes when their application/input/diagnostic state must be
isolated.

## Minimal C setup

Initialize structures to zero, set `struct_size`, and require only capabilities the host uses:

```c
strata_runtime_config config = {0};
config.struct_size = sizeof(config);
config.abi_version = STRATA_ABI_VERSION_CURRENT;
config.required_capabilities =
    STRATA_CAPABILITY_CORE_LIFECYCLE |
    STRATA_CAPABILITY_APPLICATION_LIFECYCLE |
    STRATA_CAPABILITY_COMPILER_ACTIVATION |
    STRATA_CAPABILITY_SURFACE_RUNTIME |
    STRATA_CAPABILITY_SURFACE_RENDER_PACKET;
config.clock = (strata_clock){sizeof(strata_clock), clock_state, now_nanoseconds};

strata_runtime* runtime = NULL;
strata_result result = strata_runtime_create(&config, &runtime);
```

Configure the application schema, compile/activate an entry source, then create a Surface. The complete
buildable flow is [c_smoke.c](../native/samples/c_smoke.c); it uses only the installed header and
library.

## C++ facade

The C++ wrapper owns callback storage, input text, configurations, adapter state, snapshots, and
handles while preserving the same ABI contracts:

```cpp
strata::RuntimeOptions runtime_options;
runtime_options.diagnostic = [](const strata::Diagnostic& diagnostic) {
    log(diagnostic.code, diagnostic.message);
};
strata::Runtime runtime(std::move(runtime_options));

runtime.configure_application(strata::ApplicationOptions{
    .id = "example.application",
});
if (!runtime.activate(strata::SourceActivation{
        .generation = 1,
        .entry_source_id = "ui/main.strata",
        .entry_text = source_text,
    }).activated()) {
    throw std::runtime_error("source activation was rejected");
}

strata::SurfaceOptions surface_options;
surface_options.id = "example.surface";
surface_options.root_role = strata::SurfaceRootRole::screen;
surface_options.root_name = "Main";
surface_options.environment.framebuffer_width = 1280;
surface_options.environment.framebuffer_height = 720;
surface_options.environment.logical_width = 1280.0;
surface_options.environment.logical_height = 720.0;
strata::Surface surface = runtime.create_surface(surface_options);

static_cast<void>(surface.enqueue(strata::InputEvent::pointer(
    strata::InputKind::pointer_move, strata::Point{32.0, 48.0}
)));
const strata_surface_frame_info frame = surface.frame(now_nanos);
const std::vector<std::uint8_t> packet = surface.render_packet();
const std::vector<std::uint8_t> terminal = surface.prepare_release_packet();
host.consume(terminal);
surface.acknowledge_release_packet();
surface.close();
runtime.close();
```

`Runtime` exposes owned `std::function` adapters for resources, durability, asynchronous host data,
clipboard, IME, and domain effects. It also provides owned diagnostics, activation results, action
results, source-map results, and RAII runtime/application-state snapshots. `AbiError` includes the
matching diagnostic code and message whenever the ABI reports a diagnostic identity. Owned
runtime/Surface profiler snapshots and host-frame telemetry use the same facade. Raw C
structures remain available as an escape hatch.

A C++ `Surface` retains shared runtime ownership, so moving it out of the runtime's lexical scope is
safe. Explicit release-packet consumption and `close()` remain the correct shutdown path. If a live
Surface is forgotten, its destructor uses the delivery-impossible abandon path and emits
`STRATA.SURFACE.RELEASE_ABANDONED` instead of terminating the process. See
[cpp_smoke.cpp](../native/samples/cpp_smoke.cpp) for the installed typed flow.

Applications that need an ordinary Win32 window should not manually consume render packets. Link
`Strata::desktop` and use `<strata/desktop.hpp>`; it owns the production packet decoder, D3D11
renderer, resource/clipboard services, input translation boundary, and teardown barrier. The
installed `desktop_app.cpp` sample is a complete window loop. See
[Win32 desktop hosting](desktop-hosting.md).

Linux deliberately has no bundled GUI backend. A Vulkan, OpenGL, or other renderer links
`Strata::render_host` and includes `<strata/render_packet.hpp>` instead of duplicating packet-v9
parsing:

```cpp
strata::host::RenderPacketDecoder decoder;
const strata::host::RenderPacket& plan = decoder.decode(surface.render_packet());

for (const strata::host::ResourceOperation& resource : plan.resources) {
    backend.apply(resource);
}
backend.upload_geometry(plan.vertices, plan.indices, plan.geometry_epoch);
for (const strata::host::SubmissionBatch& batch : plan.batches) {
    backend.submit(batch);
}
```

The decoder is stateful because settled packets can retain an earlier geometry epoch while updating
the frame index and resource operations. Consume every framed packet in order with one decoder per
Surface/backend stream. Compact packets are deltas and cannot initialize a fresh decoder; call
`reset()` only when discarding the stream, not while continuing to consume it. `RenderPacket`
exposes ordered texture mutations, fixed-layout vertex bytes, indices, scissors,
material/blend/texture bindings, draw batches, blur batches, backdrop/content effect batches, and
content stack markers. Effect batches carry a maximum refresh rate; custom backends may retain the
latest filtered sample between deadlines while continuing to composite every frame. The consumer
remains responsible for GPU resources, shader/material
implementation, presentation, and the Surface release-packet barrier.

Native submission keeps used geometry inside retained capacity arenas. A local draw-topology change
that still fits those arenas is byte-diffed against the preceding epoch and remains a geometry
patch; it does not force a complete Surface payload merely because planned-item counts changed.
Backends should apply those ranges directly to retained buffers. Capacity growth or a patch larger
than replacement remains an explicit full-epoch boundary.

Before framing, enumerate `Runtime::material_declarations(shaderBackend)` and
`Runtime::effect_pass_declarations(shaderBackend)`. Effect declarations are a flat table ordered by
effect id/pass index and carry blur constants or parameter slots plus the requested backend's
shader resource id. A custom backend retains its compiled programs and executes each packet effect
against the bounded sixteen-float parameter block. The C equivalents are
`strata_runtime_read_material_declarations` and
`strata_runtime_read_effect_pass_declarations`.

## Typed C++ host models

C hosts use the JSON ABI directly. Ordinary C++ application code should generate its contract from
the same application schema consumed by the compiler, include `strata/host.hpp`, and link
`Strata::host`. The installed CMake package exposes `Strata_AUTHORING`; invoke it with
`--write-cpp-contract <schema> <namespace> <output.hpp>` from a custom command.

The generated header provides model structures, complete/per-field snapshot encoders, action IDs,
typed payload decoders, enums, maps/unions, and an action variant:

```cpp
namespace contract = my::application::contract;
strata::host::Revision revision;
std::vector<contract::AppItemsItem> items = load_items();
strata::host::Bindings host(runtime, "my.application");

host.snapshot(
    "my.application.model",
    [&] { return revision.value(); },
    [&] { return contract::encode_app_items(items); }
);

host.on(contract::AppSaveAction::id, [&](const strata::host::ActionEvent& event) {
    const contract::AppSaveAction action = contract::AppSaveAction::decode(event);
    save(action.path);
    return strata::host::ActionResult::handled;
});

// Once per host tick, before framing surfaces:
host.synchronize();
```

Model code never assembles or parses JSON, and schema drift becomes a C++ compile failure.
`Bindings::synchronize()` republishes only changed revisions and rethrows any application exception
that had to be contained at the C callback boundary. For small standalone values,
`strata::host::Observable<T>` owns the revision automatically. The dynamic `Value` API remains the
intentional escape hatch for scenario runners and remote protocols whose schema is selected only at
runtime.

## Low-level host data, actions, and services

- Publish immutable host snapshots with strictly increasing generations. Schemas define their typed
  paths; missing required data rejects activation without replacing the last-good unit. The C++
  `Runtime::publish_host_snapshot` method owns generation allocation when a custom publication
  strategy is needed.
- Register action handlers per runtime. At the C boundary, payloads and event values are canonical
  JSON borrowed only for the callback and registrations have explicit release handles. The C++
  binding layer owns those details.
- Resource adapters return borrowed bytes; Strata copies any data it retains before returning.
  Resource adapter generations are nonzero and strictly increasing. A null, repeated, or stale
  replacement is rejected without mutating the active loader or gating live Surfaces.
- Clipboard, IME, and domain-effect callbacks are independently optional capabilities. Install
  only complete callback structures and advertise only services whose installation succeeded.
  Clipboard reads/writes used by editors cross this runtime-owned boundary; when no platform
  clipboard is installed, a runtime-shared fallback keeps sibling Surfaces coherent.
- A focused editable node activates IME after layout and publishes its transformed logical caret
  rectangle. The host converts that rectangle to platform coordinates. IME ownership is arbitrated
  per runtime so an inactive sibling Surface cannot disable the active Surface; releasing focus or
  the owning Surface deactivates it.
- The effect adapter receives only explicit domain effects emitted through
  `strata_runtime_emit_effect_json`. The authoring-language `effect(...)` value is a visual render
  program consumed by the backend and never calls the host effect adapter.
- Diagnostics use the size- and version-tagged `strata_diagnostic` payload. The callback carries
  stable identity/sequence, severity, complete source range, component path, expected/recovery
  text, first/latest frame, occurrence count, and the canonical store's dropped-record count.
- Require `STRATA_CAPABILITY_DIAGNOSTIC_SNAPSHOTS` to read bounded typed history through
  `strata_runtime_read_diagnostics` or its Surface alias without enabling lazy frame JSON.
  `strata_runtime_clear_diagnostics` (or the Surface alias) clears retained history, queued
  publication, dropped accounting, and Surface-owned pending diagnostic queues together. Because
  application diagnostics are runtime-shared, a Surface clear applies to every sibling Surface.

## Framing and packet consumption

Adopt a complete Surface environment generation atomically: framebuffer and logical sizes, scale,
safe insets, snapping, density, reduced-motion preference, and input capabilities. Enqueue input in
ordered batches, call `strata_surface_frame`, then read packet v9 through a bytes sink.

The packet bytes are borrowed only during the sink callback. Copy them if the backend submits later;
consume them directly if submission is synchronous. Packet-v8 full packets contain native geometry,
indices, scissors, materials, textures, and draw/blur/effect batches. Compact packets reference the
last full geometry epoch in the ordered stream; both forms can carry one-shot GPU resource
operations. Hosts must not redo layout, glyph generation, or batch planning.

Canonical frame JSON is an optional inspection/conformance projection and is deliberately lazy. It
is not required for normal rendering.

Before destroying a Surface, prepare its terminal release packet, synchronously submit/consume it
while the host texture owner is alive, explicitly acknowledge that consumption, and then release the
Surface. Preparation alone never authorizes release. Failed or out-of-order release retains the
handle for retry, and a runtime refuses release while it still owns any Surface. Explicit abandon is
reserved for cases where delivery is impossible and the host independently discards remaining GPU
resources. The C++ destructor treats a forgotten live Surface as that failure mode and reports it
through the diagnostic sink.

## Reload and failure recovery

Compilation/activation is last-good: a rejected generation returns diagnostics and leaves the active
unit and compatible state untouched. Resource reload similarly builds candidate font/image state
before swapping it into a Surface. After successful resource reload, frame again before reading the
new packet or canonical frame. A rejected reload leaves the prior resources usable. Host-facing
texture identities are generated per process/runtime/Surface and never reuse adapter source paths.

## Allocator and memory telemetry

If a custom allocator is supplied, its `user_data` and callbacks must outlive every runtime-derived
handle. Allocation and deallocation always return through the same callback pair. Runtime memory
info reports allocator-routed ABI handles and compile-arena high-water marks; ordinary internal STL
storage is intentionally outside those routed counters.

## Installed acceptance

The CMake install includes an independent sample project under `share/strata/samples`. The repository
gate configures that project against the install prefix and runs its C, C++, and Win32 desktop
applications plus the installed compiler/Surface corpus through CTest. This catches accidental
dependencies on repository paths, build-tree state, demo registries, or private C++ headers.
