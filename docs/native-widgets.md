# Native extension authoring

Use `.strata` first. Components, slots, styles, themes, state, bindings, ordinary motion, and
application composition are language concerns; host persistence, networking, and game integration
are typed actions. A native extension is the right tool only for genuinely new mechanics: custom
render primitives, custom hit geometry, or interaction a component cannot express by composition.

An extension package is declared once in C++ with `<strata/extension.hpp>` and built as its own
shared library. That single definition exports both the runtime bundle passed to
`strata_surface_config::extensions` and the compiler schema applied by `strata_compile`, so a widget
name, parameter, or action contract cannot drift between the two. Hosts load the library through the
stable C ABI in `<strata/extension_plugin.h>`; no C++ object crosses the library boundary.

## A complete draggable widget

```cpp
#include <strata/extension.hpp>

#include <algorithm>

using namespace strata::extension;

namespace {

constexpr auto level = retained<number>("meter.level", 0.5, Invalidation::paint);
constexpr auto dragging = retained<boolean>("meter.dragging", false, Invalidation::input);

bool point_meter(Input& input, const Pointer& pointer) {
    if (pointer.button != 0) return false;
    if (pointer.kind == Pointer::Kind::press) {
        input.set(dragging, true);
        static_cast<void>(input.claim_gesture());
    } else if (!input.get(dragging)) {
        return false;
    }

    if (pointer.kind == Pointer::Kind::cancel) {
        input.set(dragging, false);
        static_cast<void>(input.cancel_gesture());
        return true;
    }

    const Rect bounds = input.bounds();
    const double next = std::clamp(pointer.local_x / bounds.width, 0.0, 1.0);
    input.set(level, next);
    if (pointer.kind == Pointer::Kind::move) {
        static_cast<void>(input.live(next));
    } else if (pointer.kind == Pointer::Kind::release) {
        input.set(dragging, false);
        static_cast<void>(input.commit(next));
    }
    return true;
}

bool key_meter(Input& input, const Key& key) {
    if (key.name != "left" && key.name != "right") return false;
    const double direction = key.name == "right" ? 1.0 : -1.0;
    const double next = std::clamp(input.get(level) + direction * 0.01, 0.0, 1.0);
    input.set(level, next);
    static_cast<void>(input.commit(next));
    return true;
}

void present_meter(Present& present) {
    const Rect bounds = present.bounds();
    present.rounded_rect(
        bounds,
        6.0,
        rgba(31U, 39U, 54U),
        stroke(1.0, rgba(93U, 109U, 137U))
    );
    Rect fill = bounds;
    fill.width *= present.get(level);
    present.rounded_rect(fill, 6.0, rgba(65U, 151U, 130U));
    if (present.focus_visible()) {
        present.border(bounds, 6.0, stroke(2.0, rgba(116U, 194U, 255U)));
    }
}

void describe_meter(Semantics& semantics) {
    const double current = semantics.get(level);
    semantics.name("Meter level");
    semantics.value_range(current, 0.0, 1.0);
    semantics.add_action("decrement");
    semantics.add_action("focus");
    semantics.add_action("increment");
}

std::unique_ptr<Package> meter_package() {
    auto meter = widget("Meter")
        .retained(level)
        .retained(dragging)
        .no_children()
        .focusable()
        .intrinsic_size(220.0, 24.0)
        .semantics_role("slider")
        .depends_on_status()
        .on_pointer(&point_meter)
        .on_key(&key_meter)
        .on_semantics(&describe_meter)
        .present(&present_meter);

    auto created = package("example.meter.v1");
    created->widget(std::move(meter));
    return created;
}

} // namespace

STRATA_EXTENSION_PACKAGE(meter_package)
```

The pressed widget retains pointer capture through release or cancellation, including movement
outside its bounds. `claim_gesture()` wins arbitration against competing gestures; cancellation is
always delivered to the owner. `Pointer::x`/`y` are surface coordinates, `local_x`/`local_y` are
relative to the widget, and `delta_x`/`delta_y` preserve the host-provided movement delta.
`on_scroll` receives the same capture/target/bubble route with surface and local coordinates plus
wheel or trackpad deltas.

