# `.strata` language syntax

This page describes the language itself. The generated
[widget/action/property reference](generated/strata-reference.md) is the authoritative list of
registered names, types, defaults, enum values, events, and retained state. The
[diagnostics catalog](generated/diagnostics.md) gives hand-authored cause and recovery guidance for
every diagnostic code present in production source; the build verifies exact coverage.

## Files, imports, and comments

A module is a UTF-8 `.strata` file. Imports use a quoted path resolved relative to the importing
file and remain inside the application's configured development roots.

```strata
import "components/forms.strata";

// line comment
/* block comment */
```

Imports form one compiled module. Duplicate declarations and import cycles are errors. Editing any
dependency recompiles the module atomically; a rejected edit leaves the last-good unit active.

## Lexical forms

- Identifiers begin with `_` or a Unicode letter and continue with letters, digits, or `_`.
- Strings use double quotes and backslash escapes.
- Numbers support `_` separators, fractions, exponents, and `ms`/`s` duration suffixes.
- Colors use `#RGB`, `#RGBA`, `#RRGGBB`, or `#RRGGBBAA`.
- Literals are strings, numbers/durations, colors, `true`, `false`, `null`, lists, and objects.
- Statements and style properties end with `;`. Widget calls do not require a trailing semicolon.

The VS Code extension under `editor/vscode` packages the generated TextMate grammar and catalog
metadata. It provides schema-aware widget/action/host/property completions, hovers, signature help,
symbols, import/local-definition navigation, and exact diagnostics from `strata_compile
--check-module-json`. It does not maintain a second JavaScript parser: the production compiler
remains the validation authority. Build `strata_vscode_extension` to produce the installable VSIX.

## Screens and overlays

Screens and overlays are named entry points. Exactly one `root` widget supplies their root node.

```strata
screen Main {
  root Panel(layout: { kind: "COLUMN", width: "fill", height: "fill" }) {
    Text(text: "Hello")
  }
}

overlay Dialogs {
  root Modal(open: true) { Text(text: "Saved") }
}
```

The host registers which entry points correspond to named screen/layer operations.

## Components, parameters, defaults, and slots

Components encapsulate parameters and local state. Parameters may carry a semantic type and a
default. A parameter without a default is required. Components can set widget defaults for their
subtree; an explicit argument at a widget call always wins.

```strata
component Card(title: string, compact: boolean = false) {
  defaults: {
    Button: { variant: "compact", style: CardButton },
    Text: { style: CardText }
  }
  Panel(variant: compact ? "compact" : "default") {
    Slot(name: "header") { Text(text: title) }
    Slot(name: "content", required: true)
    Slot(name: "footer") { Text(text: "Default footer") }
  }
}

overlay Demo {
  root Card(title: "Account") {
    Slot(name: "content") { Text(text: "Caller-owned content") }
    Slot(name: "footer") { Button(label: "Save") }
  }
}
```

Slot names and occupancy are checked statically. A slot body is its default when the caller omits
that slot; `required: true` rejects omission. Unnamed caller children remain shorthand when the
component has exactly one slot (or a slot named `content`). Filled expressions keep the caller's
scope and stable identity; component parameters and state remain private.

Component templates are values and may bind additional parameters before they are passed to a
framework owner. The remaining signature is checked structurally against the owner's contract:

```strata
component MeterPresentation(
  key: key,
  suffix: string,
  control: progressState
) {
  Text(key: key, text: format("{0}{1}", control.value, suffix))
}

Progress(
  key: "upload.progress",
  value: uploaded,
  presentationTemplate: MeterPresentation(suffix: "%")
)
```

The owner still supplies `key` and `control`; pre-binding an owner-supplied parameter is a compile
error. Bound actions and other executable values retain the caller's lexical scope when the
template is forwarded through another component.

## Styles and theme values

Styles compose left to right. A derived style overrides its bases in declaration order; inline
composition applies named styles first and named overrides last. Unknown bases and cycles are
compile errors. `theme.*` values remain symbolic until the surface resolves its effective theme.
When `clip: true` is set, descendants are clipped to the same authored `radius` as the container;
the rounded clip follows presentation transforms rather than degrading to its axis-aligned bounds.

```strata
style Transparent { background: null; border: null; }
style Spaced { padding: 12; gap: 8; }
style CardStyle extends Transparent, Spaced {
  kind: "COLUMN";
  width: { weight: 1 };
  height: "content";
  radius: 6;
  background: theme.surfaceRaised;
}

Panel(style: style(CardStyle, padding: 8))
```

