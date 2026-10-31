# Frame cost

## Goal

A frame costs what changed in it, not what the page holds. A spinner beside thousands of static
rows, a ticking counter, a hovered cell and a scrolled list each cost about the same on a page of a
hundred nodes as on a page of a hundred thousand: the engine touches the nodes, records, commands
and draws that changed, and reuses everything else where it is.

Where it stands (measured October 2026, desktop host, 2,000 static cells beside the moving part):

| Page | Before | Now | What is left |
| --- | --- | --- | --- |
| One animated arc, in its own component | 0.73 ms | 0.16 ms | describing the arc's component, about 0.04 ms |
| A counter whose text changes every frame | 0.43 ms | 0.10 ms | shaping the changed text, about 0.04 ms |
| Scrolling a 2,000-row list | 0.92 ms + 0.38 ms packet | 0.50 ms + 0.37 ms packet | moving every record and replanning every draw (phase 1) |
| Hovering across 2,000 sibling cells | 0.37 ms render | 0.37 ms render | checking every sibling for reuse (phase 2) |
| First frame of the hover page | 41 ms description | 41 ms description | about 10 µs per node, spread thin (phase 3) |

The showcase benchmark (`strata_core_benchmark`, a page of ordinary size) is the same or better in
every phase than before this work: scrolling 0.395 to 0.353 ms a frame (p95 1.07 to 0.80).

Ground rules:

- Measure first: on the desktop host, with a stress page and a temporary print of
  `surface().profiler().sections` from `desktop/application.cpp`'s `frame()` (run `--uncapped`);
  and on the showcase benchmark against the commit before the change, built in a worktree. A
  change that does not move a measurement past run-to-run noise is reverted.
- Nothing that is reused may be observably stale: the differential test (every backend against the
  reference renderer) and the fast-path-against-full-plan tests stay on.
- The full `windows-x64` and Vulkan CTest gates pass at the end of each phase.

## Phase 1: scrolling moves a layer, not the page

Layout records hold positions that include every scroll offset above them. Each scroll step
therefore translates every record of the scrolled content (0.30 ms for 2,000), render verifies the
move against a snapshot of every child (0.14 ms), and every draw command changes, so submission
replans and re-encodes all of them (0.37 ms). Browsers keep scrolled content in its own coordinate
space and move it on the compositor; a scroll step changes one transform.

- [ ] Records of a scroll view's content are in content space: the scroll offset is a transform
  the view applies to its content, not a translation baked into each record. Layout arranges
  content once per content change, never per scroll step.
- [ ] Everything that reads record positions in screen space (hit testing, input placement, IME,
  semantics bounds, reveal, anchors and portals, sticky positioning) composes the scroll transforms
  above a record instead of reading it raw. Find every reader before changing the model.
- [ ] Render draws a scroll view's content inside a presentation group whose table entry carries
  the offset: a scroll step updates the group table and nothing else, as a moving motion group does
  today (`presentation_frame`). Culling of grouped draws needs the group's offset to stay exact.
- [ ] Virtualized lists keep realizing rows from the viewport, now computed from the offset rather
  than from translated records.
- [ ] Measure: a scroll step on the 2,000-row page costs well under 0.1 ms in core and packet both.

Who reads record positions as surface positions today (surveyed October 2026; paths under
`native/`):

- Already composing a transform, so they take the scroll transform with the presentation one: hit
  geometry (`ui/input/hit_test.cpp` `prepare_pointer_geometry`, `presented_bounds`), spatial
  focus navigation, input injection and capture rectangles, IME and text hit offsets
  (`ui/surface.cpp`), render's command emission and the planner's culling (both under a transform
  stack).
- Reading raw positions against the raw pointer, to be given a localized pointer: scrollbars
  (`ui/scroll_geometry.cpp`), drag and drop targets, previews and edge auto-scroll
  (`ui/input/drag*.cpp`), and widgets comparing `event.position` with their bounds (slider, range
  slider, colour picker, zoom view, table header in `ui/widget/`).
