# Headless application testing

`strata_headless` is a deterministic host for complete `.strata` applications. It is not a second
UI runtime and it contains no widget-specific test hooks. The tool uses the public C ABI to compile
and activate an application, create a normal `Surface`, enqueue ordinary input, frame it against a
caller-owned clock, and consume render packet v9.

Each capture contains both views needed by automated testing:

- `<name>.json` is the canonical Surface frame snapshot: retained inspection, semantics, state,
  focus/layers, events, action outcomes, diagnostics, render commands, and operation counters.
- `<name>.png` is produced by the selected render backend. The portable CPU reference backend runs
  on Linux and Windows. Windows can instead use a windowless D3D11/WARP target sharing the desktop
  texture store, glyph atlases, text, clipping, blending, blur, and authored-HLSL
  material/backdrop/content-effect pipelines.
- `result.json` summarizes the selected backend, every frame, captured host action/effect, async
  request, diagnostic, capture name, and any material that genuinely lacked a backend source.

This makes the system useful for visual regression, interaction tests, conformance investigation,
and automated tooling without putting an automation-only interface into the core.

## Running a scenario

After building the Windows preset, the production-renderer showcase scenario is:

```bat
build\cmake\windows-x64\native\RelWithDebInfo\strata_headless.exe ^
  --resources src\main\resources ^
  --scenario native\tests\fixtures\headless\showcase.json ^
  --output build\headless\showcase
```

On Linux, run the portable reference scenario:

```sh
build/cmake/linux-x64/native/strata_headless \
  --resources src/main/resources \
  --scenario native/tests/fixtures/headless/portable.json \
  --output build/headless/portable
```

CTest registers the portable batch/interactive scenarios as `strata.headless.portable` and
`strata.headless.portable.interactive`. Windows additionally registers
`strata.headless.d3d11` and `strata.headless.d3d11.interactive`, which verify authored shader
compilation and the shared production renderer. `strata.headless.effects` renders the same authored
optic-glass/content-effect fixture through D3D11 and twice through the software approximation; it
requires D3D shader output to differ from the approximation and both software images to be
byte-for-byte deterministic.

## Persistent interactive control

Pass `--interactive` to start the application once and control the live Surface through
newline-delimited JSON on standard input and output:

```bat
build\cmake\windows-x64\native\RelWithDebInfo\strata_headless.exe ^
  --resources src\main\resources ^
  --scenario native\tests\fixtures\headless\showcase.json ^
  --output build\headless\interactive ^
  --interactive
```

Interactive mode uses the scenario's application, surface, resources, extensions, and initial host
snapshots, but does not pre-execute its batch `steps`. It emits a `ready` response and then keeps that
application and all of its runtime/UI state alive until `close` or input EOF. Each request is one
JSON object with one operation and an optional correlation `id`:

```json
{"id":"data","click":{"role":"tab","name":"DATA"}}
{"id":"folder","click":{"key":"data.tree.folder.0"}}
{"id":"settle","advance":{"milliseconds":300,"frames":1}}
{"id":"look","inspect":{}}
{"id":"done","close":{}}
```

Every response is exactly one compact JSON line followed by a flush. Successful responses include:

- the request `id`, monotonically increasing `sequence`, current frame counters, viewport, and
  backend;
- cumulative host actions, effects, async requests, diagnostics, and material fallbacks;
- a flattened `elements` view of the current semantic tree, including roles, names, keys, state,
  supported actions, structural paths, visibility, and clipped `hitBounds`;
- `current.png` and `current.json` artifact paths, rewritten to the current frame.

The browser view joins semantics with the framework's exact retained and presenter-owned subtarget
geometry. Consequently virtual controls such as individual tabs and menu entries are discoverable
and selector-clickable even though they are painted by one retained widget. Generated tree rows
retain their stable item keys. A tool can therefore discover `DATA`, click it, inspect the resulting
screen, discover `Folder 0`, and click that folder without reading the `.strata` source or guessing
coordinates. Selector clicks still enqueue ordinary pointer move/press/release events.

A failed request returns `{"ok":false,"event":"error",...}` without terminating the session, so a
controller can inspect again and recover. The platform-specific interactive CTest scenarios verify
that one persistent process can navigate, select generated content, mutate the page, and expose the
retained result.

## Launch/scenario document

The strict JSON document describes the application, Surface, resources, and initial host snapshots.
Its `steps` array is optional: batch runs execute it, while interactive runs intentionally start from
the initialized application and accept operations from the control stream instead.

