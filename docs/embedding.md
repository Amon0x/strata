# Embedding a Strata application

The stable embedding boundary is `strata/strata.h`. A C++ host may include `strata/strata.hpp` for
typed ownership and result exceptions, but both paths use the same ABI and capability model. No language VM or external build runtime is involved.

## Lifecycle

```text
negotiate ABI/capabilities
  -> create runtime + install host adapters
  -> configure application schemas
  -> publish host snapshots + register actions
  -> compile and activate source (last-good)
  -> create Surface with environment/fonts/images
  -> enqueue input -> frame -> consume packet
  -> prepare terminal packet -> consume -> acknowledge -> release Surface
  -> release registrations/snapshots/runtime
```

Every host owns its clock, environment generations, resource generations, and handle lifetime.
Separate windows should use separate runtimes when their application/input/diagnostic state must be
isolated.

### Threading and reentrancy

A runtime and its Surfaces are single-threaded. Calls may come from any thread but must not
overlap; a call made while another thread is inside the runtime returns
`STRATA_STATUS_WRONG_THREAD` without touching it. The async completion functions and
`strata_runtime_request_frame` may be called concurrently from worker threads. Host callbacks run
inside the call that triggered them and may
call back only for reads and host-service calls; release, activation, publication, input, framing,
and other structural calls return `STRATA_STATUS_REENTRANT_CALL` from inside a callback. Record the
change and perform it after the outer call returns, as `strata::host::Bindings::synchronize` does
for host snapshots. The exact list is in the Threading comment at the top of `strata.h`. No C++
exception crosses the ABI: exports are `noexcept`, and callbacks must not throw.

### Preparation and frame wakes

ABI v8 adds `STRATA_CAPABILITY_TASK_EXECUTION`, `strata_runtime_config::task_execution` and
`frame_wake`. The zero-initialized execution mode is `STRATA_TASK_EXECUTION_ASYNCHRONOUS`.
Engine workers prepare owned inputs; only a serialized owner frame adopts their completions.
`STRATA_TASK_EXECUTION_DETERMINISTIC` instead executes preparation and adoption at submission,
preserving exact-frame headless captures. The C++ equivalents are
`RuntimeOptions::task_execution` and `TaskExecution::{asynchronous,deterministic}`.

A sleeping host supplies a complete `strata_frame_wake_adapter`, or
`RuntimeOptions::frame_wake` / `host::SessionOptions::frame_wake`. The callback only signals the
event loop. It can run on a worker or during an owner call, must be thread-safe and nonblocking,
and must not enter or destroy its runtime, even for reads. Such reentry returns
`STRATA_STATUS_REENTRANT_CALL`. A callback failure is reported on the next owner frame.
An all-zero adapter disables notifications for hosts that drive their own frame loop.

`strata_runtime_request_frame` (`Runtime::request_frame`) is notification-only: it never frames,
mutates UI state, or runs adoption. Notifications coalesce until the owner drains completions at
frame start; a monotone wake generation preserves each Surface's independent observation.
Async host-data mailbox posts use the same wake endpoint. Quiesce external posting callers before
runtime release; release itself quiesces engine wake callbacks before callback storage may die.
Do not move or close a C++ facade concurrently with callers using it.

`strata_runtime_get_task_info` (`Runtime::task_info`) is an ordinary serialized query. It reports
engine tasks still queued, running or awaiting adoption, whether completions are ready, the wake
generation and execution mode. It does not count external async host requests or animations.
A Surface may sleep while preparation is pending: its completion wake, rather than polling,
requests the next frame.

Preparation exceptions publish a failed resource and an owner-side
`STRATA.TASK.PREPARATION_FAILED` diagnostic. A failure does not silently select a synchronous
fallback. Cancellation discards obsolete preparation without reporting it as a resource error.

Animated GIF, APNG and WebP playback uses bounded shared-pool slices, not dedicated producer
threads. Ready and reserved lookahead together hold at most three frames. Each slice owns its
decoder exclusively and returns at capacity; owner adoption transfers the continuation. Playback
uses the Surface frame timestamp, not wall-clock time. A late decoder holds the previous canvas
and gives the next frame its full display duration. Finite playback freezes on its last canvas;
a malformed continuation preserves its valid prefix and reports the failure on the owner.
Closing a stream cancels adoption without joining a decoder.

Static image bindings copy adapter bytes on the serialized owner, then prepare usable raster
pixels or parsed, geometry-validated SVG on the pool. Initial dimensions, content and animation
classification publish together. Declared bindings shadow same-id runtime images while pending
or failed. Deterministic mode finishes preparation during Surface creation and reports validation
errors there. Packet texture uploads accept prepared RGBA8 only; they never decode image files.

Image consumers retain preparation observations. Runtime image appearance, size replacement and
release invalidate only their metadata readers; same-size pixel updates retain fit geometry.
Animated images use resource frame callbacks rather than motion timelines. Decoder completion
rearms the callback, including at an unchanged timestamp; only that callback advances playback
with the current owner clock. Pixel-only callbacks preserve cached drawing and stop when finite
playback ends. Cached frame callbacks retain their declared paint/layout/resource cost across a
preparation wait; waiting itself does not become elapsed playback time.
Readiness/description activation may resume a hook at the current timestamp. Ordinary input and
self-rearming frame requests still target a successor frame, not another callback in the input frame.

