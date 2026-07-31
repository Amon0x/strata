#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <windowsx.h>

#include <strata/strata.h>

#include "host.hpp"
#include "performance.hpp"

namespace {

constexpr wchar_t window_class_name[] = L"StrataNativeDesktopWindow";
std::size_t active_window_count = 0U;

[[nodiscard]] std::string character_utf8(const wchar_t first, const wchar_t second = 0) {
    const wchar_t characters[2]{first, second};
    const int count = second == 0 ? 1 : 2;
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, characters, count, nullptr, 0, nullptr, nullptr
    );
    if (size <= 0) throw std::runtime_error("Win32 character input is not valid Unicode");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            characters,
            count,
            result.data(),
            size,
            nullptr,
            nullptr
        ) != size) {
        throw std::runtime_error("Win32 character conversion was incomplete");
    }
    return result;
}

struct WindowApplication final {
    std::unique_ptr<strata::desktop::Host> host;
    std::string failure;
    HWND window_handle = nullptr;
    wchar_t high_surrogate = 0;
    std::uint32_t captured_buttons = 0U;
    bool registered = false;

    void failed(const std::exception& error) noexcept {
        try {
            failure = error.what();
        } catch (...) {
            failure = "native desktop failed without a diagnostic";
        }
        PostQuitMessage(1);
    }

    void resize(const HWND window, const LPARAM dimensions) {
        if (host == nullptr) return;
        const std::uint32_t width = static_cast<std::uint32_t>(LOWORD(dimensions));
        const std::uint32_t height = static_cast<std::uint32_t>(HIWORD(dimensions));
        if (width == 0U || height == 0U) return;
        const UINT dpi = GetDpiForWindow(window);
        host->resize(width, height, dpi == 0U ? 1.0 : static_cast<double>(dpi) / 96.0);
    }

    void character(const wchar_t value) {
        if (value >= 0xD800 && value <= 0xDBFF) {
            high_surrogate = value;
            return;
        }
        if (value >= 0xDC00 && value <= 0xDFFF) {
            if (high_surrogate == 0) return;
            host->text(character_utf8(high_surrogate, value));
            high_surrogate = 0;
            return;
        }
        high_surrogate = 0;
        if (value >= 0x20 && value != 0x7f) host->text(character_utf8(value));
    }
};

