# Frame threads

## Goal

A UI never freezes because it is doing big work. Input, scrolling and running animations keep
their frame rate while new content is prepared; new content arrives a frame or a few late and
never as a stall. Opening the showcase (F7), opening a page the first time and loading a large
document must not drop a frame on the path the user sees.

Today one thread does everything a frame needs, in order: the host synchronizes snapshots, every
session describes, lays out, renders and encodes its packet, then the host submits the packets to
the GPU and presents. Anything slow anywhere in that chain (activating a 5 MB module, rasterizing
a page of glowing headings, shaping a hundred labels) is a frame the window does not draw.

### How others build it

- **Browsers** split the work: the main thread runs script, style and layout and *commits* an
  immutable layer tree; the compositor thread draws the latest commit every vsync, scrolls and
  runs transform and opacity animations on it without the main thread, and answers scroll input
  itself; raster threads draw tiles; images decode and fonts load off the main thread and are
  shown when ready. A navigation keeps painting the old page until the new one has something to
  show (paint holding).
- **Flutter** has a UI thread (build, layout, paint into a layer tree) and a raster thread that
  draws the last submitted layer tree; heavy work goes to isolates.
- **React** (concurrent rendering, Suspense) prepares a new tree in slices between frames and
  keeps the old one on screen until the new one is ready, or shows a fallback a boundary names.

The shared idea: a frame on screen depends only on already-prepared content; preparing new
content never blocks it.

### What Strata already has

- The render packet is a complete, self-contained frame (commands, geometry, resources), the
  natural commit: the GPU side needs nothing else from the core.
- Presentation groups carry each group's transform, opacity and tone separately from geometry
  (`RenderPacket::groups`): moving a group changes the table, not the draws. Phase 1 of
  `frame-cost.md` puts scroll offsets there too.
- `font::RasterPool` and `font::parallel_for`: persistent worker lanes, today used for atlas
  misses and vector glyphs within a frame.
- `ImageStream`: a worker decoding ahead of a player that adopts frames on the Surface thread,
  the pattern every async resource should follow.
- A frame raster budget (`raster_deadline`): text that only changed scale may draw its last
  preparation, resampled, until the budget allows rasterizing it again.
- A runtime is free-threaded but never re-entered: calls may come from any thread, never two at
  once (`EntryGuard`). Separate runtimes may already run on separate threads.

## Before this plan: finish frame-cost.md

**Do not start this plan until `frame-cost.md` is done.** Its open phases come first, in its
order:

1. Phase 1 (scrolling moves a layer, not the page) is a hard prerequisite. A scroll offset has to
   be a transform on a presentation group, not a position baked into every record, before a render
   thread can scroll content without the UI thread (Phase 6 here). Every reader of record
   positions it lists must be moved to the new model first.
2. Phase 2 (recompose a parent from the children that changed) and the open items of Phases 3 to 5
   land next, so the UI thread's own frame cost is as low as it goes before work is split across
   threads. Threading a frame that does needless work only hides the work.

Check every box in `frame-cost.md` (or record why an item was dropped) before ticking anything
here.

## The target

Three kinds of threads, each with one job:

