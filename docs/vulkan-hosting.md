# Rendering into a host-owned Vulkan target

`Strata::vulkan` renders packet-v10 Surfaces with Vulkan 1.1 or newer. It provides a C++
`Renderer` and `Presenter` in `<strata/vulkan.hpp>`, and an exception-contained C presenter API
in `<strata/vulkan.h>`. The installed package exports the same target and headers. No Kitten or
Minecraft dependency is introduced.

## Building on Linux

The `linux-x64` preset enables the backend and GPU acceptance tests. Install a C++23 compiler,
CMake, Ninja, libpng development files, Vulkan headers/loader, shaderc development files, a Vulkan
ICD for the GPU, and Khronos validation layers. On Arch Linux:

```sh
sudo pacman -S --needed gcc cmake ninja libpng vulkan-headers vulkan-icd-loader shaderc vulkan-validation-layers
cmake --preset linux-x64
cmake --build --preset linux-x64 --parallel 8
ctest --preset linux-x64 --output-on-failure
```

`-DSTRATA_TEST_VULKAN=OFF` disables driver-dependent tests while still building the renderer.
`-DSTRATA_BUILD_VULKAN=OFF` builds the portable CPU lane without Vulkan or shaderc. Linux PNG
support remains available in both lanes. Windows keeps D3D11 as its default; Vulkan can be enabled
with `STRATA_BUILD_VULKAN=ON` and the Vulkan SDK's shaderc plus libpng available to CMake.

## Ownership and submission

The host owns the instance, physical device, device, graphics queue, target image/view, and window.
Strata creates no swapchain, changes no window state, and performs no presentation. Device handles
must remain valid until the presenter is destroyed. Calls are owner-thread serialized, including
all other access to the borrowed queue.

The initial implementation submits its own command buffer and waits for its own fence before
returning. Submit the host's preceding work first, then call Strata outside any host render pass,
then submit subsequent host work. Queue ordering plus explicit image barriers order these accesses.
The host must arrange semaphore dependencies and queue-family ownership transfers when another
queue produces or consumes the target. Do not hold back an earlier host submission that the Strata
call depends on. This is a synchronous embedding API, not an API for recording into an unsubmitted
Minecraft command buffer; step-two integration must choose a compatible submission boundary.

Targets must be single-sampled 2D color images with color-attachment, transfer-source, and
transfer-destination usage. Supported formats are RGBA8 UNORM, BGRA8 UNORM, and RGBA16F. Supply the
actual initial layout and desired final layout. Preservation requires initialized target contents;
use `TargetLoadAction::clear` for an undefined target. The host owns subsequent layout tracking.
Use premultiplied-alpha compositing when sampling a transparent Strata output into another target.

```cpp
strata::vulkan::Device gpu{physicalDevice, device, graphicsQueue, graphicsQueueFamily};
strata::vulkan::Presenter presenter(runtime, gpu);
// Program bytes come from the Runtime resource adapter by default.
presenter.attach("overlay", surface);
strata::vulkan::RenderTarget target{
    image, imageView, VK_FORMAT_R8G8B8A8_UNORM,
    pixelWidth, pixelHeight, logicalWidth, logicalHeight,
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
};
auto frame = presenter.present("overlay", surface, target, monotonicNanoseconds);
// Before destroying a Surface, consume and acknowledge its terminal resource packet:
presenter.detach("overlay");
surface.close();
// Before destroying or resizing the host target:
presenter.release_target();
```

`Renderer` accepts an already-decoded `RenderPacket` for custom hosts. Keep one ordered decoder per
Surface and a unique layer ID. Resource creates/uploads/releases, retained geometry epochs, and
vertex/index patches follow the same packet contract as D3D11. Layer release drops retained GPU
geometry and effects. Resource-only terminal packets need no render target. If a submission fails,
recreate the renderer and streams: resource mutations from a failed frame must not be replayed as
though they completed.

## Shaders and fidelity

Vulkan consumes the existing `hlsl` material and effect declarations. Shaderc compiles them in
process to SPIR-V; rendering does not spawn a compiler executable. Shared shader headers under
`native/gpu/` define the same `PixelInput`, `material(...)`, `EffectInput`, `effect(...)`, parameter
accessors, clipping, and shading functions used by D3D11. This includes grayscale/MSDF text,
textures and PNG uploads, rounded rectangles/borders, shadows, blend modes, nested rounded clips,
backdrop blur, multi-pass HLSL effects, nested content isolation, and `SURFACE` backdrop captures.
Shape-shadow declarations are already lowered to draw geometry by the core.

A missing authored program or shader compilation error fails explicitly. `reload_program_source`
replaces matching programs; an invalid shader leaves the previously compiled shader usable. Shader
compilation is synchronous. Vulkan does not claim support for arbitrary standalone GLSL entrypoints
or additional shader bindings outside the existing Strata HLSL contract.

Settled geometry stays in GPU buffers; compact packets upload their patches. Effects reuse output
within their declared refresh interval when geometry, resources, parameters, layout, and dimensions
remain compatible. Clock rollback and shader reload invalidate reuse. Intermediate images and
upload/descriptor storage are reused across frames. Effects currently invalidate conservatively
for any geometry/resource change, and scratch images cover the full target (with blur downsampling).
The synchronous queue boundary and broad image barriers favor correct integration; no latency or
throughput parity with the mature D3D11 renderer is claimed.

## Rendering checks

Set `surface.backend` to `vulkan` in a headless scenario. The host selects a discrete GPU when one is
available; set `STRATA_VULKAN_DEVICE` to a device-name substring to select another GPU.
`STRATA_VULKAN_VALIDATION=1` enables both core and synchronization validation.

```sh
STRATA_VULKAN_DEVICE=AMD ctest --preset linux-x64 -L vulkan --output-on-failure
```

The GPU suite exercises live C and C++ Surface presenters and checks exact pixels for materials,
partial texture uploads, retained geometry,
clipping, blend/preserve behavior, shader reload and rejection, nested effects, surface backdrops,
blur, effect refresh, clock rollback, and resize/scale. Eight existing application scenarios also
render through Vulkan with no shader fallbacks or validation messages. Captures are written under
`build/cmake/linux-x64/native/vulkan-*/capture/` for inspection.

The Linux host remains windowless: PNG captures use real GPU rendering and readback, while native
Linux window/input/clipboard/IME adapters are separate host responsibilities. Kitten integration
is deliberately a subsequent change.
