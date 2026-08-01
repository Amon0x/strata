# Native extension authoring

Use `.strata` first. Components, slots, styles, themes, state, bindings, ordinary motion, and
application composition are language concerns; host persistence, networking, and game integration
are typed actions. A native extension is the right tool only for genuinely new mechanics: custom
render primitives, custom hit geometry, or interaction a component cannot express by composition.

An extension package is declared once in C++ with `<strata/extension.hpp>`. That single definition
produces both the runtime bundle passed to `strata_surface_config::extensions` and the compiler
schema applied by `strata_compile`, so a widget name, parameter, or action contract cannot drift
between the two.

## A complete package

```cpp
#include <strata/extension.hpp>

using namespace strata::extension;

namespace {

/* Fields are declared once: name, type, and default live here and nowhere else. */
constexpr auto level = retained<number>("meter.level");
constexpr auto step = parameter<number>("step", 1.0);

bool activate_meter(Input& input) {
    input.set(level, input.get(level) + input.get(step));
    input.emit("meter.changed", R"({"level":1})");
    return true;                       // consumed
}

bool key_meter(Input& input, const Key& key) {
    if (key.name != "right") return false;
    input.set(level, input.get(level) + 1.0);
    return true;
}

void present_meter(Present& present) {
    const Rect bounds = present.bounds();
    present.rounded_rect(bounds, 6.0, rgba(31U, 39U, 54U), stroke(1.0, rgba(93U, 109U, 137U)));
    Rect fill = bounds;
    fill.width *= std::clamp(present.get(level) / 10.0, 0.0, 1.0);
    present.rounded_rect(fill, 6.0, rgba(65U, 151U, 130U));
    if (present.focus_visible()) present.border(bounds, 6.0, stroke(2.0, rgba(116U, 194U, 255U)));
}

void describe_meter(Semantics& semantics) {
    semantics.value_text("level");
    semantics.add_action("activate");
}

} // namespace

std::unique_ptr<Package> meter_package() {
    auto meter = widget("Meter")
        .parameter(step)
        .retained(level)
        .no_children()
        .focusable()
        .intrinsic_size(220.0, 24.0)
        .semantics_role("button")
        .semantics_actions({"activate"})
        .emits(ActionContract{
            "meter.changed",
            "Report a meter level change",
            "MeterLevel",
            "optional",
            {ActionArgument{"level", "number"}},
        })
        .on_activate(&activate_meter)
        .on_key(&key_meter)
        .on_semantics(&describe_meter)
        .present(&present_meter);

    auto created = package("example.meter.v1");
    created->widget(std::move(meter));
    return created;
}
```

Use `Present::focused()` for semantic state and `Present::focus_visible()` for focus paint. Pointer
focus intentionally keeps the former while suppressing the latter; keyboard and spatial input
restore the visible indicator.

`retained<Kind>` and `parameter<Kind>` produce constexpr handles. The widget adopts them, every hook
reads and writes through them, and `get` on a parameter uses the default declared with it — so a
field name is never spelled twice, a default is never restated at a call site, and a mistyped or
wrongly typed field is a compile error rather than a silent fallback.

Register it in `native/src/extensions/packages.cpp`:

```cpp
void register_builtin_packages(Registry& registry) {
    registry.add(demo_package());
    registry.add(example::meter_package());
}
```

That is the only edit outside the package itself: no compiler or host source changes, and no schema
JSON to keep in sync.

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

`strata_compile --check-module` resolves those ids through the package registry and applies the
projected declarations before the application's own, so `.strata` sees `Meter`, its parameters, and
`meter.changed` without a hand-written copy.

Hosts select the same ids. The desktop and headless hosts forward each package's `schema_json()`
through `strata_application_config::extension_schemas_json` and its `bundle()` through
`strata_surface_config::extensions`.

Selecting an unregistered id fails immediately with a diagnostic naming the unknown id and every
registered package.

## Supported lifecycle

The authoring layer is a projection of the internal `WidgetLifecycle` model. The publicly supported
surface is exactly what the builder exposes:

| Phase | Public capability |
| --- | --- |
| describe | typed parameter handles with defaults, child policy, intrinsic size, padding, clip, framework disclosure motion |
| input | activation, key press, focusability, popup retained field |
| retained | typed field handles with an invalidation class and fallback; number, boolean, and text reads and writes |
| semantics | role, declared actions, and a derive hook for name, value, checked, expanded, selected |
| inspection | hit bounds narrowing |
| present | content, overlay, detached overlay, motion and status feedback participation |
| render | solid and rounded rects, borders, text, images and atlas regions, nine-patch, custom mesh with material state, blur, shadow, scoped clip, and text measurement |
| behaviors | pointer events across capture/target/bubble, focusability, emitted actions |

Not public, by decision rather than omission: custom layout measurement and arrangement, custom
frame simulation, text editing, and command surfaces. The first two have no internal hook either —
`WidgetLifecycle` has exactly describe, input, semantics, inspection, command, and present, so
exposing them means new engine capability rather than a projection. The last two exist internally
but carry draft buffers, IME, undo, and command indices that no extension has needed yet.

Behavior *options* have no compiler validation today. The neutral registry declares behavior ids
only, so a package projects its behavior ids and their option objects are not type checked.

## Rules the layer enforces

- Retained fields must be declared. Writing a field the widget never adopted returns
  `STRATA_STATUS_NOT_FOUND` — `Input::set` reports it as `false` — instead of silently creating
  untracked state. Each declaration carries its invalidation class (`properties`, `layout`, `style`,
  `text`, `semantics`), recorded on every write, and its read fallback.
- A detached overlay requires the retained boolean that gates it, and that field must be declared.
- Duplicate widget types, behavior ids, parameters, retained fields, or package ids are rejected at
  definition time with a message naming the offender.
- A package is sealed once its bundle is taken; a late addition raises `std::logic_error`.
- Custom mesh vertices are normalized inside the draw bounds and must form indexed triangles. A mesh
  that violates either is dropped as a single draw, because the submission planner treats the same
  defect in engine-authored geometry as a fatal frame error and one extension must not blank a
  Surface. Material ids are validated against the application material contracts; an unknown id or
  parameter is dropped from the packet with a diagnostic.
- Clipping is only reachable through the `ClipScope` guard returned by `Present::clip`, so a push
  cannot outlive its pop.
- Hooks are plain function pointers over a package-owned hook table passed as `user_data`. Nothing
  is allocated per callback, per frame, or per registry lookup: descriptors, JSON defaults, and the
  retained table are materialized once when the bundle is first taken.

## Testing

`native/tests/extension_tests.cpp` (`ctest --preset <platform> -R strata.extension`) is
the harness. It builds a package covering every supported phase, then checks registration, duplicate
rejection, schema and compile parity, retained identity and rejection of undeclared fields,
activation, key input, semantics, hit bounds, detached overlay painting, behavior dispatch, and
action contracts against a headless Surface — no host window involved.

The shipped demo package (`native/src/extensions/demo_package.cpp`) is the reference: `DemoPulse`,
`DemoDisclosure`, and `demo.inspector-pick` in roughly 200 lines with no schema copy and no host
wiring.
