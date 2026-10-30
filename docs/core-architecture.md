# Proportional frames: Strata's performance-first core

Status: design, not implemented. Supersedes nothing yet; each phase below lands behind the
benchmark gates in §6 and is measured against them before the next starts.

## 1. The rule

**A frame costs what changed, not what exists.** Nothing changed: no frame. A node moved: one
transform. A colour changed: one node's paint. A list item was added: that item. The size of the
scene never appears in the cost of a change that does not touch all of it.

Strata breaks the rule today. Every change enters the pipeline at its top and flows through every
stage, and each stage keeps its cost down by checking what it can skip. Checking is not free and
does not compose: nine stages each walking the scene to learn that nothing happened is the
milliseconds this document removes, and the stage that cannot skip (a zoom changes every glyph's
raster scale) turns into a stall.

### Measured today

Blueprint tab, 1500×950, D3D11, uncapped, RelWithDebInfo at `973d406`:

| Interaction | Core CPU per frame | What it needs |
|---|---|---|
| Idle | 0.32 ms (a frame every vsync) | no frame |
| Hover sweep | 0.11 ms | repaint the hovered node |
| Right-drag pan | 0.74 ms | one view transform |
| Node drag | 2.0 ms (p95 2.8) | one node transform, its wires |
| Wheel zoom | 5.65 ms (p95 11.2, ~9 ms of it glyph rasterization) | one view transform |

### Status

| Phase | State | What landed |
|---|---|---|
| 0 Gates | done | Per-frame work counters in the ABI frame info and the desktop report; `strata_benchmark_blueprint`; a `settle` scenario step |
| 1 Frames on demand, raster off the frame | done | `idle`/`unchanged` per frame; the showcase host skips drawing unchanged frames unless a live material is shown; persistent raster pool; per-frame rescale budget |
| 2 Layers | in part | Presentation-only frames update group table entries and keep the stream; a zooming group keeps its raster density until it settles; ZoomView reports its view at rest. Not yet: per-layer display lists, instanced drawing, delta packets |
| 3 Canvas, layout, hit testing | in part | Hit entries kept relative to their presentation group and placed again when only groups moved. Not yet: parent-relative layout records, a Canvas primitive with culling and level of detail |
| 4 Bindings and path state | not started | |
| 5 Compositor thread | not started | |
| 6 Interpreter | in part | In-place item functions; hashed, ring-kept, self-limiting memoization of every function |

Blueprint, core CPU per frame, at design time and now:

| Interaction | Design time | Now |
|---|---|---|
| Idle | 0.32 ms | 0.02 ms, no work |
| Pan | 0.93 ms | 0.18 ms |
| Wheel zoom | 5.65 ms (p95 11.2) | 0.38 ms (p95 2.2) |
| Node drag | 2.0 ms | 1.48 ms |

A node drag is now split between describing the page (about 40%: the page body and its
`wireShapes` script, which recomputes every wire when any node moves), the drag handler, and the
render, submission and packet stages (about 30%, the part that still grows with scene size).

## 2. The pipeline today

Everything below runs on the host loop thread, in order, every frame something changed.

### Frame phases

`Surface::frame` (`ui/surface.cpp:1691`) fills a `FrameDirtyModel` and takes one of three paths.
A settled frame with no input does almost nothing (good). An input-only frame drains one batch.
Anything else runs the whole pipeline: scroll animations, build, layout, up to four input rounds,
post-layout, semantics, render. Only layout is gated, and coarsely (`retained || motion ||
layout`); semantics and render always run, and several steps walk the whole tree whatever
changed:

- `prune_exiting` walks the tree on the first motion sample of every frame.
- The motion runtime sweeps a `configs` map that holds effectively every node.
- When the layout engine runs at all, `record_unarranged` and `retain_exit_layout` walk the tree
  and three caches are swept by `erase_if`.
- Pointer geometry (`ui/input/hit_test.cpp:734`) is a flat vector of every node, rebuilt in two
  full walks whenever the tree, layout, notification or command generation changes or any of
  eight global dirty lanes moves; hit testing scans it linearly. There is no spatial index.
- `set_retained_tree` copies the whole tree into a description snapshot after any retained-value
  write (a scroll, a drag offset, a tooltip).

`WorkScheduler` (`ui/scheduler.hpp`) was built so a settled tick costs O(1), but `Surface` does
not use it.

### Build