1. **The UI thread** (one per runtime, the host's choice of thread) runs actions, state,
   description, layout and render, and commits a packet. It never waits on the GPU or on a worker:
   whatever is not ready yet is pending in the commit.
2. **The render thread** (one per window, owned by the host) draws the latest committed packet of
   every layer at vsync. It owns the GPU, applies presentation group values (scroll offsets,
   transform and opacity animations) itself every frame, and answers scroll input on content the
   packet marks scrollable. It never calls into a runtime's state.
3. **Workers** (one engine-wide pool) do everything heavy that has a result: glyph rasters,
   MSDF and vector glyphs, image and video decoding, font loading and fallback, shaping large
   text, module decoding and activation, host snapshot parsing, path tessellation. A result
   arrives as a completion the UI thread adopts at the start of a frame.

And one rule for what the user sees: **nothing pending is drawn wrong.** A pending resource has a
defined look (the previous content held, an author's fallback, or nothing yet), never garbage, and
a boundary may hold the previous content for a bounded time so a page appears whole.

## Phase 1: one task pool, and readiness in the core

Execution, October 31:

- Prerequisite work is committed in `899bda0`, `7b2e88d`, `d65d320` and `c84edca`.
  `frame-cost.md` records the measured changes, rejected alternatives and final Windows/Vulkan
  gates. No threading implementation is included in those commits.
- Three fresh committed-tree showcase runs measured TYPE first-open at 29.048–29.692 ms
  (packet encode 18.226–18.672 ms), LAYOUT at 24.769–26.623 ms, and CONTENT at
  18.089–18.831 ms. Initial showcase frames were 18.562–20.589 ms. These are the blocking
  preparation baseline, not the later render-thread presentation measurements.
- The old raster pool has three persistent helpers and a process-wide batch mutex. Its atlas
  caller hands out contiguous font/size runs; that locality and caller participation must survive
  the cutover. General work uses priority queues, not another worker set.
- Owner completions currently drain before a Surface's idle fast path. Posting does not wake the
  host, and existing async ABI admission reads a lazily installed application pointer without
  synchronized publication. A lifetime-independent completion/wake endpoint must close before
  runtime destruction; worker code must never retain a raw runtime or mutate Surface flags.
- Implementation order: shared pool and blocking-call migration; owner queue/cancellation and
  deterministic execution; ABI/host wake plus settle; resource-readiness observation and profiler
  integration. Verify cancellation, nested blocking work, owner teardown, multiple Surfaces,
  synchronous completion and async settling before the phase's backend gates.
- The shared base pool now runs atlas and vector preparation, with the old raster pool removed.
  Queued helper batches withdraw when their caller finishes, so a busy background worker cannot
  hold a completed blocking batch hostage. Isolated regressions cover priority ordering, cancellation
  before work and before adoption, failures, inline execution, owner close during preparation,
  and nested batches with occupied helpers. The Windows build and core/packet/layering suites pass.
  Three real showcase runs measured TYPE 28.962–33.166 ms, LAYOUT 23.806–26.536 ms and CONTENT
  19.536–20.695 ms. This blocking-only migration establishes correctness, not a frame-time win;
  async runtime adoption and its latency/profiler gates remain open.
- Runtime ownership, ABI v8 task modes and coalesced wake callbacks now pass the Windows build
  and seven focused runtime/ABI/host suites. The real desktop F7 path ran with zero diagnostics;
  that run spent 17.549 ms in showcase core and 11.501 ms encoding its packet, so preparation
  remains blocking at this stage.
- Pending readiness is now observed by native description hooks, including widget-call memo
  dependencies, and projected in node inspection. Regressions cover ready and failed adoption,
  a cached pending call surviving an unrelated state write, independent Surfaces, unchanged prior
  descriptions, and per-Surface task-profiler deltas. The next Windows build passed core,
  profiler, description invalidation, ABI, UI and layering (6/6). The portable real host rendered
  its initial/clicked captures with zero diagnostics; `after_click.png` was inspected.
- Task counters are cumulative at the runtime endpoint and differenced independently per Surface,
  including packet preparation. They report queue/running/pending depths, terminal counts,
  submission-to-adoption latency, queue time and per-kind work time, without worker access to
  owner profiler sections. Bounded asynchronous `settle` is implemented; proof with actual
  resource preparation follows the first Phase 2 consumer. Backend-wide gates remain open.
- The resource ownership audit found process-shared, synchronized font parse/outline/bitmap
  caches and exclusive FreeType face leases (not thread-local faces). TextEngine, atlas packing,
  vector buffer adoption and runtime image registries remain owner-only. Worker inputs must own
  bytes/font handles; the current ResourceLoader borrows runtime adapter storage. Animated image
  streaming cannot place its blocking producer unchanged on the three-helper shared pool.
- The complete Windows gate passed 84/85, with only generated diagnostic documentation stale
  after the new ABI diagnostics. `strata_authoring --write .` regenerated the contracts and the
  authoring check then passed. The complete Vulkan build and suite passed 95/95, including the
  installed consumer. No compiled-code failure was hidden behind stale tests.
- The first Phase 2 consumer now proves async settling: the real portable application prepared
  vector glyphs and grid fits in three frames at timestamp zero. Its settled initial and clicked
  PNGs are byte-identical to the deterministic captures, with zero diagnostics. Both pending and
  settled images were inspected. Scenario `taskExecution` and a bounded `settle` operation expose
  this path without changing existing golden scenarios.

- [x] `base::TaskPool` replaces `font::RasterPool`: priorities (frame-critical work the frame
  waits on within its budget; prefetch for content about to show; idle), cancellation (a task for
  content that went away is dropped before it runs), and completions posted to the owning
  runtime's queue rather than read by the caller. `parallel_for` stays as its blocking form.
- [x] A completion wakes the host: a thread-safe `strata_runtime_request_frame` the pool calls
  (through a host adapter) so a host that sleeps while idle frames again when work lands.
- [x] `Pending<T>` in the core: a resource is pending, ready or failed; the frame reads it without
  blocking and the description records which nodes waited on what, so the inspector and tests can
  see it.
- [x] Deterministic mode: a runtime option runs tasks inline at the point they are posted, so
  every existing headless and golden test keeps its exact frames; a `settle` call frames until
  nothing is pending, for tests of the asynchronous path.
- [x] Measure: the pool's queue depth, task latency and per-kind time in the profiler, per frame.

## Phase 2: glyphs and images arrive when ready

Execution design:

- As in a browser's display-item cache, preparation readiness belongs to the cached draw that
  consumed it. A pending vector result is not an unsupported outline and must never fall through
  to synchronous rasterization. Ready transitions replan affected text draws, not the whole
  description or layout. A waiting draw may sleep until the runtime completion wake.
- Worker inputs own font handles, outlines, hint zones and scalar raster settings. Atlas packing,
  vector indices and ordered packet appends stay on the owner. Batch raster requests by font/size
  to preserve the measured face-lease locality. Cancel unobserved queued preparation.
- Resource reads and copies remain serialized until the adapter ownership/thread contract has
  been migrated. Moving only image inspection is insufficient: the current first-use packet
  upload decodes still images again. The prepared result must contain usable pixels.
- Verify the first real async glyph consumer with native headless settling before closing the
  Phase 1 settle acceptance. Keep the old deterministic frames for goldens and compare settled
  asynchronous output against them.
- Vector glyphs and grid fits now prepare in owned batches, using worker-side lanes. Font handles
  and once-initialized hint zones survive Surface teardown. Cached text carries readiness reads;
  adoption replans affected draws even when command storage is unchanged. Pending vectors never
  fall through to synchronous rasterization, and unobserved queued batches are cancelled.
  The Windows build and core, packet, font and rendering-differential suites passed (4/4).
- One actual showcase first-frame run measured core 7.214 ms and packet encoding 0.900 ms,
  versus the earlier blocking run's 17.549/11.501 ms. It reported one running/pending vector task,
  with queue latency 14.3 microseconds. This is an initial measurement, not the phase gate:
  pending text is omitted, raster/MSDF work still blocks, and repeated first-page/visibility
  measurements remain required.
- Before moving raster and font-catalog operations, their process-shared cache lifetime needs
  the same audit as their locking: FreeType and bitmap-cache function statics currently destruct
  at process teardown, whereas detached preparation can outlive a runtime. The vector work above
  uses retained per-font data rather than those raster-cache globals.
- Packet readiness now reaches actual retained text consumers, including source paths and a
  `phase` distinguishing packet preparation from description observations. Owner adoption reports
  preparation exceptions as `STRATA.TASK.PREPARATION_FAILED` while still publishing terminal
  resource state. Replacing pending text with a non-text draw cancels its unused work and clears
  its dependencies. The Windows build and seven focused suites passed; the real portable pending
  inspection identified the waiting labels and increment button.
- Atlas coverage, subpixel, color and MSDF misses now prepare in owned batches. Font/size-sorted
  contiguous runs preserve face-lease locality; workers retain font handles and a shared FreeType
  context whose faces independently retain their library. Shared bitmap-cache insertion and atlas
  packing stay on the owner. Standalone atlases create their deterministic owner only on a miss.
  Asynchronous cold misses are queued rather than speculatively rasterized on the UI thread:
  the measured cold work is large, and a non-preemptible glyph cannot promise to fit a deadline.
- The Windows build passed. Core, UI, layering, rendering differential and portable headless passed
  (5/5), then font residuals and the final packet regressions passed. The latter exercise coverage,
  subpixel and MSDF adoption without command rebuilds, valid text held during rescaling, rejection
  of UVs after display-scale invalidation, and removal before adoption. Color emoji were also
  exercised by the real TYPE replay. Two initial test assumptions were corrected, not engine
  behavior: presentation zoom has a wall-clock debounce, and retained geometry buffers may contain
  unused slots; assertions now inspect active draw ranges rather than storage length or material
  lowering names.
- Real D3D11 headless TYPE replay produced byte-identical settled asynchronous/deterministic PNGs.
  Pending and settled images were inspected. Both executions reported the same
  `STRATA.DSL.RUNTIME_MISSING_PROPERTY` from the existing headless showcase snapshot; neither
  reported a preparation failure. The real desktop initial-frame profile reported zero diagnostics,
  core 6.113 ms, packet encoding 0.900 ms and overlay setup 47.688 ms.
- Three uncontended asynchronous first-open benchmark runs measured initial frames 6.553–6.668 ms,
  TYPE 13.295–14.045 ms, LAYOUT 20.414–20.960 ms and CONTENT 15.648–17.543 ms. The earlier three-run
  sample overlapped one desktop profile and is not used as the timing gate. TYPE is materially below
  the committed blocking baseline of 29.048–29.692 ms, but still above 8 ms. Its last run attributed
  3.706 ms to font resolution and 4.031 ms to packet encoding. Keep the raster cutover; font loading,
  variable instances and fallback remain on the critical path. These are UI costs, not proof of
  visibility within three display frames. The benchmark used the showcase headless fixture with
  `taskExecution: "asynchronous"` and mode `first`; settled captures remain under
  `build/headless/frame-threads-raster-{async,inline}`.

Font-preparation dependency steps:

- Cached measurement and paint now retain independent preparation observations. Font adoption marks
  text dirtiness, including input geometry; node-owned measurement metadata prevents equal local
  IDs in different trees from sharing a wrap constraint. Catalog generations replace the two
  dedicated font threads and their name-index wait; enumeration/parsing happen outside publication
  locks, and queued queries retain their directory snapshot.
- Named fonts, automatic fallback and variable instances now prepare owned inputs on the pool.
  Layouts retain source clusters and valid primary metrics while a fallback is pending. Live-reader
  cancellation spans every consuming tree; unused ready instances are discarded before adoption.
  The Windows build and eight focused suites pass, including terminal misses, independent measured
  wraps, shared-reader removal, cancellation/reappearance, and deterministic caret/selection parity.
- Real settled async/deterministic TYPE captures are byte-identical. Inspection caught a regression
  that tests initially missed: script-preference initializer-list entries had empty names in the
  Windows build, selecting monochrome symbols before emoji. Inline candidate arrays fix it; a
  portable script-precedence regression and a new actual capture confirm colored emoji. Temporary
  tracing was removed. The replay still reports the same headless host-snapshot missing-property
  diagnostic, not a preparation failure.
- Three uncontended runs after font resolution moved measured TYPE at 10.177–10.315 ms, versus
  13.295–14.045 ms after the raster cutover. Font resolution fell to 0.182–0.215 ms; packet encoding
  remains 3.454–3.753 ms. Initial frames were 6.578–7.192 ms, LAYOUT 20.401–20.545 ms and CONTENT
  15.327–16.072 ms. Keep the measured improvement, but TYPE's 8 ms and three-display-frame gates
  remain open. Bound initial font parsing and image/Lottie preparation are still synchronous.

- [x] Record resource reads around cached measurement and widget paint operations. A completion
  marks only affected retained consumers; shared TextEngines must not confuse two trees' node IDs.
  Verify cached consumers, removal, and text-dependent input geometry.
- [x] Retain catalog generations and parsing-cache lifetime before detached font calls. Font
  selection must follow stable candidate precedence, not concurrent cache-population order.
- [x] Prepare owned initial font tables, named/system fallback and variable instances; publish
  only on the owner. Pending fallback keeps primary metrics and source clusters without premature
  missing-font diagnostics. Compare settled layouts and repeat TYPE attribution.

- Bound font tables now copy adapter bytes on the owner and parse as one worker-owned registry.
  Pending lookup is explicit through layout and packet preparation; no bootstrap face is parsed
  synchronously. Native/ABI regressions cover source offsets and line boxes, atomic failure, borrowed
  adapter storage replacement, owner-only callbacks, optional diagnostics, teardown and deterministic
  errors. The Windows build and nine focused suites pass; settled TYPE captures still match.
- The three-frame visibility gate is **not** passed. An actual D3D11 capture probe spent
  29.887–63.954 ms per frame including GPU readback, so it could not establish 60 Hz cadence.
  Its third reduced-motion frame still omitted emoji and variable-font glyphs; three packet
  preparation jobs remained. With authored motion, the outgoing EXTEND page was still displayed.
  Captures are under `build/headless/frame-threads-font-cadence{,-reduced}`. Font/instance/glyph
  staging and the page transition need explicit latency work; do not treat `settle` parity as proof.

Image-preparation dependency steps:

- [x] Introduce persistent GIF/WebP/PNG frame cursors. Both collecting decode and bounded streaming
  use the same cursor; preserve disposal, looping, validation budgets and hidden-default APNG.
- [x] Replace ImageStream's joining/blocking producer with one exclusively owned cursor and at most
  three reserved lookahead frames. Each pool slice returns at capacity; owner adoption transfers
  mutable continuation ownership. Verify cancellation, looping, deterministic clocks and failure
  after a valid prefix.
- [x] Prepare usable raster pixels, first animation frames and validated SVG off-owner. Pending
  registry lookup must reach cached measurement/paint, resource frame callbacks and texture uploads;
  no second still-image decode may remain on packet encoding.
- [x] Stage Lottie parsing, external asset reads/decoding and UI-layer scene preparation using owned
  inputs. Preserve last usable frames, source replacement, asset failure and nonblocking teardown.
  - [x] Retain immutable widget command runs through cached body/presentation fragments. Verify
    unchanged rendering and measure first-page costs before relying on zero-copy worker adoption.
  - [x] Stage document discovery, owner-only external reads and worker asset decoding; publish a
    complete animation and its usable asset textures. Verify terminal source and per-asset failure.
  - [x] Prepare font-backed scenes and bounded latest-frame requests. Verify fixed-time settling,
    moving-time progress, source replacement and close with work pending; compare actual captures
    and repeat the twelve-animation first-frame measurement.

- The cursor dependency passes the Windows build and seven focused suites, including the existing
  Pillow pixel fixtures and damaged-input corpus. Owned cursors survive transfer, resume with
  disposal state intact, rewind exactly, and stream beyond the collecting frame-count budget.
  PNG validates the complete container without retaining an animation-length frame table or copied
  compressed payloads; its second walk retains only the current frame. A real D3D11 application
  rendered GIF, APNG, animated WebP and progressive JPEG with zero diagnostics
  (`build/headless/frame-threads-codecs/output/codecs.png`).
- `Pending<void>` adds readiness for owner-side transfer of exclusive mutable results. Native
  regressions cover deferred and inline adoption, late cancellation, and terminal publication when
  an adoption callback throws. Workers do not receive owner state through this API.
- ImageStream now transfers its exclusive decoder between bounded pool slices and owner adoption.
  Ready plus reserved lookahead never exceeds three frames; a full queue occupies no worker.
  Initial preparation retains the second frame and its continuation rather than decoding the
  prefix again. Playback follows owner timestamps, holds the current canvas when starved, gives
  a late frame its full delay, and preserves a valid prefix when a later decode fails.
- The Windows build and nine focused suites pass. Real D3D11 captures at 0, 300 and 1500 ms are
  byte-identical between asynchronous and deterministic execution, with zero diagnostics. GIF,
  APNG and WebP visibly advance; JPEG pixels remain unchanged. Evidence is under
  `build/headless/frame-threads-codecs/{asynchronous,deterministic}`.
- Initial raster and SVG preparation now publishes one coherent resource from owned input.
  Static ids remain reserved while pending/failed; dimensions-only native fixtures still work.
  Prepared RGBA8 replaces encoded texture inputs, removing packet-thread decoding. Retained
  metadata revisions handle runtime appearance, size replacement, release and re-publication
  without invalidating pixel-only updates or unrelated consumers.
- Animation metadata no longer belongs in the motion configuration cache. Image uses the generic
  widget frame lifecycle, with retained preparation observations and a resource-only cost; its
  obsolete one-hour timeline and predecoded frame-alias path are removed. A completion reactivates
  the specific callback even at the same timestamp, without rebuilding pixel-only geometry.
- The first frame-callback gate exposed Lottie's zero-delta boundary bug: forward playback treated
  its starting position as completion. Completion is now direction-sensitive; actual framebuffer
  tests cover forward and reverse playback and their completion markers.
- A stricter starvation regression rejected advancing playback inside decoder adoption: that used
  the previous frame's clock and shortened a late canvas. Adoption now publishes frames only;
  the observed resource callback applies the current owner timestamp. The regression failed before
  this change and passes afterward.
- The Windows build and fourteen focused suites pass. Real five-format D3D11 captures (including
  SVG) are byte-identical between asynchronous and deterministic execution, with zero diagnostics,
  at 0, 300 and 1500 ms. The active 300 ms frame performs zero description, measurement, arrangement,
  motion-player or render-node work and builds zero fragments while animated pixels advance.
  Evidence is under `build/headless/frame-threads-codecs/clock-{asynchronous,deterministic}`.
- Runtime image metadata now has its own generation hint, so ordinary pixel replacement does not
  scan dimensional consumers. Unobserved metadata signals retire when new ids are looked up or
  metadata changes, rather than accumulating every id a widget ever displayed. The final image
  tree passes the same fourteen-suite gate and actual D3D11 replay.
- Three uncontended showcase runs after image preparation measured initial frames at
  5.287/4.696/4.741 ms, TYPE at 10.109/10.993/9.529 ms, LAYOUT at 21.010/19.728/20.212 ms and
  CONTENT at 16.503/15.434/17.101 ms. Initial frame cost improved beyond the prior 6.578–7.192 ms
  range; TYPE did not establish another improvement and remains above its 8 ms gate.
- Before Lottie changes, a real scene containing all twelve bundled animations measured
  7.281/6.862/7.557 ms for its first frame. The last run attributed 5.321 ms to rendering and
  1.035 ms to packet encoding. Its D3D11 capture is under
  `build/headless/frame-threads-lottie/before`; the font-backed tile remained blank after settle.
  Catalog name prefetch is not observed by the runtime's settling endpoint. Owned scene preparation
  must resolve its fonts completely and retain immutable command runs, not copy prepared paths
  back into a new owner-side vector.
- Cached widget fragments now retain immutable command runs; complete projected vectors transfer
  ownership instead of copying path payloads. The Windows build and all 85 suites pass, and the
  actual twelve-animation D3D11 capture is byte-identical to the baseline, with zero diagnostics.
  The existing blank font-backed tile remains a scene-preparation defect, not accepted output.
- The full suite caught an input scheduling regression: ordinary frame requests must defer to a
  successor frame, whereas readiness/description activation may resume at the same timestamp.
  These operations now have distinct entry points; the existing extension timing contract passes.
- Three immutable-run measurements were 7.125/7.131/6.796 ms for the twelve-animation first frame.
  Showcase initial frames were 5.406/5.243/4.724 ms, TYPE 12.159/10.790/10.018 ms, LAYOUT
  19.373/21.776/21.286 ms, and CONTENT 16.810/17.388/18.649 ms. These overlap previous ranges;
  no standalone speedup is claimed. The retained runs are the ownership prerequisite for worker
  projection, not an additional speculative optimization.
- Lottie discovery and asset decoding now run on owned worker inputs. Resource acquisition remains
  on serialized owner calls between those stages. Publication exposes a complete immutable document
  and its usable textures; source failures are terminal and reported once, while failed assets leave
  only their layers blank. Closing cancels adoption without joining. Native regressions cover owner
  callback affinity, coherent dimensions/pixels, deferred publication, cached failures and retirement.
- The Windows build and seven focused suites pass. The real asynchronous twelve-animation capture
  has zero diagnostics; 66 pixels differ from the previous capture by at most one channel value.
  First frames measured 4.049/3.118/3.003 ms versus 6.796–7.131 ms for synchronous immutable runs.
  This is document/asset deferral only: font-backed scene preparation and frame projection are next,
  and the font tile remains blank. Evidence is under
  `build/headless/frame-threads-lottie/staged-resources`.
- Immutable scenes now retain parent-first transform plans and prepared text documents. Repeated
  glyphs share outline storage; projection never performs font lookup or layout. Each widget owns
  one active projection plus its newest request, retains compatible completed runs, and rejects stale
  source/viewport results. Source selection uses owner identities rather than string hashes.
- The first compile rejected a parameter shadowing warning; the cause was renamed before testing.
  The corrected Windows build and seven focused suites pass. Actual reference/D3D11 regressions
  cover fixed-time settling, 50 changing requests without blank frames, source replacement,
  directional completion, paused-time exclusion, reduced motion, and closing with preparation
  pending. Their font-backed area checks now settle through the runtime endpoint rather than an
  untracked font-name polling loop.
- The twelve-animation captures now include the previously blank text tile, with zero diagnostics;
  asynchronous/deterministic RGB values differ by at most one. First frames measured
  3.709/3.285/3.064 ms: the same range as staged document preparation, with scene/frame work now
  off-owner. Evidence is under `build/headless/frame-threads-lottie/prepared-{async,inline}`.
  Obsolete untracked name-prefetch APIs have been removed. Final Windows and Vulkan builds pass,
  with 85/85 and 95/95 suites respectively, including actual Lottie rendering on every available
  backend. The final D3D11 replay is under `build/headless/frame-threads-lottie/final-async`.

- [x] The glyph atlas and vector glyph pool answer a miss with pending. The frame rasterizes or
  builds what fits its budget itself (on the pool's lanes, as today) and queues the rest; a run
  whose glyphs are pending draws nothing for them, or its last preparation if it has one, and is
  planned again when they land.
- [x] Image decoding (still images, the first frame of an animation, SVG) and Lottie
  precomposition go to the pool: a Surface is created with its images pending, not decoded.
- [x] Font loading and system fallback resolution go to the pool: text in a script no loaded font
  covers lays out with the fallback pending, its line metrics from the primary font, and lays out
  again when the font arrives (browsers' font-display: swap).
- [x] Measure: TYPE's first frame under 8 ms on the UI thread (1.5-4.1 ms, later frames 6.9-8.4 ms);
  the showcase's first frame under 8 ms (5.5 ms). "Headings visible within three frames" was the wrong
  goal and is replaced: with `contentHold`, headings appear whole when ready, about 27 ms after the
  click when settled in the benchmark, rather than in a fixed number of frames.

Remaining latency dependencies:

- A real desktop TYPE activation measured 10.4956 ms core and 11.2839 ms total; its next host frame
  was 6.4625 ms core. The third host-frame capture still shows outgoing EXTEND, while the settled
  capture has the expected TYPE content. Evidence is under
  `build/headless/frame-threads-type-desktop`. Resource deferral alone has not met the TYPE gate.
- The last isolated TYPE attribution was 2.124 ms layout, 1.517 ms render, 1.373 ms description and
  3.346 ms packet encoding (1.557 ms submission planning, 1.006 ms mesh work). The remaining work
  needs the later owned-commit/content-holding/sliced-preparation mechanisms, not another claim that
  glyph or image deferral alone completes the phase. Continue those dependencies with this gate open.
- The existing desktop `--performance` path forcibly sets `uncapped = true` in `main.cpp`; not
  passing `--uncapped` does not establish vsync. Its capture maps a staging texture before Present.
  Neither that host-frame count nor the earlier per-frame readback probe proves display cadence.
- Official portable PresentMon 2.6.0 was verified against its release SHA-256, but Windows denied
  starting the ETW session: this account lacks administrative/Performance Log Users permission.
  The user subsequently authorized launching it with a UAC prompt. Use an elevated capture for the
  independent-presentation/display-frame gates when that path is running. No successful ETW trace
  exists yet; uncapped host timings remain insufficient.

## Phase 3: sessions prepare off the UI thread

Execution dependencies:

- [x] Make shared resource caches safe for concurrent reads while retaining borrowed byte lifetime;
  enforce thread-safe static initialization and verify the remaining shared-cache ownership audit.
- [x] Add an engine-owned, cancellable pre-runtime preparation endpoint through the C ABI. A
  host-static Session must never create a second pool. Work/result contexts own their inputs and
  cleanup; close discards adoption without joining, and deterministic execution stays inline.
- [x] Prepare Sessions from owned resource access and owner-captured initial snapshots. Buffer
  diagnostics until adoption and install UI-only services/actions on the owner, never invoke them
  from preparation. Migrate every desktop/showcase overlay caller.
- [x] Stage snapshot parsing with reserved producer generations and ordered owner adoption. Preserve
  failure observability and revision retry semantics instead of marking failed accepted work published.

Live snapshot cutover:

- [x] Reserve per-producer accepted generations before posting owned JSON/schema inputs; process the
  contiguous completed prefix on the owner so overlapping roots retain acceptance-order precedence.
- [x] Expose retained pending/adopted/failed/cancelled receipts and explicit owner polling. Failed
  accepted generations remain consumed; runtime close cancels outstanding receipts without joining.
- [x] Keep explicit inline publication for initial Session snapshots, rejecting it while earlier
  asynchronous publications remain pending rather than blocking a pool lane.
- [x] Migrate facade generation allocation and binding pending/adopted revision cursors together.
  Verify cross-producer ordering, failure/retry, cancellation, frame-start adoption and actual live UI.

- The snapshot cutover passes the Windows build and fifteen focused runtime/ABI/host/desktop suites.
  Regression coverage distinguishes accepted/adopted state, overlapping producers, consumed failed
  generations, cancelled predecessors, receipts surviving runtime close, direct ABI/facade
  interleaving, unchanged-revision retries and typed-key geometry at frame-start adoption.
  Dual-store publication reserves/validates both stores before either commits.
- Actual D3D11 asynchronous publication changed a blue 80×80 panel into a green 240×80 panel with
  its typed key retained and no diagnostics (`build/headless/frame-threads-live-snapshots`).
  The complete F7 replay is now valid, but still measured 10.1708 ms input, 13.6961 ms first drawable
  frame and 9.5219 ms glyph adoption. Initial model capture and owned-commit/sliced preparation are
  still required. A separate real run correctly rejected the version-1 comparison baseline.
- The first full Windows run passed 82/85. It exposed blanket snapshot-kind error suppression,
  an obsolete assumption that live snapshot admission immediately replaces Lottie input, and stale
  generated diagnostic documentation. Publication-owned failures now defer through a distinct
  outcome wrapper; unrelated snapshot tasks retain the generic reporter. The Lottie regression
  waits for the publication receipt, then still checks incompatible-frame rejection before Lottie
  settles. Authoring outputs were regenerated and checked. Corrected full builds and suites passed
  85/85 Windows and 95/95 Vulkan, including actual Lottie rendering on each backend.
  The separate Vulkan live-snapshot smoke also produced the expected 240×80 green panel with no
  diagnostics (`build/headless/frame-threads-live-snapshots-vulkan`).
- An isolated five-run capture profile measured table materialization at 3.338–3.921 ms and grid
  materialization at 0.580–0.971 ms, versus tree 0.047–0.127 ms and demo 0.009–0.013 ms.
  Table/grid JSON encoding adds 1.281–1.707 / 0.255–0.335 ms, already off-owner for Session startup
  but not live bindings. Source capture explains part of F7 admission, not its entire 10–11 ms;
  attribute the real admission stages before choosing the owned-encoding change.
- Real F7 attribution found a larger, avoidable transfer cost: 6.121 ms while appending the captured
  roots. MSVC's map-containing `Value` is not nothrow-movable, so vector growth copied prior roots.
  The source calls themselves were tree 0.103 ms, table 3.542 ms, grid 0.578 ms and demo 0.015 ms;
  Session submission was 0.226 ms. Transfer the whole vector when no explicit startup roots precede
  it and reserve once for append otherwise, in both desktop paths. All timing probes were removed.
  A new owned-encoding API is not the first fix for this measurement.
- Three uninstrumented real F7 runs after transfer measured input at 4.396/4.346/4.536 ms and the
  triggering host frame at 6.861/6.539/7.045 ms. The prior complete-path run had 10.171 ms input.
  Keep the transfer fix: it removes the measured 6 ms copy cost. The Windows build and ten focused
  host/desktop/model suites pass. Later first-drawable frames still reach 13.113–14.178 ms, so the
  all-frames/visibility gate remains open. Profiling and live-rendering throwaway sources were removed;
  reports, images and the capture-attribution data remain under `build`.

- [x] Audit every cache shared between runtimes (font catalog, shared raster cache, image caches,
  shader caches, the builtin catalog) and make each either immutable after construction,
  internally synchronized or per runtime. Document the result in `docs/embedding.md`.

- `ResourceRoot` now protects lookup/publication while leaving I/O outside its lock; an eight-reader,
  32-resource regression checks exact immutable contents and stable borrowed/shared storage.
  Thread-safe local-static initialization is mandatory. Worker-visible compiler/runtime/font/UI
  catalogs, indexes and sentinels outlive detached work; shader settings use synchronized
  process-lifetime storage without introducing an engine-private dependency into either backend.
- The Windows build and focused ownership suites pass. Actual desktop replay is valid and its
  settled TYPE capture contains the expected multilingual text, colored emoji and variable fonts
  (`build/headless/frame-threads-cache-audit`). These are correctness prerequisites, not a claimed
  speedup. Vulkan has not yet been rebuilt after this audit; its gate remains due on the next batch.
- The pre-runtime C ABI endpoint now uses the existing pool and admission policy. Owned contexts
  and results clean up exactly once; taking transfers exclusive ownership, terminal failures own
  their messages, cancellation/release discard late results without joining, and wake reentry is
  refused. The Windows build and five ABI/host/layering suites pass. A direct DLL smoke compiled
  and activated a real application on an engine worker, took its runtime on the caller, queried
  its next identity there and released it with exactly one input/result cleanup.
- Session cutover requires owner-side bindings to exist before the runtime. Capture owned initial
  snapshot values, not model callbacks; attach handlers and seed publication revisions on adoption.
  Resource access must retain a root independently of Win32 services. Material/effect source reads
  also belong to preparation; GPU declaration stays with the renderer owner.

- Session preparation and both desktop callers now pass the Windows build and all twelve focused
  ABI/host/desktop/layering suites. Eight separate real desktop runs opened every overlay and their
  captured surfaces were inspected (`build/headless/frame-threads-overlays`). Pending F7 hide/reopen
  produced an empty hidden capture and the expected reopened showcase; a separate process closed
  with `preparationPending: true` and no application frame. Native regressions cover nonjoining
  retirement, owner-handler lifetime, revision changes, required actions and restored durability.
- Performance report v2 now includes immediate input-handler cost. A measured 500 ms window includes
  preparation, adoption and glyph completions rather than stopping after a few uncapped calls.
  The first sample exposed 11.3557 ms input work, a 13.0364 ms first drawable host frame and an
  11.8365 ms glyph-completion frame. The run's only validity failure was its intentionally blank
  pending startup frame; subsequent startup scenarios explicitly permit that frame. These numbers
  explain remaining work, not acceptance. The first drawable Present returned about 67.1 ms after
  F7's measured start; this is not scan-out proof. All latency/display gates remain open.

- [x] Host services and adapters declare their thread: resources and text reads are callable from
  any thread; IME, clipboard and platform effects only from the UI thread, and a session being
  prepared does not call them.
- [x] `host::Session::prepare` (and its ABI): creates the runtime, publishes its first snapshots,
  activates the module and creates the Surface on a worker, and hands back a session the UI thread
  adopts. Snapshot publication parses its JSON on a worker too and adopts at a frame start.
- [x] The desktop host opens every overlay this way: pressing F7 starts preparing; the overlay
  shows on the frame after it is ready (or with a spinner after a budget).
- [x] Measure: no frame over 8 ms on the frame F7 is pressed or any after it: 6.44 ms core at most on
  the real desktop host.
- [ ] The overlay visible in under 60 ms is **not met**: the first drawn frame is about 80 ms after F7.
  That time is preparing the Session on a worker, so nothing on screen waits for it. Preparing the
  overlay ahead of time at startup would meet it, at a startup and memory cost for a debug surface.

## Phase 4: boundaries that hold content until it is ready

Decision, November 2 (Claude, taking over from Astra): the whole-Surface candidate fork that first
filled this section was removed. It was about 5,100 production and 2,500 test lines (tree fork and
retained journal, structural rebasing, deferred description publication, description forks, staged
function scopes, captured-read revisions, input and editor projection). No production path ever
created a candidate, and normal re-describing frames paid for capture recording and scope staging.
It also added a language rule (a pure computation could not write a captured variable) that only
candidate isolation needed. The last state is on the local branch `frame-threads-snapshot`.

Measured against that tree, three runs each, the backout changed nothing beyond noise: TYPE
8.9-9.9 to 9.3-9.8 ms, LAYOUT 18.8-20.5 to 18.5-22.0 ms, CONTENT 13.9-17.1 to 14.1-14.6 ms, tab
click 4.1-4.2 to 4.1 ms. The removal is a simplification, not a speedup.

Standalone fixes from that work stayed: equal-valued closures over different cells no longer compare
equal, a function returning a closure is not cached, image paint no longer advances playback, and
Lottie completion is direction-sensitive. `InputPresentation`, the read side of input, also stayed.

The replacement is an offstage subtree in the one tree, under the one input router, as Flutter's
`Offstage` and React's `Activity` do it: built, laid out and prepared, but drawing nothing and taking
no hits, focus or semantics. Readiness tells the page host when to flip.

Built, November 2. `contentHold` extends `contentKey`, which already retained outgoing content for
its transition: the boundary marks the outgoing subtree and the incoming one and starts neither
transition until the incoming has reached a packet with nothing pending under it, or the bound
passes. Incoming content draws through a presentation group at opacity 0, so its geometry and glyphs
are planned and uploaded and the flip changes only the group table. It takes no hits, focus or
semantics; its widgets still run, so an image's clock starts when attached. A key changed again during
a hold drops what never showed. Deterministic runtimes never hold, so golden frames are unchanged.

During the hold the outgoing page is frozen and takes no input, which differs from React's
transitions, where the old tree stays interactive. A hold is bounded (the showcase uses 200 ms) and the
tab bar outside it stays live, so a second click retargets. Keeping the old page fully live is what
required the candidate fork above, and nothing observed so far justifies that cost.

D3D11 capture of a first TYPE open, frame by frame: EXTEND stays whole while the header already says
TYPE; the crossfade's incoming page already has its emoji, Arabic, Hebrew, Han, kana and variable
fonts. Regressions cover the deadline, a clean packet, pending preparation, retargeting, returning
to the page on screen, and deterministic runtimes.

- [x] A language-level boundary: `contentHold` on `contentKey`, showing the previous content for at
  most a given time, then what is ready. A page switch inside one holds the old page instead of
  flashing a half-made one.
- [x] The showcase's page host uses it, so a tab opens whole.
- [x] Measure: opening a showcase tab never shows a frame with its text missing (TYPE, the page with
  the most cold fonts and glyphs, inspected frame by frame on D3D11).

## Phase 5: the render thread

Owned-stream prerequisite:

- [x] Publish immutable refcounted encoded storage from the existing encoder, with bounded exclusive
  spare reuse. Never rewrite a retained compact packet's frame index or replay its resource jobs.
- [x] Replace borrowed packet delivery across the C ABI and facade; preserve explicit terminal
  consumption/acknowledgement. Keep wire v25 and advance the breaking C ABI version.
- [x] Migrate every existing host and recording caller to owned encoded handles; decoded plans remain
  borrowed from one stateful decoder per consuming stream. The threaded transport below must use
  this same ownership contract, not copy or share mutable decoded frames.
- [x] Verify retained bytes through later full/compact/patch frames and Surface/runtime teardown,
  ordered appends/uploads/releases, failed publication retry and cross-thread final release.

- The ABI-v9 cutover passes complete Windows (85/85) and Vulkan (95/95) builds/suites, including
  installed consumers and actual backend rendering. Authoring outputs were regenerated and checked.
  The new failure regression preserves the prior packet and pending glyph resources through a
  rejected cyclic group table, publishes them on retry, and does not repeat them afterward.
  Retained full/patch/compact bytes and cross-thread release survive producer/runtime teardown.
- Actual D3D11 F7 replay remains valid and its settled capture was inspected. Input measured
  4.578 ms; the first drawable host frame still took 13.779 ms and glyph adoption 9.007 ms.
  This establishes the ownership prerequisite, not a standalone latency win or a presentation gate.

Render-owner implementation:

- Delayed host feedback now carries its originating frame index; late timing/spike samples do not
  overwrite current UI counters. The first Windows build and 16 focused suites passed, and actual
  F7 replay remained valid. Backend replay now checks repeated presentation after consumed jobs
  are retired, comparing actual pixels.
- The first Vulkan pass caught an invalid attempted shortcut: treating equal frame/epoch values
  as proof that a full payload is unchanged breaks low-level callers which explicitly replace
  geometry. Reverted that heuristic. Instead, the decoder exposes explicit draw acknowledgement
  for the retained-presenting consumer, leaving the low-level full-payload contract intact.
  The corrected Vulkan gate remains pending.
- The shared presentation owner now carries declarations, ordered stream commits, captures and
  terminal receipts. Session framing only acquires owned bytes; the consuming stream decodes.
  Both Windows hosts default to threaded GPU ownership, while performance/smoke/uncapped paths
  remain caller-driven. Normal close keeps the HWND alive and dispatches messages until GPU
  destruction completes. The Windows build and twelve focused suites pass, including the new
  real-pixel ordering/capture/retirement regression and installed desktop consumer.
- A real static-window probe exposed a completion feedback loop: 149,916 UI turns in roughly
  three seconds, although only one frame was presented. Resource-free superseded commits now
  discard their pending accounting without waking the producer. Skipped resource work retains
  its feedback. The corrected probe measured 108 turns, one Present and zero diagnostics.
- The first autonomous-material probe stopped after one Present: pending shaders were not marked
  as needing another frame, so the owner slept before compilation adoption. Visible pending
  shaders now keep framing until adoption. A deterministic D3D11 regression declares after the
  adoption boundary and checks fallback pixels, subsequent authored pixels and return to idle.
  Windows build, presentation and D3D11 suites pass. The real animated window completed twenty
  Presents during a 114.658 ms UI-thread stall and closed with zero diagnostics; its visible client
  capture was inspected. Present return counts alone are not scan-out proof.
- The authorized elevated PresentMon trace succeeded. QPC correlation observed nineteen displayed
  frames during a 107.819 ms UI stall: display intervals 5.5279–5.5847 ms (median 5.5570 ms),
  matching the 180 Hz display, with no undisplayed submissions during the stall. Diagnostics were
  zero and ordered close completed. Evidence is `build/frame-threads-presentation-smoke/display-*`.
  This closes the D3D11 autonomous-material display-stall sub-gate, not the Vulkan, independent
  input or scrolling gates. The existing composed-copy swap chain adds 16.1295 ms median
  Present-to-display delay; Phase 6 must account for that measured latency, not equate GPU
  submission independence with one-frame input response.
- Vulkan program replacement previously destroyed pipelines still referenced by submitted draws.
  The new real-device regression failed with `VUID-vkDestroyPipeline-pipeline-00765`; pipelines
  now retire through the same newest-reader fence as images and buffers, without an added wait.
  An initial test gate using a host-set `VkEvent` was rejected by validation (09543): a pending
  command cannot wait on the event while the host sets it. Removed that gate. Replacing an
  already-compiled program before any fence observation gives the required failing-before/
  passing-after check without timing sleeps or invalid synchronization.
- The corrected Vulkan build and eighteen focused suites pass, including low-level full-payload
  geometry, repeated retained presentation, real-pixel reloads, cross-backend replay and the new
  threaded Vulkan resource/capture/retirement regression. A separate real headless Vulkan
  application clicked its increment button and produced the expected image with zero diagnostics
  (`build/headless/frame-threads-vulkan-presentation`); its surface was inspected. This verifies
  owned GPU consumption, not a Vulkan visible-host display clock.
- A new Win32 platform regression reproduced a late capture replying to a reopened textual
  client id. Captures now retain their client lifetime and are sealed to the exact presentation
  request; later requests cannot consume an older returned image. Deferred window replies retain
  the same lifetime token, dialog replies check cancellation, and dialog threads join before the
  reply queue, mutex and OLE state are destroyed. Windows build and nine desktop/presentation
  suites pass, and F7 caller replay remains valid. A separate installed-SDK threaded application
  clicked an authored `node.capture`, adopted its image and displayed it below the original;
  both completion indicators and the copied tile were inspected, with zero diagnostics
  (`build/frame-threads-presentation-smoke/capture-column.*`).
- [x] Separate Session framing/encoded publication from decoder ownership; keep caller-thread
  streams for embedders. Renderer feedback carries the originating frame identity.
- [x] Transfer declarations, packet commits, visibility order, resize/target changes and terminal
  packets through ordered owned messages. Apply resources once even when an intermediate visual
  frame is skipped; redraw retained geometry without replaying uploads or vector appends.
- [x] Keep GPU creation, mutation and teardown on its owner. Return terminal receipts, captures,
  backdrop samples and measured frame feedback to serialized host calls; never enter a runtime
  from the render thread. HWND teardown must not deadlock a GPU call dispatching window messages.
- [x] Establish vsync-paced independent presentation and nonperturbing cadence evidence; uncapped
  performance iterations and pre-Present readback are not display-interval proof.

Visible Vulkan dependency:

- Before this cutover the SDL preview rendered offscreen, read back every frame, uploaded an SDL
  texture and called SDL_RenderPresent on the UI thread. Moving that mixed frame call would not
  establish independent presentation. SDL video/clipboard/IME remain on the main thread.
- [x] Add an owned Vulkan swap-chain driver to the shared presentation mechanism: platform surface
  creation marshalled to its window owner, GPU/device/swap-chain lifetime on the consumer, ordered
  image acquisition/submission/presentation, and capture readback only when requested.
- [x] Migrate the Vulkan preview to prepared Session publication and feedback without entering the
  runtime from the GPU owner. Preserve explicit caller/reference headless paths and coherent reload.
- [x] Build and exercise actual visible Vulkan output, resize, capture and ordered shutdown; trace
  display cadence across the same 100 ms runtime-owner stall before closing the backend gate.

- The Vulkan swap-chain driver and prepared-Session preview build successfully. An actual WSI
  smoke rendered/captured the clock material (`build/headless/frame-threads-preview-wsi.png`).
  Acquisition failure now preserves pending commits/captures for retry; offscreen completion is
  distinct from target unavailability. Resize/restore and coherent F5 shader ownership remain open.
- A deterministic real-pixel regression exposed a D3D11 coalescing error: drawing epoch 1, decoding
  changed epoch 2 and then compact epoch 2 could advance the GPU epoch without uploading vertices.
  Incremental upload now requires actual patches; otherwise the decoder's complete geometry is
  uploaded. The test target no longer resets geometry by drawing an empty packet before every
  real frame. Reference, D3D11 and Vulkan variants pass; the D3D11 variant failed before the fix.
- Each staged preview Session now owns its SDL adapters. IME requests remain staged until
  promotion; late teardown of the previous Session cannot stop the successor's text input.
  The SDL state-transition regression and eighteen affected Vulkan-build suites pass.
- The authorized visible Vulkan PresentMon trace, with validation enabled, observed eighteen
  displayed frames during a 100.4425 ms UI stall. Display intervals were 5.5143–5.5800 ms
  (median 5.5559 ms); maximum measured owner work was 2.6419 ms. The previous SDL offscreen/
  readback path displayed zero frames during its 100.0614 ms stall, with a 116.687 ms display gap.
  Evidence is `build/frame-threads-presentation-smoke/preview-{before,after}*`. This establishes
  autonomous Vulkan material cadence, not independent input, scrolling, or full Phase 5 closure.
- Actual threaded Vulkan resize, minimize/restore and ordered close completed
  with validation enabled and no diagnostics. Client captures grew from 640×420 to 804×531 and
  resumed animated output after restore (`preview-{initial,resized,restored}.png`).
- [x] Give presentation streams isolated GPU program bindings and release them at terminal
  retirement. Reproduce a partially failed candidate declaration batch retaining the old scene's
  pixels, then verify successful promotion and repeated reload without stale program ownership.
  This is document-local style/program ownership, not an F5-only resource exception.
- The real-pixel candidate regression failed on both D3D11 and Vulkan before stream-scoped program
  bindings. Both now preserve red output after a batch installs blue under the same authored id
  and then fails another declaration. The never-framed candidate retires, a new candidate reuses
  the same textual stream name, and retiring the old scene preserves the new blue program.
  A real Vulkan F5 smoke confirms red → failed reload/red → successful reload/blue, with the
  expected shader diagnostic and no Vulkan validation errors (`reload/result.json`).
- Repeating a rejected shader exposed another D3D11 bug: synchronous compilation inserted an empty
  cache entry before failing, so a retry could succeed with no shader. Material/effect programs now
  enter that cache only after successful creation. The repeated-material failure was reproduced;
  material and effect retries now remain errors.
- Target-unavailable regressions retain captures and image updates until restore without needing
  a new UI commit. Both builds pass: sixteen affected Windows suites and twenty-one Vulkan-build
  suites. One Vulkan build hit MSVC `LNK1236` in a generated decoder object; regenerating that object
  restored a successful full build before tests ran. No compiler flags or source checks were relaxed.
- Actual Vulkan gallery input created a project, native keyboard input appended text, and F5 reset
  the authored local state. The first synthetic F5 probe omitted Win32 scan codes and did not
  exercise reload; it was corrected before claiming success. Likewise, posted character messages
  were not accepted as native SDL typing; real keyboard input verified editing.
- New real-pixel regressions reproduced D3D11 effects and retained GPU art incorrectly reporting
  idle. Effect liveness now includes frame-input/pending shaders even while reusing a rate-limited
  sample; art reports its retained jobs until replacement/release. A real installed-SDK effect
  window advanced twenty Presents during a 112.2504 ms owner stall, with zero diagnostics and
  ordered close (`effect-clock.*`). Its visible capture was inspected; this count is not another
  scan-out trace. The earlier PresentMon traces remain the display-cadence proof.
- Both temporary `STRATA_PREVIEW_PROBE` production blocks were removed after preserving evidence.

- [x] Packets become owned, refcounted, immutable handles (`strata_render_packet`) instead of
  bytes borrowed for one call, so a host can keep the last commit while the next is made.
- [x] Resource operations in a packet (texture uploads, vector glyph appends, raster jobs) are
  applied by the render thread in commit order; nothing the core does assumes they have run.
- [x] The desktop, D3D11 and Vulkan hosts draw on their own thread: it presents the latest commit
  of each layer at vsync; the UI thread frames when there is input, time-driven work or a
  completion, and commits when it has something new.
- [x] Hosts that embed Strata in their own render loop (a game) keep the single-thread mode: the
  same packets, drawn when they ask.
- [x] Measure: with a synthetic 100 ms stall on the UI thread, the window keeps presenting at the
  display's rate (frame times from the render thread, not the UI thread).

## Phase 6: the compositor moves what it can

Decision, November 2: materials are done; scrolling and animation tracks on the render thread are not
built. They are not ruled out, and the reasons are below for whoever reopens this.

What the display does now. A threaded, vsynced window (`--windowed-timing`) ran F7 and then the first
opens of TYPE, LAYOUT, CONTENT and BLUEPRINT under an elevated PresentMon trace: 2,424 display intervals,
median 5.556 ms at 180 Hz, p99 5.602 ms. Three ran late (11.1, 16.6, 11.1 ms), all around F7's startup
before any tab opened; the tab opens have none. So preparing new content no longer costs a displayed
frame, which was the goal of this plan. The late three are not yet attributed.

Why not scrolling. The gate is scrolling through a 100 ms UI stall, and two things stand in the way.
Win32 input arrives through the message pump on the thread that runs the UI (`native/desktop/main.cpp`),
so the render thread never sees a wheel event during a stall. It needs a separate platform thread
first, with clipboard, IME, OLE drag and dialogs marshalled across it. Then scrolling needs offsets
owned on the render thread, with clips, sticky content, scrollbars, virtualization margins and
`strata.scroll` feedback following them. What remains to justify that is a long stall that is not
preparation: an authored handler, or one large description. Scrolling costs 0.15 ms on the UI thread, and
Flutter scrolls on its UI thread too; browsers do not.

Why not tracks. Their main case in the showcase was page transitions stuttering while fonts and glyphs
adopt. The hold now finishes that work before the transition starts. What they would still improve is
a running animation during a slice: a sliced open pauses UI-driven motion for up to three frames.

- [x] Materials and art that read time or the pointer already run on the GPU each frame; the
  render thread feeds them its own clock and pointer, so they keep moving through a UI stall
  (Phase 5's PresentMon traces, on both backends).
- [ ] Scrolling on the render thread: not built, see above. A large description still blocks scrolling
  for as long as it runs, since a stage cannot stop partway.
- [ ] Time-driven presentation values as packet tracks: not built, see above.
- [ ] Measure: scrolling during a 100 ms UI stall. **Not met**, and expected not to be: a scroll waits out
  the stall.

## Phase 7: parallel and sliced preparation

Decision, November 2: slicing comes first, and it brought every page but CONTENT under the gate, so
parallel shaping was not built. Attribution showed shaping is a small share (LAYOUT: 2.98 ms text
preparation of about 20 ms), and pool width did not help either: first opens settle alike with 3 and 7
workers, and 15 workers cost the UI thread 2-3 ms per open. The pool now uses half the logical cores,
with `strata_limit_task_workers` for embedders.

`Surface::slice_frame` stops a frame at a stage boundary (built, laid out, rendered), leaving its
dirty marks for the next frame, while a first frame or held content prepares (after 1.5 ms) and in any
frame that has spent 5 ms. Packet encoding is the last stage. Each stage stops once per packet and one
more time when already late, so a page with running motion still draws. A host opts in with
`strata_surface_set_frame_slicing`; a host framing once gets a whole frame.

A stress run with every budget forced to zero found the first bug here: inspection threw on a node
described but not yet laid out. Worth repeating when this path changes.

UI-thread time per frame (`frame` plus `packet-encode`; packet decoding belongs to the render thread),
worst of the open and every frame until 30 after settling, three runs each, showcase first opens:

| Page | Before | After |
|---|---:|---:|
| BLUEPRINT | 12.1-13.5 | 5.9 |
| TYPE | 8.9-9.9 | 7.5-8.4 |
| LAYOUT | 18.5-22.0 | 7.4-8.7 |
| CONTENT | 13.9-17.1 | 7.9-9.4 |

The "before" column is whole-frame wall time and includes about 1.5 ms of decoding, so it overstates
the comparison by that much. Other pages are under 6 ms.

The real threaded window at 180 Hz (`--windowed-timing`, three runs, largest core time in the 1.5 s after
each click): BLUEPRINT 5.4-6.0, TYPE 7.9-8.7, LAYOUT 7.2-8.4, CONTENT 8.9-9.7 ms. CONTENT's remaining
frame over 8 ms is one render, raster and encode frame. The display does not miss a frame for it, since
the render thread keeps presenting; the page arrives slightly later.

CONTENT first measured 11.9-12.5 ms here and looked like an indivisible layout stage. It was not: the
benchmark's warm caches hid it until `--only=TYPE,LAYOUT,CONTENT` reproduced the cold path, and then
layout was 4.5 ms of an 11.7 ms frame, with an authored timer in `run_timers` taking the other 7. The cut
after the build stage had only fired after a rebuild. Parallel shaping, the obvious fix for "slow layout",
would have missed it: shaping is 2.4 ms there.

The cost is latency: an open takes up to four frames more, and a tab highlight in the same frame waits
with it, so at 60 Hz a click can take about 66 ms to show. That replaces one long frame.

- [x] Describing and laying out a large new subtree is split across UI-thread turns with a
  budget, the render thread showing the previous commit meanwhile; a boundary (Phase 4) decides
  whether the half-made subtree shows.
- [x] Measure: the LAYOUT page's first frames under 8 ms of UI-thread time.
- [ ] Measure: the CONTENT page's first frames under 8 ms. **Close, not met**: 8.9-9.7 ms in the real window,
  one frame per open. TYPE and LAYOUT also touch 8.4-8.7 ms in some runs.
- Dropped: shaping runs in parallel on the pool. Too small a share on both LAYOUT and CONTENT.

## Risks

- Shared caches are the bug farm: every one found in the Phase 3 audit needs an owner or a lock
  before anything moves, and the audit comes first.
- Determinism: golden tests stay on the deterministic mode; the asynchronous paths get their own
  tests through `settle`. The Linux preview build can run them under ThreadSanitizer, which MSVC
  does not have.
- The ABI changes (owned packets, prepare, wake) are breaking for hosts; the single-thread mode
  stays for embedders that drive their own loop.
- Pending content needs a look in every widget that shows text or images; the default must be
  correct (hold or draw nothing), never stale coordinates.
