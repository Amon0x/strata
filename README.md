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

The framework ships two native application hosts:

- `strata_desktop`: Win32/D3D11 windows backed by the production renderer.
- `strata_headless`: deterministic batch or interactive sessions backed by offscreen D3D11/WARP.

## Windows build and test

Requirements:

- CMake with the `Visual Studio 18 2026` generator
- x64 preview MSVC toolset 14.52 with C++23 support
- Java suitable for running the Gradle wrapper

Run the complete repository gate from a Windows command shell:

```bat
gradlew.bat check build
```

This configures and builds the native runtime, tools, desktop/headless hosts, tests, installed-package
consumer samples, checked-in application artifacts, and generated authoring documentation.

To build only the native targets:

```bat
gradlew.bat buildNative
```

## Native desktop

After `buildNative`:

```bat
build\native\windows-x64\RelWithDebInfo\strata_desktop.exe src\main\resources
```

`F6` toggles durable settings, `F7` toggles the application showcase, `F8` cycles the diagnostics
surface, and `F9` toggles the passive frame-time HUD. `--multi-window` opens two independent hosts in
one process; `--uncapped` disables VSync and the message-loop frame cap.

## Headless application testing

`strata_headless` runs complete applications with a deterministic clock and ordinary input routing.
It emits canonical state/semantics/inspection JSON and renders packet-v4 geometry into an offscreen
D3D11/WARP target through the production texture, blur, and authored-HLSL material pipeline.

The host supports replayable scenarios and a persistent newline-delimited JSON inspect/control loop
for exploratory tooling. See [Headless application testing](docs/headless-testing.md).

## Desktop performance testing

`gradlew.bat benchmarkDesktop` runs a visible foreground, uncapped Win32/D3D11 workload and writes
machine-readable measurements plus an HTML frame-time report. See
[Desktop performance testing](docs/performance-testing.md).

## Embedding and authoring

C++ application hosts can use `strata/host.hpp` (`Strata::host`) for structured values, typed action
handlers, and revision-watched snapshots; JSON remains confined to the stable C ABI.

- [C/C++ embedding guide](docs/embedding.md)
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

Copyright (c) 2026 Strata contributors. All rights reserved; see [LICENSE.txt](LICENSE.txt).
Third-party notices are documented in [`native/THIRD_PARTY.md`](native/THIRD_PARTY.md) and packaged
license files.
