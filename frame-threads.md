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

- [ ] `base::TaskPool` replaces `font::RasterPool`: priorities (frame-critical work the frame
  waits on within its budget; prefetch for content about to show; idle), cancellation (a task for
  content that went away is dropped before it runs), and completions posted to the owning
  runtime's queue rather than read by the caller. `parallel_for` stays as its blocking form.
- [ ] A completion wakes the host: a thread-safe `strata_runtime_request_frame` the pool calls
  (through a host adapter) so a host that sleeps while idle frames again when work lands.
- [ ] `Pending<T>` in the core: a resource is pending, ready or failed; the frame reads it without
  blocking and the description records which nodes waited on what, so the inspector and tests can
  see it.
- [ ] Deterministic mode: a runtime option runs tasks inline at the point they are posted, so
  every existing headless and golden test keeps its exact frames; a `settle` call frames until
  nothing is pending, for tests of the asynchronous path.
- [ ] Measure: the pool's queue depth, task latency and per-kind time in the profiler, per frame.

## Phase 2: glyphs and images arrive when ready

- [ ] The glyph atlas and vector glyph pool answer a miss with pending. The frame rasterizes or
  builds what fits its budget itself (on the pool's lanes, as today) and queues the rest; a run
  whose glyphs are pending draws nothing for them, or its last preparation if it has one, and is
  planned again when they land.
- [ ] Image decoding (still images, the first frame of an animation, SVG) and Lottie
  precomposition go to the pool: a Surface is created with its images pending, not decoded.
- [ ] Font loading and system fallback resolution go to the pool: text in a script no loaded font
  covers lays out with the fallback pending, its line metrics from the primary font, and lays out
  again when the font arrives (browsers' font-display: swap).
- [ ] Measure: TYPE's first frame under 8 ms on the UI thread, its headings visible within three
  frames; the showcase's first frame under 8 ms.

## Phase 3: sessions prepare off the UI thread

- [ ] Audit every cache shared between runtimes (font catalog, shared raster cache, image caches,
  shader caches, the builtin catalog) and make each either immutable after construction,
  internally synchronized or per runtime. Document the result in `docs/embedding.md`.
- [ ] Host services and adapters declare their thread: resources and text reads are callable from
  any thread; IME, clipboard and platform effects only from the UI thread, and a session being
  prepared does not call them.
- [ ] `host::Session::prepare` (and its ABI): creates the runtime, publishes its first snapshots,
  activates the module and creates the Surface on a worker, and hands back a session the UI thread
  adopts. Snapshot publication parses its JSON on a worker too and adopts at a frame start.
- [ ] The desktop host opens every overlay this way: pressing F7 starts preparing; the overlay
  shows on the frame after it is ready (or with a spinner after a budget).
- [ ] Measure: no frame over 8 ms on the frame F7 is pressed or any after it; the overlay visible
  in under 60 ms.

## Phase 4: boundaries that hold content until it is ready

- [ ] A language-level boundary (`Suspense`-like: a widget or a node property) that shows its
  previous content, or an author's fallback, while anything inside it is pending, for at most a
  given time, then shows what is ready. A page switch inside one holds the old page instead of
  flashing a half-made one.
- [ ] The showcase's page host uses it, so a tab opens whole.
- [ ] Measure: opening each showcase tab never shows a frame with its text missing.

## Phase 5: the render thread

- [ ] Packets become owned, refcounted, immutable handles (`strata_render_packet`) instead of
  bytes borrowed for one call, so a host can keep the last commit while the next is made.
- [ ] Resource operations in a packet (texture uploads, vector glyph appends, raster jobs) are
  applied by the render thread in commit order; nothing the core does assumes they have run.
- [ ] The desktop, D3D11 and Vulkan hosts draw on their own thread: it presents the latest commit
  of each layer at vsync; the UI thread frames when there is input, time-driven work or a
  completion, and commits when it has something new.
- [ ] Hosts that embed Strata in their own render loop (a game) keep the single-thread mode: the
  same packets, drawn when they ask.
- [ ] Measure: with a synthetic 100 ms stall on the UI thread, the window keeps presenting at the
  display's rate (frame times from the render thread, not the UI thread).

## Phase 6: the compositor moves what it can

- [ ] Scrolling: a scroll view's content is a presentation group (Phase 1 of `frame-cost.md`);
  the packet marks it scrollable with its extent and physics. Wheel, touch and drag on it are
  answered by the render thread, which updates the offset every frame and reports it back to the
  UI thread as an event (for virtualization, scroll-linked effects, `strata.scroll`).
- [ ] Time-driven presentation values: an animation of a group's transform, opacity or tone that
  depends on nothing but time (keyframes, springs with a known target) is sent as a track in the
  packet and evaluated by the render thread; the UI thread only hears when it ends.
- [ ] Materials and art that read time or the pointer already run on the GPU each frame; the
  render thread feeds them its own clock and pointer, so they keep moving through a UI stall.
- [ ] Measure: scrolling the 2,000-cell page and the LAYOUT page's reading pane during a 100 ms
  UI stall keeps every frame under the display interval; input-to-scroll latency is one frame.

## Phase 7: parallel and sliced preparation

- [ ] Measuring: a layout pass shapes all of its new text runs on the pool's lanes before it
  arranges (shaping is per run and independent), instead of one after another as it measures.
- [ ] Describing and laying out a large new subtree is split across UI-thread turns with a
  budget, the render thread showing the previous commit meanwhile; a boundary (Phase 4) decides
  whether the half-made subtree shows.
- [ ] Measure: the LAYOUT and CONTENT pages' first frames under 8 ms of UI-thread time each.

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
