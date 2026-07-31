# Project map

## Active roots

- `native/` — portable C++23 runtime, public ABI, native hosts, tools, tests, and samples.
- `src/main/resources/` — registry/lexical declarations and bundled `.strata`, font, texture, and
  shader assets.
- `editor/` — generated completion metadata and VS Code extension source.
- `docs/` — current guides and generated reference/catalog documentation.
- `tools/` — repository-level asset generation utilities.

## Native source ownership

- `native/include/strata` — installed C ABI and C++ RAII facade.
- `native/src/core` — allocator telemetry, arenas, clocks, UTF-8, identities, and base lifetime
  machinery.
- `native/src/data` — strict JSON model, views, parsing, and canonical encoding.
- `native/src/compiler` — source/module graph, lexer/parser, schema/semantic checks, and portable IR.
- `native/src/runtime` — application units, values, bindings, actions, state, collections, layers,
  async host data, durability, undo, diagnostics, and host services.
- `native/src/ui` — retained tree, reconciliation, layout, input/editor/focus, motion, widgets,
  semantics/inspection, text, render planning, and packet encoding.
- `native/src/font` — OpenType reading, shaping/fallback, rasterization, and glyph atlas ownership.
- `native/src/resource` — encoded image and resource handling.
- `native/host` — reusable packet decoding, extension selection, and safe module-path resolution.
- `native/d3d11` — target-independent packet-v4 pipeline, textures, blur, and HLSL materials shared
  by windowed and offscreen hosts.
- `native/desktop` — Win32 input/services, swap-chain ownership, and multi-window executable.
- `native/headless` — persistent semantic browser/control sessions, deterministic replay, offscreen
  D3D11/WARP capture, optional CPU reference rendering, and PNG output.
- `native/tools` — compiler and authoring command-line tools.
- `native/tests` — core/compiler/runtime/UI/editor/ABI/headless tests and scenarios.
- `native/samples` — installable C and C++ embedding smoke applications.

Large subsystems have explicit directories and translation-unit boundaries. Features extend their
owning subsystem rather than adding application-specific branches to the ABI or hosts.

## Dependency direction

```text
.strata/resources -> strata_core -> strata_c
                                  -> desktop host -> D3D11
                                  -> headless host -> D3D11/WARP or reference renderer
```

Core code never imports a host. Host callbacks are per-runtime capabilities, and hosts consume
already planned render packets without acquiring compiler, tree, layout, widget, or semantics
ownership.

## Repository constraints

- Keep the public C ABI free of STL types and exceptions.
- Document every borrowed lifetime and host-owned callback boundary.
- Keep generated artifacts reproducible through the checked-in build tasks.
- Keep platform adapters outside the portable runtime.
