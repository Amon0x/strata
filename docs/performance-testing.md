# Desktop performance testing

Strata's authoritative performance harness runs through the real visible Windows desktop path. It
uses the Win32 window, D3D11 device, swap chain, render-packet decoder, GPU submission, and
`Present(0, 0)` used by `strata_desktop --uncapped`. It is intentionally not a headless timing mode.

## Run the canonical showcase workload

From the repository root on Windows:

```bat
cmake --build --preset windows-x64 --target strata_benchmark_desktop
```

The target opens a 1280×800 foreground window and writes:

- `build/cmake/windows-x64/performance/showcase-desktop/performance.json` — versioned machine-readable measurements
- `build/cmake/windows-x64/performance/showcase-desktop/performance.html` — phase table and frame-time timeline

Do not interact with another window while a foreground-required run is active. Focus loss,
minimization, a hidden window, interrupted message processing, or an occluded DXGI present marks
the run invalid instead of silently contaminating its averages.

The normal task does not change process or thread priority. The report records actual priority,
CPU identity, GPU adapter and memory, display resolution/refresh/DPI, AC status, renderer, VSync,
and foreground policy. Warmup frames allow driver clocks, shaders, glyph caches, and retained UI
caches to settle. Cold startup is reported separately and is never mixed into warm FPS.

## Reading the report

Each phase reports:

- average FPS and average frame time
- 1%-low FPS equivalent
- p50, p95, p99, minimum, and maximum frame time
- standard deviation
- host total, Strata core, render-packet/GPU submission, tooling, and present distributions
- per-operation distributions
- every measured frame with its operation, counters, timings, and validity
- an adaptive spike list and the threshold used for that phase

Average FPS is uncapped host-loop throughput, not the monitor's physical scan-out rate. D3D11 GPU
execution is asynchronous: `submit` and `present` are CPU wall durations for the real API calls,
not forced GPU-completion timings. The harness deliberately does not insert a per-frame GPU flush,
because that would change the user path it is measuring.

Average FPS alone is not an acceptance criterion. Compare phase distributions and the timeline;
an idle frame and a tab replacement intentionally perform very different work.

The canonical workload currently measures:

1. cold showcase creation and first presentation
2. warmed uncapped idle frames
3. repeated EXTEND/FORMS/SHELL tab replacement
4. repeated scrolling of clipped retained content
5. editor focus and Section collapse/reopen interactions

## Compare against a baseline

Keep a known-good `performance.json` outside the task output directory, then run:

```bat
cmake --preset windows-x64 -DSTRATA_PERFORMANCE_BASELINE=C:\benchmarks\strata-good.json
cmake --build --preset windows-x64 --target strata_benchmark_desktop
```

Comparison is refused when the workload fingerprint/report schema, GPU and driver, client
resolution, monitor mode/DPI, VSync mode, process priority, or baseline validity differs. Matching
phases must have the same measured-frame counts and are checked against the scenario's
average/p95/p99 percentage budgets. `minimumAbsoluteMillis` prevents tiny sub-millisecond
variance from being classified as a meaningful percentage regression. A regression or
incomparable environment returns a nonzero exit status and is recorded in the new report's
`comparison` object.

A single run can still contain ordinary system noise. For consequential decisions, collect several
valid runs under the same power/thermal conditions and compare their distributions rather than
selecting the most favorable result.

## Scenario format

Desktop performance scenarios are JSON documents independent of headless scenarios:

```json
{
  "version": 1,
  "name": "showcase-desktop",
  "window": {"width": 1280, "height": 800},
  "validity": {"requireForeground": true},
  "spikes": {
    "floorMillis": 2.0,
    "baselineMultiplier": 2.5,
    "maximumReported": 32
  },
  "regression": {
    "maximumAveragePercent": 10,
    "maximumP95Percent": 15,
    "maximumP99Percent": 20,
    "minimumAbsoluteMillis": 0.1
  },
  "setup": [{"key": "f7"}, {"frames": 3}],
  "phases": [
    {
      "name": "warm-idle",
      "warmupFrames": 120,
      "iterations": 1,
      "steps": [{"frames": 360}]
    },
    {
      "name": "tab-switching",
      "warmupIterations": 2,
      "iterations": 20,
      "steps": [
        {"click": {"role": "tab", "name": "FORMS"}},
        {"frames": 4},
        {"click": {"role": "tab", "name": "EXTEND"}},
        {"frames": 4}
      ]
    }
  ]
}
```

Supported steps are `frames`, `click`, `move`, `scroll`, and `key`. Pointer targets use the same
joined semantic/inspection selector model as the headless browser: coordinates, `key`, `path`, or
`role`/`name`. Selector resolution occurs outside the measured desktop frame because real user
pointer targeting does not serialize frame JSON.

Run a custom scenario directly:

```bat
build\cmake\windows-x64\native\RelWithDebInfo\strata_desktop.exe ^
  --performance path\to\scenario.json ^
  --output build\performance\custom ^
  src\main\resources
```

`strata.desktop.performance_smoke` only verifies the runner and report protocol with a very short,
hidden, non-foreground-gated workload. It does not steal focus during ordinary `check`, and its
numbers are not performance results.
