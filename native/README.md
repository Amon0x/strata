# Strata native runtime

`strata_core` is the portable C++23 implementation. `strata_c` is the stable shared-library
boundary. C++ hosts use the ownership facade in `include/strata/strata.hpp`, which calls the same C ABI
rather than exposing implementation classes.

## ABI and ownership

- ABI v6 uses opaque runtime, snapshot, registration, and Surface handles with paired release
  functions and exposes modality-aware focus presentation, durable-state, and typed asynchronous
  host-data contracts. Built-in language declarations are native, and typed visual effect programs
  are enumerable by render backends. Releasing `NULL` is harmless; v6 is the minimum negotiable host
  contract.
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

`strata_surface_frame` updates a Surface and prepares packet v9. A bytes sink borrows the packet only
for its callback. Resource create/upload/release operations are one-shot; settled frames use compact
packets that reference the latest full geometry epoch. Consume every framed packet in order with one
stateful decoder per Surface/backend stream. Reading canonical frame JSON is optional and lazily
materialized.

`strata_surface_reload_resources` constructs candidate fonts/images before adoption. On success it
invalidates the prior frame and packet; call `strata_surface_frame` again before reading them. On
failure, the prior resources remain active and the result carries a diagnostic.

Replacing a runtime resource adapter is a host-wide loader transition, not a lazy cache hint. Every
live Surface enters `RESOURCE_RELOAD_REQUIRED`; the host must call
`strata_surface_reload_resources` successfully for each Surface before framing it again. Each
Surface reload remains transactional. A failed Surface keeps its prior materialized resources but
stays gated, preventing a frame that mixes an old font/image set with the new adapter. Adapter
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
host GPU resources alive. The normal C++ path uses explicit `Surface::close()` or `abandon()`; a
live Surface reaching its destructor automatically uses that abandon path and emits
`STRATA.SURFACE.RELEASE_ABANDONED`; explicit packet consumption and `Surface::close()` remain the
normal leak-free path.

## Platform builds

The root presets are the supported build interface:

```bat
cmake --workflow --preset windows-x64
```

```sh
cmake --workflow --preset linux-x64
```

The single Windows lane uses Visual Studio 2026 x64 without pinning a toolset version. The Linux
preset remains available for native compatibility checks and uses GCC, Ninja, and a RelWithDebInfo
single-configuration tree. Both enable tools, samples, tests, strict
warnings, and installed-package acceptance. ASan uses a separate build with
`-DSTRATA_ENABLE_ASAN=ON`; MSVC does not claim UBSan support.

`strata_headless` is the non-windowed application host. It drives the same C ABI and packet-v9
boundary as other hosts, but supplies a deterministic clock, scripted input/services, and canonical
frame capture. On Windows it can use offscreen D3D11/WARP through the desktop host's shared
production texture, blur, and HLSL material pipeline. Linux builds only the CPU reference backend;
no Linux window or GPU backend is provided. The replay and JSON-lines protocols are documented in
[`docs/headless-testing.md`](../docs/headless-testing.md).

## Installed package

Install an already-built root preset with:

```bat
cmake --install build\cmake\windows-x64 --config RelWithDebInfo
```

```sh
cmake --install build/cmake/linux-x64
```

The default prefixes are `build/install/windows-x64` and `build/install/linux-x64`. Every package
exports `Strata::c`, `Strata::host`, `Strata::extensions`, and `Strata::render_host`, plus public
headers, a generated JSON catalog projection, runtime assets, tools, and samples. `Strata::extensions` is static
authoring support linked into independently loaded package libraries; the installed
`strata_configure_extension` CMake helper assigns their discovery-safe names. Windows also exports
`Strata::desktop`. `Strata_RESOURCES` names the installed `share` directory so consumers do not
reconstruct package paths; `Strata_DESKTOP_RUNNER` exists only when the desktop runner was installed.

The installed sample project configures against only the install prefix. Its portable C and C++
programs configure an application, activate `.strata`, create/frame a Surface, decode packet v9,
exercise resource reload, inspect allocator telemetry, and release every handle. The public C++
facade is split into focused owned-value headers (`diagnostic.hpp`, `input.hpp`, `adapters.hpp`,
`profiler.hpp`, and `config.hpp`) aggregated by `strata.hpp`; custom hosts do not need to keep
borrowed C strings or callback bridge records alive. `Strata_AUTHORING` names the installed
schema-to-C++ generator; the installed consumer gate generates and compiles a typed host contract
without repository paths. `Strata_VSCODE_EXTENSION` names the installed schema-aware VSIX when tools
were built. On Windows the gate also builds a hidden one-frame program linked through
`Strata::desktop`. See
[`docs/embedding.md`](../docs/embedding.md) for portable package and custom-renderer consumption, and
[`docs/desktop-hosting.md`](../docs/desktop-hosting.md) for Win32 deployment.