Surface profiler counters `tasks.queued`, `tasks.running` and `tasks.pending` are runtime-wide
gauges. Submission, completion, adoption, cancellation, failure, queue time, work time and
submission-to-adoption latency are differences since that Surface's preceding frame. Per-kind
`tasks.<kind>.work_nanos` separates glyph raster, vector glyph, image, font, shaping, module,
snapshot, geometry, Lottie and other preparation. Packet encoding extends the same frame's
accounting. These are overlapping worker durations, not UI-thread time; do not sum counters from
Surfaces sharing one runtime as though they represented independent work.

Native widget description hooks can observe `base::Readiness` through
`WidgetDescriptionScope::observe`. Pending-to-ready or pending-to-failed adoption invalidates
only the bodies and cached widget calls that observed it. Node inspection exposes a `readiness`
array with resource `id`, `kind`, `label`, the `observed` state used by that consumer and its current
`status` (`PENDING`, `READY`, `FAILED`). `phase` distinguishes `description`, `layout`, `paint`
and `packet` observations, including glyph work attached to the actual retained text node.
Cached measurement and paint operations retain independent reads; cached draws replan when
their resources change even if command storage is unchanged. Font adoption also invalidates
text-dependent caret, selection and hit geometry. Observation and immutable prior descriptions
remain independent across Surfaces.

Glyph raster and vector misses prepare in owned batches. Pending glyphs draw nothing unless an
earlier preparation remains valid for that content; rescaled text may retain its previous geometry
until replacement rasters arrive. Atlas generation changes forbid retaining old UVs. Font handles
and a shared, internally synchronized FreeType context pin worker inputs independently of Surface
lifetime. Shared immutable bitmap-cache insertion, page packing and upload ordering remain on the
serialized owner. Raster-work durations describe adopted CPU preparation, not time blocking the UI.

Bound font bytes are copied from the resource adapter during Surface creation, then parsed on the
pool into one complete immutable registry. Required logical IDs and duplicates are rejected before
submission. Until adoption, no face is exposed: text retains source offsets and authored line boxes,
but draws no glyphs. Async parse failures report `STRATA.TASK.PREPARATION_FAILED` on the owner;
deterministic mode preserves synchronous font-validation errors. Optional-table warnings also
publish on the owner. Native font lookup returns explicit readiness instead of an assumed font
reference.

Named system faces, automatic fallback and variable-font instances prepare on the pool. Pending
named faces and variations use an available base face; an uncovered pending character retains its
source cluster and primary line metrics without drawing a missing glyph or reporting a terminal
font miss. Owner adoption rebuilds only affected cached consumers. Unused requests are cancelled
against all live consuming trees, so closing one Surface cannot cancel another's shared request.

System-font clients retain a catalog generation. Changing directories publishes a new generation
without waiting for old enumeration or parsing; existing clients and jobs keep their configuration.
File enumeration, mapping, parsing and coverage extraction run outside short publication locks.
Fallback precedence is script-specific candidates, general candidates, then sorted files, never
cache-completion order. Mapped faces retain their bytes independently of catalog lifetime. The
process-lived OpenType parse index holds weak entries, and Lottie family resolutions belong to
their catalog generation rather than a separate process-static cache.

Lottie resource reads occur only on serialized owner calls, between worker document discovery and
asset decoding. Scene preparation retains its immutable animation and catalog generation, resolves
fonts fully, and retains text outlines and parent-first transform plans. A frame job captures only
that scene and scalar time/viewport/clip values. Widgets keep one active projection and one latest
desired request; completed command runs transfer ownership without copying their path payloads.
Source changes and node retirement cancel stale adoption, and held detached content keeps its last
completed run. Frame clocks begin at first drawable content and exclude paused time.

### Shared-cache ownership

Separate runtimes may prepare concurrently. Shared storage is immutable after publication,
internally synchronized, or retained by a particular owner:

| Storage | Ownership and synchronization |
|---|---|
| Builtin catalogs, schema admission and runtime helper tables | Immutable process-lifetime storage; initialization is thread-safe. |
| Interned symbols and OpenType parse index | Internally synchronized process-lifetime indexes. Borrowed symbol names survive detached work; parsed faces retain their mapped bytes. |
| System-font discovery and family resolution | Retained catalog generations with synchronized publication; directory enumeration and parsing occur outside publication locks. |
| FreeType context and shared raster cache | Process-lifetime admission and cache synchronization; immutable completed bitmaps. Atlas packing, vector indices and upload ordering remain per owner. |
| Image, SVG and Lottie resources | Per-runtime/per-Surface registries. Workers retain owned bytes, decoder continuations or immutable scenes, never registry or Surface pointers. |
| Description topology and CodeView expansion/highlighting tables | Immutable entries published through synchronized process-lifetime caches; retained node/render state remains owner-local. |
| D3D11/Vulkan shader cache configuration | Process-lifetime settings protected by a mutex. Compilation uses a copied directory configuration; cache files publish atomically. |
| Host `ResourceRoot` bytes | Cache lookup/publication is mutex-protected; file and system-font I/O runs outside the lock. Published byte vectors are immutable and borrowed views last until the retained root is destroyed. Concurrent cold readers may duplicate I/O instead of holding a lock across it. |

