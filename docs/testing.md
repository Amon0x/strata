# Testing Strata

Strata has four verification layers. A screen-specific assertion does not replace a missing core
contract.

## Native unit and ABI tests

CTest builds separate executables for core ownership, strict data, compiler, runtime, UI,
input/editor, external extension loading, and the public ABI. Installed-package C and C++
consumers also build and query an independent extension shared library.
Run the complete platform gate from the repository root:

```bat
cmake --workflow --preset windows-x64
```

```sh
cmake --workflow --preset linux-x64
```

For incremental work, use `cmake --build --preset <platform>` followed by
`ctest --preset <platform>`. Sanitizer configurations use separate CMake build directories so the
ordinary build remains incremental.

## Deterministic headless application tests

`strata_headless` runs a complete application through the public C ABI with a controlled clock,
viewport, host snapshots/services, extension packages, and ordinary input routing. Captures pair
canonical frame JSON with PNG output.

`strata.headless.portable` and `strata.headless.portable.interactive` run everywhere through the CPU
reference backend. They verify fonts, packet generation and decoding, geometry rendering, physical
pointer input, state mutation, PNG capture, and the persistent JSON-lines protocol. Windows also
runs D3D11/WARP showcase tests through the production texture, blur, blending, and HLSL material
pipeline.

See [Headless application testing](headless-testing.md) for the batch and interactive protocols.

## Windowed host integration

On Windows, `strata.desktop.smoke` creates two hidden Win32 windows with independent runtimes and
renderers. It verifies isolated host snapshots, packet submission, and lifecycle cleanup through the
production D3D11 path.

The installed-package test stages a real CMake installation, configures a separate consumer against
only that prefix, then builds and runs the portable C/C++ applications. Windows additionally builds
and runs the hidden one-frame `Strata::desktop` application. The release-dependency test rejects
repository-path leaks and undeclared runtime dependencies.

## Generated and bundled artifacts

The full CTest gate validates every bundled `.strata` module, checks compiled artifact freshness,
verifies generated authoring/reference/host-contract files, parses machine-readable compiler
diagnostics, checks the VS Code JavaScript, and opens the packaged VSIX to verify its required
assets. Explicit maintenance targets are available when source artifacts intentionally change:

```sh
cmake --build --preset linux-x64 --target strata_generate_artifacts
cmake --build --preset linux-x64 --target strata_generate_authoring
cmake --build --preset linux-x64 --target strata_vscode_extension
```

Use `windows-x64` on Windows. Validation-only targets are `strata_validate_modules`,
`strata_check_artifacts`, and `strata_check_authoring`.

## Manual acceptance

Human acceptance remains appropriate for physical window behavior, clipboard/IME integration,
interactive resource reload, final hardware/driver output, and subjective animation quality.
Windows D3D11/WARP captures are the deterministic production-renderer oracle; the desktop host is
the final window-system acceptance path. Linux deliberately has no bundled GUI backend.
