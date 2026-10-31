# Frame cost

## Goal

A frame costs what changed in it, not what the page holds. A spinner beside thousands of static
rows, a ticking counter, a hovered cell and a scrolled list each cost about the same on a page of a
hundred nodes as on a page of a hundred thousand: the engine touches the nodes, records, commands
and draws that changed, and reuses everything else where it is.

Starting measurements for this plan (October 2026, desktop host, 2,000 static cells beside the
moving part; the later matched workload measurements and gates are recorded below):

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

Execution, October 31:

- Baseline `cmake --build --preset windows-x64 --parallel 16` passed.
- Baseline core showcase (`./build/cmake/windows-x64/native/RelWithDebInfo/strata_core_benchmark.exe
  src/main/resources native/tests/fixtures/headless/showcase.json 1`): scrolling average
  0.426 ms, p95 1.004 ms; render 0.053 ms, layout 0.019 ms, packet encode 0.150 ms.
  Cold host/setup was 97.417 ms and first frame 21.381 ms. A separate cold-only run measured
  89.329 / 23.127 ms. The earlier invocation without `./` returned no output before its
  600-second shell deadline; these successful explicit-relative-path runs provide the baseline.
- The cutover keeps arranged content coordinates stable and represents scroll movement separately,
  as browser scroll property trees do. Clip ancestry must retain the coordinate space of each
  clip; a pre-intersected surface rectangle cannot remain authoritative after scrolling.
- Before changing geometry, audit input, inspection, portals, sticky/pinned children and
  ancestor-windowed virtual lists. Ordinary content must not pay for those exceptional branches.
- Gates remain the 2,000-row core/packet measurement, presented geometry and differential
  rendering regressions, then both Windows suites. No phase is complete yet.

The reproducible 2,000-cell fixtures now live in
`native/tests/fixtures/headless/frame-cost-{scroll,hover}.json`; `strata_core_benchmark` has matching
`scroll` and `hover` modes. Baseline engine revision `9fe28b2` was built in
`build/frame-cost-baseline` with the same benchmark driver and the same current fixture/resources.
Three consecutive runs, with no concurrent build:

| Baseline workload | Whole frame average | Core frame | Input | Layout | Render | Packet encode |
| --- | --- | --- | --- | --- | --- | --- |
| 2,000 rows, wheel every frame | 3.929–3.968 ms | 2.228–2.255 ms | 0.892–0.905 ms | 0.362–0.367 ms | 0.960–0.974 ms | 1.638–1.655 ms |
| 2,000 cells, moving hover | 2.343–2.349 ms | 0.546–0.549 ms | 0.133–0.136 ms | 0.008–0.009 ms | 0.393–0.395 ms | 1.780–1.789 ms |

The wheel workload includes input on every frame, unlike the historical animation-only breakdown
above. Cold hover description measured 38.881–44.717 ms. The fixture uses two bounded 1,000-item
loops rather than raising the language's eager-loop safety limit.

The initial content-space cutover measured layout at 0.014 ms with zero translated records, but
whole wheel frames still averaged 3.674–3.770 ms: input rebuilt geometry (0.950–0.973 ms), and
hover following the newly exposed row made render visit all 2,004 nodes (0.401–0.429 ms).
Consequently Phase 2's changed-child slicing is also required by the Phase 1 wheel gate and is
being integrated before that gate is closed. Hit geometry must retain the same scroll-coordinate
boundaries rather than rebuild descendants. No performance checkbox is satisfied by the layout
improvement alone.

The next correctness/performance checkpoint:

- Hit entries and placements now stay relative to motion/scroll anchors. A scroll view has a
  separate content anchor; changing its offset recomposes anchor transforms and clips, not all
  descendant entries. Pointer observers, detached targets and capture rectangles query the same
  current geometry.
- Render has a change journal that survives layout consumption, with generation coverage for
  renderers called after frame cleanup. A 384-frame fresh-versus-retained comparison caught an
  additional whole-frame-cache hazard after dirty marks were cleared; that guard now checks the
  render change generation too. The regression includes successive children and a background
  added/removed within a child.
