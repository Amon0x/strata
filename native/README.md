# Strata native runtime

`strata_core` is the portable C++23 implementation. `strata_c` is the stable shared-library
boundary. C++ hosts use the ownership facade in `include/strata/strata.hpp`, which calls the same C ABI
rather than exposing implementation classes.

## ABI and ownership

- ABI v4 uses opaque runtime, snapshot, registration, and Surface handles with paired release
  functions and exposes modality-aware focus presentation, durable-state, and typed asynchronous
  host-data contracts. Releasing `NULL` is harmless; v4 is the minimum negotiable host contract.
- All public structures start with `struct_size`; reserved fields are zeroed. Required capabilities
  are negotiated before construction and fail explicitly when unsupported.
- Public text is length-delimited UTF-8. Callback strings/bytes are borrowed only for the callback.
  No STL type, C++ exception, or internal allocator pointer crosses the boundary.
- The caller owns a signed 64-bit monotonic nanosecond clock. Runtime mutations are owner-thread
  serialized. Immutable snapshots keep their documented lifetime independently of the runtime.
- Allocator callbacks are a complete allocate/deallocate pair and remain valid until every derived
  handle is released. Telemetry covers allocator-routed ABI handles and the production compile
  arena. It intentionally does not label ordinary internal STL storage as routed memory.
- Runtime host snapshots, action/effect handlers, resources, clipboard/IME, durability, async
  requests, diagnostics, identities, state, input, text/atlas state, and caches are instance-local.
  Optional clipboard/IME/effect adapters are installed independently. Durable and async adapters
  are installed once because loaded state and in-flight ownership depend on their identity. Editor
  clipboard fallback and cross-Surface IME ownership are runtime-shared. IME cursor rectangles are
  published from post-layout logical caret geometry.
- Durable state is one canonical, versioned application document with application/widget/command/
  shell namespaces. Core performs validation, migration, declared-value restore, and write-behind;
  the host owns load and coalesced off-thread atomic byte replacement plus shutdown drain. Async
  host data uses typed IDLE/LOADING/READY/FAILED snapshots, per-binding schema validation,
  latest-wins completion, progress, debounce, and retained-owner cancellation. Host threads post
  copied completions into an owner-thread mailbox; cancellation invalidates a request before the
  host callback runs.
- Host effect callbacks are an explicit domain-effect boundary. Visual DSL `effect(...)` values
  stay in the native material/render pipeline and are never forwarded to the host callback.
- Every exported function contains exceptions. Authoring/service failures return structured
  diagnostics; invariant and internal failures remain distinct statuses.

## Surface and packet lifetime

`strata_surface_frame` updates a Surface and prepares packet v4. A bytes sink borrows the packet only
for its callback. Resource create/upload/release operations are one-shot; settled geometry is
cached and the frame index is updated in place. Reading canonical frame JSON is optional and lazily
materialized.

`strata_surface_reload_resources` constructs candidate fonts/textures before adoption. On success it
invalidates the prior frame and packet; call `strata_surface_frame` again before reading them. On
failure, the prior resources remain active and the result carries a diagnostic.

Replacing a runtime resource adapter is a host-wide loader transition, not a lazy cache hint. Every
live Surface enters `RESOURCE_RELOAD_REQUIRED`; the host must call
`strata_surface_reload_resources` successfully for each Surface before framing it again. Each
Surface reload remains transactional. A failed Surface keeps its prior materialized resources but
stays gated, preventing a frame that mixes an old font/texture set with the new adapter. Adapter
generations are nonzero and strictly increasing; null, repeated, or stale replacements are rejected
without changing the active adapter or gating any Surface. Source resource paths remain private to
the adapter. Packets use collision-free process/runtime/Surface host resource identities.

Surface destruction is an ordered host-consumption barrier for every Surface-owned static texture
and glyph atlas. Call
`strata_surface_prepare_release_packet`, synchronously consume that resource-only packet while the
host texture owner is alive, call `strata_surface_acknowledge_release_packet`, and only then call
`strata_surface_release`. Preparation is idempotent and terminal, but is not proof of consumption;
out-of-order release is refused and leaves the handle recoverable. Runtime release likewise refuses
live Surfaces. `strata_surface_abandon` is an explicit delivery-impossible fallback that may leave
host GPU resources alive. The C++ facade requires explicit `Surface::close()` or `abandon()`; a live
Surface reaching its destructor is a fatal ownership invariant violation.

## Windows build

From a Visual Studio developer command prompt:

```bat
cmake -S native -B build\native\windows-x64 ^
  -G "Visual Studio 18 2026" -A x64 -T version=14.52 ^
  -DSTRATA_BUILD_TESTS=ON -DSTRATA_BUILD_TOOLS=ON ^
  -DSTRATA_BUILD_SAMPLES=ON -DSTRATA_WARNINGS_AS_ERRORS=ON
cmake --build build\native\windows-x64 --config RelWithDebInfo --parallel
ctest --test-dir build\native\windows-x64 --build-config RelWithDebInfo --output-on-failure
```

The ordinary repository gate configures/builds this automatically. ASan uses a separate build with
`-DSTRATA_ENABLE_ASAN=ON`; MSVC does not claim UBSan support.

`strata_headless` is the non-windowed application host. It drives the same C ABI and packet-v4
boundary as other hosts, but supplies a deterministic clock, scripted input/services, canonical
frame capture, and offscreen D3D11/WARP rendering through the desktop host's shared production
texture, blur, and HLSL material pipeline. It supports both replayable scenarios and a persistent
newline-delimited JSON inspect/control session whose semantic browser includes exact retained and
virtual-subtarget hit geometry. An explicit CPU reference backend remains available for portable
packet/geometry checks. Its protocols are documented in
[`docs/headless-testing.md`](../docs/headless-testing.md).

## Installed package

`gradlew.bat installNative` creates `build/native/install/windows-x64`. Installation exports
`Strata::c`, `Strata::host`, `Strata::desktop`, `Strata::extensions`, public headers, the neutral
registry, runtime assets, tools, and samples. `Strata_RESOURCES` names the installed `share`
directory so consumers do not reconstruct package paths.

The installed sample project configures against only the install prefix. Its C and C++ programs
configure an application, activate `.strata`, create/frame a Surface, validate packet v4, exercise
resource reload, inspect allocator telemetry, and release every handle. Its Win32 program links
`Strata::desktop`, opens the sample `.strata` application through the production D3D11 backend, and
runs as a hidden one-frame CTest smoke without repository includes. See
[`docs/desktop-hosting.md`](../docs/desktop-hosting.md) for package consumption and deployment.
