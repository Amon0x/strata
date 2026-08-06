# Rendering into a host-owned D3D11 target

`Strata::d3d11` submits decoded packet-v10 layers into a D3D11 device, immediate context, texture,
and render-target view owned by an embedding application. It does not create a window or swap
chain, clear existing content by default, resize host resources, or present. This makes it suitable
for engines, editors, overlays, plug-ins, and applications that already own their graphics loop.

The target is intentionally separate from the UI runtime:

```text
any Strata language binding
  -> stable Surface packet-v10 bytes
  -> backend-specific presenter or Strata::render_host decoder
  -> host-owned graphics target
```

Applications with another graphics API retain the same runtime, Surface, input, resource, and
packet contracts and replace only the final submission target. A Vulkan or OpenGL backend should
own its native target type and consume `Strata::render_host` rather than acquire dependencies on
the D3D11 implementation. C++ backend adapters can use `host::SurfacePacketStream` for the common
frame/read/decode and terminal-delivery ordering. The D3D11 presenter is therefore an optional
adapter, not a graphics abstraction embedded in the runtime.

## CMake

The Windows SDK exports the renderer as a static target:

```cmake
find_package(Strata CONFIG REQUIRED)

add_library(my_host MODULE host.cpp)
target_link_libraries(
  my_host
  PRIVATE Strata::host Strata::d3d11 Strata::win32
)
```

Include the presenter and optional platform input adapter:

```cpp
#include <strata/d3d11.hpp>
#include <strata/win32.hpp>
```

The high-level presenter is a C++ composition over the same stable Surface ABI and packet decoder.
The lower-level `Renderer` still has no runtime dependency: a host can obtain packet bytes through
the C ABI, another language binding, IPC, or a recorded stream and use D3D11 submission directly.

`<strata/d3d11.h>` exposes the presenter itself as the opaque
`strata_d3d11_presenter` C handle. Its target records use `void*` native D3D11 interfaces and its
operations return `strata_adapter_result`, including borrowed UTF-8 failure detail. C++, Rust, C#,
JVM/JNI, and other bindings can wrap that handle without recreating packet or layer lifecycle.
`<strata/d3d11.hpp>` is the owned C++23 facade and also keeps the packet-only `Renderer` API.

