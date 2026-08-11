# Extension widget authoring plan

## Objective

Finish the extension authoring boundary so custom widgets can be implemented through the installed
public SDK without understanding Strata's retained tree, input router, dirty scheduler, ABI bridge,
host package loader, or repository-specific CMake wiring. The Control Deck color picker is the first
demanding consumer and the end-to-end proof, not a reason to add color-specific behavior to core.

## Non-goals

- No color-picker branches in the input router, retained tree, renderer, compiler, host, or ABI bridge.
- No host-action round trip for local visual feedback.
- No global or package-shared mutable widget-instance state.
- No property reconciliation, layout, text shaping, or semantics work on ordinary drag updates.
- No second packaging or activation convention beside the public extension path.

## Authoring boundary

An extension author should need only:

- the installed `<strata/extension.hpp>` API;
- the lifecycle capabilities selected by the widget;
- the widget's own state, input, presentation, semantics, and event contracts;
- one public CMake package helper; and
- one application-level package declaration.

An extension author should not need to inspect or modify:

- `native/src/ui` lifecycle and routing internals;
- dirty-generation or reconciliation implementation;
- C ABI trampolines;
- dynamic-library filename and visibility conventions;
- compiler extension search-path wiring; or
- duplicated compiler, host, and scenario package lists.

## Generic capability model

### Input

The public widget lifecycle must support:

- pointer press, move, release, and cancellation;
- stable pointer identity, button, modifiers, and gesture phase;
- widget-local and surface coordinates plus movement deltas;
- explicit pointer capture and release with cancellation safety;
- keyboard and focus operation;
- wheel/scroll input where a widget claims it; and
- widget-owned hit regions or subtargets without router-specific code.

The vocabulary must remain suitable for sliders, knobs, range controls, scrubbers, crop handles,
maps, timelines, graphs, and canvases. It must not contain color-specific concepts.

### State and invalidation

Retained state remains typed and per widget instance. Its invalidation class must state its cost:

- `input`: gesture/session bookkeeping without presentation work;
- `paint`: presentation regeneration only;
- `layout`: measurement or arrangement changed;
- `text`: text projection or shaping changed;
- `semantics`: accessibility projection changed; and
- `properties`/`structure`: authored description must be reconsidered.

The SDK documentation must make these guarantees explicit. Authors must not infer them from core
scheduler code.

### Values and events

The public API must support:

- controlled and uncontrolled values;
- local live feedback independent of host action dispatch;
- typed live-change and committed-change event contracts;
- suppression of unchanged values; and
- author-controlled event frequency, including commit-on-release behavior.

### Presentation

Public presentation capabilities must cover rectangles, borders, text, images, clipping, meshes,
materials, focus/hover/pressed/disabled status, widget bounds, and scale. Hot pointer and paint paths
must not allocate avoidably.

### Layout and composition

Children, padding, intrinsic size, clipping, disclosure, and detached overlays must form one documented
model. Generic gaps should be fixed in the SDK rather than bypassed in individual widgets.

### Accessibility

Continuous controls must be able to publish an accessible name and value, expose appropriate actions,
and support keyboard adjustment. Pointer-only extension widgets are incomplete.

## Packaging and activation

Provide one public CMake helper resembling:

```cmake
strata_add_extension_package(
    TARGET control_deck_extension
    PACKAGE strata.control-deck.v1
    SOURCES color_picker.cpp
)
```

The helper owns SDK linkage, shared-library naming, export visibility, platform install paths, schema
projection dependencies, and build defaults.

The application declares an extension package once. Compiler schema activation, runtime loading, and
host surface creation derive from that declaration instead of maintaining synchronized lists.

## Proving cases

The API is evaluated against distinct lifecycle shapes:

| Case | Capabilities |
| --- | --- |
| Existing pulse | Activation, retained state, action emission |
| Existing disclosure | Authored children, layout, clipping, motion |
| Existing inspector behavior | Ambient phased pointer handling and actions |
| Control Deck color picker | Continuous 2D and scalar input, multiple drag regions, paint-only state, keyboard, semantics, live and committed values |

The existing examples are migrated to the public package helper. The picker is implemented as an
ordinary external package using public headers only.

## Performance contract

During continuous manipulation:

- authored components are not reconciled;
- layout and text shaping do not run;
- gesture bookkeeping does not repaint unless requested;
- duplicate values cause no state write, invalidation, or event;
- visual feedback does not require a host action;
- pointer bursts are bounded to useful presentation work rather than raw event-frequency frames;
- the hot extension input and presentation paths perform no avoidable heap allocation; and
- profiler or conformance evidence identifies the lifecycle phases that actually ran.