- Changed child lengths are indexed; unchanged runs are retained ranges. Flat segment metadata
  still cost 0.113 ms of hover rendering after visits dropped to about 8/frame. Shared balanced
  command branches reduced that to 0.085 ms in the first measured run. Variable-length splice
  and immutable-predecessor coverage exercises 1,024 edits; longer sweeps remain a measurement gate.
- A draw-kind change under unchanged scopes need not discard the whole submission plan. Allowing
  those changes reduced hover planning from 1.364 to 0.014 ms; mesh updates still cost about
  0.29 ms. Wheel input fell from 0.950–0.973 to 0.148–0.150 ms. Neither the core/packet gate nor
  the hover render gate is closed.
- The focused UI, interaction, binding and packet suites pass. D3D11 differential rendering passed
  before the balanced command storage change; its current-state repeat and both full backend
  gates remain due. Translation and perspective clip ownership are checked independently of the
  raster-class transition, which legitimately invalidates text planning.
- Both stress scenarios ran on D3D11 without diagnostics. The scroll capture shows rows 16–48
  after a 384-unit scroll, with viewport clipping. Settled scroll packets are 152 bytes, but the
  first full scroll packet is still 12,530,743 bytes and its first hover-changing scroll packet
  6,014,294 bytes. Those preparation/topology costs must not be hidden by the settled fast path.

Focused timers then separated the remaining costs:

- Authored-presentation synchronization scanned all 2,000 eligible widget types even when none
  had a template: 101–102 µs per changed hover. It now indexes actual template owners, updates
  that set from covered description changes, and synchronizes only changed hover/focus/press
  identities. Added hover/press/release/leave checks on an authored Button.
- Overlay collection similarly checked 2,000 tooltip-capable controls: 55–59 µs per frame.
  Retained widget participation now follows the render/layout journal, with full fallback when
  journal coverage or command/status generations change. Behavior overlays still evaluate their
  existing indexed candidates each frame because drag sessions need not change node-owned state.
- Mesh probes found draw-kind changes whose geometry fit the old slot but whose individual keys
  differed (rounded rectangle to text). The enclosing batch already supported both. Placements
  now identify that batch; compatible changes keep its effective texture/blend/scissor/clip
  state. Different textures, incompatible blends and outgrown slots still fall back. Full-plan
  comparison now includes switching a draw between a rectangle and text.
- The balanced command sequence stayed stable across 6,000 hover frames. After the candidate
  changes, render was 0.032 ms and packet encode 0.038 ms. The expanded Windows UI/widget/command
  selection and rendering differential passed on that tree.
- Scroll hit queries still tested about 1,904 entries and took 12.088 µs each. Spatial bounds
  now index entries within each retained anchor, preserving painter-order priority and exact
  shape/clip tests. A 3,000-frame wheel run measured input 0.019 ms, render 0.040 ms, core
  0.080 ms and packet encode 0.071 ms. A 6,000-frame hover run measured input 0.015 ms,
  render 0.025 ms, core 0.053 ms and packet encode 0.032 ms. The focused suites pass.
- These longer runs include the early topology-growth frames; the default 600-frame wheel
  measurement, direct wide scroll parents, current backend gates and cold grouped preparation
  still need closure. No checkbox is being inferred from a longer-run average alone.
- A direct-child probe (the same 2,000 Buttons immediately under Scroll, without the content
  Panel) confirms that gap: 600 wheel frames averaged 6.593 ms; layout 0.279 ms, render
  2.204 ms, packet planning 2.753 ms. Only three records were arranged, but the parent still
  checked every child, copied child snapshots and emitted a group push/pop for each one.
  General parent-local repaint with retained child ranges, contiguous scroll scopes and
  offset-only arrangement must remove that work; wrapping the page in a Panel is not the fix.
- Parent-local recomposition reduced that direct-child render cost to 0.045 ms. Offset-only
  arrangement now updates the viewport record and binary-searches cached monotone flow
  rectangles; it measures 0.015 ms rather than 0.275 ms. Separate owner/descendant arrangement
  revisions preserve simultaneous nested scroll mutations. Six focused suites passed, including
  variable-size/zero-height visibility, clamping and nested offsets. Both direct and wrapped
  D3D11 captures show rows 16–48 after scrolling, with no diagnostics.