- Undoing the baked-in scroll, which flips to doing nothing: `reveal_rect` (`ui/input/scroll.cpp`),
  scroll snapping (`ui/surface.cpp`), the collection marquee (adds the root's `scroll_offset` back),
  and move animations (`scroll_influence` in `ui/motion/move.cpp`).
- Hard:
  - `LayoutRecord::clip` is every ancestor's clip already intersected in surface space. It is read
    by render, static text selection, inspection and `host/browser_model.cpp`, so it needs a
    definition in one space.
  - Portals and popups place against their anchor's surface position (`ui/layout/engine.cpp`,
    `ui/widget/menu_model.cpp`, `ui/widget/subtarget_projections.cpp`), while their nodes sit under
    scroll views.
  - Windowed virtual lists find their window from the surface-space `inherited_clip`
    (`ui/layout/arrange.cpp`).
  - Sticky boxes and pins are counter-scroll offsets.
- Unaffected: semantics (it reads no geometry) and virtual realization windows (local sizes and
  offsets). Inspection JSON reports raw `bounds` and `clip` but presented `hitBounds`; decide what
  it reports.

## Phase 2: recompose a parent from the children that changed

When one child of a large parent changes (a hovered cell among 2,000 siblings), the parent's
composition is rebuilt by visiting every child; each unchanged child is reused, but checking it costs
about ten hash lookups (its record, its fragment, its interaction state, the parent's per-child layout
check), so the parent costs O(children).

- [ ] A composition plan keeps each child's first and last command offset in the parent's stream.
- [ ] Recomposing appends runs of unchanged children as slices of the parent's previous stream (one
  slice per run, not per child) and visits only the children whose subtree, presentation or layout
  generation moved. Which children moved comes from the tree's dirty set, not from asking each child.
- [ ] Unchanged children skipped by a slice stay cached: fragments are pruned only when the tree's
  structure changes, so skipping their visit is safe (the old `visited` mark was never read and is
  gone).
- [ ] Before building, profile where a reused child's ~180 ns goes: cutting its hash lookups (one
  record lookup per child instead of three, input state read only when needed) did not move the
  measurement, so the lookups are not it.
- [ ] Measure: hovering across the 2,000-cell grid costs render under 0.05 ms.

## Phase 3: a cold page costs less per node

The first frame of the 2,000-cell hover page spends 41 ms describing it: 16 ms building, 20 ms
projecting themes, 3 ms reconciling, about 10 µs per node in all. Timing parts of theme projection
by hand found no single cost: token resolution in authored styles and per-node property setup each
take about 2 µs a node, and removing two whole-map copies a node was within noise.

- [x] Profile a cold start with a sampling profiler. Recorded with `wpr -start CPU` (an elevated
  prompt) over nine launches of the hover page, and read with `xperf -i trace.etl -symbols -a
  profile -detail` against the build's PDBs. The engine's own code was 2.3 s of the 24.8 s the
  process spent; compiling HLSL was 6.2 s, about 0.7 s every launch.
- [x] Compiled shaders persist between runs (`d3d11::set_shader_cache_directory`, under
  `%LOCALAPPDATA%\Strata\shaders` on the desktop host): creating the D3D11 renderer went from
  723 ms to 4 ms on every start after the first.
- [ ] The Vulkan backend compiles its HLSL through shaderc on every run too: the same cache, for
  SPIR-V.
- [ ] Description at about 10 µs a node: from the same trace, sorted by inclusive time with stacks
  (`-a stack`), since self time spreads it over allocation, `Value::field` and map lookups.
- Skipping draw state that did not change between batches (shader, scissor, blend, clips,
  textures) did not move the uncapped frame rate past run-to-run noise: those frames are bound by
  the GPU and driver, not by the calls. Not kept.
- [ ] Measure: the hover page's first frame describes in under 15 ms.