```json
{
  "version": 1,
  "application": {
    "id": "example.headless",
    "module": "assets/example/app.strata",
    "schemas": "assets/example/app.schemas.json",
    "root": "ExampleRoot",
    "packages": [],
    "extensionPaths": [],
    "actions": ["example.save"]
  },
  "surface": {
    "id": "example.headless",
    "role": "overlay",
    "backend": "d3d11",
    "width": 1280,
    "height": 800,
    "scale": 1,
    "reducedMotion": false,
    "clearColor": "#090b0fff",
    "fonts": [
      {"id": "example:default", "resource": "assets/example/default.ttf"}
    ],
    "images": [
      {
        "id": "example:icon",
        "resource": "assets/example/icon.svg"
      },
      {
        "id": "example:photo",
        "resource": "assets/example/photo.png",
        "sampling": "linear"
      }
    ]
  },
  "snapshots": [
    {"id": "example.headless", "values": {"example": {"status": "ready"}}}
  ],
  "steps": [
    {"advance": {"milliseconds": 0, "frames": 1}},
    {"advance": {"milliseconds": 300, "frames": 1}},
    {"capture": "initial"},
    {"click": {"key": "example.save"}},
    {"key": {"name": "tab", "modifiers": ["shift"]}},
    {"text": "hello"},
    {"capture": "edited"}
  ]
}
```

Resource and module paths are relative to `--resources`; absolute and root-escaping paths are
rejected. `surface.images` accepts encoded PNGs and the bounded static SVG subset documented in
[PNG and SVG images](svg.md). Raster `sampling` is ignored for SVG geometry. Imported modules resolve relative to their importer through the shared host module-path
resolver used by the native hosts.

`application.actions` installs a generic recording handler for each declared host action. Framework
state actions still execute in the core. Activation validates the complete candidate module, before
a Surface root is selected, so this list must cover every required or forwarded host action
referenced by any screen, overlay, or component in that module—not only the scenario's `root`.
Activation failures print every captured diagnostic code/message (including all missing action
handlers) before the process exits. `extensionPaths` are absolute or relative to the scenario
document. Selected package shared libraries export both their schemas and runtime bundles, so
compiler and Surface capabilities cannot drift; the headless host keeps them loaded through Surface
release.

## Steps and selectors

Supported steps are:

| Step | Behavior |
| --- | --- |
| `advance` | Advances the deterministic clock by `milliseconds`, distributed over `frames`, framing each sample. |
| `capture` | Writes the current PNG and canonical frame JSON; creates the first frame if necessary. |
| `click` | Resolves a target, then frames pointer move, press, and release through the normal input router. An optional `button` selects `primary`/`left`, `secondary`/`right`, or `middle`. |
| `move` | Moves the fine pointer to a target. |
| `scroll` | Sends `deltaX`/`deltaY` at a target. |
| `key` | Frames a key press and release. Modifiers are `shift`, `control`, `alt`, and `super`. |
| `text` | Sends committed UTF-8 text to the focused editor. |
| `resize` | Adopts a new logical `width`, `height`, and `scale`, then frames it. |
| `publish` | Publishes a new typed host snapshot and frames the resulting update. |

A pointer target can use logical coordinates:

```json
{"x": 240, "y": 80}
```

or retained/semantic selection:

```json
{"key": "settings.save"}
{"role": "button", "name": "Save"}
{"path": "/0/0/0/0/1/2000007"}
```

`path`, `key`, `role`, and `name` may be combined. `path` is especially useful for selecting a
specific element returned by an interactive response. Selection must resolve to exactly one current
semantic element; an ambiguous selector fails with candidate details instead of silently clicking
an arbitrary match. The click point is the center of its visible, clip-correct `hitBounds`. For a
virtual semantic element those bounds come from the same presenter-owned subtarget geometry used by
ordinary hit testing.

Secondary-click a context-menu owner with the same selector syntax:

```json
{"click": {"key": "settings.row", "button": "right"}}
```

Collapsed menu descendants remain in the logical semantic tree, but have no current subtarget
geometry or actions; interactive inspection therefore reports them as `visible: false` and
`actionable: false`, and selector clicks reject them until their popup level is open.

## Render backends and fidelity boundary

`surface.backend` is `d3d11` by default on Windows and `reference` on Linux. D3D11 creates no window
or swap chain: a WARP
software device renders into an RGBA texture, using the shared production D3D11 pipeline, and the
host reads that texture back for PNG encoding. Application HLSL is loaded from its ordinary material
declarations and compiled exactly as it is for desktop. The deterministic scenario clock is also
passed to authored material time.

Set `surface.backend` to `reference` for portable packet/geometry tests. That backend is the explicit
CPU implementation of built-in unified materials. Because arbitrary HLSL has no portable CPU
meaning, it uses the built-in shape fallback and records the material id in
`result.json.materialFallbacks`; it is not the visual-regression default.

The offscreen backend establishes renderer and shader output without pretending to replace physical
clipboard/IME integration or final hardware/driver acceptance.