- The matched 600-frame direct/wrapped runs now measure core 0.085/0.084 ms and packet
  0.158/0.159 ms. A slot probe explains the remaining mesh rebuilds: a contiguous changed run
  alternates rectangle-to-label (352 to 1,760 vertex bytes) with label-to-rectangle (2,816 to
  352 bytes). The run fits its existing arena; individual slots do not. Redistributing only a
  changed run's capacity can preserve unchanged geometry, ordered indices and batch state.
  Redistributing those runs reduced mesh encoding from 0.099 to 0.024–0.027 ms and packet
  encoding from 0.159 to 0.062–0.067 ms. Incompatible batch state or a run that really outgrows
  its capacity still rebuilds. The regression now compares ordered drawn triangles and effective
  batch state, not private slot layouts, padding or cache counters.

Three matched 600-input-frame runs after these changes, without concurrent builds/tests:

| Workload | Whole frame average | Core frame | Render | Packet encode |
| --- | --- | --- | --- | --- |
| 2,000 wrapped scroll rows | 0.414–0.438 ms | 0.081–0.088 ms | 0.038–0.043 ms | 0.064–0.067 ms |
| 2,000 direct scroll children | 0.404–0.426 ms | 0.079–0.084 ms | 0.038–0.040 ms | 0.062–0.064 ms |
| 2,000 hover cells | 0.103–0.109 ms | 0.055–0.058 ms | 0.026–0.027 ms | 0.038–0.040 ms |

The showcase control run measured scrolling at 0.141 ms average, p95 0.263 ms, versus the initial
0.426 / 1.004 ms. Its setup/first frame were 93.677 / 20.511 ms. First pointer entry still changes
the command count and takes the conservative full-submission path: whole-frame maxima in the
600-frame scroll runs were 15.5–16.7 ms. These frames are included in the reported averages, not
removed with an input warmup.

Phases 1 and 2 passed their final gates:

- Full Windows build and all **85/85** tests passed; bundled artifact regeneration completed.
- Full Vulkan build and all **95/95** tests passed, including the three-backend differential suite.
- Both 2,000-cell stress fixtures and the direct-child scroll probe were replayed separately
  through CPU, D3D11 and Vulkan. All frames compared successfully. Widest mean channel difference
  was 0.0188599, outlier share 0.000003125.
- Shared-element transitions now have both ordinary and scrolled direct-child fixtures. They
  compare the packet's painted rectangle (excluding the analytic antialiasing skirt) against
  presented input geometry at start, mid-flight, landing and reverse flight.

Before this phase, layout records held positions including every scroll offset above them. Each step
therefore translated every record of the scrolled content (0.30 ms for 2,000), render verified the
move against a snapshot of every child (0.14 ms), and every draw command changes, so submission
replans and re-encodes all of them (0.37 ms). Browsers keep scrolled content in its own coordinate
space and move it on the compositor; a scroll step changes one transform.

- [x] Records of a scroll view's content are in content space: the scroll offset is a transform
  the view applies to its content, not a translation baked into each record. Layout arranges
  content once per content change, never per scroll step.
- [x] Everything that reads record positions in screen space (hit testing, input placement, IME,
  semantics bounds, reveal, anchors and portals, sticky positioning) composes the scroll transforms
  above a record instead of reading it raw. Find every reader before changing the model.
- [x] Render draws a scroll view's content inside a presentation group whose table entry carries
  the offset: a scroll step updates the group table and nothing else, as a moving motion group does
  today (`presentation_frame`). Culling of grouped draws needs the group's offset to stay exact.
- [x] Virtualized lists keep realizing rows from the viewport, now computed from the offset rather
  than from translated records.
- [x] Measure: a scroll step on the 2,000-row page costs well under 0.1 ms in core and packet both.

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

- [x] A composition plan keeps each child's first and last command offset in the parent's stream.
- [x] Recomposing appends runs of unchanged children as slices of the parent's previous stream (one
  slice per run, not per child) and visits only the children whose subtree, presentation or layout
  generation moved. Which children moved comes from the tree's dirty set, not from asking each child.
- [x] Unchanged children skipped by a slice stay cached: fragments are pruned only when the tree's
  structure changes, so skipping their visit is safe (the old `visited` mark was never read and is
  gone).