Detached jobs may outlive the runtime which submitted them and ordinary static destruction.
Shared engine indexes therefore use process-lifetime storage, not destructible local statics.
This does not make mutable state thread-safe: every mutable shared cache still needs its own
synchronization, and each task must retain the resources it reads. Extension-provided caches and
callbacks must satisfy the same concurrent-runtime contract.

The shared worker pool itself is process-lived. Keep the engine binary loaded until process
exit: releasing every runtime or preparation handle is not a DLL-unload barrier.

### Preparation before a runtime exists

`STRATA_CAPABILITY_PREPARATION` exposes an independent `strata_preparation` endpoint backed by
the same engine pool. `strata_preparation_start` accepts owned work/context/result cleanup
callbacks; no host-static pool is needed. A successful start transfers the input context,
including when deterministic work has already failed. An unsuccessful start leaves it with the
caller. Cleanup runs exactly once on whichever thread finally owns the box. Callback code and
its retained dependencies must remain available until cleanup.

The work callback receives a borrowed cancellation token and returns an owned opaque result.
That result must retain its own inputs: the original context is cleaned after owner adoption.
Non-null partial results returned on failure are also cleaned. Failure status and error text
(at most 1 MiB) are retained independently of the input context. As with other ABI callbacks,
work and cleanup callbacks must not throw.

`strata_preparation_get_info` drains completions without waiting. It exposes pending, ready,
failed, cancelled or taken state. `strata_preparation_take_result` transfers a ready result once;
other states return `STRATA_STATUS_NOT_FOUND`. Cancel discards untaken results, and release
quiesces notifications without joining heavy work. Late results are destroyed without adoption.
The handle uses the runtime admission policy: calls must be serialized, and a wake callback
cannot enter or release its own endpoint. Wakes remain notification-only; failures surface on
the next owner drain. Deterministic execution runs work and adoption inside start.

`host::Session::prepare(SessionPrepareOptions)` uses this endpoint rather than creating a
host-static worker pool. It retains a `ResourceRoot`, captures initial values and revision stamps,
activates source or compiled modules, reads backend programs and creates the Surface. Its
`SessionPreparation::ready()` polls without waiting; `take()` adopts the complete Session and
owned material/effect declarations. Backend declarations are still installed by the GPU owner.
The ordinary Session constructor remains the explicit ready/native path.

`Bindings(owner)` may exist without a runtime. `capture_snapshots()` owns values without advancing
publication cursors. `capture_actions()` retains independent endpoints with weak references to the
real owner handlers; preparation does not retain UI callback captures. Premature action dispatch is
an error, not a dummy success. Adoption attaches the original bindings, enables dispatch and seeds
only successfully published initial revisions. Model changes made while preparing are synchronized
afterward.

Prepared resource/text reads are callable on workers. Clipboard, IME, platform effects, host async
handlers, diagnostic observers and live durability writers remain owner-only. Startup durability
uses an owned reader; adoption installs its writer without reloading or replacing restored state.
Retiring preparation quiesces notification callbacks without joining heavy work.

The engine's worker pool uses half the logical cores (at least three). An embedder keeping cores
for itself calls `strata_limit_task_workers` before creating anything.

A host that frames continuously, as a window does, should enable
`strata_surface_set_frame_slicing` (`SessionOptions::frame_slicing`). A frame may then stop at a
stage boundary and finish in the next: while a first frame or `contentHold` content prepares, and
whenever a frame has spent 5 ms. It is off by default, so a host that frames once and reads the
result gets a whole frame. A threaded host asks `Presentation::ready_for_frame` before framing,
which paces the UI to the display and coalesces input while a commit waits for the render thread.

A window can keep the wheel working while its UI thread is busy. `strata_surface_set_busy_poll`
installs a hook the engine calls about every millisecond once a frame has run for a few, including
inside scripts. The hook must not enter the runtime. It takes wheel messages from the queue and
passes them to `Presentation::wheel`. `Presentation::take_scrolls` then says what became of each: an
applied scroll is already showing and goes to `strata_surface_adopt_scroll`, then
`PresentationLayerCommit::scroll_ack` once a packet includes it; any other wheel is replayed as
ordinary input. On Win32, `PeekMessage` runs the window procedure for sent messages even when
filtered by queue, so the hook must not peek while one is pending and the window procedure must not
enter the runtime during the hook. See `native/desktop/window_input.hpp`.

## Minimal C setup

Initialize structures to zero, set `struct_size`, and require only capabilities the host uses:

