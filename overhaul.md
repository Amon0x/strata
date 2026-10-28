# Core overhaul

## Goal

Every fact in the Strata core has exactly one owner. The compiler resolves names and types once and
lowering projects that result. A component instance owns its identity, inputs, dependencies, output
and effects, and invalidation is pushed to the instances that read what changed. Built-in widgets
use the same lifecycle contract as extensions. Backends execute packets and do not invent policy.
The C ABI is an exception-safe, versioned shell over a layered core. Every bug found by the October
2026 review is fixed with a regression test that fails before the fix.

Ground rules:

- Each item lands with a test that reproduces the defect or pins the new contract.
- Keep: lexer and Pratt parser, diagnostic codes, immutable descriptions, retained tree and
  reconcile, dirty-reason lanes, deterministic clock, motion players, the packet format and decoder
  validation, presentation groups, the `WidgetLifecycle` registry, shared HLSL.
- The full `windows-x64` CTest gate passes after every phase.

## Phase 1: correctness fixes

### Compiler

- [x] Duplicate component declarations no longer index the first schema with the second
      declaration's parameters (heap overflow in `semantic.cpp` `validate_declaration`).
- [x] Source spans stop copying whole lines: remove the unused `excerpt`, pass tokens by reference,
      so long single-line inputs are linear.
- [x] Animation- and style-typed slots resolve names with the same precedence in semantic analysis
      and lowering (an in-scope parameter is a variable, not a literal name).
- [x] Expression map keys are rejected with a diagnostic instead of lowering to `dynamic_N`.
- [x] `+` infers string when either operand is a string; operand types are checked.
- [x] Derived values used before their declaration are typed, not `unknown`.
- [x] Unknown parameter type names report an unknown-type diagnostic; lowering shares semantic's
      `resolve_type` (nullable list elements preserved).
- [x] Lowering diagnostics no longer duplicate semantic ones; `infer` diagnostics are reported once.
- [x] `from/to/in/where/state/root/if/derived` are contextual: usable as member names.
- [x] A `when` arm with a bare identifier match does not parse as a lambda.
- [x] `persistenceKey` uniqueness is enforced between siblings.
- [x] Slots declared inside a child component's fill are registered (slot forwarding).
- [x] Remove the JVM substring scan in `security.cpp`.

### Runtime and description

- [x] Repeater dependency observer forwards host and lexical reads upstream.
- [x] Explicit `key:` replaces the loop segment in component instance paths.
- [x] A parent checks its own currency before refreshing children; diagnostics and side effects
      from discarded builds are rolled back.
- [x] Cacheability depends on error-severity diagnostics raised by the component itself.
- [x] Repeater keys are memoized at generation time (O(1) `index_of_key`).
- [x] Lazy row bodies record dependencies and state-scope bindings.
- [x] Duplicate object fields produce a diagnostic, not `invalid_argument`.

### Surface and input

- [x] Events and action outcomes are recorded as one pair.
- [x] Removing a focused node blurs and commits its editor first; virtualized lists keep the
      focused row realized.
- [x] Invalidation raised after layout (reveal/scroll) survives `complete_frame`; the settled check
      includes the layout invalidation generation.
- [x] Grapheme segmentation follows UAX #29 (Indic, Thai, Hangul, emoji tags); boundaries cached per
      editor revision.
- [x] Sticky table and item-grid headers clip hit testing like rendering.
- [x] Ordinary subtargets are hit-tested in transformed space.
- [x] Hover is re-evaluated after wheel scroll, motion and popup changes.

### Rendering

- [x] Vulkan captures and blurs only the effect sample region, and only when a refresh is due.
- [x] Vulkan applies geometry patches (epoch is previous + 1).
- [x] OpenType PairPos/ligature expansion is bounded by table size; zero-format class pairs skipped.
- [x] Effect refresh default is unbounded; caches only engage when a material asks for a rate.
- [x] D3D11 effect staleness uses the sample region and resource uploads; cache entries are evicted.
- [x] Blur downsample rounding matches across backends.
- [x] Delete the unused `shaders/render2d`.

### ABI and hosts

- [x] Every C export is behind one exception firewall; authoring trampolines catch.
- [x] An extension fragment with unbalanced clips is dropped alone, not the frame.
- [x] Reentrant and wrong-thread calls into a runtime return a status instead of corrupting it.
- [x] Durable writer retries and isolates failures per path, flushing before rename.
- [x] Errors map to accurate statuses; missing resources are not cached forever.
- [x] Each source file compiles into exactly one CMake target.

### Phase 1 leftovers

- [x] ItemGrid's 28px descendant clip strip comes from real header geometry
      (`description_item_grid.cpp`), not a constant.
- [x] `d3d11/render_context.cpp` uses `host::packet_vertex_bytes` instead of its own 88.
- [x] `SurfaceEnvironment::validate` checks `scale` against framebuffer/logical size.
- [x] Row builders are reused across Repeater generations; `for` loops stop cloning their first node
      to set `materialization_key`.
- [x] A stale lazy row re-describes only itself, not the body holding its Repeater.
- [x] A dedicated test pins `async_succeed_json` failure statuses.

## Phase 2: one owner per fact

- [x] Compiler: semantic analysis emits a resolved, typed tree (node ids, type and symbol side
      tables, resolved call schemas). Lowering and the source map are projections over it. `Validator`
      splits into collect, resolve, type-check and policy passes.
- [x] Compiler: declarative catalog with named types and capability flags; runtime helper table and
      editor completions generated from it.
- [x] Runtime: `ComponentInstance` owns identity (numeric id), inputs, dependencies, output and
      effects; lazy rows are child instances sharing builder and caches.
- [x] Runtime: push-based invalidation with reverse indexes (state address, host path, retained
      field) replacing the full validation walk and the retained-snapshot pointer.
- [x] Runtime: building is pure; persistence and durability writes happen in a commit phase.
- [x] UI: built-in subtargets, popup dismissal, modal priority and editor geometry move onto
      lifecycle hooks; generic code holds no widget names.
- [x] UI: one hit-geometry pass records presented transforms and clips; every consumer reads it.
      (Emitting it from `render.cpp` was rejected: input routes mid-batch against a rebuilt tree
      before any render runs.)
- [x] UI: frame coordinator with explicit phases and a single dirty model.
- [x] Theme: open typed token map, per-widget style schemas, presentation colors as tokens.
- [x] Render: effect staleness computed once in the core/decoder; packet blend, clip and material
      ids typed; images decoded once in the core; cross-backend differential tests.
- [x] ABI: extensions get their own versioned function table; real struct-size versioning.
- [x] Hosts: one portable `host::Session`; the showcase host and control deck move out of core.
- [x] Layering: `base`/`compiler`/`runtime`/`ui` targets with enforced include direction; the
      compiler no longer depends on `ui`.

### Phase 2 leftovers

- [x] `description.cpp` and `tree.cpp` read Repeater/VirtualList/Slot roles from catalog
      capability flags; `widget_literals.cmake` drops their exclusions.
- [x] `.strata` code reads open and palette theme tokens (`theme.<name>` is no longer a closed set).
- [x] Interpreter: no string copies on variable reads, one list/map literal representation, cached
      host-path segments, an explicit dependency recorder.
- [x] Slots inside a widget's own generated rows (tables) receive caller content.
- [x] The showcase `Host` runs its sessions on `host::Session`.
- [x] The core PNG decoder and inflate have a fuzzer and bounded output.
- [x] Lambda bodies beyond `map` are type-checked; cascade suppression on broken input is per
      missing declaration, not global.
- [x] The runtime's evaluator-name table is generated from the catalog.