- [x] Before building, profile where a reused child's ~180 ns goes: cutting its hash lookups (one
  record lookup per child instead of three, input state read only when needed) did not move the
  measurement, so the lookups are not it.
- [x] Measure: hovering across the 2,000-cell grid costs render under 0.05 ms.

## Phase 3: a cold page costs less per node

The first frame of the 2,000-cell hover page spends 41 ms describing it: 16 ms building, 20 ms
projecting themes, 3 ms reconciling, about 10 µs per node in all. Timing parts of theme projection
by hand found no single cost: token resolution in authored styles and per-node property setup each
take about 2 µs a node, and removing two whole-map copies a node was within noise.

Current investigation:

- `strata_vulkan_preparation_benchmark` now reports renderer setup separately from pipeline
  preparation. Without a driver cache, setup/preparation measured 378.514/1,075.14 ms. Two runs
  with that cache measured setup 333.295–336.731 ms but preparation only 5.251–5.465 ms:
  driver persistence does not remove shaderc compilation.
- The current process has a medium-integrity token, so a temporary process-local DbgHelp sampler
  is being used for inclusive native stacks without an elevated WPR session. Its suspensions
  distort wall times; only unsampled benchmark timings are performance evidence.
- The first sample identified `observe_retained_value` in 21 of 36 description-building stacks.
  It linearly searches all prior reads, making a wide body's dependency collection quadratic.
  Retained reads will use an identity index, like the existing state/host dependency tracking,
  while retaining exact equality on collisions and the existing owned read records.
- The denser 60-launch profile collected 8,007 stacks; retained-read observation appeared in
  981 of 1,843 widget-description stacks. The identity index passed five focused suites.
  Unsampled cold description fell from 38.8–44.7 ms to 25.1–28.9 ms; build time fell from
  24–29 ms to 11.0–12.8 ms. Keep it; theme materialization remains 11.6–13.1 ms.
- Vulkan shader persistence caches the compiler result, before dual-source decoration.
  Entries include source, stage, entry, optimization mode and compiler identity, are bounded and
  checked by SPIRV-Tools before use, and publish by atomic replacement with process-unique
  temporary names. The host-owned directory and optional-cache failure semantics follow D3D11;
  this remains separate from device/driver pipeline persistence. Warm renderer setup measured
  29.900–31.716 ms against 333.295–336.731 ms with driver persistence alone. The Vulkan build,
  installed-consumer check, pixel tests and showcase passed. Selecting the SDK's shared
  SPIRV-Tools validator resolved the static archive's missing-PDB linker warnings without
  suppressing diagnostics.
- The post-index profile collected 3,444 stacks: theme materialization appeared in 455 and
  widget description in 432. Ordinary controls still eagerly build authored-presentation
  arguments and observe retained interaction state without a template. Argument factories now
  defer that work until a template exists. Five focused suites and the D3D11 grid smoke passed,
  but description remained 25.1–28.5 ms; its isolated performance benefit is not established.
- Computed styles now share the most recent identical projection per theme contribution within
  a materialization pass. The key includes all four style containers, authored-container
  presence and direct motion-override presence; unrelated widget arguments still resolve
  independently. No authored inputs survive beyond the pass in this sharing table.
  Materialization fell to 5.005–5.111 ms, total description to 17.226–17.789 ms. Six focused suites
  passed, including fresh-versus-shared comparisons across style precedence, explicit-null
  motion overrides, scope changes and per-node timing diagnostics. The D3D11 grid rendered
  correctly without diagnostics. Keep this change; the 15 ms description gate remains open.
- A subsequent 60-launch sample selected 952 stacks containing DescriptionBuilder, including
  782 in build_layers and 111 in teardown. Layout-default construction appeared in 146 of
  774 widget-building stacks. Reusing the contiguous field builder for default collection and
  normalization did not help: description 17.735–17.916 ms, build 10.006–10.371 ms. The six
  focused suites passed, but the storage change was reverted because it did not beat noise.
- An eager-argument A/B probe with style sharing retained measured build 11.741–13.273 ms and
  description 19.642–21.371 ms, versus the lazy path's 9.792–10.042 / 17.226–17.789 ms.
  The lazy path is restored; this isolated comparison supports keeping it.
