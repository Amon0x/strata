#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <strata/desktop.hpp>

namespace {

constexpr wchar_t window_class_name[] = L"StrataInstalledDesktopExample";

struct WindowState final {
    std::unique_ptr<strata::desktop::ApplicationHost> host;
    std::string failure;

    void fail(const std::exception& error) noexcept {
        try {
            failure = error.what();
        } catch (...) {
            failure = "desktop example failed without a diagnostic";
        }
        PostQuitMessage(1);
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
        if (app->host != nullptr && message != WM_CLOSE && message != WM_DESTROY) {
            const std::optional<std::intptr_t> handled =
                app->host->handle_window_message(message, word, long_value);
            return handled.has_value() ? static_cast<LRESULT>(*handled)
                                       : DefWindowProcW(window, message, word, long_value);
        }
        switch (message) {
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            app->host.reset();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, word, long_value);
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
                throw std::invalid_argument(
                    "usage: strata_desktop_sample [--smoke] <resource-root>");
        }
        if (resources.empty())
            throw std::invalid_argument(
                "desktop example requires the installed Strata resource root");

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
                                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900,
                                            560, nullptr, nullptr, instance, &app);
        if (window == nullptr)
            throw std::runtime_error("could not create the desktop example window");

        strata::desktop::ApplicationConfig config;
        config.application_id = "example.desktop";
        config.module_resource = "assets/strata/samples/desktop_app.strata";
        config.root_name = "DesktopExample";
        app.host = std::make_unique<strata::desktop::ApplicationHost>(window, resources,
                                                                      std::move(config));
        app.host->activate();
        if (smoke) {
            const std::optional<std::intptr_t> unicode_query =
                app.host->handle_window_message(WM_UNICHAR, UNICODE_NOCHAR, 0);
            const std::optional<std::intptr_t> pointer_move =
                app.host->handle_window_message(WM_MOUSEMOVE, 0U, MAKELPARAM(4, 4));
            const std::optional<std::intptr_t> pointer_leave =
                app.host->handle_window_message(WM_MOUSELEAVE, 0U, 0);
            const std::optional<std::intptr_t> ime_start =
                app.host->handle_window_message(WM_IME_STARTCOMPOSITION, 0U, 0);
            const std::optional<std::intptr_t> ime_end =
                app.host->handle_window_message(WM_IME_ENDCOMPOSITION, 0U, 0);
            if (unicode_query != 1 || pointer_move != 0 || pointer_leave != 0 || ime_start != 0 ||
                ime_end != 0) {
                throw std::runtime_error("desktop window-message integration rejected smoke input");
            }
        } else {
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
            static_cast<void>(
                MsgWaitForMultipleObjectsEx(0U, nullptr, 16U, QS_ALLINPUT, MWMO_INPUTAVAILABLE));
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
