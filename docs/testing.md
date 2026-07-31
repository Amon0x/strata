# Testing Strata

Strata has four verification layers. A screen-specific assertion does not replace a missing core
contract.

## Native unit and ABI tests

CTest builds separate executables for core ownership, strict data, compiler, runtime, UI,
input/editor, extensions, and the public ABI. Installed-package C and C++ consumers are also tested.
Through Gradle:

```bat
gradlew.bat checkNative
```

Sanitizer configurations use separate CMake build directories so the ordinary build remains
incremental.

## Deterministic headless application tests

`strata_headless` runs a complete application through the public C ABI with a controlled clock,
viewport, host snapshots/services, extension packages, and ordinary input routing. Captures pair
canonical frame JSON with PNG output from offscreen D3D11/WARP. The renderer shares textures, blur,
blending, and HLSL materials with the desktop host.

`strata.headless.smoke` drives the bundled showcase and verifies selector-driven physical input,
host action dispatch, capture output, and authored shader compilation. `strata.headless.interactive`
keeps one application alive across a JSON-lines session, discovers a virtual tab, selects a generated
tree row, and observes subsequent state mutation.

See [Headless application testing](headless-testing.md) for the batch and interactive protocols.

## Windowed host integration

`strata.desktop.smoke` creates two hidden Win32 windows with independent runtimes and renderers. It
verifies isolated host snapshots, packet submission, and lifecycle cleanup through the production
D3D11 path.

The installed-package smoke configures a separate CMake consumer against only the installation
prefix, then builds and runs both C and C++ applications.

## Full gate

```bat
gradlew.bat check build
```

The full gate runs native tests, installed consumer tests, `.strata` validation, compiled-artifact
freshness checks, and generated authoring/reference checks.

## Manual acceptance

Human acceptance remains appropriate for physical window behavior, clipboard/IME integration,
interactive resource reload, final hardware/driver output, and subjective animation quality.
Headless D3D11/WARP captures are the deterministic visual oracle; the desktop host is the final
window-system acceptance path.