- A 60-launch materialization sample found property insertion in 115 of 382 stacks and
  vector reallocation in 74. Palette and motion consumers now use the immutable effective
  Theme already retained by each node, instead of serialized property copies and reparsing.
  Materialization measured 3.735–4.439 ms. Six focused suites and the diagnostic-free D3D11
  grid smoke passed; an open DatePicker now checks palette updates and restoration through
  native rendering rather than asserting a private serialized-property representation.
- The remaining empty timing scans appeared in 59 of 382 materialization stacks. Shared styles
  now record whether their projected layout can mention timings; direct node fields still force
  the per-node check. Description measured 14.959–15.341 ms and materialization 2.877–3.206 ms.
  All six focused suites passed, including distinct node-local unknown timings on shared styles.
  The 15 ms gate is still borderline, not closed. Snapshot construction is the next allocation
  target: its key index currently allocates a map node and singleton vector per keyed node.
- A sorted snapshot key index now borrows keys from snapshot-owned records and preserves scoped
  uniqueness, including empty duplicates. Its new regression also keeps an older snapshot across
  mutations and tree destruction. Six focused suites passed, but 14.834–17.655 ms cold runs are
  too variable to establish this small change's benefit in isolation.
- The larger remaining candidate is property-dependent native defaults (146 sampled stacks):
  one description transaction now memoizes each widget's latest projection. It compares all input
  names, values read by the default hook, and authored layout; missing reads remain valid only
  while the name set matches. Read tracking uses bounded stack storage; wide argument maps remain
  uncached. Orientation, missing-versus-present arguments and authored width overrides are checked
  through actual layout. Six focused suites passed; three cold runs measured description
  12.992–14.669 ms and build 7.727–8.425 ms. Keep the cache. The D3D11 grid was visually checked
  and reported no diagnostics. Showcase scrolling remained 0.141 ms average / 0.260 ms p95,
  versus 0.141 / 0.263 ms after Phases 1–2; its cold description fell from 2.264 to 1.934 ms.
- Twenty matched cold pairs alternated execution order with no concurrent build or tests.
  Together, the borrowed snapshot-key index, immutable single-object normalization and borrowed
  synchronous retained-query capture reduced median description from 13.578 to 12.888 ms and
  median build from 8.327 to 7.734 ms. Median paired description improvement was 0.686 ms,
  positive in 15/20 pairs. Keep the group; this does not establish an individual contribution
  for any of its three parts. The retained variant ranged 12.771–15.486 ms, with nearest-rank
  p95 14.593 ms. The outlier prevents an every-run-under-15-ms claim.
- Phases 3–4 final gates passed: full Windows build and **85/85** tests, full Vulkan build and
  **95/95** tests, including differential rendering and installed consumers. Three fresh launches
  after the final build measured description 12.660–13.826 ms (median 12.773 ms). A fresh D3D11
  grid capture reported no diagnostics and was byte-identical to the visually inspected grid.
  The cold description gate is satisfied by these fresh runs and the paired run's median/p95,
  not an absolute wall-time guarantee against scheduler outliers.

- [x] Profile a cold start with a sampling profiler. Recorded with `wpr -start CPU` (an elevated
  prompt) over nine launches of the hover page, and read with `xperf -i trace.etl -symbols -a
  profile -detail` against the build's PDBs. The engine's own code was 2.3 s of the 24.8 s the
  process spent; compiling HLSL was 6.2 s, about 0.7 s every launch.
- [x] Compiled shaders persist between runs (`d3d11::set_shader_cache_directory`, under
  `%LOCALAPPDATA%\Strata\shaders` on the desktop host): creating the D3D11 renderer went from
  723 ms to 4 ms on every start after the first.
- [x] The Vulkan backend compiles its HLSL through shaderc on every run too: the same cache, for
  SPIR-V.
- [x] Description at about 10 µs a node: inclusive native stacks identified retained-read
  searches, repeated style/default projection, metadata copies and empty timing scans. The
  measurements and retained/rejected changes are recorded above.
- Skipping draw state that did not change between batches (shader, scissor, blend, clips,
  textures) did not move the uncapped frame rate past run-to-run noise, and moved CPU time in
  `Renderer::render` by 3 to 5% within overlapping ranges (it includes waiting on Present). Not
  kept.

