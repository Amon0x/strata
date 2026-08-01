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

#include <strata/desktop.hpp>
#include <strata/strata.h>

#include "data/json.hpp"
#include "headless/scenario.hpp"
#include "host.hpp"
#include "ime.hpp"
#include "performance.hpp"

namespace {

constexpr wchar_t window_class_name[] = L"StrataNativeDesktopWindow";
std::size_t active_window_count = 0U;

[[nodiscard]] std::wstring utf8_to_wide(const std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0
    );
    if (size <= 0) throw std::runtime_error("desktop title is not valid UTF-8");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size
        ) != size) {
        throw std::runtime_error("desktop title conversion was incomplete");
    }
    return result;
}

struct WindowApplication final {
    std::unique_ptr<strata::desktop::Host> host;
    std::unique_ptr<strata::desktop::ApplicationHost> application_host;
    std::string failure;
    HWND window_handle = nullptr;
    wchar_t high_surrogate = 0;
    std::uint32_t captured_buttons = 0U;
    bool tracking_mouse_leave = false;
    bool registered = false;

    [[nodiscard]] bool has_host() const noexcept {
        return host != nullptr || application_host != nullptr;
    }

    void failed(const std::exception& error) noexcept {
        try {
            failure = error.what();
        } catch (...) {
            failure = "native desktop failed without a diagnostic";
        }
        PostQuitMessage(1);
    }

    void resize(const HWND window, const LPARAM dimensions) {
        if (!has_host()) return;
        const std::uint32_t width = static_cast<std::uint32_t>(LOWORD(dimensions));
        const std::uint32_t height = static_cast<std::uint32_t>(HIWORD(dimensions));
        if (width == 0U || height == 0U) return;
        const UINT dpi = GetDpiForWindow(window);
        const double scale = dpi == 0U ? 1.0 : static_cast<double>(dpi) / 96.0;
        if (host != nullptr) host->resize(width, height, scale);
        else application_host->resize(width, height, scale);
    }

    void pointer(const std::uint32_t kind, const std::int32_t button,
                 const double x, const double y) {
        if (host != nullptr) host->pointer(kind, button, x, y);
        else if (application_host != nullptr) application_host->pointer(kind, button, x, y);
    }

    void scroll(const double x, const double y, const double delta_x, const double delta_y) {
        if (host != nullptr) host->scroll(x, y, delta_x, delta_y);
        else if (application_host != nullptr) application_host->scroll(x, y, delta_x, delta_y);
    }

    void key(const std::uint32_t virtual_key, const std::uint32_t action) {
        if (host != nullptr) {
            host->key(virtual_key, action);
        } else if (application_host != nullptr) {
            application_host->key(virtual_key, action);
        }
    }

    void text(std::string value) {
        if (host != nullptr) host->text(std::move(value));
        else if (application_host != nullptr) application_host->text(std::move(value));
    }

    void ime_preedit(
        std::string value,
        const std::size_t selection_start,
        const std::size_t selection_end
    ) {
        if (host != nullptr) {
            host->ime_preedit(std::move(value), selection_start, selection_end);
        } else if (application_host != nullptr) {
            application_host->ime_preedit(std::move(value), selection_start, selection_end);
        }
    }

    void ime_composition(const std::intptr_t flags) {
        const strata::desktop::win32::ImeUpdate update =
            strata::desktop::win32::read_ime_update(window_handle, flags);
        if (update.committed.has_value() && !update.committed->empty()) text(*update.committed);
        if (update.preedit.has_value()) {
            ime_preedit(*update.preedit, update.selection_start, update.selection_end);
        }
    }

    void cancel_interactions() noexcept {
        if (host != nullptr) host->cancel_interactions();
        else if (application_host != nullptr) application_host->cancel_interactions();
    }

    void frame() {
        if (host != nullptr) host->frame();
        else if (application_host != nullptr) application_host->frame();
    }

    void reset_host() noexcept {
        application_host.reset();
        host.reset();
    }