```c
strata_runtime_config config = {0};
config.struct_size = sizeof(config);
config.abi_version = STRATA_ABI_VERSION_CURRENT;
config.required_capabilities =
    STRATA_CAPABILITY_CORE_LIFECYCLE |
    STRATA_CAPABILITY_APPLICATION_LIFECYCLE |
    STRATA_CAPABILITY_COMPILER_ACTIVATION |
    STRATA_CAPABILITY_SURFACE_RUNTIME |
    STRATA_CAPABILITY_SURFACE_RENDER_PACKET;
config.clock = (strata_clock){sizeof(strata_clock), clock_state, now_nanoseconds};
/* This one-shot example needs all preparation in its first frame. */
config.task_execution = STRATA_TASK_EXECUTION_DETERMINISTIC;

strata_runtime* runtime = NULL;
strata_result result = strata_runtime_create(&config, &runtime);
```

Configure the application schema, compile/activate an entry source, then create a Surface. The complete
buildable flow is [c_smoke.c](../native/samples/c_smoke.c); it uses only the installed header and
library.

## C++ facade

The C++ wrapper owns callback storage, input text, configurations, adapter state, snapshots, and
handles while preserving the same ABI contracts:

```cpp
strata::RuntimeOptions runtime_options;
runtime_options.task_execution = strata::TaskExecution::deterministic;
runtime_options.diagnostic = [](const strata::Diagnostic& diagnostic) {
    log(diagnostic.code, diagnostic.message);
};
strata::Runtime runtime(std::move(runtime_options));

runtime.configure_application(strata::ApplicationOptions{
    .id = "example.application",
});
if (!runtime.activate(strata::SourceActivation{
        .generation = 1,
        .entry_source_id = "ui/main.strata",
        .entry_text = source_text,
    }).activated()) {
    throw std::runtime_error("source activation was rejected");
}

strata::SurfaceOptions surface_options;
surface_options.id = "example.surface";
surface_options.root_role = strata::SurfaceRootRole::screen;
surface_options.root_name = "Main";
surface_options.environment.framebuffer_width = 1280;
surface_options.environment.framebuffer_height = 720;
surface_options.environment.logical_width = 1280.0;
surface_options.environment.logical_height = 720.0;
strata::Surface surface = runtime.create_surface(surface_options);

static_cast<void>(surface.enqueue(strata::InputEvent::pointer(
    strata::InputKind::pointer_move, strata::Point{32.0, 48.0}
)));
const strata_surface_frame_info frame = surface.frame(now_nanos);
const strata::EncodedRenderPacket packet = surface.render_packet();
host.consume(packet.bytes());
const strata::EncodedRenderPacket terminal = surface.prepare_release_packet();
host.consume(terminal.bytes());
surface.acknowledge_release_packet();
surface.close();
runtime.close();
```

`Runtime` exposes owned `std::function` adapters for resources, durability, asynchronous host data,
clipboard, IME, and domain effects. It also provides owned diagnostics, activation results, action
results, source-map results, and RAII runtime/application-state snapshots. `AbiError` includes the
matching diagnostic code and message whenever the ABI reports a diagnostic identity. Owned
runtime/Surface profiler snapshots and host-frame telemetry use the same facade. Raw C
structures remain available as an escape hatch.

Host-frame telemetry v2 requires the originating packet's `frame_index`. Deliver render-thread
feedback through serialized owner calls, not from the GPU thread. Late timings retain their frame
identity in rolling statistics without replacing the current frame's counters; late spikes carry
the reported host counters, never another frame's UI work. Uncaptured frames do not become captured
when their feedback crosses capture enablement. Zero and future frame identities are rejected.

A C++ `Surface` retains shared runtime ownership, so moving it out of the runtime's lexical scope is
safe. Explicit release-packet consumption and `close()` remain the correct shutdown path. If a live
Surface is forgotten, its destructor uses the delivery-impossible abandon path and emits
`STRATA.SURFACE.RELEASE_ABANDONED` instead of terminating the process. See
[cpp_smoke.cpp](../native/samples/cpp_smoke.cpp) for the installed typed flow.

Applications that need an ordinary Win32 window should not manually consume render packets. Link
`Strata::desktop` and use `<strata/desktop.hpp>`; it owns the production packet decoder, D3D11
renderer, resource/clipboard services, input translation boundary, and teardown barrier. The
installed `desktop_app.cpp` sample is a complete window loop. See
[Win32 desktop hosting](desktop-hosting.md).

For Vulkan, link `Strata::vulkan` and use its C or C++ presenter to render into host-owned targets
with the existing HLSL materials and effects. See [Vulkan hosting](vulkan-hosting.md) for queue and
image ownership, synchronous submission, and Surface release ordering.

Custom renderers can instead link `Strata::render_host` and include `<strata/render_packet.hpp>`
to consume packet-v25 without duplicating parsing:

```cpp
strata::host::RenderPacketDecoder decoder;
const strata::EncodedRenderPacket packet = surface.render_packet();
const strata::host::RenderPacket& plan = decoder.decode(packet.bytes());

for (const strata::host::ResourceOperation& resource : plan.resources) {
    backend.apply(resource);
}
backend.upload_geometry(plan.vertices, plan.indices, plan.geometry_epoch);
for (const strata::host::SubmissionBatch& batch : plan.batches) {
    backend.submit(batch);
}
```