Conditional style layers use any Boolean state rather than a fixed set of pseudo-states. Layers
whose conditions are false contribute nothing, and later active layers win:

```strata
Panel(style: style(
  Thumb,
  whenStyle(hovered || focusVisible, ThumbHover),
  whenStyle(pressed, ThumbPressed)
))
```

The generated reference separates general style properties, layout properties/size forms, text
properties, theme tokens, and enum values.

### Layer placement and anchors

Children of layered layouts (`PANEL`, `OVERLAY`, and `STACK`) can position either axis against the
parent content box. Numeric positions are logical pixels; `{ fraction: value }` positions are a
fraction of the available axis. `anchorX`/`anchorY` select the corresponding point on the child,
and scalar offsets are applied last:

```strata
Panel(layout: { kind: "PANEL", width: 240, height: 40 }) {
  Panel(layout: {
    width: 8,
    height: 24,
    placement: {
      x: { fraction: progress },
      anchorX: 0.5,
      offsetY: 2
    }
  })
}
```

Percentage sizes use the same `{ fraction: expression }` arithmetic. Fill allocation is
proportional to its authored weight; an explicit `{ weight: 0 }` receives zero remaining space.

`anchorTarget` is also valid on an ordinary child. The child becomes out-of-flow, is measured
against its containing content box, and is arranged after its keyed sibling target, so chained
sibling anchors are deterministic. Ordinary keyed targets must share the same parent and must not
be portals; use `"parent"` for the containing node.

```strata
Panel(key: "account.trigger", layout: { width: 120, height: 28 })
Panel(layout: {
  anchorTarget: "account.trigger",
  anchorSide: "BOTTOM",
  anchorAlign: "END",
  anchorGap: 6
})
```

Missing targets and cyclic chains produce layout diagnostics instead of order-dependent geometry.

### Anchored portals

A portal is measured against the root viewport, excluded from its parent's row/column/grid flow,
and rendered as a detached root. `anchorTarget` names a stable widget key (or `"parent"`/`"root"`).
`anchorPoint: { x, y }` supplies an absolute logical point instead and is used by pointer-positioned
context menus.
Placement is backend-independent:

```strata
Panel(
  layout: {
    kind: "PORTAL",
    portalTarget: "root",
    detachFromParentClip: true,
    anchorTarget: "account.trigger",
    anchorSide: "BOTTOM",
    anchorAlign: "END",
    anchorGap: 6,
    anchorFlip: true,
    anchorShift: true,
    matchAnchorWidth: true
  }
) {
  AccountPopup()
}
```

Sides are `TOP`, `BOTTOM`, `LEFT`, and `RIGHT`; alignment is `START`, `CENTER`, or `END`. Flip
changes the main-axis side when the preferred side cannot fit and the opposite side has more room.
Shift clamps the cross-axis result to the root viewport. Anchored portals participate normally in
retained rendering, hit testing, focus, semantics, clipping, motion, and nested portal placement.

### Exterior shadows

`shadows` adds up to four ordered exterior shadow layers to any styled widget. Each layer is
rendered behind the widget and its backdrop effect, outside the source silhouette rather than
darkening translucent content:

```strata
style FloatingGlass {
  radius: 18;
  shadows: [
    { color: #0000002E, radius: 18, spread: 1, offsetY: 7 },
    { color: #00000018, radius: 5, spread: 0, offsetY: 2 }
  ];
}
```

`radius` is the blur extent; `spread` expands or contracts the shadow silhouette; `offsetX` and
`offsetY` move it in logical pixels. A widget's own clip does not cut off its shadow, while an
ancestor clip still constrains the composed subtree.

## Paints and gradients

`background`, `fill`, `track` and `scrim` take a paint: a colour, or a gradient authored in the
filled shape's normalized space, where `(0,0)` is its top-left corner and `(1,1)` its bottom-right
one. A gradient therefore survives resizing, motion and fragment reuse without being re-authored.

