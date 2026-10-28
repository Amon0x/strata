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

- [ ] Duplicate component declarations no longer index the first schema with the second
      declaration's parameters (heap overflow in `semantic.cpp` `validate_declaration`).
- [ ] Source spans stop copying whole lines: remove the unused `excerpt`, pass tokens by reference,
      so long single-line inputs are linear.
- [ ] Animation- and style-typed slots resolve names with the same precedence in semantic analysis
      and lowering (an in-scope parameter is a variable, not a literal name).
- [ ] Expression map keys are rejected with a diagnostic instead of lowering to `dynamic_N`.
- [ ] `+` infers string when either operand is a string; operand types are checked.
- [ ] Derived values used before their declaration are typed, not `unknown`.
- [ ] Unknown parameter type names report an unknown-type diagnostic; lowering shares semantic's
      `resolve_type` (nullable list elements preserved).
- [ ] Lowering diagnostics no longer duplicate semantic ones; `infer` diagnostics are reported once.
- [ ] `from/to/in/where/state/root/if/derived` are contextual: usable as member names.
- [ ] A `when` arm with a bare identifier match does not parse as a lambda.
- [ ] `persistenceKey` uniqueness is enforced between siblings.
- [ ] Slots declared inside a child component's fill are registered (slot forwarding).
- [ ] Remove the JVM substring scan in `security.cpp`.

### Runtime and description

- [ ] Repeater dependency observer forwards host and lexical reads upstream.
- [ ] Explicit `key:` replaces the loop segment in component instance paths.
- [ ] A parent checks its own currency before refreshing children; diagnostics and side effects
      from discarded builds are rolled back.
- [ ] Cacheability depends on error-severity diagnostics raised by the component itself.
- [ ] Repeater keys are memoized at generation time (O(1) `index_of_key`).
- [ ] Lazy row bodies record dependencies and state-scope bindings.
- [ ] Duplicate object fields produce a diagnostic, not `invalid_argument`.

### Surface and input

- [ ] Events and action outcomes are recorded as one pair.
- [ ] Removing a focused node blurs and commits its editor first; virtualized lists keep the
      focused row realized.
- [ ] Invalidation raised after layout (reveal/scroll) survives `complete_frame`; the settled check
      includes the layout invalidation generation.
- [ ] Grapheme segmentation follows UAX #29 (Indic, Thai, Hangul, emoji tags); boundaries cached per
      editor revision.
- [ ] Sticky table and item-grid headers clip hit testing like rendering.
- [ ] Ordinary subtargets are hit-tested in transformed space.
- [ ] Hover is re-evaluated after wheel scroll, motion and popup changes.

### Rendering

- [ ] Vulkan captures and blurs only the effect sample region, and only when a refresh is due.
- [ ] Vulkan applies geometry patches (epoch is previous + 1).
- [ ] OpenType PairPos/ligature expansion is bounded by table size; zero-format class pairs skipped.
- [ ] Effect refresh default is unbounded; caches only engage when a material asks for a rate.
- [ ] D3D11 effect staleness uses the sample region and resource uploads; cache entries are evicted.
- [ ] Blur downsample rounding matches across backends.
- [ ] Delete the unused `shaders/render2d`.

### ABI and hosts

- [ ] Every C export is behind one exception firewall; authoring trampolines catch.
- [ ] An extension fragment with unbalanced clips is dropped alone, not the frame.
- [ ] Reentrant and wrong-thread calls into a runtime return a status instead of corrupting it.
- [ ] Durable writer retries and isolates failures per path, flushing before rename.
- [ ] Errors map to accurate statuses; missing resources are not cached forever.
- [ ] Each source file compiles into exactly one CMake target.

## Phase 2: one owner per fact

- [ ] Compiler: semantic analysis emits a resolved, typed tree (node ids, type and symbol side
      tables, resolved call schemas). Lowering and the source map are projections over it. `Validator`
      splits into collect, resolve, type-check and policy passes.
- [ ] Compiler: declarative catalog with named types and capability flags; runtime helper table and
      editor completions generated from it.
- [ ] Runtime: `ComponentInstance` owns identity (numeric id), inputs, dependencies, output and
      effects; lazy rows are child instances sharing builder and caches.
- [ ] Runtime: push-based invalidation with reverse indexes (state address, host path, retained
      field) replacing the full validation walk and the retained-snapshot pointer.
- [ ] Runtime: building is pure; persistence and durability writes happen in a commit phase.
- [ ] UI: built-in subtargets, popup dismissal, modal priority and editor geometry move onto
      lifecycle hooks; generic code holds no widget names.
- [ ] UI: render emits hit regions; hit testing consumes them.
- [ ] UI: frame coordinator with explicit phases and a single dirty model.
- [ ] Theme: open typed token map, per-widget style schemas, presentation colors as tokens.
- [ ] Render: effect staleness computed once in the core/decoder; packet blend, clip and material
      ids typed; images decoded once in the core; cross-backend differential tests.
- [ ] ABI: extensions get their own versioned function table; real struct-size versioning.
- [ ] Hosts: one portable `host::Session`; the showcase host and control deck move out of core.
- [ ] Layering: `base`/`compiler`/`runtime`/`ui` targets with enforced include direction; the
      compiler no longer depends on `ui`.
