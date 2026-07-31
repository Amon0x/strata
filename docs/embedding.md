# Embedding a Strata application

The stable embedding boundary is `strata/strata.h`. A C++ host may include `strata/strata.hpp` for
typed ownership and result exceptions, but both paths use the same ABI and capability model. No JVM
or Gradle runtime is involved.

## Lifecycle

```text
negotiate ABI/capabilities
  -> create runtime + install host adapters
  -> configure registry/application schemas
  -> publish host snapshots + register actions
  -> compile and activate source (last-good)
  -> create Surface with environment/fonts/textures
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

Configure the neutral registry, compile/activate an entry source, then create a Surface. The complete
buildable flow is [c_smoke.c](../native/samples/c_smoke.c); it uses only the installed header and
library.

## C++ facade

The C++ wrapper converts result checks and releases into normal scope ownership:

```cpp
strata::Runtime runtime(runtime_config);
runtime.configure_application(application_config);
if (runtime.activate(activation_config).status != STRATA_ACTIVATION_ACTIVATED) {
    throw std::runtime_error("source activation was rejected");
}

strata::Surface surface = runtime.create_surface(surface_config);
const strata_surface_frame_info frame = surface.frame(now_nanos);
const std::vector<std::uint8_t> packet = surface.render_packet();
const std::vector<std::uint8_t> terminal = surface.prepare_release_packet();
host.consume(terminal);
surface.acknowledge_release_packet();
surface.close();
runtime.close();
```

A C++ `Surface` retains shared runtime ownership, so moving it out of the runtime's lexical scope is
safe. See [cpp_smoke.cpp](../native/samples/cpp_smoke.cpp) for activation, resource reload, canonical
frame reading, and telemetry.

## Host data, actions, and services

- Publish immutable host snapshots with strictly increasing generations. Schemas define their typed
  paths; missing required data rejects activation without replacing the last-good unit.
- Register action handlers per runtime. Payloads and event values are canonical JSON borrowed only
  for the callback. Handler registrations have explicit release handles.
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
  `strata_runtime_emit_effect_json`. The authoring-language `effect(...)` value is a visual material
  instruction consumed by native rendering and never calls the host effect adapter.
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
ordered batches, call `strata_surface_frame`, then read packet v3 through a bytes sink.

The packet bytes are borrowed only during the sink callback. Copy them if the backend submits later;
consume them directly if submission is synchronous. Packet v3 already contains native geometry,
indices, scissors, materials, textures, draw/effect batches, and one-shot GPU resource operations.
Hosts must not redo layout, glyph generation, or batch planning.

Canonical frame JSON is an optional inspection/conformance projection and is deliberately lazy. It
is not required for normal rendering.

Before destroying a Surface, prepare its terminal release packet, synchronously submit/consume it
while the host texture owner is alive, explicitly acknowledge that consumption, and then release the
Surface. Preparation alone never authorizes release. Failed or out-of-order release retains the
handle for retry, and a runtime refuses release while it still owns any Surface. Explicit abandon is
reserved for cases where delivery is impossible and the host independently discards remaining GPU
resources.

## Reload and failure recovery

Compilation/activation is last-good: a rejected generation returns diagnostics and leaves the active
unit and compatible state untouched. Resource reload similarly builds candidate font/texture state
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
gate configures that project against the install prefix and runs its C/C++ applications plus the
installed compiler/Surface corpus through CTest. This catches accidental dependencies on repository
paths, Kotlin classes, Gradle, demo registries, or private C++ headers.