`Invalidation::paint` rebuilds this widget's content fragment without description reconciliation,
layout, text shaping, or semantics. `Invalidation::input` stores transient gesture/session state
without scheduling any downstream projection. Typed `live` helpers publish local
number/boolean/text events without dispatching a host action. Typed `commit` helpers publish the
committed event and schedule one semantic projection, so accessibility updates once on release
rather than on every pointer move. Use a package-declared action only when the value actually
crosses the application boundary.

Use `Present::focused()` for semantic state and `Present::focus_visible()` for focus paint. Pointer
focus intentionally keeps the former while suppressing the latter; keyboard and spatial input
restore the visible indicator. `Input::scale()` and `Present::scale()` expose the logical-to-display
scale without requiring host access.

`retained<Kind>` and `parameter<Kind>` produce constexpr handles. The widget adopts them, every hook
reads and writes through them, and `get` on a parameter uses its declared default. `has(parameter)`
distinguishes an omitted optional property from an authored controlled value, enabling one widget
to support controlled and retained fallback modes without stringly typed property checks. A field
name and default are never restated at call sites, and undeclared writes fail rather than
materializing hidden state.

## Compound controls

Use fixed-capacity `structured<T>` retained state for a bounded collection that moves frequently:

```cpp
struct StopState {
    std::uint32_t count = 0;
    std::array<Stop, 8> stops{};
};

constexpr auto stops = retained<structured<StopState>>(
    "gradient.stops", StopState{}, Invalidation::paint
);
constexpr auto controlled_stops = parameter<any>("stops");
constexpr auto outline = parameter<color>("outline");
```

`T` must be trivially copyable and standard-layout. The first write allocates node-local byte
storage; equal-size updates reuse it. `ValueView` reads `any` values as borrowed lists/objects and
typed boolean, number, text, or color leaves without JSON serialization. This lets an authored list
act as the controlled value while `structured<T>` supplies the per-instance retained fallback and
active drag draft.

Project independently hittable regions with `.subtargets(...)`. Bounds are widget-local; ids and
indices must remain stable when collection order changes. Higher `z_index` wins overlap, then later
declaration order. Mark a region `semantic: true` when its index corresponds to a
`Semantics::child(...)` virtual child:

```cpp
void project_stops(Subtargets& projection) {
    const StopState state = projection.get(stops);
    for (const Stop& stop : active_stops(state)) {
        projection.add(Subtarget{
            stop_id(stop.id),
            stop.id,
            handle_bounds(stop.position),
            stop.id == state.selected ? 20 : 10,
            true,
            true,
        });
    }
}
```

`Pointer::subtarget_id` and `subtarget_index` identify the pressed region and stay routed through
capture. Ordinary paint-only movement deliberately does not rebuild subtarget geometry; capture
continues to the owner while the handle moves. On release, `invalidate(Invalidation::input)`
requests one fresh hit projection and `invalidate(Invalidation::semantics)` refreshes the virtual
children.

`Present::get(parameter<color>)` provides typed authored colors, `Present::get(parameter<any>)`
provides a `ValueView`, and `Present::style(name)` reads a computed/direct style value through the
same facade. Material draws continue to use typed `material_number`, `material_boolean`,
`material_text`, and `material_color` parameters.

The coordinate overload of `Present::text` takes the shaped line's top-left origin; it is not a
baseline or a control-alignment API. Labels inside buttons, rows, and other bounded controls should
use the rectangle overload with horizontal and vertical `TextAlignment`. That path aligns from the
actual shaped line metrics, so font, scale, and rasterization changes do not require guessed offsets.

## Bounded frame callbacks

