# Strata

Strata is a C++23 compiler and retained-UI runtime for declarative `.strata` applications. The
runtime owns compilation, typed host data and actions, retained state, layout, input, semantics,
text, widgets, render planning, and packet encoding. Hosts integrate through the versioned C ABI in
[`native/include/strata/strata.h`](native/include/strata/strata.h); the C++ facade wraps that same
ABI.

## Repository layout

| Area | Ownership |
| --- | --- |
| `native/` | C++23 core, C/C++ ABI, tools, tests, samples, and host implementations |
| `src/main/resources/` | Bundled fonts, shaders, textures, schemas, and `.strata` applications |
| `docs/` | Embedding, authoring, language, testing, and generated reference documentation |
| `editor/` | Generated editor metadata and the VS Code language extension source |

The native framework supports two platform profiles:

- **Windows x64:** the portable framework plus `strata_desktop`, the production Win32/D3D11 host.
  The headless host can use D3D11/WARP for production-renderer fidelity.
- **Linux x64:** the platform-neutral framework, C/C++ host APIs, extensions, compiler/authoring
  tools, public packet-v4 decoder, and CPU reference headless host. Strata does not ship a Linux GUI
  backend; consumers can implement Vulkan/OpenGL submission on top of `Strata::render_host`.

## Build and test

CMake and CTest are the repository build and verification interfaces. The root presets keep Windows
and Linux artifacts in separate build trees.

### Windows x64

Requirements:

- CMake with the `Visual Studio 18 2026` generator
- x64 preview MSVC toolset 14.52 with C++23 support

From a Windows command shell:

```bat
cmake --workflow --preset windows-x64
```

This configures, builds, and runs the complete test gate in `build\cmake\windows-x64`.

### Linux x64

Requirements:

- CMake 3.25 or newer
- GCC 13 or newer with C++23 support
- Ninja

```sh
cmake --workflow --preset linux-x64
```

This configures, builds, and runs the complete test gate in `build/cmake/linux-x64`.

For incremental work, each workflow is also available as separate commands:

```sh
cmake --preset linux-x64
cmake --build --preset linux-x64 --parallel
ctest --preset linux-x64
```

Use `windows-x64` instead on Windows.

## Installable SDK

Install an already-built preset with:

```sh
cmake --install build/cmake/linux-x64
```

```bat
cmake --install build\cmake\windows-x64 --config RelWithDebInfo
```

The default prefixes are `build/install/linux-x64` and `build/install/windows-x64`. The package
exports these portable targets:

| Target | Purpose |
| --- | --- |
| `Strata::c` | Stable shared C ABI. |
| `Strata::host` | C++ ownership and structured host-data/action bindings. |
| `Strata::extensions` | Native extension package authoring implementation. |
| `Strata::render_host` | Stateful public packet-v4 decoder for custom render backends. |

Windows additionally exports `Strata::desktop`. See [Embedding](docs/embedding.md) for custom
renderer integration and [Win32 desktop hosting](docs/desktop-hosting.md) for DLL/resource
deployment and the installed desktop consumer.

## Native desktop

After building the Windows preset:

```bat
build\cmake\windows-x64\native\RelWithDebInfo\strata_desktop.exe src\main\resources
```

`F6` toggles durable settings, `F7` toggles the application showcase, `F8` cycles the diagnostics
surface, and `F9` toggles the passive frame-time HUD. `--multi-window` opens two independent hosts in
one process; `--uncapped` disables VSync and the message-loop frame cap.

Run an arbitrary application from a launch document instead of opening the bundled showcase:

```bat
build\cmake\windows-x64\native\RelWithDebInfo\strata_desktop.exe ^
  --application path\to\application.json ^
  --resources path\to\resource-root
```

Applications embedded in another executable link the installed `Strata::desktop` target. The
complete Win32 window/input example is
[`native/samples/desktop_app.cpp`](native/samples/desktop_app.cpp).

## Headless application testing

`strata_headless` runs complete applications with a deterministic clock and ordinary input routing.
It emits canonical state/semantics/inspection JSON and renders packet-v4 geometry to PNG. Linux uses
the portable CPU reference backend; Windows also tests the shared production D3D11/WARP texture,
blur, and authored-HLSL material pipeline.

The host supports replayable scenarios and a persistent newline-delimited JSON inspect/control loop
for exploratory tooling. See [Headless application testing](docs/headless-testing.md).

## Desktop performance testing

On Windows, build the visible foreground benchmark target:

```bat
cmake --build --preset windows-x64 --target strata_benchmark_desktop
```

It runs the uncapped Win32/D3D11 workload and writes machine-readable measurements plus an HTML
frame-time report. See [Desktop performance testing](docs/performance-testing.md).

## Embedding and authoring

C++ application hosts use `strata/host.hpp` (`Strata::host`) plus schema-generated model/action
headers. The package exposes `Strata_AUTHORING` for contract generation, so application schemas stay
the single source of truth and JSON remains confined to the stable C ABI.

- [C/C++ embedding and custom renderer guide](docs/embedding.md)
- [Win32 desktop hosting and SDK consumption](docs/desktop-hosting.md)
- [Native ABI and build notes](native/README.md)
- [Headless application testing](docs/headless-testing.md)
- [Desktop performance testing](docs/performance-testing.md)
- [`.strata` authoring guide](docs/strata-authoring.md)
- [Language syntax](docs/strata-language.md)
- [Testing guide](docs/testing.md)
- [Generated registry reference](docs/generated/strata-reference.md)

## Optional profiling

`STRATA_ENABLE_TRACY=ON` enables local Tracy zones. CMake fetches the pinned Tracy revision only when
that option is enabled; profiler executables and third-party source archives are not stored in this
repository.

## License

Strata is available under the permissive [MIT License](LICENSE.txt). Third-party notices are
documented in [`native/THIRD_PARTY.md`](native/THIRD_PARTY.md) and packaged license files.