On Windows, an application that already owns a D3D11 device and render target can link
`Strata::d3d11` instead of implementing packet submission. Its `Presenter` owns Surface framing,
packet ordering, declaration synchronization, backend layer resources, and terminal packet
delivery while preserving existing target contents and host context state. It never owns the
window, device, swap chain, target, or presentation. A lower-level packet-only `Renderer` remains
available for language bindings and remote/recorded streams. See
[Rendering into a host-owned D3D11 target](d3d11-hosting.md).

An existing Win32 message loop can independently link `Strata::win32` and pass messages for the
currently active Surface to `InputAdapter`. The application keeps shortcut handling and Surface
selection; the adapter owns pointer/key/text/IME translation and capture cancellation. See
[Win32 input adapter](win32-input.md).

The decoder is stateful because settled packets can retain an earlier geometry epoch while updating
the frame index and resource operations. Consume every framed packet in order with one decoder per
Surface/backend stream. Compact packets are deltas and cannot initialize a fresh decoder; call
`reset()` only when discarding the stream, not while continuing to consume it. `RenderPacket`
exposes ordered texture mutations (images arrive decoded, as straight-alpha RGBA8), fixed-layout
vertex bytes, indices, scissors, draw batches, blur batches, backdrop/content effect batches, and
content stack markers. The content stack nests as deep as the document draws, up to the decoder's
sanity bound `host::maximum_content_effect_depth` (1024,
`STRATA_RENDER_PACKET_MAXIMUM_CONTENT_EFFECT_DEPTH`); a backend draws every level it is handed
rather than assuming a small fixed depth. The bundled backends keep the innermost few open layers
in framebuffer-sized targets and park the rest at the pixels later passes read back from them
(`native/gpu/isolation.hpp`). Draw batches name their material by index into
`RenderPacket::materials` and carry a `DrawBlend` and `ClipCoverage` the core already adapted to
their rounded clips, so a backend maps enums to pipeline state and resolves the id table once per packet. Effect batches name
their effect by index into `RenderPacket::effects` and carry a maximum refresh rate, zero (the
default) for every frame; for a nonzero rate custom backends may retain the latest filtered sample
and composite it while its `sample_revision` is unchanged and the rate is not yet due. The decoder
moves the revision whenever anything the effect's output depends on changed, so a backend has no
staleness rule of its own. A backdrop
effect batch with a nonzero `probe` belongs to a node that observes its backdrop: a backend that can
measure reports the mean luma of what that effect captured through
`strata_surface_report_backdrop` (`Surface::report_backdrop`), in any later frame, and one that
cannot simply does not. Presentation groups carry a `tone` that the vertex stage applies to ink
colours with `host::tone_color`; effect batches expose their group's tone as `EffectBatch::tone`.
Grayscale glyph atlases hold plain coverage: a backend turns it into alpha per pixel with
`host::text_coverage`, which weights it by the lightness of the ink being drawn. The consumer
remains responsible for GPU resources, shader/material
implementation, presentation, and the Surface release-packet barrier.

Native submission keeps used geometry inside retained capacity arenas. A local draw-topology change
that still fits those arenas is byte-diffed against the preceding epoch and remains a geometry
patch; it does not force a complete Surface payload merely because planned-item counts changed.
Adjacent changed draws in one batch can redistribute their existing slot capacity without
re-encoding unchanged draws. Painter order and the batch's effective material, texture, blend and
clip state remain unchanged.
Backends should apply those ranges directly to retained buffers. Capacity growth or a patch larger
than replacement remains an explicit full-epoch boundary.

Running presentation motion (opacity, translation, scale, rotation) does not touch geometry. Each animating
node is a presentation group: its content is encoded in layout space, every vertex carries its
group index in position z, and `RenderPacket::groups` holds each group's current scale, shear,
translation and opacity. A backend's vertex stage places each vertex at
`(scale_x * x + shear_x * y, shear_y * x + scale_y * y) + translate` (the translation rounded to
whole framebuffer pixels; `PresentationGroup::place_x`/`place_y` compute it, and the shears are zero
unless the group rotates) and multiplies material opacity (draw-data float
15) by the group opacity; the shared `native/gpu` vertex shader already does. The decoder presents
grouped blur/effect bounds itself, and a changed table adds dirty regions covering the moved
content. Settled nodes leave their group and bake their final presentation back into geometry.
Scroll content stays in its own presentation group: changing the offset moves its content without
moving the viewport or scrollbars. Layout coordinates beneath a scroller remain unscrolled.
Each packet clip names its own group, independently of the draw's group, so nested scroll clips
move with the content that owns them rather than with every descendant draw. The decoder resolves
`RoundedClip::inverse_transform` and `inverse_projection` into an inverse homogeneous map from
surface pixels to clip coordinates. Custom renderers divide the two affine numerators by
`inverse_projection[0] * x + inverse_projection[1] * y + inverse_projection[2]`; a nonpositive
divisor is outside the clip. D3D11, Vulkan and the reference renderer share this contract.
Resource operations that replace texels add the bounds of the draws sampling them, so
`geometry_dirty_all` and `geometry_dirty_regions` describe everything a retained effect sample may
have read; a sample is stale when they touch its bounds grown by its blur or shader reach
(`native/gpu/effect_policy.hpp`), or when the backend did not render the previous epoch.