## Phase 4: submitting a frame to D3D11

The same trace, by inclusive time: on the hover page the desktop host's D3D11 submission
(`RenderContext::render_layer`) costs about five times the whole core frame (`Surface::frame`).
Most of `draw` is `RoundedClipBuffer::bind`, which maps, fills and unmaps the clip constant buffer
for every batch (`Map` with `WRITE_DISCARD`), then `TextureStore::bind`.

- [x] Count batches and clip uploads per frame. The hover page submitted 4,001 draws a frame with
  one clip stack among them all: the clips were the same every time; the batches were many. Each
  cell's background (straight alpha) and its label (subpixel text, dual-source blended) had
  different blends, so every cell split the batch twice.
- [x] Straight alpha draws join subpixel text batches: the pixel stage writes a straight alpha
  draw's per-channel alpha as its alpha, so it blends identically there (on every backend, the
  reference renderer included). The hover page went from 4,001 draws a frame to 1 and its
  submission loop from 0.58 ms to 0.001 ms; the primitives sample from 60 draws to 21.
- [x] Dropped the per-frame clip-stack upload rewrite: batch fusion reduced the hover page from
  4,001 draws to one and submission from 0.58 ms to 0.001 ms. There is no remaining measured
  upload bottleneck on these pages; revisit only with a many-distinct-clips profile.
- [x] Measure: the hover page's first frame describes in under 15 ms. Final three-run range
  12.660–13.826 ms; twenty-run median/p95 12.888/14.593 ms, with a 15.486 ms maximum.

## Phase 5: the first time a page opens

Current compiled-artifact baseline (`strata_desktop --profile-showcase src/main/resources`):
overlay setup 56.883 ms, artifact decode 9.360 ms, activation 10.966 ms. Its first surface frame
was 5.383 ms, packet encoding 11.330 ms, GPU submission 4.897 ms; this run reported zero
diagnostics. The core benchmark's setup instead compiles source, so it is not an activation
baseline for the bundled binary. That report did not separate validation, indexing and lowering.

Five compiled-showcase launches with temporary subphase clocks measured validation
4.439–5.203 ms, indexing 0.415–0.569 ms and lowering 4.230–5.114 ms; artifact decoding measured
7.896–8.716 ms. Twenty alternating matched pairs then compared exact input-count reservations
for lowering's temporary arrays and borrowed state-sort keys against the prior implementation.
Median indexing fell 0.416 to 0.393 ms (18/20 paired wins), lowering 4.109 to 3.978 ms
(15/20), and activation 9.163 to 8.894 ms (14/20). Keep these bounded allocation reductions.
The candidate had one 15.357 ms activation outlier. Decoding also moved despite having no code
change, so its 0.499 ms median difference is not attributed to the lowering changes.
Every launch reported zero diagnostics. The temporary clocks have been removed.

Validation/lowering fusion is not the selected fix: lowering does not visit every semantically
validated category, including unused animation/material declarations. Keeping the complete
trust-boundary pass avoids turning a performance change into weaker malformed-artifact rejection.
Session preparation still moves the entire validated activation off the UI thread in the threading
plan. Three uninstrumented launches after removing the probes measured activation
9.089–9.323 ms and overlay setup 46.268–48.066 ms, with zero diagnostics.

After the cold-description work, three showcase `first` runs measured BLUEPRINT opening at
13.322–14.424 ms, description at 5.038–5.366 ms and description build at 3.818–4.124 ms.
These are the interpreter investigation's current baseline, not the standalone sampling fixture.

An 80-launch sample filtered to `DescriptionBuilder::build_layers` selected 461 stacks:
268 included expression evaluation, 155 function invocation, 53 lexical dependency recording,
34 value construction and 13 UTF-8 validation. Block-function binding appeared only once;
precomputing its declaration-presence flag is not the selected target. Existing constant-function,
call and predicate caches remain in use. The next candidate concatenates validated values without
rescanning the output or copying existing strings into formatting temporaries.