```strata
style Card {
  background: {
    kind: "linear",
    angle: 155,               // 0 points up, angles advance clockwise, resolved against the
    stops: [                  // shape's aspect so the painted angle is the visual one
      { color: #1B2338FA, offset: 0 },
      { color: #141A2BFA, offset: 0.55 },
      { color: #191430FA, offset: 1 }
    ]
  };
}

style Glow {
  background: {
    kind: "radial",
    center: { x: 0.5, y: 0.2 },
    radiusX: 0.9, radiusY: 0.6,   // or one `radius` for both
    extend: "clamp",              // clamp | repeat | mirror
    stops: [ { color: #7CFFDA55 }, { color: #7CFFDA00 } ]
  };
}
```

A linear axis can also be given explicitly as `from`/`to` instead of `angle`. Stops are ordered,
between two and sixteen, and `offset` may be omitted to spread them evenly. Gradients are expanded
into geometry whose vertex colours reproduce the ramp exactly at every stop, so they cost no shader
switch and behave identically on every backend.

## Vector drawing

`Draw` paints authored outlines inside its own bounds, in the same normalized space paints use.
Every shape declares a `fill`, a `stroke`, or both; stroke widths and dash lengths are logical
pixels so they stay honest at any UI scale.

```strata
Draw(layout: { width: { weight: 1 }, height: 58 }, shapes: [
  { kind: "circle", center: { x: 0.5, y: 0.5 }, radius: 0.4, fill: #7CFFDAFF },
  { kind: "arc", center: { x: 0.5, y: 0.5 }, radius: 0.34, start: -90, sweep: 270,
    stroke: #56E1C6FF, strokeStyle: { width: 2, cap: "round" } },
  { kind: "polyline", points: hub.frameHistory,
    stroke: { kind: "linear", angle: 90, stops: [ { color: #56E1C6FF }, { color: #6E8BFFFF } ] },
    strokeStyle: { width: 2, cap: "round", join: "round" } },
  { kind: "path", commands: "M 0.22 0.66 C 0.36 0.30, 0.64 0.86, 0.78 0.42",
    stroke: #E8F1FFEE, strokeStyle: { width: 3, cap: "round", dash: [6, 4] } }
])
```

Shape kinds are `line`, `polyline`, `polygon`, `rect`, `circle`, `ellipse`, `arc` and `path`. A
`path` outline uses the compact `M`/`L`/`H`/`V`/`Q`/`C`/`Z` commands, in absolute or relative
(lowercase) spelling, and is parsed at compile time: a malformed outline is a compile error on the
literal, not a shape missing at runtime. `strokeStyle` carries `width`, `cap` (`butt`/`round`/
`square`), `join` (`miter`/`round`/`bevel`), `miterLimit`, `dash` and `dashOffset`. Fills and
strokes are antialiased by the tessellator, and a `points` list may come straight from host data.

## Authored materials

An application declares its own materials in its `*.schemas.json`, alongside its widgets and
actions:

```json
"materials": {
  "definitions": [
    {
      "id": "demo:aurora",
      "blendMode": "straight_alpha",
      "fallback": "strata:rounded_rect",
      "parameters": [
        { "name": "intensity", "materialType": "FLOAT", "type": { "kind": "number" },
          "required": true, "nullable": false, "aliases": [], "default": null },
        { "name": "tint", "materialType": "COLOR", "type": { "kind": "color" },
          "required": true, "nullable": false, "aliases": [], "default": null }
      ],
      "shaders": { "hlsl": "assets/strata/shaders/materials/aurora.hlsl" }
    }
  ]
}
```

`Panel(material: material("demo:aurora", intensity: 0.85, tint: #7C6BFFFF))` then shades that
widget. Parameters are packed into the vertex draw data in declaration order, occupying floats 8
to 13: floats 0 to 7 carry the drawn shape's size, softness, border width and corner radii, and the
last two carry the draw mode and material opacity, leaving six floats for authored parameters.

An authored source writes one function and nothing else:

```hlsl
float4 material(PixelInput input) {
    return float4(input.color.rgb, input.color.a * materialCoverage(input) * materialOpacity(input));
}
```

A material shades **fills only**. Text, images, borders and shadows drawn inside its scope keep the
built-in shading, so applying a material to a panel never disturbs the labels inside it. The
surrounding prelude supplies `materialSize`, `materialOpacity`, `materialTime`, `materialRadii`,
`materialDistance`, `materialCoverage`, `materialFloat`, `materialFloat2` and `materialFloat4` —
`materialDistance` and `materialCoverage` give the shape's own rounded silhouette, so a material
never restates the geometry it was applied to, and `materialTime` returns seconds since the surface
began presenting, for materials that animate.