Before framing, enumerate `Runtime::material_declarations(shaderBackend)` and
`Runtime::effect_pass_declarations(shaderBackend)`. Effect declarations are a flat table ordered by
effect id/pass index and carry blur constants or parameter slots plus the requested backend's
shader resource id. A custom backend retains its compiled programs and executes each packet effect
against the bounded sixteen-float parameter block. The C equivalents are
`strata_runtime_read_material_declarations` and
`strata_runtime_read_effect_pass_declarations`.

## Source-tree static embedding

The installed SDK keeps `Strata::c` shared so every language and extension package crosses one
stable ABI instance. A plug-in, injected module, firmware-style executable, or other consumer that
must ship as one native binary can instead include Strata's source tree and link the build-tree-only
static targets:

```cmake
set(STRATA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(STRATA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(STRATA_BUILD_AUTHORING ON CACHE BOOL "" FORCE)
set(STRATA_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(STRATA_BUILD_DESKTOP OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/strata/native strata EXCLUDE_FROM_ALL)

target_link_libraries(my_module PRIVATE Strata::host_static)
```

Pure C consumers use `Strata::c_static`. The public ABI and facade are identical to `Strata::c` and
`Strata::host`; only linkage changes. Do not link a shared and static Strata runtime into the same
process ownership graph, and do not pass runtime handles to extension libraries backed by another
runtime instance.

`STRATA_BUILD_AUTHORING` builds only `strata_authoring` for source-tree custom commands without
enabling the compiler, headless runner, SVG tool, or other command-line hosts. Set it to `OFF` when
the embedding project consumes a checked generated contract or does not use application schemas.

Thread-safe function-local-static initialization is required; MSVC targets explicitly use
`/Zc:threadSafeInit`. Custom Windows module loaders must register the image's static TLS for every
calling thread, including preparation workers. The former
`STRATA_ENABLE_THREAD_SAFE_STATICS=OFF` escape hatch is removed: serialized first activation cannot
protect paths first reached later by concurrent preparation.

The static targets are deliberately not installed. Their private FreeType and core object graph is
resolved by the source-tree CMake build, while the relocatable installed package remains centered
on the shared, language-neutral ABI.

## Typed C++ host models

C hosts use the JSON ABI directly. Ordinary C++ application code should generate its contract from
the same application schema consumed by the compiler, include `strata/host.hpp`, and link
`Strata::host`. The installed CMake package exposes `Strata_AUTHORING`; invoke it with
`--write-cpp-contract <schema> <namespace> <output.hpp>` from a custom command.

The generated header provides model structures, complete/per-field snapshot encoders, action IDs,
typed payload decoders, enums, maps/unions, and an action variant:

```cpp
namespace contract = my::application::contract;
strata::host::Revision revision;
std::vector<contract::AppItemsItem> items = load_items();
strata::host::Bindings host(runtime, "my.application");

host.snapshot(
    "my.application.model",
    [&] { return revision.value(); },
    [&] { return contract::encode_app_items(items); }
);

host.on<contract::AppSaveAction>([&](const contract::AppSaveAction& action) {
    save(action.path);
    return strata::host::ActionResult::handled;
});

// Once per host tick, before framing surfaces:
host.synchronize();
```

Model code never assembles or parses JSON, and schema drift becomes a C++ compile failure.
`Bindings::synchronize()` adopts completed publications, submits changed revisions and rethrows
contained callback or publication errors. An unchanged pending revision is not resubmitted. Failed
accepted work does not advance the adopted revision: after observing the error, a subsequent call
can retry the same model revision with a fresh generation. Earlier failures remain observable even
when a newer revision is pending. For small standalone values,
`strata::host::Observable<T>` owns the revision automatically. Generated model structures are
equality comparable, so an observable can reject an unchanged replacement without encoding it.

### Snapshot acceptance and adoption

`STRATA_CAPABILITY_HOST_PUBLICATIONS` adds ordered asynchronous publication. The runtime copies the
producer id and JSON at admission, reserves a strictly increasing generation for that producer, then
parses and performs typed conversion using the immutable application schema captured at admission.
Workers never access the runtime or host model. Owner frames adopt the completed acceptance-order
prefix before description/input/layout reads; later producers still shadow earlier overlapping roots
regardless of worker completion order. A failed or cancelled publication unblocks its successors
without changing visible roots.

`strata_runtime_get_host_snapshot_generation` and snapshot-info report **adopted** state.
`strata_runtime_get_host_snapshot_accepted_generation` includes pending, failed and cancelled
generations, which cannot be reused. Publications admitted before application configuration keep
their original untyped, external-store target; later configuration does not silently replay them
into the application's roots.

Set `strata_host_snapshot_config::out_publication` to retain a receipt. Its status is pending,
adopted, failed or cancelled; terminal failures retain their result and message independently of
runtime lifetime. `strata_host_publication_get_info` only observes. Standalone hosts call
`strata_runtime_poll_host_snapshots` to drain/adopt without waiting; Surface frames do this
automatically. Cancellation takes effect on an owner poll/frame, and releasing a receipt alone does
not cancel publication. Closing the runtime cancels outstanding receipts without joining workers.
Errors also reach the owner diagnostic sink when adopted, including fire-and-forget publications.