[[nodiscard]] WindowApplication* application(const HWND window) noexcept {
    return reinterpret_cast<WindowApplication*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

[[nodiscard]] bool activate_foreground_window(const HWND window) noexcept {
    if (window == nullptr || !IsWindow(window)) return false;
    ShowWindow(window, SW_RESTORE);
    const HWND previous = GetForegroundWindow();
    const DWORD current_thread = GetCurrentThreadId();
    const DWORD foreground_thread = previous != nullptr
        ? GetWindowThreadProcessId(previous, nullptr)
        : 0U;
    const bool attached = foreground_thread != 0U && foreground_thread != current_thread &&
        AttachThreadInput(current_thread, foreground_thread, TRUE) != 0;
    BringWindowToTop(window);
    SetActiveWindow(window);
    SetForegroundWindow(window);
    SetFocus(window);
    if (attached) {
        static_cast<void>(AttachThreadInput(current_thread, foreground_thread, FALSE));
    }
    return GetForegroundWindow() == window;
}

LRESULT CALLBACK window_procedure(
    const HWND window,
    const UINT message,
    const WPARAM word,
    const LPARAM long_value
) {
    if (message == WM_NCCREATE) {
        const auto* const create = reinterpret_cast<const CREATESTRUCTW*>(long_value);
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams)
        );
    }
    WindowApplication* const app = application(window);
    if (app == nullptr) return DefWindowProcW(window, message, word, long_value);
    try {
        switch (message) {
        case WM_SIZE:
            app->resize(window, long_value);
            return 0;
        case WM_EXITSIZEMOVE:
            if (app->host != nullptr) app->host->persist_window_geometry();
            return 0;
        case WM_DPICHANGED: {
            const auto* const suggested = reinterpret_cast<const RECT*>(long_value);
            SetWindowPos(
                window,
                nullptr,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER
            );
            return 0;
        }
        case WM_MOUSEMOVE:
            if (app->host != nullptr) app->host->pointer(
                STRATA_INPUT_POINTER_MOVE,
                0,
                GET_X_LPARAM(long_value),
                GET_Y_LPARAM(long_value)
            );
            return 0;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            app->captured_buttons |= message == WM_LBUTTONDOWN ? 1U
                : message == WM_RBUTTONDOWN ? 2U : 4U;
            SetCapture(window);
            if (app->host != nullptr) app->host->pointer(
                STRATA_INPUT_POINTER_PRESS,
                message == WM_LBUTTONDOWN ? 0 : message == WM_RBUTTONDOWN ? 1 : 2,
                GET_X_LPARAM(long_value),
                GET_Y_LPARAM(long_value)
            );
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
            if (app->host != nullptr) app->host->pointer(
                STRATA_INPUT_POINTER_RELEASE,
                message == WM_LBUTTONUP ? 0 : message == WM_RBUTTONUP ? 1 : 2,
                GET_X_LPARAM(long_value),
                GET_Y_LPARAM(long_value)
            );
            app->captured_buttons &= message == WM_LBUTTONUP ? ~1U
                : message == WM_RBUTTONUP ? ~2U : ~4U;
            if (app->captured_buttons == 0U) ReleaseCapture();
            return 0;
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(long_value), GET_Y_LPARAM(long_value)};
            ScreenToClient(window, &point);
            if (app->host != nullptr) app->host->scroll(
                point.x,
                point.y,
                0.0,
                static_cast<double>(GET_WHEEL_DELTA_WPARAM(word)) / WHEEL_DELTA
            );
            return 0;
        }
        case WM_MOUSEHWHEEL: {
            POINT point{GET_X_LPARAM(long_value), GET_Y_LPARAM(long_value)};
            ScreenToClient(window, &point);
            if (app->host != nullptr) app->host->scroll(
                point.x,
                point.y,
                static_cast<double>(GET_WHEEL_DELTA_WPARAM(word)) / WHEEL_DELTA,
                0.0
            );
            return 0;
        }
        case WM_KEYDOWN:
            if (app->host != nullptr) app->host->key(static_cast<std::uint32_t>(word));
            return 0;
        case WM_SYSKEYDOWN:
            // Bare F10 is a Win32 system key that normally enters menu-key modality. Strata owns
            // it as an application shortcut, so forwarding it to DefWindowProc would logically
            // toggle the surface and then stall visible presentation until menu mode is dismissed.
            if (word == VK_F10) {
                if (app->host != nullptr) app->host->key(VK_F10);
                return 0;
            }
            if (app->host != nullptr) app->host->key(static_cast<std::uint32_t>(word));
            return DefWindowProcW(window, message, word, long_value);
        case WM_SYSKEYUP:
            if (word == VK_F10) return 0;
            return DefWindowProcW(window, message, word, long_value);
        case WM_CHAR:
            if (app->host != nullptr) app->character(static_cast<wchar_t>(word));
            return 0;
        case WM_KILLFOCUS:
        case WM_CANCELMODE:
            app->captured_buttons = 0U;
            if (app->host != nullptr) app->host->cancel_interactions();
            return 0;
        case WM_CAPTURECHANGED:
            if (app->captured_buttons != 0U) {
                app->captured_buttons = 0U;
                if (app->host != nullptr) app->host->cancel_interactions();
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            app->host.reset();
            if (app->registered) {
                app->registered = false;
                if (active_window_count > 0U) --active_window_count;
                if (active_window_count == 0U) PostQuitMessage(0);
            }
            return 0;
        default: return DefWindowProcW(window, message, word, long_value);
        }
    } catch (const std::exception& error) {
        app->failed(error);
        return 0;
    }
}

[[nodiscard]] std::filesystem::path default_resource_root() {
    std::wstring executable(32'768U, L'\0');
    const DWORD size = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size())
    );
    if (size == 0U || size == executable.size()) {
        throw std::runtime_error("could not locate the native desktop executable");
    }
    executable.resize(size);
    return std::filesystem::path(executable).parent_path().parent_path() / "share";
}

} // namespace