Twenty alternating showcase-first-open pairs measured median BLUEPRINT description 5.311 to
5.090 ms. Mean paired description reduction was 0.318 ms (approximate 95% mean interval
0.084–0.551 ms); keep the change. Build alone remained less certain: median 3.968 to 3.831 ms,
with its paired mean interval crossing zero. The whole opening also remained noisy
(13.123–16.507 ms before, 12.961–16.247 ms after), so no frame-latency guarantee follows.
Five focused suites passed. Unicode composition, mixed-value formatting, embedded zero bytes,
prior-value immutability and rejection of invalid raw UTF-8 are pinned. The real BLUEPRINT
interaction replay showed the starter output and the rewired result correctly, without diagnostics.
Broader interpreter/frame-storage rewrites are not justified by this profile; binding declarations
and scope creation were not dominant costs.

Phase 5 passed the final Windows **85/85** and Vulkan **95/95** suites on the retained tree.
The final wrapped-scroll, direct-scroll and hover stress replays also matched across CPU, D3D11
and Vulkan (4/4, 4/4 and 1/1 frames respectively, plus nested-layer isolation). Widest mean
channel difference remained 0.0188599; outlier share remained 0.000003125. The temporary sampler
and frozen A/B executables were removed after recording their evidence.

Opening the showcase (F7) and opening each of its tabs the first time hitched: measured with
`strata_core_benchmark <resources> <showcase.json> first` (each tab's first frame, with a profile
breakdown) and on the real GPU with the desktop harness (`--performance`, a scenario pressing F7
and then clicking every tab).

- [x] Opening the showcase: 220 ms to about 55 ms of setup, and its first frame 46 ms to 27 ms.
  Animated images play from their bytes (160 ms to 10 ms); resources are read whole and unbuffered
  (`base::read_file`: MSVC's `ifstream` copied the 5 MB artifact, fonts and images through its own
  buffer, 3.7 ms to 1.3 ms for the artifact alone); host snapshots are written compact straight
  from the `Value`, and generated contracts move a snapshot into place instead of copying it out
  of an initializer list (binding the showcase's data 26 ms to 16 ms); portable IR validation
  builds its error context only when it fails.
- [x] Large text is drawn from outlines (vector glyphs), each built at five detail levels and
  fitted to the pixel grid at its size, one glyph after another as planning asked for it: 14 ms
  for the TYPE page's 108 glyphs and 6 ms for their 165 fitted sizes, 20 ms of the showcase's first
  frame too. `VectorGlyphPool::prepare` builds a frame's new glyphs and sizes at once over the
  raster pool's lanes (`font::parallel_for`, the atlas's threads made a shared primitive), before
  planning finds them.
- [x] MSDF glyphs for text effects (a glow's reach widens their range to 18 to 46 pixels) measured
  every edge of a row at every sample. Each edge now measures only the samples within the cutoff
  of its line, with its direction and reciprocal length worked out once: the same bytes, the
  atlas's share of the TYPE page 10 ms to 6 ms.
- [x] Shaping: variable font instances vary advances per glyph when asked; GPOS pair tables are
  read where they are rather than expanded into maps; positioning works out each glyph's lookup
  class once per run and skips mark attachment for a run with no marks. OpenType time for the
  LAYOUT page's 104 new labels 6.7 ms to 2.2 ms.
- First opens now (core benchmark): TYPE 48 ms to 30 ms, LAYOUT 28 to 23, CONTENT 42 to 20,
  BLUEPRINT 16, LOTTIE 13, DATA 8; on the GPU the worst first frames are TYPE 37 ms, LOTTIE 23
  (8 ms of it uploading textures), LAYOUT 25, CONTENT 21, DATA 21, BLUEPRINT 20.
- [x] Attribute compiled activation and reduce its measured allocation costs. Exact lowering
  reservations and borrowed state-sort keys are retained; validation/lowering fusion is rejected
  here to preserve complete semantic rejection. Current subphase and A/B evidence is above.
- [x] Session preparation is owned by Phase 3 of `frame-threads.md`, not implemented here:
  activation and surface preparation leave the frame, with owner-thread adoption. That requirement
  remains mandatory there and depends on its shared task pool and cache/service thread audit.
  A separate worker mechanism here would duplicate that ownership and create a circular prerequisite.
- [x] Profile BLUEPRINT's description/interpreter work. Validated-value string composition is
  retained on measured description improvement; broad interpreter rewrites and declaration-scan
  caching are not justified by the sampled costs. Interaction and both backend gates passed.