    void character(const wchar_t value) {
        if (value >= 0xD800 && value <= 0xDBFF) {
            high_surrogate = value;
            return;
        }
        if (value >= 0xDC00 && value <= 0xDFFF) {
            if (high_surrogate == 0) return;
            text(strata::desktop::win32::utf8_character(high_surrogate, value));
            high_surrogate = 0;
            return;
        }
        high_surrogate = 0;
        if (value >= 0x20 && value != 0x7f) {
            text(strata::desktop::win32::utf8_character(value));
        }
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
        if (app->application_host != nullptr && message != WM_CLOSE && message != WM_DESTROY) {
            const std::optional<std::intptr_t> handled =
                app->application_host->handle_window_message(message, word, long_value);
            return handled.has_value()
                ? static_cast<LRESULT>(*handled)
                : DefWindowProcW(window, message, word, long_value);
        }
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
            if (!app->tracking_mouse_leave) {
                TRACKMOUSEEVENT tracking{
                    sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, HOVER_DEFAULT,
                };
                app->tracking_mouse_leave = TrackMouseEvent(&tracking) != FALSE;
            }
            app->pointer(
                STRATA_INPUT_POINTER_MOVE,
                0,
                GET_X_LPARAM(long_value),
                GET_Y_LPARAM(long_value)
            );
            return 0;
        case WM_MOUSELEAVE:
            app->tracking_mouse_leave = false;
            if (app->captured_buttons == 0U) {
                app->pointer(STRATA_INPUT_POINTER_CANCEL, 0, 0.0, 0.0);
            }
            return 0;
        case WM_XBUTTONDOWN: {
            const bool first = GET_XBUTTON_WPARAM(word) == XBUTTON1;
            app->captured_buttons |= first ? 8U : 16U;
            SetFocus(window);
            SetCapture(window);
            app->pointer(
                STRATA_INPUT_POINTER_PRESS,
                first ? 3 : 4,
                GET_X_LPARAM(long_value),
                GET_Y_LPARAM(long_value)
            );
            return 1;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
            app->captured_buttons |= message == WM_LBUTTONDOWN ? 1U
                : message == WM_RBUTTONDOWN ? 2U : 4U;
            SetFocus(window);
            SetCapture(window);
            app->pointer(
                STRATA_INPUT_POINTER_PRESS,
                message == WM_LBUTTONDOWN ? 0 : message == WM_RBUTTONDOWN ? 1 : 2,
                GET_X_LPARAM(long_value),
                GET_Y_LPARAM(long_value)
            );
            return 0;
        case WM_XBUTTONUP: {
            const bool first = GET_XBUTTON_WPARAM(word) == XBUTTON1;
            app->pointer(
                STRATA_INPUT_POINTER_RELEASE,
                first ? 3 : 4,
                GET_X_LPARAM(long_value),
                GET_Y_LPARAM(long_value)
            );
            app->captured_buttons &= first ? ~8U : ~16U;
            if (app->captured_buttons == 0U && GetCapture() == window) ReleaseCapture();
            return 1;
        }
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
            app->pointer(
                STRATA_INPUT_POINTER_RELEASE,
                message == WM_LBUTTONUP ? 0 : message == WM_RBUTTONUP ? 1 : 2,
                GET_X_LPARAM(long_value),
                GET_Y_LPARAM(long_value)
            );
            app->captured_buttons &= message == WM_LBUTTONUP ? ~1U
                : message == WM_RBUTTONUP ? ~2U : ~4U;
            if (app->captured_buttons == 0U && GetCapture() == window) ReleaseCapture();
            return 0;
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(long_value), GET_Y_LPARAM(long_value)};
            ScreenToClient(window, &point);
            app->scroll(
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
            app->scroll(
                point.x,
                point.y,
                static_cast<double>(GET_WHEEL_DELTA_WPARAM(word)) / WHEEL_DELTA,
                0.0
            );
            return 0;
        }
        case WM_KEYDOWN:
            app->key(
                static_cast<std::uint32_t>(word),
                (HIWORD(long_value) & KF_REPEAT) != 0U
                    ? STRATA_KEY_REPEAT
                    : STRATA_KEY_PRESS
            );
            return 0;
        case WM_KEYUP:
            app->key(static_cast<std::uint32_t>(word), STRATA_KEY_RELEASE);
            return 0;
        case WM_SYSKEYDOWN:
            // Bare F10 is a Win32 system key that normally enters menu-key modality. Strata owns
            // it as an application shortcut, so forwarding it to DefWindowProc would logically
            // toggle the surface and then stall visible presentation until menu mode is dismissed.
            if (word == VK_F10) {
                app->key(
                    VK_F10,
                    (HIWORD(long_value) & KF_REPEAT) != 0U
                        ? STRATA_KEY_REPEAT
                        : STRATA_KEY_PRESS
                );
                return 0;
            }
            app->key(
                static_cast<std::uint32_t>(word),
                (HIWORD(long_value) & KF_REPEAT) != 0U
                    ? STRATA_KEY_REPEAT
                    : STRATA_KEY_PRESS
            );
            return DefWindowProcW(window, message, word, long_value);
        case WM_SYSKEYUP:
            app->key(static_cast<std::uint32_t>(word), STRATA_KEY_RELEASE);
            if (word == VK_F10) return 0;
            return DefWindowProcW(window, message, word, long_value);
        case WM_CHAR:
            if (app->host != nullptr) app->character(static_cast<wchar_t>(word));
            return 0;
        case WM_UNICHAR:
            if (word == UNICODE_NOCHAR) return 1;
            if (word < 0x20U || word == 0x7FU) return 0;
            if (const std::string value = strata::desktop::win32::utf8_code_point(
                    static_cast<std::uint32_t>(word)
                ); !value.empty()) {
                app->text(value);
            }
            return 0;
        case WM_IME_STARTCOMPOSITION:
            app->ime_preedit({}, 0U, 0U);
            return 0;
        case WM_IME_COMPOSITION:
            app->ime_composition(long_value);
            return 0;
        case WM_IME_ENDCOMPOSITION:
            app->ime_preedit({}, 0U, 0U);
            return 0;
        case WM_IME_CHAR:
            return 0;
        case WM_KILLFOCUS:
        case WM_CANCELMODE:
            app->captured_buttons = 0U;
            app->high_surrogate = 0;
            app->ime_preedit({}, 0U, 0U);
            app->cancel_interactions();
            if (GetCapture() == window) ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            if (app->captured_buttons != 0U) {
                app->captured_buttons = 0U;
                app->cancel_interactions();
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            app->reset_host();
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
        std::filesystem::path application_launch_path;
        std::filesystem::path resources;
        const auto require_value = [&](int& index, const std::string_view option) {
            if (++index >= argument_count)
                throw std::invalid_argument(std::string(option) + " requires a value");
            return std::filesystem::path(arguments[index]);
        };
        for (int index = 1; index < argument_count; ++index) {
            const std::wstring_view argument(arguments[index]);
            if (argument == L"--help" || argument == L"-h") {
                std::cout
                    << "usage:\n"
                       "  strata_desktop --application launch.json [--resources resource-root]\n"
                       "  strata_desktop [--multi-window] [--uncapped] [resource-root]\n"
                       "  strata_desktop --performance scenario.json --output directory "
                       "[--baseline performance.json] [resource-root]\n";
                return 0;
            }
            if (argument == L"--smoke") smoke = true;
            else if (argument == L"--profile-showcase") profile_showcase = true;
            else if (argument == L"--multi-window") multi_window = true;
            else if (argument == L"--uncapped") uncapped = true;
            else if (argument == L"--application")
                application_launch_path = require_value(index, "--application");
            else if (argument == L"--resources")
                resources = require_value(index, "--resources");
            else if (argument == L"--performance")
                performance_scenario_path = require_value(index, "--performance");
            else if (argument == L"--output")
                performance_output = require_value(index, "--output");
            else if (argument == L"--baseline")
                performance_baseline = require_value(index, "--baseline");
            else if (resources.empty()) resources = argument;
            else
                throw std::invalid_argument("unknown desktop option");
        }
        const bool application_mode = !application_launch_path.empty();
        const bool performance = !performance_scenario_path.empty();
        const int exclusive_modes = (profile_showcase ? 1 : 0) + (performance ? 1 : 0) +
                                    (application_mode ? 1 : 0);
        if (exclusive_modes > 1)
            throw std::invalid_argument(
                "application, showcase-profile, and performance modes are mutually exclusive");
        if (application_mode && multi_window)
            throw std::invalid_argument("--multi-window is available only for the bundled showcase");
        if (!performance && (!performance_output.empty() || !performance_baseline.empty()))
            throw std::invalid_argument("--output and --baseline require --performance");
        if (performance && performance_output.empty())
            throw std::invalid_argument("--performance requires --output");
        if (smoke && !application_mode) multi_window = true;
        if (profile_showcase || performance || application_mode) multi_window = false;
        if (performance) uncapped = true;
        const bool headless = smoke || profile_showcase;
        if (resources.empty()) resources = default_resource_root();
        const std::optional<strata::desktop::PerformanceScenario> performance_scenario =
            performance
                ? std::optional(strata::desktop::load_performance_scenario(
                      performance_scenario_path
                  ))
                : std::nullopt;
        const std::optional<strata::headless::Scenario> application_launch =
            application_mode
                ? std::optional(strata::headless::load_scenario(application_launch_path))
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
            const std::wstring application_title = application_launch.has_value()
                ? utf8_to_wide(application_launch->application_id)
                : std::wstring{};
            const wchar_t* const title = application_launch.has_value()
                ? application_title.c_str()
                : index == 0U ? L"Strata Native Showcase — Alpha"
                              : L"Strata Native Showcase — Omega";
            constexpr DWORD window_style = WS_OVERLAPPEDWINDOW;
            RECT window_bounds{
                0,
                0,
                static_cast<LONG>(performance_scenario.has_value()
                    ? performance_scenario->client_width
                    : application_launch.has_value() ? application_launch->width : 1120U),
                static_cast<LONG>(performance_scenario.has_value()
                    ? performance_scenario->client_height
                    : application_launch.has_value() ? application_launch->height : 760U),
            };
            if ((performance_scenario.has_value() || application_launch.has_value()) &&
                !AdjustWindowRectExForDpi(
                    &window_bounds, window_style, FALSE, 0U, GetDpiForSystem()
                )) {
                throw std::runtime_error("could not resolve desktop window bounds");
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
                const auto host_create_started = std::chrono::steady_clock::now();
                if (application_launch.has_value()) {
                    strata::desktop::ApplicationConfig config;
                    config.application_id = application_launch->application_id;
                    config.surface_id = application_launch->surface_id;
                    config.module_resource = application_launch->module.generic_string();
                    config.schemas_resource = application_launch->schemas.generic_string();
                    config.root_name = application_launch->root;
                    config.root_role = application_launch->root_role == "screen"
                        ? strata::desktop::SurfaceRole::screen
                        : strata::desktop::SurfaceRole::overlay;
                    config.extension_packages = application_launch->packages;
                    config.extension_search_paths = application_launch->extension_search_paths;
                    config.reduced_motion = application_launch->reduced_motion;
                    if (!application_launch->fonts.empty()) {
                        config.fonts.clear();
                        for (const strata::headless::FontConfig& font :
                             application_launch->fonts) {
                            config.fonts.push_back({font.id, font.resource});
                        }
                    }
                    if (!application_launch->textures.empty()) {
                        config.textures.clear();
                        for (const strata::headless::TextureConfig& texture :
                             application_launch->textures) {
                            config.textures.push_back({
                                texture.id,
                                texture.resource,
                                texture.sampling == 0U
                                    ? strata::desktop::TextureSampling::nearest
                                    : strata::desktop::TextureSampling::linear,
                            });
                        }
                    }
                    app->application_host =
                        std::make_unique<strata::desktop::ApplicationHost>(
                            window,
                            resources,
                            std::move(config),
                            strata::desktop::ApplicationOptions{.vsync = !uncapped && !headless}
                        );
                    for (const std::string& action_id : application_launch->actions) {
                        app->application_host->bindings().on(
                            action_id,
                            [](const strata::host::ActionEvent& event) {
                                std::cout << "STRATA ACTION " << event.id
                                          << " payload=" << event.payload.json() << '\n';
                                return strata::host::ActionResult::handled;
                            }
                        );
                    }
                    for (const strata::headless::SnapshotConfig& snapshot :
                         application_launch->snapshots) {
                        app->application_host->publish(
                            snapshot.id,
                            strata::host::Value::parse(
                                strata::data::encode_canonical_json(snapshot.values)
                            )
                        );
                    }
                    app->application_host->activate();
                } else {
                    const std::string instance_label = performance
                        ? "Performance benchmark " + std::to_string(GetCurrentProcessId())
                        : index == 0U ? "Independent window alpha 7"
                                      : "Independent window omega 9";
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
                    if (smoke) app->host->key(VK_F7);
                }
                if (performance && index == 0U) {
                    performance_startup.host_create_nanos =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - host_create_started
                        ).count();
                }
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
                if (!app->has_host()) continue;
                try {
                    app->frame();
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
            if (smoke && application_mode && running &&
                applications.front()->application_host != nullptr &&
                applications.front()->application_host->has_frame()) {
                std::cout << "STRATA_DESKTOP_APPLICATION_READY\n";
                running = false;
            } else if (smoke && !application_mode && running &&
                       std::ranges::all_of(applications, [](const auto& app) {
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
        for (const auto& app : applications) app->reset_host();
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