`STRATA_HOST_SNAPSHOT_EXECUTION_INLINE` performs preparation and adoption inside publication,
returning errors immediately. It rejects publication while earlier asynchronous submissions remain
outstanding rather than waiting on a worker. Use it for initial snapshots needed by activation;
Session preparation does so on its existing worker. Runtime-default execution uses the runtime's
task mode, so deterministic hosts retain immediate behavior.

The C++ `Runtime::publish_host_snapshot` now returns an owned `HostPublication`, not a generation
number. `info()` exposes the generation and terminal result; `Runtime::poll_host_snapshots()`
provides explicit adoption. `HostSnapshotExecution::inline_execution` selects the startup path.
Generation allocation advances above the highest accepted generation, including direct C ABI
submissions.

Generating the host contract does not validate `.strata` source. Installed packages that include
`strata_compile`, and source embeddings with `STRATA_BUILD_TOOLS=ON`, expose a build-integrated
validation helper; attach it to the embedding target so a malformed module fails the build before
it can be embedded:

```cmake
strata_validate_module(
    TARGET my_application
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/ui/menu.strata"
    SCHEMAS "${CMAKE_CURRENT_SOURCE_DIR}/ui/application.schemas.json"
    # EXTENSION_PATHS may list directories containing selected extension packages.
)
```

The helper creates a module-root/schema-dependent validation stamp and makes `TARGET` depend on it.
Because Strata imports are confined to the entry module's directory, every `.strata` file in that
root is tracked without duplicating the compiler's import parser; incremental builds rerun
`strata_compile --check-module` only when a possible module input changes.

Every object host root also gets a generated model/binder. It owns one observable per field and
registers the per-field encoders with independent snapshot ids:

```cpp
contract::AppModel app;
app.bind(host, "my.application");
static_cast<void>(app.set(load_application_model()));
```

The runtime composes those immutable field fragments by host path. `set` compares and moves each
field independently, so updating status does not encode or publish items and unchanged revisions do
not cross the ABI. Individual generated field getters expose the current immutable model without
reconstructing the root. The dynamic `Value` API remains the intentional escape hatch for scenario
runners and remote protocols whose schema is selected only at runtime.

## Low-level host data, actions, and services

- Publish immutable host snapshots with strictly increasing generations. Schemas define their typed
  paths; missing required data rejects activation without replacing the last-good unit. The C++
  `Runtime::publish_host_snapshot` method owns generation allocation when a custom publication
  strategy is needed.
- Register action handlers per runtime. At the C boundary, payloads and event values are canonical
  JSON borrowed only for the callback and registrations have explicit release handles. The C++
  binding layer owns those details.
- Resource adapters return borrowed bytes; Strata copies any data it retains before returning.
  Resource adapter generations are nonzero and strictly increasing. A null, repeated, or stale
  replacement is rejected without mutating the active loader or gating live Surfaces.
- Clipboard, IME, and domain-effect callbacks are independently optional capabilities. Install
  only complete callback structures and advertise only services whose installation succeeded.
  Clipboard reads/writes used by editors cross this runtime-owned boundary; when no platform
  clipboard is installed, a runtime-shared fallback keeps sibling Surfaces coherent.
- A focused editable node activates IME after layout and publishes its transformed logical caret
  rectangle. The host converts that rectangle to platform coordinates. IME ownership is arbitrated
  per runtime so an inactive sibling Surface cannot disable the active Surface; releasing focus or
  the owning Surface deactivates it.
- The effect adapter receives only explicit domain effects emitted through
  `strata_runtime_emit_effect_json`. The authoring-language `effect(...)` value is a visual render
  program consumed by the backend and never calls the host effect adapter.
- Diagnostics use the size- and version-tagged `strata_diagnostic` payload. The callback carries
  stable identity/sequence, severity, complete source range, component path, expected/recovery
  text, first/latest frame, occurrence count, and the canonical store's dropped-record count.
- Require `STRATA_CAPABILITY_DIAGNOSTIC_SNAPSHOTS` to read bounded typed history through
  `strata_runtime_read_diagnostics` or its Surface alias without enabling lazy frame JSON.
  `strata_runtime_clear_diagnostics` (or the Surface alias) clears retained history, queued
  publication, dropped accounting, and Surface-owned pending diagnostic queues together. Because
  application diagnostics are runtime-shared, a Surface clear applies to every sibling Surface.

## Framing and packet consumption

Adopt a complete Surface environment generation atomically: framebuffer and logical sizes, scale,
safe insets, snapping, density, reduced-motion preference, and input capabilities. Enqueue input in
ordered batches, call `strata_surface_frame`, then acquire its packet-v25 handle with
`strata_surface_acquire_render_packet`. ABI v9 is the minimum host contract; the borrowed
`strata_surface_read_render_packet` API is removed.

`strata_render_packet_bytes` returns an immutable view valid while the caller owns a reference.
Retain/release are thread-safe, do not enter the runtime, and tolerate null. References survive later
full, compact and patch frames, terminal preparation, and Surface/runtime destruction. Keep the engine
binary loaded until all references are released. Packet storage uses engine-owned allocation, not
runtime allocator callbacks.

