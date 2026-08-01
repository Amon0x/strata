#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <windowsx.h>

#include <strata/desktop.hpp>

namespace {

constexpr wchar_t window_class_name[] = L"StrataInstalledDesktopExample";

[[nodiscard]] std::string character_utf8(const wchar_t first, const wchar_t second = 0) {
    const wchar_t characters[2]{first, second};
    const int count = second == 0 ? 1 : 2;
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, characters, count, nullptr,
                                         0, nullptr, nullptr);
    if (size <= 0)
        throw std::runtime_error("Win32 character input is not valid Unicode");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, characters, count, result.data(), size,
                            nullptr, nullptr) != size) {
        throw std::runtime_error("Win32 character conversion was incomplete");
    }
    return result;
}

struct WindowState final {
    std::unique_ptr<strata::desktop::ApplicationHost> host;
    wchar_t high_surrogate = 0;
    std::uint32_t captured_buttons = 0U;
    std::string failure;

    void fail(const std::exception& error) noexcept {
        try {
            failure = error.what();
        } catch (...) {
            failure = "desktop example failed without a diagnostic";
        }
        PostQuitMessage(1);
    }

    void character(const wchar_t value) {
        if (value >= 0xD800 && value <= 0xDBFF) {
            high_surrogate = value;
            return;
        }
        if (value >= 0xDC00 && value <= 0xDFFF) {
            if (high_surrogate != 0)
                host->text(character_utf8(high_surrogate, value));
            high_surrogate = 0;
            return;
        }
        high_surrogate = 0;
        if (value >= 0x20 && value != 0x7f)
            host->text(character_utf8(value));
    }
};

