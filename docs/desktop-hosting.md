# Win32 desktop hosting

Strata ships two desktop entry points:

- `Strata::desktop` is the reusable C++23 Win32/D3D11 host library for an application that owns its
  process and window loop.
- `strata_desktop.exe --application ...` is a configurable runner for viewing an application without
  writing host code. With no `--application`, the executable opens Strata's bundled showcase.

Both paths use the same public C ABI, packet-v4 decoder, D3D11 renderer, resource loader,
clipboard/IMM32 adapters, source-import resolver, complete window-message translator, and ordered
GPU-resource release barrier.

## Build and install the SDK from source

From a Windows command shell at the repository root:

```bat
cmake --workflow --preset windows-x64
cmake --install build\cmake\windows-x64 --config RelWithDebInfo
```

The default prefix is:

```text
build/install/windows-x64/
  bin/                         strata_c.dll and executable tools
  include/strata/              public C and C++ headers
  lib/                         import/static libraries
  lib/cmake/Strata/            find_package configuration
  share/strata/                neutral registry and buildable samples
  share/assets/strata/         fonts, textures, shaders, and sample UI
  share/licenses/strata/       Strata and third-party licenses
```

A consumer configures against that prefix:

```bat
cmake -S . -B build ^
  -G "Visual Studio 18 2026" -A x64 -T version=14.52 ^
  -DCMAKE_PREFIX_PATH=C:\path\to\strata\build\install\windows-x64
cmake --build build --config RelWithDebInfo
```

## CMake targets and package paths

```cmake
find_package(Strata CONFIG REQUIRED)

add_executable(my_tool main.cpp)
target_link_libraries(my_tool PRIVATE Strata::desktop)

add_custom_command(
    TARGET my_tool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:Strata::c>
        $<TARGET_FILE_DIR:my_tool>
)
```

Installed targets are:

| Target | Purpose |
| --- | --- |
| `Strata::c` | Stable shared C ABI. |
| `Strata::host` | C++ ownership and structured host-data/action bindings. |
| `Strata::desktop` | Complete reusable Win32/D3D11 application host. |
| `Strata::extensions` | Authoring support linked into independently loaded extension libraries. |
| `Strata::render_host` | Public stateful packet-v4 decoder used by custom render backends. |

The package also defines:

| Variable | Value |
| --- | --- |
| `Strata_REGISTRY` | Absolute path to `registry-v1.json`. |
| `Strata_RESOURCES` | Absolute path to the installed `share` resource root. |
| `Strata_DESKTOP_RUNNER` | Absolute path to `strata_desktop.exe`. |

A deployed application must keep `strata_c.dll` loadable and provide one resource root containing
`strata/registry-v1.json`, Strata's `assets/strata` directory, and the application's own relative
module/assets. Copying the installed `share` directory beside `bin` preserves the layout understood
by the runner. An embedded host may place the same tree anywhere and pass that directory to
`ApplicationHost`.

## Reusable application host

The complete buildable example is installed at
`share/strata/samples/desktop_app.cpp`; its UI source is
`share/assets/strata/samples/desktop_app.strata`. The essential application setup is:

```cpp
#include <strata/desktop.hpp>

strata::desktop::ApplicationConfig config;
config.application_id = "my.tool";
config.module_resource = "assets/my_tool/app.strata";
config.schemas_resource = "assets/my_tool/app.schemas.json"; // optional
config.root_name = "Main";
config.extension_packages = {"example.meter.v1"};             // optional
config.extension_search_paths = {resource_root / "extensions"};

strata::desktop::ApplicationHost host(window, resource_root, std::move(config));

host.bindings().on("tool.save", [&](const strata::host::ActionEvent& event) {
    save_document(event.payload.require_string("path"));
    return strata::host::ActionResult::handled;
});

host.publish(
    "my.tool.model",
    strata::host::object({
        {"tool", strata::host::object({{"status", "Ready"}})},
    })
);
host.activate();
```

Forward messages through the complete Win32 integration rather than translating individual events
in every application:

```cpp
if (const auto result = host.handle_window_message(message, wparam, lparam)) {
    return static_cast<LRESULT>(*result);
}
return DefWindowProcW(window, message, wparam, lparam);
```

The handler owns DPI/client resizing, mouse-leave tracking, capture, click-to-focus, wheel routing,
key press/repeat/release, UTF-16 surrogate and `WM_UNICHAR` conversion, focus cancellation, and IMM32
composition/result messages. Composition selections are converted from UTF-16 positions to the
UTF-8 byte ranges required by Strata. The installed IME adapter positions the system composition
and candidate windows at the logical editor caret using the current Surface scale.

Low-level `resize`, `pointer`, `scroll`, `key`, `text`, and `ime_preedit` methods remain available to
applications with an existing platform translation layer. `reload_resources()` invalidates the
file cache, advances the adapter generation, and performs the Surface reload barrier for changed
fonts and textures. The message loop calls `host.frame()`. The host synchronizes revision-watched bindings, frames the
Surface, decodes packet v4, submits D3D11 work, and presents. `close()` is optional during ordinary
scope destruction; calling it explicitly reports release errors. Either path consumes and
acknowledges the terminal resource packet before releasing the Surface.

Register action handlers and required snapshots before `activate()`, because activation validates
the configured application schema. Use `bindings().snapshot(...)` for revision-watched models and
`publish(...)` for an immediate standalone snapshot. `runtime()` remains available when the
application installs optional durability, async-data, IME, or effect adapters.

## Configurable desktop runner

A launch document uses the same application/surface document accepted by `strata_headless`. This
lets one file drive interactive desktop viewing and deterministic headless tests. The installed
example is `share/strata/samples/desktop_app.json`:

```json
{
  "version": 1,
  "application": {
    "id": "my.tool",
    "module": "assets/my_tool/app.strata",
    "schemas": "assets/my_tool/app.schemas.json",
    "root": "Main",
    "packages": [],
    "extensionPaths": [],
    "actions": ["tool.save"]
  },
  "surface": {
    "id": "my.tool",
    "role": "overlay",
    "backend": "d3d11",
    "width": 1000,
    "height": 700,
    "scale": 1,
    "fonts": [
      {"id": "strata:fonts/default", "resource": "assets/strata/fonts/default.ttf"},
      {"id": "strata:fonts/default-medium", "resource": "assets/strata/fonts/medium.ttf"}
    ]
  },
  "snapshots": [
    {"id": "my.tool.model", "values": {"tool": {"status": "Ready"}}}
  ]
}
```

Run it with:

```bat
bin\strata_desktop.exe ^
  --application share\strata\samples\desktop_app.json ^
  --resources share
```

The runner compiles the configured module and imports, creates one ordinary window, publishes the
initial snapshots, and installs recording handlers for the listed domain actions. Relative
`extensionPaths` resolve from the launch document; each selected package is loaded from its own
shared library and retained through Surface release. Invoked actions
are written to standard output as `STRATA ACTION ...`. The visible host derives scale from the
window's monitor DPI and viewport; `surface.backend`, `surface.scale`, and scripted `steps` remain
headless-test settings. Use `Strata::desktop` instead when actions must call directly into the tool's
application model.

`--smoke` creates the application window without showing it, presents one frame, and exits. It is
used by the repository and installed-package CTest gates.