int wmain(const int argument_count, wchar_t** const arguments) {
    try {
        bool smoke = false;
        bool profile_showcase = false;
        bool multi_window = false;
        bool uncapped = false;
        std::filesystem::path performance_scenario_path;
        std::filesystem::path performance_output;
        std::filesystem::path performance_baseline;
        std::filesystem::path resources;
        for (int index = 1; index < argument_count; ++index) {
            const std::wstring_view argument(arguments[index]);
            if (argument == L"--smoke") smoke = true;
            else if (argument == L"--profile-showcase") profile_showcase = true;
            else if (argument == L"--multi-window") multi_window = true;
            else if (argument == L"--uncapped") uncapped = true;
            else if (argument == L"--performance") {
                if (++index >= argument_count) {
                    throw std::invalid_argument("--performance requires a scenario path");
                }
                performance_scenario_path = arguments[index];
            } else if (argument == L"--output") {
                if (++index >= argument_count) {
                    throw std::invalid_argument("--output requires a directory");
                }
                performance_output = arguments[index];
            } else if (argument == L"--baseline") {
                if (++index >= argument_count) {
                    throw std::invalid_argument("--baseline requires a performance.json path");
                }
                performance_baseline = arguments[index];
            } else if (resources.empty()) resources = argument;
            else throw std::invalid_argument(
                "usage: strata_desktop [--smoke|--profile-showcase] [--multi-window] "
                "[--uncapped] [--performance scenario.json --output directory "
                "[--baseline performance.json]] [resource-root]"
            );
        }
        const bool performance = !performance_scenario_path.empty();
        const int exclusive_modes = (smoke ? 1 : 0) + (profile_showcase ? 1 : 0) +
            (performance ? 1 : 0);
        if (exclusive_modes > 1) {
            throw std::invalid_argument(
                "desktop smoke, profile, and performance modes are mutually exclusive"
            );
        }
        if (!performance && (!performance_output.empty() || !performance_baseline.empty())) {
            throw std::invalid_argument("--output and --baseline require --performance");
        }
        if (performance && performance_output.empty()) {
            throw std::invalid_argument("--performance requires --output");
        }
        if (smoke) multi_window = true;
        if (profile_showcase || performance) multi_window = false;
        if (performance) uncapped = true;
        const bool headless = smoke || profile_showcase;
        if (resources.empty()) resources = default_resource_root();
        const std::optional<strata::desktop::PerformanceScenario> performance_scenario =
            performance
                ? std::optional(strata::desktop::load_performance_scenario(
                      performance_scenario_path
                  ))
                : std::nullopt;
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = &window_procedure;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        window_class.lpszClassName = window_class_name;
        if (RegisterClassExW(&window_class) == 0U) {
            throw std::runtime_error("could not register the native desktop window class");
        }

        strata::desktop::PerformanceStartup performance_startup;
        if (performance) performance_startup.started_at = std::chrono::steady_clock::now();
        const std::size_t window_count = multi_window ? 2U : 1U;
        std::vector<std::unique_ptr<WindowApplication>> applications;
        applications.reserve(window_count);
        for (std::size_t index = 0U; index < window_count; ++index) {
            auto app = std::make_unique<WindowApplication>();
            const wchar_t* const title = index == 0U
                ? L"Strata Native Showcase — Alpha"
                : L"Strata Native Showcase — Omega";
            constexpr DWORD window_style = WS_OVERLAPPEDWINDOW;
            RECT window_bounds{
                0,
                0,
                static_cast<LONG>(performance_scenario.has_value()
                    ? performance_scenario->client_width : 1120U),
                static_cast<LONG>(performance_scenario.has_value()
                    ? performance_scenario->client_height : 760U),
            };
            if (performance_scenario.has_value() && !AdjustWindowRectExForDpi(
                    &window_bounds, window_style, FALSE, 0U, GetDpiForSystem()
                )) {
                throw std::runtime_error("could not resolve performance window bounds");
            }
            const auto window_create_started = std::chrono::steady_clock::now();
            const HWND window = CreateWindowExW(
                0U,
                window_class_name,
                title,
                window_style,
                index == 0U ? CW_USEDEFAULT : 160,
                index == 0U ? CW_USEDEFAULT : 120,
                window_bounds.right - window_bounds.left,
                window_bounds.bottom - window_bounds.top,
                nullptr,
                nullptr,
                instance,
                app.get()
            );
            if (window == nullptr) {
                throw std::runtime_error("could not create a native desktop window");
            }
            if (performance && index == 0U) {
                performance_startup.window_create_nanos =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - window_create_started
                    ).count();
            }
            app->window_handle = window;
            if (performance_scenario.has_value()) {
                RECT exact_bounds{
                    0,
                    0,
                    static_cast<LONG>(performance_scenario->client_width),
                    static_cast<LONG>(performance_scenario->client_height),
                };
                if (!AdjustWindowRectExForDpi(
                        &exact_bounds, window_style, FALSE, 0U, GetDpiForWindow(window)
                    ) || !SetWindowPos(
                        window, nullptr, 0, 0,
                        exact_bounds.right - exact_bounds.left,
                        exact_bounds.bottom - exact_bounds.top,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE
                    )) {
                    DestroyWindow(window);
                    throw std::runtime_error("could not establish performance client bounds");
                }
            }
            try {
                const std::string instance_label = performance
                    ? "Performance benchmark " + std::to_string(GetCurrentProcessId())
                    : index == 0U ? "Independent window alpha 7" : "Independent window omega 9";
                const auto host_create_started = std::chrono::steady_clock::now();
                app->host = std::make_unique<strata::desktop::Host>(
                    window,
                    resources,
                    instance_label,
                    strata::desktop::HostOptions{
                        .vsync = !uncapped && !headless,
                        .restore_window_geometry = !performance,
                        .performance_hud = !performance,
                        .profile_sampling = !performance,
                    }
                );
                if (performance && index == 0U) {
                    performance_startup.host_create_nanos =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - host_create_started
                        ).count();
                }
                if (smoke) app->host->key(VK_F7);
            } catch (...) {
                DestroyWindow(window);
                throw;
            }
            app->registered = true;
            ++active_window_count;
            if (!headless && (!performance || performance_scenario->require_visible)) {
                ShowWindow(window, performance ? SW_SHOWNOACTIVATE : SW_SHOW);
                UpdateWindow(window);
                if (performance_scenario.has_value() &&
                    performance_scenario->require_foreground) {
                    static_cast<void>(activate_foreground_window(window));
                }
            }
            applications.push_back(std::move(app));
        }

        int exit_code = 0;
        bool running = true;
        if (performance) {
            const bool foreground = !performance_scenario->require_foreground ||
                activate_foreground_window(applications.front()->window_handle);
            if (!foreground) {
                throw std::runtime_error(
                    "could not make the performance window foreground; no objective run was made"
                );
            }
            auto pump_messages = [&applications, &exit_code]() {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE)) {
                    if (message.message == WM_MOUSEMOVE) {
                        MSG next{};
                        while (PeekMessageW(&next, nullptr, 0U, 0U, PM_NOREMOVE) &&
                               next.message == WM_MOUSEMOVE && next.hwnd == message.hwnd) {
                            static_cast<void>(PeekMessageW(
                                &message, nullptr, 0U, 0U, PM_REMOVE
                            ));
                        }
                    }
                    if (message.message == WM_QUIT) {
                        exit_code = static_cast<int>(message.wParam);
                        return false;
                    }
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                return std::ranges::all_of(applications, [](const auto& app) {
                    return app->failure.empty() && app->host != nullptr;
                });
            };
            strata::desktop::PerformanceRunner runner(
                applications.front()->window_handle,
                *applications.front()->host,
                *performance_scenario,
                performance_output,
                performance_baseline,
                performance_startup
            );
            const bool valid = runner.run(pump_messages);
            exit_code = valid ? 0 : 2;
            running = false;
        }
        bool profile_started = false;
        std::size_t profile_frames_after_open = 0U;
        while (running) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE)) {
                if (message.message == WM_MOUSEMOVE) {
                    MSG next{};
                    while (PeekMessageW(&next, nullptr, 0U, 0U, PM_NOREMOVE) &&
                           next.message == WM_MOUSEMOVE && next.hwnd == message.hwnd) {
                        static_cast<void>(PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE));
                    }
                }
                if (message.message == WM_QUIT) {
                    exit_code = static_cast<int>(message.wParam);
                    running = false;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!running) break;
            for (const auto& app : applications) {
                if (app->host == nullptr) continue;
                try {
                    app->host->frame();
                } catch (const std::exception& error) {
                    app->failed(error);
                    running = false;
                    exit_code = 1;
                    break;
                }
            }
            if (profile_showcase && running) {
                strata::desktop::Host& profile_host = *applications.front()->host;
                if (!profile_started && profile_host.smoke_ready()) {
                    profile_host.reset_profile();
                    profile_host.key(VK_F7);
                    profile_started = true;
                } else if (profile_started && ++profile_frames_after_open >= 1U) {
                    std::cout << profile_host.profile_report();
                    running = false;
                }
            }
            if (smoke && running && std::ranges::all_of(applications, [](const auto& app) {
                    return app->host != nullptr && app->host->smoke_ready();
                })) {
                if (applications.size() != 2U ||
                    !applications[0]->host->smoke_identity_bound() ||
                    !applications[1]->host->smoke_identity_bound() ||
                    applications[0]->host->smoke_fingerprint() ==
                        applications[1]->host->smoke_fingerprint()) {
                    throw std::runtime_error(
                        "desktop isolation smoke did not observe distinct per-window host state"
                    );
                }
                std::cout << "STRATA_NATIVE_DESKTOP_SMOKE_READY windows=2 isolatedPackets=true\n";
                running = false;
            }
            if (!headless && !uncapped) {
                static_cast<void>(MsgWaitForMultipleObjectsEx(
                    0U, nullptr, 16U, QS_ALLINPUT, MWMO_INPUTAVAILABLE
                ));
            }
        }
        for (const auto& app : applications) app->host.reset();
        for (const auto& app : applications) {
            if (app->window_handle != nullptr && IsWindow(app->window_handle)) {
                DestroyWindow(app->window_handle);
            }
        }
        for (const auto& app : applications) {
            if (!app->failure.empty()) throw std::runtime_error(app->failure);
        }
        return exit_code;
    } catch (const std::exception& error) {
        std::cerr << "strata_desktop: " << error.what() << '\n';
        return 1;
    }
}