The declaration is backend-neutral: a host compiles the source key it implements and reports the
rest as approximations. The Win32 desktop and headless D3D11 hosts implement `hlsl`; a backend that
lacks the declared source draws the solid approximation and reports
`STRATA.RENDER2D.MATERIAL_APPROXIMATED` once per material.

## Authored render effects

Materials shade one fill. Effects filter a framebuffer input and may therefore implement glass,
refraction, color grading, or subtree post-processing. Effects are typed ordered programs in the
same application schema:

```json
"effects": {
  "definitions": [{
    "id": "demo:optic-glass",
    "input": "BACKDROP",
    "parameters": [
      { "name": "blurRadius", "effectType": "FLOAT", "type": { "kind": "number" },
        "required": true, "nullable": false, "aliases": [], "default": null },
      { "name": "tint", "effectType": "COLOR", "type": { "kind": "color" },
        "required": true, "nullable": false, "aliases": [], "default": null }
    ],
    "passes": [
      { "kind": "BLUR", "radiusParameter": "blurRadius", "downsample": 2 },
      { "kind": "SHADER",
        "shaders": { "hlsl": "assets/shaders/effects/optic-glass.hlsl" } }
    ]
  }]
}
```

`input` is `BACKDROP` (pixels already behind the widget) or `CONTENT` (an isolated rendering of the
widget subtree). Passes execute in declaration order and are `BLUR` or `SHADER`. Blur accepts
literal `radius`/`downsample` values or parameter references. Parameters occupy at most sixteen
floats in declaration order; `FLOAT`, `INT`, `FLOAT2`, `FLOAT4`, and `COLOR` use one, one, two, four,
and four slots respectively. Missing, unknown, duplicate, and wrongly typed arguments are compile
errors.

Apply the program through the ordinary widget/style effect field:

```strata
Panel(effect: effect("demo:optic-glass", blurRadius: 18, tint: #7DB8FF35))
```

Live effects default to a maximum refresh rate of 240 Hz. The renderer continues composing every
application frame while reusing the latest filtered sample between refreshes; animation time is
absolute, so a sampled shader does not run in slow motion. `refreshRate` is a generic effect
argument rather than a declared shader parameter:

```strata
Panel(effect: effect("demo:optic-glass", refreshRate: 120, blurRadius: 18, tint: #7DB8FF35))
Panel(effect: effect("demo:optic-glass", refreshRate: "UNBOUNDED", blurRadius: 18, tint: #7DB8FF35))
```

Bounds, scale, parameters, geometry epochs, and target changes invalidate the retained sample
immediately. Numeric rates must be positive; `"UNBOUNDED"` requests evaluation on every frame.

An HLSL pass defines `float4 effect(EffectInput input)`. The host prelude provides
`sampleEffectSource`, `sampleEffectBackdrop`, `effectFloat`, `effectFloat2`, `effectFloat4`,
`effectColor`, `effectTime`, `effectOpacity`, `effectDistance`, and `effectMask`. `EffectInput`
carries framebuffer `uv`/`pixel`,
logical pixels, and `localUv` within the affected widget. Samples are straight-alpha values; the
host premultiplies pass output, applies the rounded widget mask and effect opacity once at final
composition, and clips work to the intersection of the effect bounds and inherited scissor.

The D3D11 desktop and headless hosts execute the full pass program. The reference software backend
executes declared blur passes, ignores authored shader stages, and then applies the same rounded
mask, opacity, and backdrop/content composition. This approximation is intentionally deterministic
rather than a claim of shader fidelity. Packet v9 carries ordered backdrop/content batches, active
rounded-clip geometry, effect refresh-rate policy, and a bounded sixteen-float parameter block.
The public decoder rejects malformed clip/effect state, caps nested `CONTENT` effects at four
levels, and caps rounded clip stacks at sixteen.

## Local retained and derived state

State belongs to the keyed component instance that declares it.

```strata
state count = 0;
state enabled = true;
state tags: List<string> = ["account", "privacy"];
state record: Record = { title: "Ready", saved: false };
derived tagSummary = join(tags, " · ");

Button(label: format("Count {0}", count), onClick: action("state.adjust", name: "count", amount: 1))
Toggle(checked: enabled, onChange: action("state.setFromEvent", name: "enabled"))
Button(label: "Advanced", onClick: action("state.listToggle", name: "tags", value: "advanced"))
Button(label: "Saved", onClick: action("state.recordSet", name: "record", field: "saved", value: true))
```