Call `Input::request_frame(FrameCost::paint)` when a gesture releases with velocity or another
widget-local effect genuinely needs a future sample. The callback registered with `.on_frame(...)`
receives the surface frame time, the delta from that widget's previous requested frame, and the
resolved reduced-motion policy. A request schedules one callback only; a running effect must request
each successor explicitly.

Use `FrameCost::paint` when the callback changes only an input-invalidated draft consumed by
presentation. Use `FrameCost::layout` only when geometry measurement or arrangement must change.
`Input::cancel_frame()` cancels the pending callback and clears its delta history. The runtime also
cancels requests when the retained widget detaches, stops participating, or has no current layout.

Reduced motion admits one callback so the widget can snap and commit its terminal state, then
suppresses any continued request. A settled callback must omit the successor request or cancel
explicitly. This
makes an idle extension disappear from input advancement, layout, and render traversal rather than
relying on a callback that repeatedly discovers it has nothing to do.

```cpp
void advance(Input& input, const Frame& frame) {
    State state = input.get(state_field);
    if (frame.reduced_motion) {
        state.velocity = 0.0;
    } else {
        const double dt = std::clamp(
            frame.delta_nanoseconds / 1'000'000'000.0, 0.0, 0.05
        );
        integrate(state, dt);
    }
    input.set(state_field, state);
    if (state.velocity == 0.0) {
        input.cancel_frame();
        commit(input, state);
    } else {
        input.request_frame(FrameCost::paint);
    }
}
```

Build against the installed SDK. The helper creates the shared library, links the authoring layer,
sets C++23 and symbol visibility, assigns the portable discovery name, records it for same-build
module validation, and optionally installs it to the platform discovery directory:

```cmake
find_package(Strata CONFIG REQUIRED)
strata_add_extension_package(
    TARGET example_meter
    PACKAGE example.meter.v1
    SOURCES meter.cpp
    INSTALL
)
```

The package id supplied to CMake must equal the id returned by the factory. The extension links the
static `Strata::extensions` authoring layer and shared stable `Strata::c` ABI; it is not linked into
the compiler or a host.

## Activating a package

An application names the packages it activates in its `*.schemas.json`:

```json
{
  "extensionPackages": ["example.meter.v1"],
  "widgets": { "registry": "app.v1", "required": [], "definitions": [] },
  "actions": { "registry": "app.v1", "required": [], "definitions": [] },
  "host": []
}
```

Point tools at the directory containing the library:

```sh
strata_compile --extension-path build/extensions --check-module \
  app.strata app.schemas.json
```

The option is repeatable. Discovery checks explicit paths first, then `STRATA_EXTENSION_PATH`, then
the executable directory and the installed `lib/strata/extensions` location. VS Code exposes the
same list as `strata.extensions.paths`.

Hosts read package ids from the same schema document. `desktop::ApplicationConfig` and headless
scenarios still accept extension search paths, but a second package-id list is unnecessary when a
schema is present. Each host loads a package once, forwards its copied schema through
`strata_application_config::extension_schemas_json`, forwards its descriptor bundle through
`strata_surface_config::extensions`, and keeps the library loaded until the Surface has been
released. A missing library, mismatched exported id, incompatible plugin/core ABI, missing entry
point, or malformed descriptor fails before application activation with the package and library in
the error.

## Supported lifecycle

The authoring layer is a public projection of the internal lifecycle. Widget authors consume only
these capabilities:

| Phase | Public capability |
| --- | --- |
| describe | typed parameter handles with defaults/presence, child policy, intrinsic size, padding, clip, framework disclosure motion |
| input | activation, continuous pointer and scroll lifecycles with phased routing, capture arbitration, local coordinates, scale, key press, focusability, typed live/commit events, popup retained field |
| retained | typed per-instance number, boolean, text, and fixed-capacity structured fields with declared input-only, paint-only, or broader invalidation and fallback |
| semantics | typed retained/parameter reads plus role and derive hook for owner state and independently addressable virtual children |
| inspection | hit-bounds narrowing and stable widget-owned subtargets with overlap precedence, logical indices, and semantic mapping |
| present | content, overlay, detached overlay, motion/status participation, typed colors, borrowed structured values, computed/direct styles, bounds, scale, and state |
| render | solid and rounded rects, borders, text, images, atlas regions, nine-patch, custom mesh with typed material state, blur, shadow, scoped clip, and text measurement |
| behaviors | ambient pointer events across capture/target/bubble and emitted actions |

