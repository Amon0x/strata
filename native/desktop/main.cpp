#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
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
        std::filesystem::path resources;
        for (int index = 1; index < argument_count; ++index) {
            const std::wstring_view argument(arguments[index]);
            if (argument == L"--smoke") smoke = true;
            else if (argument == L"--profile-showcase") profile_showcase = true;
            else if (argument == L"--multi-window") multi_window = true;
            else if (argument == L"--uncapped") uncapped = true;
            else if (resources.empty()) resources = argument;
            else throw std::invalid_argument(
                "usage: strata_desktop [--smoke|--profile-showcase] [--multi-window] "
                "[--uncapped] [resource-root]"
            );
        }
        if (smoke && profile_showcase) {
            throw std::invalid_argument("desktop smoke and profile modes are mutually exclusive");
        }
        if (smoke) multi_window = true;
        if (profile_showcase) multi_window = false;
        const bool headless = smoke || profile_showcase;
        if (resources.empty()) resources = default_resource_root();
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

        const std::size_t window_count = multi_window ? 2U : 1U;
        std::vector<std::unique_ptr<WindowApplication>> applications;
        applications.reserve(window_count);
        for (std::size_t index = 0U; index < window_count; ++index) {
            auto app = std::make_unique<WindowApplication>();
            const wchar_t* const title = index == 0U
                ? L"Strata Native Showcase — Alpha"
                : L"Strata Native Showcase — Omega";
            const HWND window = CreateWindowExW(
                0U,
                window_class_name,
                title,
                WS_OVERLAPPEDWINDOW,
                index == 0U ? CW_USEDEFAULT : 160,
                index == 0U ? CW_USEDEFAULT : 120,
                1120,
                760,
                nullptr,
                nullptr,
                instance,
                app.get()
            );
            if (window == nullptr) {
                throw std::runtime_error("could not create a native desktop window");
            }
            app->window_handle = window;
            try {
                app->host = std::make_unique<strata::desktop::Host>(
                    window,
                    resources,
                    index == 0U ? "Independent window alpha 7" : "Independent window omega 9",
                    !uncapped && !headless
                );
                if (smoke) app->host->key(VK_F7);
            } catch (...) {
                DestroyWindow(window);
                throw;
            }
            app->registered = true;
            ++active_window_count;
            if (!headless) {
                ShowWindow(window, SW_SHOW);
                UpdateWindow(window);
            }
            applications.push_back(std::move(app));
        }

        int exit_code = 0;
        bool running = true;
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