`state.set`, `state.setFromEvent`, `state.adjust`, `state.toggle`, and `state.reset` are compiler-
known actions. Lists additionally support append, insert, remove by value/index, toggle membership,
and clear; records support checked field updates. List element types and a `Record` initializer's
fixed fields are validated at compile time. Derived declarations are pure, evaluated once per
rebuild, and rejected when their dependency graph cycles. Their exact action payload fields and
policies are generated from `DslActionRegistry`.

## Conditions and bounded loops

```strata
if env.compact {
  CompactHeader()
} else {
  WideHeader()
}

for item, index in projects.rows where item.visible {
  Button(key: item.key, label: format("{0}. {1}", index + 1, item.name))
}

when mode {
  "compact" -> { CompactHeader() }
  "wide" -> { WideHeader() }
  else -> { DefaultHeader() }
}
```

Conditions must be boolean. Host collections declare a maximum item count; unbounded or oversized
loops are rejected. A loop may bind its zero-based index and apply a boolean `where` filter. Dynamic
children should use stable host-provided keys. Multi-branch `when` requires exactly one final
`else`, so variant selection stays exhaustive.

Large or variable-height results use a keyed `Repeater` whose body contains exactly one `for`.
Unlike an eager loop, the repeater accepts a bounded collection with thousands of items and builds
only the visible range plus overscan. Every record must expose a stable `key` or `id`.

## Expressions and bindings

Expressions include property/index access, function calls, unary/binary operators, conditional
`?:`, null fallback `??`, lists, and objects. Bindings resolve in this order: local state,
component parameters, declarations, and registered host roots such as `env.*` or application data.

Common pure helpers cover formatting, case conversion, trimming, number clamp/round/precision,
list size/join, numeric/color operations, actions, effects, materials, and animations. Helper
capability and argument errors are source-ranged compiler diagnostics.

Collection helpers accept bounded pure lambdas such as `item -> item.enabled`. View helpers are
`filter`, `map`, `sortBy`, `distinctBy`, `groupBy`, `flatten`, `takeWhile`, `window`, and `page`;
terminal helpers are `count`, `any`, and `all`. A derived view exposes `items`, `total`, `matched`,
`rangeStart`, `rangeEnd`, `cacheHits`, and `rebuilds`.

Catalog-declared controls accept a typed binding shorthand, for example
`TextBox(bind: query)` or `Toggle(bind: enabled)`. The compiler lowers it to the widget's canonical
controlled property and change callback and rejects conflicts when either is also written.

## Actions

```strata
Button(
  label: "Save",
  onClick: action("application.save", document: current.id, force: false)
)
```

The action ID must be a static registered string. Payload fields are named and checked against the
same typed contract dispatched by the host. See [Actions](actions.md) for the native host boundary.

UI-local actions compose without a host callback: `sequence(first, ...)`, `parallel(first, ...)`,
and `chooseAction(condition, whenTrue, whenFalse)`. `state.setFromEvent` adapts the canonical event
value into local state. `reveal.request` is resolved after layout, so a sequence can change a filter
and then reveal/focus a newly materialized stable key. Domain effects remain typed host actions.

## Animations and timelines

```strata
animation ContentMotion {
  duration: 180ms;
  from { opacity: 0; translateY: 8; }
  to { opacity: 1; translateY: 0; }
}

animation FocusMotion {
  duration: 90ms;
  from { scale: 1; }
  to { scale: 1.03; }
}

Panel(transition: ContentMotion, move: ContentMotion, stagger: 24ms)
Panel(motions: [
  { id: "expanded", target: expanded, animation: ContentMotion },
  { id: "focus", interaction: "FOCUS_VISIBLE", animation: FocusMotion },
  { id: "radius", property: "radius", target: expanded ? 12 : 4, policy: "fast" },
  { id: "surface", property: "background", target: expanded ? #345A8AFF : #1F2937FF, policy: "standard" }
])
Panel(animateChanges: { properties: ["background", "radius", "opacity", "translateY"], policy: "standard" })
Panel(animateContentSize: { width: false, height: true, clip: true, policy: "standard" })
Panel(disclosure: { expanded: expanded, collapsedExtent: 32, policy: "standard" })
Panel(contentKey: selectedId, contentTransition: ContentMotion) { SelectedDetails() }
Button(onClick: action("layer.push", name: "details", transition: ContentMotion))
```