Not public, by decision rather than omission: custom layout measurement and arrangement, custom
frame simulation, text editing, and command surfaces. The first two have no internal hook either —
`WidgetLifecycle` has exactly describe, input, semantics, inspection, command, and present, so
exposing them means new engine capability rather than a projection. The last two exist internally
but carry draft buffers, IME, undo, and command indices that no extension has needed yet.

Behavior *options* have no compiler validation today. The built-in catalog declares behavior ids
only, so a package projects its behavior ids and their option objects are not type checked.

## Rules the layer enforces

- Retained fields must be declared. Writing a field the widget never adopted returns
  `STRATA_STATUS_NOT_FOUND`—`Input::set` reports it as `false`—instead of silently creating
  untracked state. `input` invalidation stores session state only; `paint` invalidation rebuilds
  widget content only; `layout`, `text`, `style`, `semantics`, and `properties` opt into broader
  work. Equal values cause no invalidation.
- Duplicate widget types, behavior ids, parameters, retained fields, or package ids are rejected at
  definition time with a message naming the offender.
- A package is sealed once its bundle is taken; a late addition raises `std::logic_error`.
- The exported package descriptor and every callback/string it references remain owned by the
  loaded library. The loader copies schema text but deliberately retains the library until all
  copied callbacks are unreachable.
- Custom mesh vertices are normalized inside the draw bounds and must form indexed triangles. A mesh
  that violates either is dropped as a single draw, because the submission planner treats the same
  defect in engine-authored geometry as a fatal frame error and one extension must not blank a
  Surface. Material ids are validated against the application material contracts; an unknown id or
  parameter is dropped from the packet with a diagnostic.
- Clipping is only reachable through the `ClipScope` guard returned by `Present::clip`, so a push
  cannot outlive its pop.
- Hooks are plain function pointers over a package-owned hook table passed as `user_data`.
  Descriptors, defaults, and retained-field lookup tables are materialized once when the bundle is
  taken. Scalar paths and equal-size structured pointer/paint updates require no JSON parsing or
  repeated node-local allocation; explicit text/JSON events may allocate according to their
  payload.

## Testing

`native/tests/extension_tests.cpp` (`ctest --preset <platform> -R strata.extension`) builds a
package covering every supported phase. It checks registration, schema/compiler parity, retained
identity, pointer capture outside bounds, cancellation, local coordinates, scroll routing,
controlled-property presence, borrowed structured/color access, compound subtarget overlap and
semantic children, display scale, typed live/commit events, keyboard input, numeric semantics, hit
bounds, detached overlays, behavior dispatch, and action contracts against a headless Surface.

The drag regression establishes capture, then enqueues 120 moves before one frame. Its compound
handle moves through allocation-conscious structured state. The test verifies queue coalescing, one
content-presentation rebuild, stable semantic generation and subtarget/hit geometry, and unchanged
runtime ABI allocator counts.
Release runs separately and schedules the single committed semantic projection. A scroll write
through `Invalidation::input` also proves that session bookkeeping causes neither presentation nor
semantic work. These assertions defend the lifecycle contract instead of benchmarking
machine-dependent wall-clock timing.

The shipped packages are the references: `strata_demo_extension` covers activation, children,
disclosure, motion, overlays, and behaviors; `strata_control_deck_extension` implements both a
multi-region continuous color control and a production gradient editor through public headers only.
The gradient workspace covers stable overlapping stop handles, add/remove/reorder, retained and
controlled values, palette selection, keyboard editing, semantic children, typed style colors, a
material-backed glint, and one commit on release. Installed-package tests independently build and
query another extension against the installed SDK.
