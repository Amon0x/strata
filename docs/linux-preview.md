# Interactive preview on Linux

`strata_preview` opens a real SDL window for the bundled primitives gallery or a generic
`.strata-app.json` application. SDL provides Wayland/X11 windows, pointer capture, keyboard events,
native clipboard, and IME placement. Strata retains layout, focus, editing, gestures, and rendering.

## Build and run

Install the normal [Linux build dependencies](vulkan-hosting.md#building-on-linux), plus SDL 2.0.22
or newer development files (`libsdl2-dev` on Debian/Ubuntu; `sdl2-compat` on current Arch).

```sh
cmake --preset linux-x64
cmake --build --preset linux-x64 --parallel
build/cmake/linux-x64/native/strata_preview
```

Vulkan presentation follows the display's FIFO/vsync cadence on an independent GPU owner.
The CPU reference path retains its 60 FPS limit. Select explicit uncapped, caller-thread
presentation with:

```sh
build/cmake/linux-x64/native/strata_preview --uncapped
```

Actual throughput still depends on rendering and the window compositor. Vulkan reads back pixels
only for requested captures, not for ordinary presentation.

The Linux preset enables `STRATA_BUILD_PREVIEW`. With no arguments the preview opens
`assets/strata/samples/primitives.strata-app.json`, including buttons, editors, checkboxes,
toggles, sliders, progress, selects, radio groups, menus, tabs, tooltips, and disclosure sections.
The gallery uses the shared primitive styles, rather than a separate presentation skin.

To open another generic application:

```sh
build/cmake/linux-x64/native/strata_preview path/to/app.strata-app.json
```

Resource paths resolve relative to `launch.resourceRoot` in the manifest. `--resources directory`
overrides that root. The preview uses Vulkan when built with it; `--backend reference` explicitly
selects the CPU renderer. The desktop backend overrides the manifest's capture backend, so a
manifest authored with `d3d11` can be previewed on Linux. Vulkan/shader startup failures report an
error; there is no silent rendering fallback.

Compiled Vulkan shaders persist in `$XDG_CACHE_HOME/strata/shaders`, or
`$HOME/.cache/strata/shaders` when no XDG cache root is set. The cache is disposable; source or
compiler changes select new entries, and damaged entries compile again.

Tab/Shift+Tab navigate focus. Sliders accept arrows, Home/End, and Page Up/Down; dropdowns accept
arrows, typeahead, Enter, and Escape. Editors use the native clipboard and SDL's separate committed
text/composition events. Dragging keeps pointer capture outside the window; losing window focus
cancels active interactions. Resize and high-DPI framebuffer changes update the Surface environment.
SDL selects the window system, or you can set `SDL_VIDEODRIVER=wayland` or `SDL_VIDEODRIVER=x11`.

F5 recreates the application from its current source/resources, resetting local state. A failed
reload prints diagnostics and retains the last good application. Restart the runner after changing
the manifest. Automatic `--watch` preview is currently specific to the Windows desktop runner.
Generic domain actions are recorded to stdout as `STRATA ACTION ...`; custom native host executables
should be launched directly.

Vulkan prepares a replacement Session while the current scene remains live. Shader declarations
belong to the candidate's own presentation stream, so even a partially failed shader batch cannot
change the current scene. Promotion switches the visible packet and native IME ownership; retired
Sessions finish ordered resource release without stopping the successor's text input.

## Testing and captures

```sh
ctest --preset linux-x64
build/cmake/linux-x64/native/strata_preview --smoke --screenshot /tmp/primitives.png
SDL_VIDEODRIVER=dummy build/cmake/linux-x64/native/strata_preview --smoke --backend reference
```

`--smoke` creates a hidden window, waits for an actual application draw/Present, and exits.
It uses deterministic preparation and caller-thread presentation. `--screenshot file.png` saves the
first frame; omit `--smoke` to continue interacting. CTest runs display-independent smoke and input
tests, covering scaled input, key repeat/release, UTF-8 IME selection ranges, actions, slider bounds,
dropdown keyboard selection, text editing, clipboard paste, disabled rendering, and resizing.

For a build without Vulkan/shaderc:

```sh
cmake --preset linux-x64 -DSTRATA_BUILD_VULKAN=OFF -DSTRATA_TEST_VULKAN=OFF
cmake --build --preset linux-x64 --parallel
```

For a build without SDL/window support, configure with `-DSTRATA_BUILD_PREVIEW=OFF`.

## Ownership and scope

`native/preview` owns SDL event translation and native clipboard/IME adapters. Vulkan uses
`host::Session::prepare` and `host::Presentation`: the main thread owns Sessions and SDL video
calls, including surface creation; the GPU owner owns the Vulkan instance, device, swap chain and
ordered packet consumption. The SDL window remains alive through GPU shutdown. Zero-size or
outdated targets defer drawing without discarding pending commits or captures.

The CPU reference path reuses `headless::ApplicationHost`, including its deterministic in-memory
replay services. The Vulkan interactive path installs native clipboard/IME and declared action
bindings; it does not pretend to implement platform effects or asynchronous host requests through
recording-only success callbacks. Applications needing those services need a custom host.
Neither path retains ordinary per-frame inspection history.

This is an interactive testing host. Time-driven Vulkan shaders keep drawing during a runtime-owner
stall, but SDL input still shares the main thread with the runtime. GPU independence is not
independent input handling. CPU reference rendering retains its streaming-texture presentation and
the same material/effect limitations as headless reference captures.

`cmake --install build/cmake/linux-x64` installs `bin/strata_preview` and the gallery under
`share/assets/strata/samples`. The installed executable locates the adjacent resource tree, and
`find_package(Strata)` exposes its path as `Strata_PREVIEW_RUNNER`.

SDL input follows the upstream [text input guidance](https://wiki.libsdl.org/SDL2/Tutorials-TextInput)
and [capture lifecycle](https://wiki.libsdl.org/SDL2/SDL_CaptureMouse).