Consumers that need a single native module can combine `Strata::d3d11` with the source-tree
`Strata::host_static` target described in [Embedding](embedding.md#source-tree-static-embedding).

## Recommended Surface presenter

Create one presenter for a live Runtime, D3D11 device, and immediate context:

```cpp
strata::d3d11::Presenter presenter(runtime, device, immediate_context);
presenter.synchronize_programs();
presenter.attach("main.surface", surface);
```

`synchronize_programs()` reads the active application's `hlsl` material/effect declarations and
loads their source through the Runtime resource adapter. An embedding host with an in-memory shader
override can supply `PresenterOptions::load_program_source`. After a successful source activation,
call `synchronize_programs()` again. A resource watcher can replace one declared source without
reimplementing declaration lookup:

```cpp
presenter.reload_program_source(resource_id, edited_hlsl);
```

Presenting a Surface owns the complete frame/read/decode/render sequence:

```cpp
const strata::d3d11::PresentedFrame frame = presenter.present(
    "main.surface",
    surface,
    {
        back_buffer,
        render_target_view,
        framebuffer_width,
        framebuffer_height,
        logical_width,
        logical_height,
    },
    time_nanoseconds
);
```

`attach()` is optional before the first `present()`, which attaches lazily. Explicit attachment is
useful when a host owns several surfaces but may not render all of them before shutdown. Layer ids
are stable identities for ordered Surface streams and cannot silently switch to another Surface.
`PresentedFrame` returns the Surface frame record, packet byte count, and D3D11 telemetry.

Before closing a presented Surface, let the presenter deliver and acknowledge its terminal packet:

```cpp
presenter.detach("main.surface");
surface.close();
```

`discard()` exists only when terminal GPU delivery is impossible; the host must then abandon the
Surface explicitly. Call `release_target()` before the host destroys or resizes the target.

The presenter does not own the Runtime, Surface, window, swap chain, device, target, application
models, action behavior, visibility, or presentation loop.

## Low-level packet renderer

Create one renderer for a D3D11 device and its immediate context:

```cpp
strata::d3d11::Renderer renderer(device, immediate_context);
```

The device, context, texture, and render-target view remain host-owned. The renderer retains COM
references while it uses them. The context and target must belong to the supplied device. Targets
must be single-sampled full-size 2D textures; resolve a multisampled scene before compositing
Strata.

For each decoded packet:

```cpp
const strata::host::RenderPacket& packet = decoder.decode(surface.render_packet());

const strata::d3d11::RenderTarget target{
    back_buffer,
    render_target_view,
    framebuffer_width,
    framebuffer_height,
    logical_width,
    logical_height,
};

const strata::d3d11::RenderLayerTelemetry telemetry =
    renderer.render("main.surface", packet, target);
```

Use one stable, unique layer id per ordered Surface packet stream. Geometry and temporal effect
caches are retained by layer id. Call `release_layer()` after consuming and acknowledging that
Surface's terminal release packet.

The renderer holds its most recent target so retained effects can reuse target-sized resources.
Call `release_target()` before the host destroys the target or calls `IDXGISwapChain::ResizeBuffers`.
The host remains responsible for synchronization around target replacement.

## Existing contents and presentation

The default `TargetLoadAction::preserve` draws over the current target contents. This is the normal
choice inside an existing render loop and is required for backdrop effects that sample application
content.

An application that owns the whole frame may request a clear:

```cpp
strata::d3d11::FrameOptions frame;
frame.load_action = strata::d3d11::TargetLoadAction::clear;
frame.clear_color = {0.02F, 0.03F, 0.05F, 1.0F};
frame.time_seconds = elapsed_seconds;

renderer.render("main.surface", packet, target, frame);
```

`render()` never presents. Submit at the point in the host frame where the UI should be composited,
then continue the host's normal resolve and presentation path.

## Context-state ownership

By default, `ContextStatePolicy::preserve` uses D3D11.1 device-context state swapping. Strata draws
inside an isolated context state and restores the host state on success or exception. The
application's render targets, shaders, resources, samplers, input assembly, viewports, scissors,
blend state, and depth state therefore survive submission without a hand-maintained partial state
list.

State preservation requires the device and context to expose the D3D11.1 interfaces available on
supported Windows systems. A host that already provides an equivalent isolation boundary may opt
out:

```cpp
strata::d3d11::RendererOptions options;
options.context_state = strata::d3d11::ContextStatePolicy::host_managed;
strata::d3d11::Renderer renderer(device, immediate_context, options);
```

With `host_managed`, Strata leaves its final pipeline state on the context. Use it only when the
host deliberately captures/restores state or owns the context exclusively.

## Materials, effects, and resource packets

`Presenter` owns runtime declaration enumeration, source loading, packet ordering, and terminal
resource delivery. Low-level `Renderer` consumers enumerate the runtime's `hlsl` material and
effect-pass declarations and register them through `declare_material()` and
`declare_effect_pass()`. The D3D11 backend compiles and retains those programs exactly as the
desktop and D3D11 headless hosts do.

Normal `render()` calls apply packet resource operations before drawing. A terminal Surface packet
may contain only releases and needs no target:

```cpp
const auto terminal_bytes = surface.prepare_release_packet();
renderer.consume_resources(decoder.decode(terminal_bytes));
surface.acknowledge_release_packet();
renderer.release_layer("main.surface");
surface.close();
```

Every packet must be decoded in order by one `RenderPacketDecoder` per Surface stream. Compact
packets cannot initialize a new decoder.

## Reference integration

[`d3d11_target_smoke.cpp`](../native/samples/d3d11_target_smoke.cpp) is a buildable WARP example. It
renders into an existing texture, verifies that target contents are preserved unless explicitly
cleared, verifies that host pipeline state is restored even when a frame is rejected, and exercises
the presenter and Win32 input adapter against a live Runtime and Surface.
