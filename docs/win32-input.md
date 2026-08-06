# Win32 input adapter

`Strata::win32` is an optional platform adapter for applications that already own a Win32 window
and message loop. It translates raw window messages into the portable Surface input contract; it
does not create a window, choose a Surface, run a frame loop, or consume application shortcuts.

This is independent of `Strata::d3d11`. A Win32 host may render through D3D11, another graphics
backend, IPC, or a custom packet consumer while using the same input adapter.

`<strata/win32.h>` exposes the opaque `strata_win32_input_adapter` C handle and
`strata_win32_message_result`. Adapter failures return `strata_adapter_result` with borrowed UTF-8
detail. `<strata/win32.hpp>` owns the same boundary for C++23. Other language bindings can wrap the
C handle rather than translate Win32 messages independently.

## CMake and construction

```cmake
find_package(Strata CONFIG REQUIRED)
target_link_libraries(my_host PRIVATE Strata::host Strata::win32)
```

```cpp
#include <strata/win32.hpp>

strata::win32::InputAdapter input({
    .clock = [] { return application_time_nanoseconds(); },
    .coordinate_scale = logical_scale,
    .manage_pointer_capture = true,
    .focus_on_pointer_press = true,
    .consume_system_keys = false,
});
```

The adapter clock uses `std::chrono::steady_clock` when omitted. `coordinate_scale` converts Win32
client pixels to Surface logical coordinates. Update it when the host's DPI/scale policy changes.

## Window procedure integration

Application shortcuts and Surface selection remain host policy. After handling those, forward the
message to the currently active Surface:

```cpp
if (const auto result = input.handle(
        active_surface,
        window,
        message,
        word_parameter,
        long_parameter
    ); result.has_value()) {
    return *result;
}
return DefWindowProcW(window, message, word_parameter, long_parameter);
```

The adapter handles:

- pointer movement, five buttons, leave tracking, capture, and cancellation;
- vertical and horizontal wheel coordinates/deltas;
- modifiers, key press/release/repeat, and portable key names;
- UTF-16 surrogate pairs and `WM_UNICHAR` committed text;
- IME committed/preedit text and UTF-8 selection ranges;
- focus, capture, and cancel messages that terminate active interaction state.

`consume_system_keys` controls only whether handled `WM_SYSKEY*` messages return a result to the
window procedure. They are still forwarded to the Surface. Set it when an overlay intentionally
owns system-key interaction; leave it false when the containing application or Windows menu should
continue processing them.

Call `reset(window)` when switching input ownership without receiving a normal focus/capture
message. Destruction releases pointer capture owned by the adapter.

## Portability boundary

The runtime consumes normalized input and has no Win32 dependency. Other platform packages should
provide equivalent adapters over the same Surface enqueue/cancel operations, such as SDL, GLFW,
Android, or a remote protocol. Hosts can always bypass `Strata::win32` and enqueue portable
`InputEvent` values directly.