The picker keeps hue, saturation, value, and alpha in widget-local retained state. Pointer movement
updates only paint state. A live value event is optional during movement; a committed value is emitted
on release and keyboard completion.

## Work plan

### Foundation

- Audit the installed API, ABI bridge, input routing, retained invalidation, package loading, compiler
  schema activation, and existing extension examples.
- Specify the public lifecycle and compatibility contract before editing core.

### SDK

- Expose the generic widget pointer lifecycle and explicit capture semantics.
- Add input-only and paint-only retained invalidation.
- Add typed live-change and commit helpers.
- Complete bounds, coordinates, deltas, status, scale, and semantic ergonomics needed by general
  interactive widgets.
- Grow descriptors compatibly through `struct_size`; old extension packages must remain loadable.

### Packaging

- Add and install the public CMake extension-package helper.
- Centralize package declaration discovery.
- Migrate existing demo package build and host consumers to the single path.

### Proof

- Implement the Control Deck extension package using only installed public headers.
- Author an accessible picker workspace in the Control Deck.
- Add conformance coverage for pointer lifecycle, capture cancellation, invalidation, events, keyboard,
  semantics, and package loading.
- Add regression evidence that drag updates avoid reconciliation, layout, text, and avoidable
  allocation.

### Documentation

- Document the complete external author workflow with a minimal draggable widget.
- State lifecycle costs and performance guarantees.
- Verify the proof package has no private includes or core-specific registration.

### Verification and cleanup

- Build the SDK, packages, compiler, desktop host, and relevant headless tooling.
- Regenerate and validate bundled schemas and compiled modules.
- Exercise pointer, keyboard, semantics, live value, commit value, cancellation, and high-frequency
  drag behavior end to end.
- Remove obsolete wiring and duplicate declarations.

## Definition of done

The Control Deck package is implemented through `<strata/extension.hpp>` and contains no private
runtime includes. Its ordinary move path performs widget-local paint-state writes and one content
fragment rebuild; input-only session writes schedule no downstream projection. Existing extension
examples use the same public authoring and packaging path. External authors can reproduce the
workflow from public headers and `docs/native-widgets.md` without learning Strata internals.
Committed values schedule one accessibility projection.

## Implemented outcome

- Pointer press/move/release/cancel, capture arbitration, phased scroll, local/surface coordinates,
  deltas, scale, focus state, and keyboard hooks are public extension capabilities.
- Typed retained fields distinguish input-only, content-paint, semantics, text, style, layout, and
  property invalidation. Equal writes are suppressed.
- Optional parameter presence is queryable in input, presentation, and semantics hooks, supporting
  controlled values with retained fallback state.
- Number, boolean, and text retained/parameter values are readable by accessibility hooks. Typed
  commits invalidate semantics once; live movement does not.
- `strata_add_extension_package` owns build naming, linkage, installation, and same-build discovery.
  Application schemas provide the package declaration consumed by compiler and hosts.
- `strata.control-deck.v1` supplies the accessible multi-region picker used by the Control Deck
  workspace. Hue, saturation/value, and alpha manipulation use fixed geometry and fixed-capacity
  payload formatting on the hot path.
- Stable compound subtargets, fixed-capacity structured state, borrowed value views, typed colors,
  computed styles, aligned text, and material parameters support the public-only gradient editor.
- Explicit frame request/cancel hooks expose frame time, per-widget delta, reduced motion, and paint
  versus layout cost. Requests self-expire after one callback and are discarded with detached or
  non-participating widgets.
- `DeckInertialScrubber` proves the frame contract with pointer-derived release velocity, bounded
  friction and edge response, one settled commit, keyboard access, and no idle callback.
- `CanvasTransform`, fixed-capacity `MeshBatch`, and reserved visible subtarget projection support
  public-only pan/zoom canvases without per-object render commands or full-set hit geometry.
- `DeckCurveEditor` proves the canvas contract over 8,192 stable global point identities with
  viewport-level detail, batched curve/marker geometry, point editing, lasso selection, anchored
  wheel zoom, right-drag pan, cancellation, keyboard adjustment, and bounded semantic projection.

## Acceptance evidence

- `strata.extension`: capture outside bounds, cancellation, phased scroll, property presence, scale,
  keyboard, semantics, version-1/version-2 bundle compatibility, and action/event contracts.