[[nodiscard]] WindowState* state(const HWND window) noexcept {
    return reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

LRESULT CALLBACK window_procedure(const HWND window, const UINT message, const WPARAM word,
                                  const LPARAM long_value) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(long_value);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    WindowState* app = state(window);
    if (app == nullptr)
        return DefWindowProcW(window, message, word, long_value);
    try {
        switch (message) {
        case WM_SIZE:
            if (app->host != nullptr && LOWORD(long_value) != 0U && HIWORD(long_value) != 0U) {
                const UINT dpi = GetDpiForWindow(window);
                app->host->resize(LOWORD(long_value), HIWORD(long_value),
                                  dpi == 0U ? 1.0 : static_cast<double>(dpi) / 96.0);
            }
            return 0;
        case WM_DPICHANGED: {
            const auto* bounds = reinterpret_cast<const RECT*>(long_value);
            SetWindowPos(window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
                         bounds->bottom - bounds->top, SWP_NOACTIVATE | SWP_NOZORDER);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (app->host != nullptr)
                app->host->pointer(STRATA_INPUT_POINTER_MOVE, 0, GET_X_LPARAM(long_value),
                                   GET_Y_LPARAM(long_value));
            return 0;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            app->captured_buttons |=
                message == WM_LBUTTONDOWN ? 1U : message == WM_RBUTTONDOWN ? 2U : 4U;
            SetCapture(window);
            if (app->host != nullptr)
                app->host->pointer(STRATA_INPUT_POINTER_PRESS,
                                   message == WM_LBUTTONDOWN ? 0
                                   : message == WM_RBUTTONDOWN ? 1 : 2,
                                   GET_X_LPARAM(long_value), GET_Y_LPARAM(long_value));
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
            if (app->host != nullptr)
                app->host->pointer(STRATA_INPUT_POINTER_RELEASE,
                                   message == WM_LBUTTONUP ? 0
                                   : message == WM_RBUTTONUP ? 1 : 2,
                                   GET_X_LPARAM(long_value), GET_Y_LPARAM(long_value));
            app->captured_buttons &=
                message == WM_LBUTTONUP ? ~1U : message == WM_RBUTTONUP ? ~2U : ~4U;
            if (app->captured_buttons == 0U)
                ReleaseCapture();
            return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            POINT point{GET_X_LPARAM(long_value), GET_Y_LPARAM(long_value)};
            ScreenToClient(window, &point);
            const double delta = static_cast<double>(GET_WHEEL_DELTA_WPARAM(word)) / WHEEL_DELTA;
            if (app->host != nullptr)
                app->host->scroll(point.x, point.y, message == WM_MOUSEHWHEEL ? delta : 0.0,
                                  message == WM_MOUSEWHEEL ? delta : 0.0);
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (app->host != nullptr)
                app->host->key(static_cast<std::uint32_t>(word), STRATA_KEY_PRESS);
            return message == WM_KEYDOWN ? 0 : DefWindowProcW(window, message, word, long_value);
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (app->host != nullptr)
                app->host->key(static_cast<std::uint32_t>(word), STRATA_KEY_RELEASE);
            return message == WM_KEYUP ? 0 : DefWindowProcW(window, message, word, long_value);
        case WM_CHAR:
            if (app->host != nullptr)
                app->character(static_cast<wchar_t>(word));
            return 0;
        case WM_KILLFOCUS:
        case WM_CANCELMODE:
            app->captured_buttons = 0U;
            if (app->host != nullptr)
                app->host->cancel_interactions();
            return 0;
        case WM_CAPTURECHANGED:
            if (app->captured_buttons != 0U) {
                app->captured_buttons = 0U;
                if (app->host != nullptr)
                    app->host->cancel_interactions();
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            app->host.reset();
            PostQuitMessage(0);
            return 0;
        default: return DefWindowProcW(window, message, word, long_value);
        }
    } catch (const std::exception& error) {
        app->fail(error);
        return 0;
    }
}

} // namespace

int wmain(const int argument_count, wchar_t** const arguments) {
    try {
        bool smoke = false;
        std::filesystem::path resources;
        for (int index = 1; index < argument_count; ++index) {
            const std::wstring_view argument(arguments[index]);
            if (argument == L"--smoke")
                smoke = true;
            else if (resources.empty())
                resources = argument;
            else
                throw std::invalid_argument("usage: strata_desktop_sample [--smoke] <resource-root>");
        }
        if (resources.empty())
            throw std::invalid_argument("desktop example requires the installed Strata resource root");

        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = &window_procedure;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        window_class.lpszClassName = window_class_name;
        if (RegisterClassExW(&window_class) == 0U)
            throw std::runtime_error("could not register the desktop example window class");

        WindowState app;
        const HWND window = CreateWindowExW(0U, window_class_name, L"Strata desktop example",
                                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                             900, 560, nullptr, nullptr, instance, &app);
        if (window == nullptr)
            throw std::runtime_error("could not create the desktop example window");

        strata::desktop::ApplicationConfig config;
        config.application_id = "example.desktop";
        config.module_resource = "assets/strata/samples/desktop_app.strata";
        config.root_name = "DesktopExample";
        app.host = std::make_unique<strata::desktop::ApplicationHost>(window, resources,
                                                                      std::move(config));
        app.host->activate();
        if (!smoke) {
            ShowWindow(window, SW_SHOW);
            UpdateWindow(window);
        }

        int exit_code = 0;
        bool running = true;
        while (running) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    exit_code = static_cast<int>(message.wParam);
                    running = false;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!running)
                break;
            app.host->frame();
            if (smoke) {
                std::cout << "STRATA_DESKTOP_SAMPLE_READY\n";
                break;
            }
            static_cast<void>(MsgWaitForMultipleObjectsEx(0U, nullptr, 16U, QS_ALLINPUT,
                                                           MWMO_INPUTAVAILABLE));
        }
        app.host.reset();
        if (IsWindow(window))
            DestroyWindow(window);
        if (!app.failure.empty())
            throw std::runtime_error(app.failure);
        return exit_code;
    } catch (const std::exception& error) {
        std::cerr << "strata_desktop_sample: " << error.what() << '\n';
        return 1;
    }
}
