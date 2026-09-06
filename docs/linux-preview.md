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

The preview defaults to a 60 FPS limit. Disable the frame delay and SDL renderer VSync with:

```sh
build/cmake/linux-x64/native/strata_preview --uncapped
```

Actual throughput still depends on rendering, GPU readback, and the window compositor.

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

## Testing and captures

```sh
ctest --preset linux-x64
build/cmake/linux-x64/native/strata_preview --smoke --screenshot /tmp/primitives.png
SDL_VIDEODRIVER=dummy build/cmake/linux-x64/native/strata_preview --smoke --backend reference
```

`--smoke` creates a hidden window, presents one frame, and exits. `--screenshot file.png` saves the
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

`native/preview` owns only SDL presentation, event translation, and native clipboard/IME adapters.
It reuses `headless::ApplicationHost`, generic manifest parsing, resource loading, extensions, and
ordered Surface release. The application host accepts optional native services; deterministic
in-memory services remain the default for replay tests. Live preview skips per-frame inspection JSON
and drains observation history, so leaving a window open does not accumulate frame traces.

This is an interactive testing host. Vulkan renders into the existing offscreen target, and SDL
presents the captured RGBA frame through a streaming texture, capped at 60 Hz by default (or uncapped
with `--uncapped`). GPU readback and upload
make it unsuitable for production performance measurements. A production Linux embedding can use
[Strata::vulkan](vulkan-hosting.md) with its own swapchain to avoid those transfers. CPU reference
rendering has the same material/effect limitations as headless reference captures.

`cmake --install build/cmake/linux-x64` installs `bin/strata_preview` and the gallery under
`share/assets/strata/samples`. The installed executable locates the adjacent resource tree, and
`find_package(Strata)` exposes its path as `Strata_PREVIEW_RUNNER`.

SDL input follows the upstream [text input guidance](https://wiki.libsdl.org/SDL2/Tutorials-TextInput)
and [capture lifecycle](https://wiki.libsdl.org/SDL2/SDL_CaptureMouse).