- The move regression establishes capture, coalesces 120 moves into one queued move, performs one
  content-fragment rebuild, leaves semantic generation and pointer-hit geometry unchanged, and
  leaves runtime routed allocation count unchanged. Release emits one commit and schedules one
  semantic projection.
- The Control Deck headless scenario exercises plane, hue, alpha, and keyboard changes and records
  committed color events in canonical frame output.
- `strata.headless.control_deck_gradient` exercises multi-stop editing and stable semantic children.
- `strata.headless.control_deck_motion` drives a sampled pointer drag through inertia and settlement,
  records the final action, and verifies subsequent idle frames perform no layout or render traversal.
- `strata.headless.control_deck_canvas` edits one virtual curve point, selects through a 60-sample
  lasso over the 8,192-object set, zooms and pans the viewport, preserves the selected point's
  accessible identity, emits committed canvas state, and verifies idle render/layout quiescence.

## Extension roadmap

The next work stays proof-driven: each generic capability lands with one demanding public-only
package, conformance evidence, and a desktop workspace suitable for manual interaction and FPS
evaluation. Milestones are delivered sequentially so later APIs are justified by a real consumer.

### Milestone A — compound controls and typed visuals

The public SDK gains stable widget-owned subtargets with bounds, overlap precedence, pointer identity,
keyboard selection, inspection identity, and independently projected semantic children. Structured
per-instance state must support bounded collections without JSON reconstruction or heap allocation on
ordinary movement. Typed color, structured value, computed style, and material parameter access must
remain available through public headers and the stable C ABI.

The proof is a production-quality native gradient editor in a dedicated Control Deck workspace:

- add, select, remove, reorder, and drag color stops;
- preserve capture and selection when handles overlap or cross;
- provide fine/coarse keyboard movement and deletion;
- support an optional authored controlled value and an allocation-conscious retained fallback;
- render a theme-aware, material-capable gradient preview;
- emit local live feedback during movement and one committed action on release or keyboard completion;
- expose each stop to inspection and accessibility under one stable compound-widget identity; and
- keep movement to one content-fragment rebuild with no reconciliation, layout, text, semantic,
  pointer-hit geometry, or runtime-routed allocation churn.

### Milestone B — bounded animation

Add explicit request/cancel-frame capability with frame time, delta, reduced-motion policy, automatic
suspension when hidden or detached, and distinct paint versus layout costs. Prove it with a control
whose behavior genuinely requires inertia or spring motion; never introduce a permanently pumping
idle widget.

Implemented. The installed SDK now exposes `Input::request_frame`, `Input::cancel_frame`,
`FrameCost`, and `.on_frame(...)`. Each request admits exactly one callback; retained
input-invalidated state plus paint cost keeps kinetic movement out of reconciliation, layout, text,
and semantics. Layout cost remains explicit for geometry-changing callbacks. Reduced motion permits
one terminal snap frame, and requests are removed when their owner detaches or loses layout.

The proof is `DeckInertialScrubber` in the dedicated Control Deck Motion workspace. Pointer samples
derive release velocity, bounded frame deltas integrate friction and edge response, keyboard edits
commit immediately, and settlement emits one `control-deck.motion.commit` action without leaving an
idle frame request.

### Milestone C — serious canvas controls

Add pan/zoom transforms, selection and lasso routing, batched geometry, viewport-aware projection,
large virtual interactive-object sets, and stable accessible identities. Prove the contract with a
curve editor before generalizing it to timelines, waveforms, node graphs, or vector tooling.

Implemented. The installed SDK now provides `CanvasTransform` for anchored world/surface projection,
pan, zoom, and visible-world derivation; `MeshBatch` for all-or-nothing fixed-capacity geometry
assembly; and `Subtargets::reserve` for bounded visible interaction projection. These are generic
public-header and stable-C-ABI capabilities, not curve-editor branches in core.

The proof is `DeckCurveEditor` in the Control Deck Canvas workspace. Its world contains 8,192 stable
point ids while the current viewport projects only a bounded level of detail. One curve mesh and one
marker batch replace per-object render commands. Point capture, lasso selection, anchored wheel zoom,
right-button pan, keyboard editing, cancellation, committed actions, and semantic children all retain
global point identity across viewport changes.

### Deferred platform work

- Custom layout measurement and arrangement remain deferred until a proof cannot use ordinary Strata
  composition.
- Native text editing remains a separate IME, selection, clipboard, and undo project.
- Command-surface integration remains deferred until an editor requires menus, shortcuts, palettes,
  and toolbar state.