The C++ `EncodedRenderPacket` is a copyable owning reference. `SurfacePacketFrame::encoded` retains
that reference; its `packet` pointer remains a borrow from the stream's mutable decoder and expires
on the next decode/reset. Transfer encoded references between threads, with one decoder on each
consuming Surface/backend stream. Headless recording retains these references instead of copying
the encoded payload for every observation.

After applying a decoded packet's resource operations, `RenderPacketDecoder::acknowledge_resources`
removes those one-shot jobs without copying geometry. Its retained draw plan can then be drawn again
without replaying uploads or vector appends. After drawing, `acknowledge_draw` also retires transient
full/patch geometry uploads while preserving the complete decoded arrays. These are distinct from
acknowledging a Surface terminal:
the latter remains an explicit runtime-owner call after successful ordered backend consumption.

Packet-v25 full packets contain native geometry,
indices, scissors, materials, textures, and draw/blur/effect batches. Compact packets reference the
last full geometry epoch in the ordered stream; both forms can carry one-shot GPU resource
operations. Every committed stream item must reach the decoder/resource consumer in order, even
when intermediate visual frames are not presented. Owning old bytes does not retain historical GPU
textures or permit replay after their replacement/release. Hosts must not redo layout, glyph
generation, or batch planning.

Canonical frame JSON is an optional inspection/conformance projection and is deliberately lazy. It
is not required for normal rendering. It is compact (key-ordered, no insignificant whitespace, one
trailing newline) and host-side `Value::parse` accepts it at any size. Each retained record carries
only its own semantic entry; the complete semantic tree is the frame's top-level `semantics`.
Tools that need a few nodes, such as tests locating a control, should read them with
`strata_surface_read_inspection_node_json` (C++ `Surface::inspection_node_json(key, depth)`), which
returns one keyed record in the same format, with children to the requested depth, without building
the whole snapshot or changing inspector selection.

Before destroying a Surface, prepare its terminal release packet, consume it in stream order
while the host texture owner is alive, explicitly acknowledge that completed consumption, and release the
Surface. Preparation alone never authorizes release. Failed or out-of-order release retains the
handle for retry, and a runtime refuses release while it still owns any Surface. Explicit abandon is
reserved for cases where delivery is impossible and the host independently discards remaining GPU
resources. The C++ destructor treats a forgotten live Surface as that failure mode and reports it
through the diagnostic sink.

Terminal preparation returns another owned reference to the same terminal object on repeated calls.
Releasing an encoded reference never acknowledges GPU consumption. The wire format remains v25.
Packet profiling reports one combined `packet-serialization` section (and
`packet-cold-serialization` for cold frames): geometry and resources are encoded together once.

## Themes

`strata_surface_register_theme` and `strata_surface_set_theme` take a complete `strata_theme`:
tokens, widget styles, and a motion policy as nested structures. A host that only needs tokens can
pass one JSON document to `strata_surface_register_theme_json` or `strata_surface_set_theme_json`:

```json
{"modelVersion": 1, "name": "brand", "parent": "base",
 "tokens": {"accent": "#4F8CFFFF", "surfaceRaised": "#1E2126", "spacingUnit": 4}}
```

Standard tokens keep their types and any other name is a custom color or number token.
`.strata` code reads a custom token as `theme.<name>` once the application schemas declare it
under `themeTokens` (see [Styles and theme values](strata-language.md#styles-and-theme-values)).
Colors are `"#RRGGBB"` or `"#RRGGBBAA"` strings, and an
omitted token keeps the registered parent's value (or the default). Unknown fields and mistyped
tokens are rejected with `STRATA_STATUS_INVALID_ARGUMENT` and a diagnostic. Widget styles and motion policies are not part of
the JSON document yet; they still go through `strata_theme`.

Each projected node retains its immutable effective theme. Native popup palettes and motion
policies read that context directly, so changing or removing a local scope updates already-open
chrome without resetting the widget's retained state. Identical style projections may share
immutable values within one materialization pass; this does not retain arbitrary authored styles
in a process-wide cache.

## Reactivation and failure recovery

Compilation/activation is last-good: a rejected generation returns diagnostics and leaves the active
unit and compatible state untouched. A host may explicitly reactivate compatible `.strata` source
against the configured application contract. Fonts, images, extension packages, schemas, generated
bindings, and native handlers are immutable session inputs; changing any of them requires rebuilding
the owning artifact and recreating the Runtime and its Surfaces. Host-facing texture identities are
generated per process/runtime/Surface and never expose adapter source paths.

## Allocator and memory telemetry

If a custom allocator is supplied, its `user_data` and callbacks must outlive every runtime-derived
handle. Allocation and deallocation always return through the same callback pair. Runtime memory
info reports allocator-routed ABI handles and compile-arena high-water marks; ordinary internal STL
storage is intentionally outside those routed counters.

## Installed acceptance

The CMake install includes an independent sample project under `share/strata/samples`. The repository
gate configures that project against the install prefix and runs its C, C++, and Win32 desktop
applications plus the installed compiler/Surface corpus through CTest. This catches accidental
dependencies on repository paths, build-tree state, demo registries, or private C++ headers.