A state write marks the instances that read the state; each re-runs its body, and since
`973d406` skips calls and functions whose recorded reads compare equal. Describe feeds
`share_unchanged_description`, theme projection and
`reconcile` (`ui/tree_reconcile.cpp:171`). Reconcile and theming are incremental: unchanged
description pointers stop the walk, and materialized themes are cached per description node. But
any created, removed or moved node re-indexes the whole tree into `std::map`s, any theme catalog
change re-themes everything, and a Button with a `presentationTemplate` writes
`$presentationState` on hover, which invalidates the description: hovering a button re-describes
the app.

Dirty marks are per node and per lane (eleven lanes), but tree-wide consumers (pointer geometry,
semantics, hover refresh) compare global per-lane counters, so one node's change invalidates their
whole-tree products.

### Layout

Measurement is incremental and good: dirty nodes are re-measured bottom-up, unchanged sizes stop
propagation, and text measurement is cached by content and style. Arrangement is not: records hold
absolute surface coordinates, so moving a node copies and offsets every record in its subtree, and
**scrolling copies every record in the scrolled content** each frame. A position set through a
layout property (a canvas child's `inset`) re-measures the child and its parent before
re-arranging.

### Viewports and motion

- **ZoomView** pan and zoom are already render-time transforms: `strata.content.view` is applied
  through `parent_content_view` without layout. What makes them expensive is downstream (the
  submission's full re-plan and rasterization) and sideways: each change rebuilds pointer
  geometry, bumps the semantics generation, and raises `onViewChange`, which re-describes an app
  that binds the view.
- **Motion** applies transform and opacity at render time without layout, but a running animation
  keeps the frame unsettled and pays every whole-tree step above.
- **Drag** offsets are render-time translates written as input-lane marks.
- **Scroll** offsets are arrangement values: the layout cost above, plus a re-index when a virtual
  list's window changes.

### Render: the command stream

`RenderEngine::render` (`ui/render.cpp:344`) clears its `RenderCommandBuffer` and appends the whole
frame again: retained per-node fragments (`CachedFragment`, `ui/render/cache.hpp:54`) are copied
in, never patched in place. Only a frame with no dirty node, no running motion and unchanged
generations shares the previous buffer. A uniform layout translation reuses a fragment wrapped in
a `TransformPush`, but the stream around it is still rebuilt, and `publish_groups` walks all of it
to find which presentation groups are referenced. A drag offset (`strata.movement.offset`) is an
input-reason dirty mark, which rebuilds the dragged node's fragment.

### Submission: compare, then plan

`RenderSubmissionCache::resolve` (`ui/render/submission_cache.cpp:29`) compares the new stream with
the last one command by command. When they differ but keep the same shape, `update_changed_draws`
re-plans the changed draws, but first recomputes four whole-stream vectors (visible text, text
group scales, subpixel runs, zoomed runs). A changed batch key (scissor, rounded clip, texture,
material) or a changed group raster scale falls back to a full plan: every command through the
planner, glyph warm-up, mesh encode into 88-byte vertices, and a byte diff of the whole vertex and
index arrays. Grouped text is never visibility-culled, so a zoom step prepares every text run in a
canvas at the new scale, and since `686905e` it rasterizes every glyph at the exact scale every
frame. Path masks are keyed to a 1/16-pixel phase, so fractional moves create new masks.

### Packet and host

Each packet carries the full group table and re-serializes every batch; on a submission change it
also carries geometry patches or the full payload (`ui/render/packet_native.cpp:393`). The host
copies the packet, re-parses every batch, recomputes group extents over every vertex, and applies
patches to a CPU mirror (`host/render_packet.cpp:863`). D3D11 patches its vertex buffer in place;
Vulkan copies the whole geometry into each frame slot whenever the epoch changes. Then the host
clears and redraws every batch and re-evaluates every effect that is not rate-limited, blurs and
glass included, and it does so every loop: the desktop host renders continuously
(`desktop/main.cpp:803`), settled or not.

Presentation groups (`89e90ac`) are the one part shaped like the target: content encoded once in
layout space, every vertex tagged with a group index, a 512-entry transform table in a constant
buffer. But a node is a group only while a motion player runs (or as a direct child of a content
canvas), the table is rebuilt from a stream walk, and outside groups transforms are baked into
vertices.

### Where the cost scales with the scene

| Stage | Scales with |
|---|---|
| Command stream rebuild and copy, `publish_groups` | all commands |
| Submission equality check, incremental diff, four whole-stream scans | all commands |
| Full plan on any scale step or shape change | all commands, all vertices |
| Packet batch serialization, host re-parse, group extents | all batches, all vertices |
| Vulkan geometry copy per frame slot | all vertices |
| GPU redraw and effect evaluation, every loop | all batches, all effect area |

## 3. Change tiers

Every change is classified where it is made, and enters the pipeline at the lowest stage that
can represent it. Nothing above that stage runs.

| Tier | Changes | Work | Never touches |
|---|---|---|---|
| **T0 Compositor** | transform, opacity, clip offset, scroll offset, a viewport's pan and zoom, a child's position in a canvas | one layer property; the GPU redraws retained geometry | description, layout, recording, rasterization |
| **T1 Paint** | colour, border, material parameter, text colour, a drawn shape | re-record one layer's display list | description, layout |
| **T2 Layout** | a size-affecting property, text content | relayout the dirty subtree up to its layout boundary, then re-record what moved | description of anything but the changed binding |
| **T3 Structure** | a node appears, disappears or changes type; a component's inputs change shape | describe that instance | instances whose inputs did not change |

A change's tier is a property of *what it writes*, not of who writes it: a script that sets
`node.x` on a canvas child makes a T0 change exactly as an animation of `transform` does.

## 4. Target architecture

### 4.1 State: identity, paths, push

Today a state is one immutable value per address. `nodes = [...]` replaces the list, and every
reader of `nodes` is suspected and re-run, then compared.

- **Keyed collections.** A list of records with an identity (`id` or a declared key) is stored as a
  keyed collection. `nodes[n2].x = 640` is a path write `(nodes, #n2, x)`, not a replacement.
  Script assignment already rebuilds the value along a path; it reports the path instead of the
  root.
- **Path dependencies.** Reads are already recorded per field (`LexicalRead{name, path}`, commit
  `973d406`). They extend through element identity: `nodes[#n2].x`. A write notifies exactly the
  readers of the paths it changed.
- **Push invalidation.** A write marks its readers; nothing is re-run to find out whether it
  changed. Derived values and memoized functions become nodes of one dependency graph, marked on
  write and recomputed on read (push-pull), so an unobserved derivation costs nothing.

### 4.2 Bindings, not rebuilds

A widget property whose value is an expression is a **binding**: the expression, the reads it made,
and the retained node and property it feeds. Describing a body creates bindings; a later change to
what a binding read re-evaluates that binding alone and writes the result into the retained node
with the property's tier:

- a T0 property (`layout.inset` of a canvas child, `transform`, `opacity`) updates a layer,
- a T1 property re-records the node's paint,
- a T2 property dirties layout from the node.

The body is described again only when a change reaches structure: a loop's item set, a
conditional's branch, a component's input that is itself a template. This is the step from
"describe again and compare" to "update what was bound", and it removes describe from every
interaction in the table in §1.

### 4.3 Layers

The retained tree is partitioned into **layers**. A layer is a subtree that moves, fades or clips
as a whole: the content of a scroll container or viewport, a canvas child, anything with a
transform or opacity animation, anything an effect samples. Layers form a tree.

Each layer owns:

- a **display list** recorded in layer-local coordinates (draws, text runs, effect regions),
- its **GPU geometry**: a range in a persistent instance arena, uploaded when the display list
  changes and not otherwise,
- its **properties**: transform, opacity, clip, scroll offset, as an entry in a layer buffer the
  vertex stage indexes, as it indexes the group table today.

A frame records the layers whose display lists changed, updates the properties that changed, and
draws the layer list. Moving a canvas child changes one transform; zooming a viewport changes one;
neither re-records or re-uploads anything.

**Instances, not fat vertices.** Today every vertex is 88 bytes (position, colour and sixteen
material floats), four per rectangle plus indices, with transforms outside groups baked in on the
CPU. Display lists instead hold one compact instance per primitive (a rect, a rounded rect, a
glyph, a stroke segment run) carrying its local bounds, paint and the index of its layer, expanded
to quads in the vertex shader. A rectangle shrinks from ~350 bytes to ~48; re-recording a layer
writes a fraction of the bytes; and since nothing is baked, nothing is rewritten when it moves.

### 4.4 Packets carry changes

Today's packet (v23) carries the whole group table, every batch and, on any submission change,
geometry patches or the full payload; the host re-parses every batch and recomputes group extents
over all vertices. The next version is a delta stream against retained host state: *layer
created/released*, *layer properties*, *instance range replaced*, *atlas operations*, *texture
uploads*, *draw order changed*. A frame in which one layer moved is a few dozen bytes. The host
keeps the arena and the layer table; the D3D11, Vulkan and reference backends all decode the same
stream, so headless captures keep testing the production path.

The packet stays a byte stream because it is the C ABI boundary and what headless replays and
records. In-process hosts (the desktop host, the showcase) stop paying for it twice: the stream is
decoded into the host's layer table without the copy `SurfacePacketStream::frame` makes today.

### 4.5 Rasterization off the frame

**Text is not rasterized.** A glyph with a TrueType outline is flattened once into straight
segments in font units, its overlapping contours merged into one boundary and its segments grouped
into horizontal bands (`font::build_vector_glyph`). The segments reach the GPU in per-Surface
buffers that only grow (packet resource 5). Every frame the pixel stage measures each pixel's
exact coverage from them, through the derivatives of the glyph position (draw modes 13 gray, 14
per subpixel; `host::vector_text_coverage` is the CPU twin). A zoom, a turn or a fractional
position changes only the vertex transform: text is exact in every frame, with no raster scale,
no settle and nothing to wait for. Where a glyph shows upright at the size it was laid out for, it
draws a variant fitted to the pixel grid at that size (stems moved whole onto the font's baseline,
x-height and cap height; `font::fit_vector_glyph`), made once the first time the size appears; a
zoom draws the outline as it is, so stems never jump a pixel at a time. The pixel stage only
measures coverage: the fit inside it tripled the uber-shader's registers for every draw. ClearType
weights the pixel's thirds with FreeType's filter. Colour glyphs, bitmap and CFF fonts, MSDF
text and synthetic bold still go through the atlas below.

Coverage masks, and the glyphs above that still use the atlas, are rasterized for a density. When
a layer's scale changes:

1. **While it moves** the layer keeps its current rasters, resampled by the GPU. Scales are
   quantized (quarter octaves), so a long zoom crosses a handful of steps, not a step per frame.
2. **When it settles** (scale unchanged for ~150 ms) the exact rasters are requested.
3. Rasterization runs on a persistent worker pool and under a per-frame upload budget. A frame
   draws what is ready and swaps in the rest when it lands. Only text appearing for the first time
   is waited for, so nothing pops in empty.

Paths that are drawn large or transformed (wires, curves, the grid) move from atlas masks to
tessellated meshes in layer-local space, which are resolution-independent and never rasterize on
zoom. Masks remain for what needs them: blurred shadows, effects, complex fills at small sizes.

### 4.6 Layout with boundaries

Measurement keeps today's incremental scheme. Arrangement changes in two ways:

- **Records are parent-relative.** A record holds its offset in its parent, not its position on
  the surface. Moving or scrolling a subtree changes one offset; nothing beneath it is copied.
  Absolute positions are composed on demand, through the layer tree, by hit testing and recording.
- **Layout boundaries.** Dirtiness stops at a node whose size does not depend on its children
  (fixed or fill-sized), at a scroll container's content and at a canvas. A canvas child's
  position and a scroll offset are not layout at all: they are T0 properties of a layer.

The whole-tree steps that run today whatever changed (`record_unarranged`, `retain_exit_layout`,
the cache sweeps, `prune_exiting`, the motion `configs` sweep) become work on dirty and exiting
nodes only, tracked in sets as dirty marks already are.

### 4.7 Input against layers

Hit testing walks the layer tree, transforming the point by each layer's inverse transform, and
uses a spatial index per layer for its own nodes, updated when a node's local bounds change. A T0
change touches no index: moving a canvas child moves its layer, and the point is transformed into
it on the next query. Nothing rebuilds pointer geometry for the whole tree.

Hover, press and focus are interaction state on the node they style: T1, a binding update of the
widget's presentation, never a description change. A Button's `presentationTemplate` reads its
state through a binding instead of `$presentationState` invalidating the description.

Consumers that watch the whole tree today through global lane counters (pointer geometry,
semantics, hover refresh) subscribe to the nodes they depend on instead.

### 4.8 Frames on demand, and a compositor thread

- **No change, no frame.** The host sleeps on input, timers and completions. A Surface reports the
  next time it needs a frame (an animation tick, a settle deadline, a raster completion); hosts
  schedule against it instead of rendering every vsync.
- **Compositor thread.** Layer properties and the arena are what the GPU needs; a render thread
  owns them, presents at display rate and runs T0 animations (scroll flings, zoom animations,
  transitions of transform and opacity) on its own. A long owner-thread frame then delays content,
  not motion.

### 4.9 Canvas

`Canvas` is the primitive node editors, maps, diagrams and timelines need: children positioned in
world coordinates, a view transform that is a T0 property, culling against the view, and
level-of-detail predicates (`visibleAbove: 0.4` hides pin labels when zoomed out).

ZoomView already applies its view at render time, so it is the starting point rather than a
replacement. What it lacks is everything around the transform: its children's positions are
layout insets, its view change rebuilds pointer geometry and re-plans the submission, and its
`bind`/`onViewChange` push the view back into application state on every frame of a gesture, which
re-describes whatever reads it. As a Canvas, child positions are layer offsets, the view is a
layer property, and the view reaches application state when the gesture settles (or every frame
only for a binding that asks for it).

## 5. What stays, what changes

Nothing is kept because it exists. What survives does because it is sound and independent of the
spine: the language, compiler and IR, StrataScript, text shaping, the layout algorithms, the
analytic pixel shader, GPU raster jobs, effects and materials, durability, async data, and the
headless and benchmark harnesses. Everything that carries a change from where it is made to the
GPU is replaced, including the describe-time reuse added in `973d406` (component skip, widget-call
reuse, memoized calls): it is comparison work that bindings make unnecessary, and it is removed
once they land rather than kept alongside them.

| Today | Becomes | Why |
|---|---|---|
| A state is one immutable value per address; a write replaces it | Keyed collections and path writes (§4.1) | A drag writes one field, not the graph |
| Instances re-run their body and compare (component skip, widget-call reuse, memoized functions, field reads) | Bindings updated in place; bodies run only on structural change (§4.2) | Comparing is the cost being removed. The `973d406` read tracking is the binding's dependency list |
| `RenderEngine::render` clears and re-appends the whole command stream on any change; the submission compares it command by command | Per-layer display lists; only changed layers re-record (§4.3) | Stream size stops appearing in every frame |
| Presentation groups: transform/opacity table, only while a motion player runs, 511 max, `publish_groups` walks the stream | Layers: persistent, any transform-like property drives them, uncapped (paged constant buffers) | Same GPU idea, no longer an animation-only exception |
| Transforms outside groups baked into vertices; a drag offset is a `TransformPush` that replans | Every movable subtree is a layer; moving it is a property write | No replan, no vertex rewrite, no mask re-keying by sub-pixel phase |
| One submission per Surface; a zoom step resets it and plans everything | Plans per layer; a layer's scale step re-plans that layer's text only, under the raster budget | A zoom is a property change plus deferred refinement |
| Packet re-serializes every batch, host re-parses them and recomputes group extents over all vertices; Vulkan copies all geometry per frame slot on any epoch change | Delta packets against a host-side layer table and arena (§4.4) | Bytes and host work proportional to the change |
| Host redraws every batch and re-runs every effect each loop, settled or not | Frames on demand; an unchanged frame is not drawn (§4.8) | Idle costs nothing |
| Glyph misses spawn up to three threads per `warm` call; text is rasterized at the exact zoomed scale every frame (`686905e`) | Persistent raster pool, quantized scales while moving, exact on settle (§4.5) | No frame waits on rasterization |
| Grouped text is never visibility-culled | Layers cull against the view | Zoomed-out canvases stop preparing off-screen text |
| Everything on the host loop thread | Owner thread plus compositor thread (§4.8) | Content can be late; motion cannot |
| Layout records in absolute surface coordinates; moving or scrolling copies every record beneath | Parent-relative records (§4.6) | A scroll is one offset |
| Pointer geometry: flat vector of every node, rebuilt on nearly any change, scanned linearly | Per-layer spatial indexes, updated per node (§4.7) | Hit testing stops scaling with the tree |
| Hover on a templated Button re-describes the app | Interaction state as T1 bindings (§4.7) | Hover repaints one node |
| Per-frame whole-tree steps: `prune_exiting`, motion `configs` sweep, layout walks and cache sweeps, description snapshot copy | Dirty and exiting sets (§4.6) | A non-idle frame stops paying for the tree |
| Global per-lane dirty counters watched by whole-tree consumers | Per-node subscriptions | One node's change invalidates one node's products |
| `WorkScheduler` unused | It drives frames on demand: active work and deadlines decide whether a frame is needed (§4.8) | It already does the right thing |

## 6. Plan

Each phase ships on its own and is accepted against its gate. Gates are written as **work counters
first, timings second**: "a pan re-records no layer" holds on any machine and fails loudly in CI,
where "a pan takes 0.2 ms" drifts with hardware.

### Phase 0: gates

- Scenario suite in the repository, run by the desktop harness: idle, hover sweep, node drag, pan,
  wheel zoom (Blueprint with 12 and with 500 nodes), a 10 000-row list scroll, typing in the code
  editor, tab switch.
- Per-frame counters exported to the report and to headless: frames produced, nodes described,
  bindings re-evaluated, layers re-recorded, instances encoded, packet bytes, batches drawn, glyphs
  rasterized, raster milliseconds spent.
- Each later phase adds assertions on these counters to `ctest`.

### Phase 1: frames on demand, rasterization off the frame

- `strata_surface_frame_info` reports whether the Surface is settled and the next time it needs a
  frame (animation tick, settle deadline, pending raster). The desktop and showcase hosts wait on
  input and that deadline instead of rendering every loop.
- A persistent raster pool replaces the per-call threads. Scaled text and masks use quantized
  scales while their scale moves and exact ones after it settles, under a per-frame budget
  (§4.5); this reverses `686905e`, whose exact-every-frame policy is the zoom stall.
- Gate: an idle Surface reports idle and unchanged frames that do no work (`strata.abi`), and the
  host neither draws nor presents them unless a live material (one reading time or the pointer)
  is on screen; wheel zoom never rasterizes for more than 1.5 ms in a frame. Zoom's remaining cost
  is the full re-plan and packet a scale step causes, which Phase 2 removes.

### Phase 2: layers and instanced drawing

- Layer tree derived from the retained tree (§4.3); per-layer display lists of compact instances;
  a persistent host arena; delta packets (§4.4) decoded by D3D11, Vulkan and the software renderer.
- Presentation groups are folded into layers and removed.
- Gate: pan and zoom re-record no layer and send under 1 KB per frame; a canvas child's drag
  re-records only the wires that touch it; zoom p95 under 2 ms.

### Phase 3: Canvas, layout boundaries, hit testing on layers

- `Canvas` (§4.9); ZoomView becomes a Canvas configuration; the view is a T0 property.
- Incremental layout stops at layout boundaries (§4.6); hit testing transforms into layers (§4.7).
- Gate: Blueprint with 500 nodes pans and zooms at the same work counters as with 12.

### Phase 4: bindings and path state

- Keyed collections and path writes (§4.1); bindings with tiers (§4.2); body re-description only
  for structural change. The describe-time reuse from `973d406` is deleted.
- Gate: a node drag describes nothing and re-evaluates only the bindings reading the dragged node.

### Phase 5: compositor thread

- A render thread owns the layer table, arena and present, and runs T0 animations (§4.8).
- Gate: a 30 ms stall injected on the owner thread drops no frame of a running zoom animation.

The order front-loads what is felt (Phases 1 and 2 fix zoom, pan and idle) and puts the largest
runtime change (Phase 4) after the layers it routes changes into exist.

## 7. Risks

- **Two update paths must agree.** A binding update and a full description of the same state must
  produce the same retained tree. Test builds run a shadow description after binding updates and
  compare; any difference is a failure, not a warning.
- **Determinism.** Asynchronous rasterization and frames on demand must not change headless
  captures. Headless rasterizes synchronously and drives settle deadlines from its virtual clock.
- **Text during motion.** Outline text is exact every frame (§4.5); what still uses the atlas
  (colour glyphs, CFF fonts) is briefly resampled while a zoom moves and sharpens when it settles.
  The cost moved to the pixel stage: a glyph's pixels read the segments of the bands they cross,
  so text-dense frames are watched on the GPU timer, not only the core's.
- **Too many layers.** Each layer is a constant-buffer entry and a draw-order boundary. Automatic
  layers are limited to canvas children, scroll and viewport content, transform or opacity motion
  and effect sources; layers that stop moving are squashed back into their parent's display list.
- **Backdrop effects over moving layers.** An effect samples what lies under it; a layer moving
  beneath it invalidates its cached sample. Effect regions are tracked per layer so only effects
  over a moved layer refresh.
- **ABI break.** The packet format changes and every in-repository host moves with it in the same
  phase; out-of-tree hosts negotiate the version as they do today.
- **Scope.** This is the largest change Strata has had. The gates exist so each phase can stop
  short without leaving the engine worse than it found it.