Animation timing properties, triggers, and animatable values are catalog-derived in the generated
reference. `transition` plays one timeline forward on insertion and backward on removal, reversing
continuously from its current position when visibility changes mid-flight. Use separate `enter` and
`exit` timelines only for deliberately asymmetric motion; mixing either with `transition` is a
compile error. Exit motion retains disappearing nodes until its finite timeline completes;
keyed `move` uses layout deltas, and `stagger` adds a bounded per-loop delay. Layer push/pop/replace
and modal transitions use the named animation timing. Existing focus/state restoration is unchanged
across transitions. Reduced-motion policy is supplied through `env.reducedMotion` and snaps these
presentation transitions to their final state.

`motions` separates activation from reusable presentation timing. Each attachment has a stable
literal `id`, one named animation, and exactly one activation source: a Boolean `target` expression
or the `HOVER`, `PRESSED`, `FOCUS`, or `FOCUS_VISIBLE` framework interaction. `FOCUS` is
semantic focus and remains active after pointer acquisition; `FOCUS_VISIBLE` follows the
keyboard/spatial indicator policy and is normally the right source for visual focus treatment.
Boolean targets reverse continuously
from the currently displayed value when interrupted. Initial composition and reduced-motion mode
snap to the target endpoint without starting a clock. Independent channels may animate disjoint
properties; assigning one property to multiple channels, or embedding a legacy `trigger` in a
channel timeline, is a compile error rather than an order-dependent override.

A `motions` entry with `property` and a numeric or color `target` is a typed value channel; it does
not require duplicated endpoints. `animateChanges` is the opt-in implicit path for concrete
resolved style/layout values and retargets from the current presentation after rapid state, theme,
or hot-reload changes. Fixed, percentage, and fill-unit width/height targets interpolate without
being flattened to pixels; `placementX`/`placementY` animate the corresponding layer-placement
axis. Per-edge `marginLeft`/`marginTop`/`marginRight`/`marginBottom` and padding properties resolve
from the authored edge object. Unit changes and unsupported non-concrete endpoints snap rather
than passing through dimensionally invalid intermediate values.

`animateContentSize` follows measured child geometry on content/auto axes. `disclosure` adds the
expanded target, collapsed extent, clipping, and immediate descendant input/focus exclusion.
`contentKey` plus `contentTransition` retains the outgoing keyed child while the incoming child and
container size coordinate. Replacement defaults to `contentTransitionMode: "OUT_IN"`, which uses
the authored duration as one sequence and prevents outgoing/incoming text from sharing a baseline.
Use `"TOGETHER"` explicitly for image/surface crossfades where simultaneous presentation is wanted.
Fixed/fill/percentage axes remain owned by layout and cannot be silently
overridden by content-size motion. All named `policy` values resolve through the effective theme's
`UiMotionPolicy`; reduced motion reaches the same final style and layout in the same frame.

During hot reload, an already-running single-run keyframed timeline is compatible when it still
owns the same animation properties and keeps the same reverse model. Duration, delay, easing, fill mode,
keyframe offsets, and keyframe values may change compatibly: the runtime preserves the exact
currently displayed numeric/color values and blends them into the edited timeline. Changing the
owned property set, reverse mode, or repeating timeline is structural and restarts it from its new
initial frame. Target-value, resolved-style, and measured-size channels always retarget from their
current presentation value because their stable channel identity already defines compatibility.

## Keys

Keys identify retained nodes across rebuilds. Use keys for dynamic/reordered children, focus
targets, forms, layers, and controlled state. Static leaves normally do not need keys. Duplicate
explicit sibling keys are invalid.

## Validation from the command line

Run the standalone production compiler without launching a window or game:

Validate all bundled applications through the configured CMake build:

```sh
cmake --build --preset linux-x64 --target strata_validate_modules
```

Use `windows-x64` on Windows. For one custom module, invoke the installed compiler directly:

```sh
strata_compile --check-module path/to/module.strata path/to/module.schemas.json
```

Prefix repeatable `--extension-path <directory>` options when the schema selects external native
packages. Use `--check-module-json` for the versioned `strata.diagnostics` document consumed by
editors. Built-in declarations come directly from the compiler's immutable native catalog; the
schema argument is optional and supplies only application declarations. Command-line diagnostics include the diagnostic code,
`file:line:column`, and message. Runtime diagnostic callbacks additionally carry the full source
range, component path, expected value, and occurrence metadata.
