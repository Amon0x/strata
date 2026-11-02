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

`windows-x64` is the single Visual Studio 2026 Windows lane. Keep the Linux workflow available for
native compatibility checks, but do not routinely compile every platform when a change is local to
this Windows host.

For incremental work, use `cmake --build --preset <platform>` followed by
`ctest --preset <platform>`. Sanitizer configurations use separate CMake build directories so the
ordinary build remains incremental. Likewise, `-DSTRATA_BUILD_FUZZERS=ON` (MSVC or Clang) in its own
build directory instruments the Strata targets for coverage and builds a libFuzzer harness for each
parser of untrusted input (add `-DSTRATA_ENABLE_ASAN=ON` to catch memory errors as well as crashes):

| Harness | Input | Seed corpus |
| --- | --- | --- |
| `strata_opentype_fuzzer` | an OpenType font, parsed and shaped | the bundled fonts |
| `strata_png_fuzzer` | a mode byte, then a PNG (CRCs rewritten) or a header and scanlines | the bundled PNGs, each prefixed with a 0 byte |
| `strata_svg_fuzzer` | an SVG document, parsed and rasterized small | the bundled SVGs |
| `strata_render_packet_fuzzer` | length-prefixed packets decoded in order by one decoder | frames of a `packets.strp` headless recording |

Each harness's header comment gives its input format and a typical command line. Run them with
`-malloc_limit_mb` and `-rss_limit_mb` so an unbounded allocation is a finding.

## Text against Chrome

`python tools/text_compare.py` draws the same lines in nine fonts in Chrome and in Strata on this
machine, on dark and light, and reports each line's ink and pixel difference. Small text in the desktop's
system route should stay within about 1% of Chrome's ink and under 0.3 of 255 once aligned; run it
after changing rasterization, shaping or `host::text_coverage`.

## Threads

Most tests run deterministically, so they say little about threads. `strata_thread_soak` runs the real
showcase on real threads and keeps switching tabs, scrolling from the render thread, resizing, reloading
and closing at random until its deadline. It passes when nothing threw and nothing hung; CTest gives it
5 seconds. Run it for longer under a sanitizer after changing anything that crosses a thread:

```sh
cmake -S . -B build/tsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSTRATA_ENABLE_TSAN=ON -DSTRATA_WARNINGS_AS_ERRORS=OFF
TSAN_OPTIONS="report_bugs=0 exitcode=0" setarch x86_64 -R cmake --build build/tsan
cd build/tsan && TSAN_OPTIONS=allocator_may_return_null=1 setarch x86_64 -R ctest
cd native && setarch x86_64 -R ./strata_thread_soak <resources> <repo>/native/tests/fixtures/headless/showcase.json --seconds=120 --windows=3
```

The build runs its own tools, so `report_bugs=0` keeps it going; `setarch -R` is there because
ThreadSanitizer cannot start under WSL's address randomization. On Windows, use an
`-DSTRATA_ENABLE_ASAN=ON` build and add `--backend=d3d11` to put real GPU resources on the render thread.
`--workers=1` shows a task waiting on another, and `--seed=N` varies what it does.

## Deterministic headless application tests

`strata_headless` runs a complete application through the public C ABI with a controlled clock,
viewport, host snapshots/services, extension packages, and ordinary input routing. Captures pair
canonical frame JSON with PNG output.

`strata.render.differential` replays recorded headless packet streams and scripted effect-cache
sequences through every backend the build has and compares them; see
[the reference renderer's oracle scope](headless-testing.md#what-the-reference-renderer-is-an-oracle-for).

`strata.headless.portable` and `strata.headless.portable.interactive` run everywhere through the CPU
reference backend. They verify fonts, packet generation and decoding, geometry rendering, physical
pointer input, state mutation, PNG capture, and the persistent JSON-lines protocol. Windows also
runs D3D11/WARP showcase tests through the production texture, blur, blending, and HLSL material
pipeline.

See [Headless application testing](headless-testing.md) for the batch and interactive protocols.

## Windowed host integration

On Windows, `strata.desktop.smoke` creates two hidden Win32 windows with independent runtimes and
renderers. It verifies isolated host snapshots, packet submission, and lifecycle cleanup through the
production D3D11 path. The installed desktop smoke also compiles and runs the public window-message
integration that owns pointer capture/leave, Unicode, DPI, and IMM32 routing.

The installed-package test stages a real CMake installation, configures a separate consumer against
only that prefix, then builds and runs the portable C/C++ applications. Windows additionally builds
and runs the hidden one-frame `Strata::desktop` application plus the public `Strata::d3d11`
host-target sample under WARP. The latter verifies preserve/clear target load actions, context-state
restoration, exception cleanup, live-Surface presentation/terminal delivery, Win32 message
translation, the opaque C adapter handles, and the installed static adapter targets. A separate C17
consumer verifies that the adapter headers and exported symbols do not require C++. The
release-dependency test rejects repository-path leaks and undeclared runtime dependencies.

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
final hardware/driver output, and subjective animation quality. Windows D3D11/WARP captures are the
deterministic production-renderer oracle; the desktop host is the final window-system acceptance
path. Linux deliberately has no bundled GUI backend.
