#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
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
#include "window_input.hpp"
#include "window_placement.hpp"
#include "launch.hpp"
#include "performance.hpp"
#include "preview.hpp"

namespace {

constexpr wchar_t window_class_name[] = L"StrataNativeDesktopWindow";
std::size_t active_window_count = 0U;

[[nodiscard]] std::wstring utf8_to_wide(const std::string_view value) {
    if (value.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0)
        throw std::runtime_error("desktop title is not valid UTF-8");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), size) != size) {
        throw std::runtime_error("desktop title conversion was incomplete");
    }
    return result;
}

struct WindowApplication final {
    std::unique_ptr<strata::desktop::Host> host;
    std::unique_ptr<strata::desktop::ApplicationHost> application_host;
    std::unique_ptr<strata::desktop::PreviewSession> preview;
    std::string failure;
    HWND window_handle = nullptr;
    strata::desktop::win32::WindowInputState input;
    /** Where this window's bounds are remembered; empty for benchmark and hidden windows. */
    std::filesystem::path placement_path;
    /** The id documents name this window by in window.close; "main" for the first. */
    std::string window_id;
    /** Opened by a document (window.open): closing it never ends the application. */
    bool child = false;
    bool registered = false;

    [[nodiscard]] std::vector<strata::desktop::WindowRequest> take_window_requests() {
        if (host != nullptr)
            return host->take_window_requests();
        if (application_host != nullptr)
            return application_host->take_window_requests();
        return {};
    }

    void answer_window_request(const strata::desktop::WindowRequest& request, std::string json) {
        if (host != nullptr)
            host->answer_window_request(request, std::move(json));
        else if (application_host != nullptr)
            application_host->answer_window_request(request, std::move(json));
    }

    void remember_placement() const noexcept {
        if (!placement_path.empty())
            strata::desktop::win32::save_window_placement(window_handle, placement_path);
    }

    [[nodiscard]] bool has_host() const noexcept {
        return host != nullptr || application_host != nullptr;
    }

    /** The last frame showed something new: its present already waited for the display. */
    [[nodiscard]] bool presented() const noexcept {
        if (host != nullptr)
            return host->presented();
        return application_host != nullptr && application_host->presented();
    }

    /** The last frame left nothing to do until input or a host change. */
    [[nodiscard]] bool idle() const noexcept {
        if (host != nullptr)
            return host->idle();
        return application_host == nullptr || application_host->idle();
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
        if (!has_host())
            return;
        const std::uint32_t width = static_cast<std::uint32_t>(LOWORD(dimensions));
        const std::uint32_t height = static_cast<std::uint32_t>(HIWORD(dimensions));
        if (width == 0U || height == 0U)
            return;
        const UINT dpi = GetDpiForWindow(window);
        const double scale = dpi == 0U ? 1.0 : static_cast<double>(dpi) / 96.0;
        if (host != nullptr)
            host->resize(width, height, scale);
        else
            application_host->resize(width, height, scale);
    }

    void pointer(const std::uint32_t kind, const std::int32_t button, const double x,
                 const double y) {
        if (host != nullptr)
            host->pointer(kind, button, x, y);
        else if (application_host != nullptr)
            application_host->pointer(kind, button, x, y);
    }

    void scroll(const double x, const double y, const double delta_x, const double delta_y) {
        if (host != nullptr)
            host->scroll(x, y, delta_x, delta_y);
        else if (application_host != nullptr)
            application_host->scroll(x, y, delta_x, delta_y);
    }

    void key(const std::uint32_t virtual_key, const std::uint32_t action) {
        if (host != nullptr) {
            host->key(virtual_key, action);
        } else if (application_host != nullptr) {
            application_host->key(virtual_key, action);
        }
    }

    void text(std::string value) {
        if (host != nullptr)
            host->text(std::move(value));
        else if (application_host != nullptr)
            application_host->text(std::move(value));
    }

    void ime_preedit(std::string value, const std::size_t selection_start,
                     const std::size_t selection_end) {
        if (host != nullptr) {
            host->ime_preedit(std::move(value), selection_start, selection_end);
        } else if (application_host != nullptr) {
            application_host->ime_preedit(std::move(value), selection_start, selection_end);
        }
    }

    [[nodiscard]] std::optional<std::uint32_t> pointer_cursor() noexcept {
        if (host != nullptr)
            return host->pointer_cursor();
        if (application_host != nullptr)
            return application_host->pointer_cursor();
        return std::nullopt;
    }

    void cancel_interactions() noexcept {
        if (host != nullptr)
            host->cancel_interactions();
        else if (application_host != nullptr)
            application_host->cancel_interactions();
    }

    void frame() {
        if (preview != nullptr)
            preview->poll();
        if (host != nullptr)
            host->frame();
        else if (application_host != nullptr)
            application_host->frame();
    }

    void reset_host() noexcept {
        preview.reset();
        application_host.reset();
        host.reset();
    }
};

[[nodiscard]] WindowApplication* application(const HWND window) noexcept {
    return reinterpret_cast<WindowApplication*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

[[nodiscard]] bool activate_foreground_window(const HWND window) noexcept {
    if (window == nullptr || !IsWindow(window))
        return false;
    ShowWindow(window, SW_RESTORE);
    const HWND previous = GetForegroundWindow();
    const DWORD current_thread = GetCurrentThreadId();
    const DWORD foreground_thread =
        previous != nullptr ? GetWindowThreadProcessId(previous, nullptr) : 0U;
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

// Dispatches every queued message, collapsing a run of mouse moves to one window into its latest.
// Returns false once WM_QUIT is dequeued, leaving its exit code in `exit_code`.
[[nodiscard]] bool dispatch_pending_messages(int& exit_code) {
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
            return false;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}

LRESULT CALLBACK window_procedure(const HWND window, const UINT message, const WPARAM word,
                                  const LPARAM long_value) {
    if (message == WM_NCCREATE) {
        const auto* const create = reinterpret_cast<const CREATESTRUCTW*>(long_value);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    WindowApplication* const app = application(window);
    if (app == nullptr)
        return DefWindowProcW(window, message, word, long_value);
    try {
        if (message == WM_EXITSIZEMOVE || message == WM_DESTROY)
            app->remember_placement();
        if (app->application_host != nullptr && message != WM_CLOSE && message != WM_DESTROY) {
            const std::optional<std::intptr_t> handled =
                app->application_host->handle_window_message(message, word, long_value);
            return handled.has_value() ? static_cast<LRESULT>(*handled)
                                       : DefWindowProcW(window, message, word, long_value);
        }
        if (app->host != nullptr) {
            if (const std::optional<std::intptr_t> handled =
                    app->host->window_message(message, word, long_value);
                handled.has_value()) {
                return static_cast<LRESULT>(*handled);
            }
        }
        if (const std::optional<LRESULT> handled = strata::desktop::win32::translate_window_input(
                *app, app->input, window, message, word, long_value);
            handled.has_value()) {
            return *handled;
        }
        switch (message) {
        case WM_SIZE:
            app->resize(window, long_value);
            return 0;
        case WM_DPICHANGED: {
            const auto* const suggested = reinterpret_cast<const RECT*>(long_value);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            app->reset_host();
            // The application lasts as long as a window it began with; the ones its documents
            // opened close with it.
            if (app->registered) {
                app->registered = false;
                if (!app->child && active_window_count > 0U)
                    --active_window_count;
                if (!app->child && active_window_count == 0U)
                    PostQuitMessage(0);
            }
            return 0;
        default:
            return DefWindowProcW(window, message, word, long_value);
        }
    } catch (const std::exception& error) {
        app->failed(error);
        return 0;
    } catch (...) {
        // Nothing may unwind into the Win32 message dispatcher.
        app->failed(std::runtime_error("the desktop window procedure failed"));
        return 0;
    }
}

[[nodiscard]] std::string answer_json(strata::data::JsonValue::Object fields) {
    return strata::data::encode_canonical_json(strata::data::JsonValue(std::move(fields)));
}

/** What a desktop application's windows have in common, for the ones its documents open. */
struct WindowDefaults final {
    bool vsync = true;
    bool performance = false;
    /** Registers actions and publishes snapshots on an application window's host. */
    std::function<void(strata::desktop::ApplicationHost&)> configure_application;
};

/** Opens the window `request` asks `parent` for, answering with { id, opened }. */
[[nodiscard]] std::string open_window(std::vector<std::unique_ptr<WindowApplication>>& applications,
                                      WindowApplication& parent,
                                      const strata::desktop::WindowRequest& request,
                                      const WindowDefaults& defaults) {
    using strata::data::JsonValue;
    for (const auto& existing : applications) {
        if (existing->window_id == request.id && existing->has_host()) {
            // One window per id: asking again brings it forward.
            ShowWindow(existing->window_handle,
                       IsIconic(existing->window_handle) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(existing->window_handle);
            return answer_json({{"existing", JsonValue(true)},
                                {"id", JsonValue(request.id)},
                                {"opened", JsonValue(false)}});
        }
    }
    // Sizes are device-independent, at the DPI of the window that asked.
    const UINT dpi = std::max(GetDpiForWindow(parent.window_handle), 96U);
    const double scale = static_cast<double>(dpi) / 96.0;
    constexpr DWORD window_style = WS_OVERLAPPEDWINDOW;
    RECT bounds{0, 0, static_cast<LONG>(std::lround(request.width.value_or(800.0) * scale)),
                static_cast<LONG>(std::lround(request.height.value_or(600.0) * scale))};
    AdjustWindowRectExForDpi(&bounds, window_style, FALSE, 0U, dpi);
    RECT parent_bounds{};
    GetWindowRect(parent.window_handle, &parent_bounds);
    const int x = request.x.has_value() ? static_cast<int>(std::lround(*request.x * scale))
                                        : parent_bounds.left + static_cast<int>(48.0 * scale);
    const int y = request.y.has_value() ? static_cast<int>(std::lround(*request.y * scale))
                                        : parent_bounds.top + static_cast<int>(48.0 * scale);
    auto app = std::make_unique<WindowApplication>();
    app->window_id = request.id;
    app->child = true;
    const std::wstring title = utf8_to_wide(request.title.empty() ? request.root : request.title);
    const HWND window = CreateWindowExW(0U, window_class_name, title.c_str(), window_style, x, y,
                                        bounds.right - bounds.left, bounds.bottom - bounds.top,
                                        nullptr, nullptr, GetModuleHandleW(nullptr), app.get());
    if (window == nullptr) {
        return answer_json({{"error", JsonValue("the window could not be created")},
                            {"id", JsonValue(request.id)},
                            {"opened", JsonValue(false)}});
    }
    app->window_handle = window;
    try {
        if (parent.host != nullptr) {
            app->host = parent.host->open_window(window, request);
        } else if (parent.application_host != nullptr) {
            app->application_host = parent.application_host->open_window(
                window, request, strata::desktop::ApplicationOptions{.vsync = defaults.vsync});
            if (defaults.configure_application)
                defaults.configure_application(*app->application_host);
            app->application_host->activate();
        }
    } catch (const std::exception& error) {
        app->reset_host();
        DestroyWindow(window);
        return answer_json({{"error", JsonValue(std::string(error.what()))},
                            {"id", JsonValue(request.id)},
                            {"opened", JsonValue(false)}});
    }
    app->registered = true;
    ShowWindow(window, defaults.performance ? SW_SHOWNOACTIVATE : SW_SHOW);
    UpdateWindow(window);
    applications.push_back(std::move(app));
    return answer_json({{"id", JsonValue(request.id)}, {"opened", JsonValue(true)}});
}

/**
 * Opens and closes the windows documents asked for since the last frame, and forgets windows
 * that have closed since.
 */
void serve_window_requests(std::vector<std::unique_ptr<WindowApplication>>& applications,
                           const WindowDefaults& defaults) {
    using strata::data::JsonValue;
    using Kind = strata::desktop::WindowRequest::Kind;
    // Opening appends to `applications`, so the askers are gathered first.
    std::vector<std::pair<WindowApplication*, strata::desktop::WindowRequest>> asked;
    for (const auto& app : applications) {
        for (strata::desktop::WindowRequest& request : app->take_window_requests())
            asked.emplace_back(app.get(), std::move(request));
    }
    for (auto& [asker, request] : asked) {
        std::string answer;
        if (request.kind == Kind::open) {
            answer = open_window(applications, *asker, request, defaults);
        } else {
            bool closed = false;
            for (const auto& app : applications) {
                if (app->window_id == request.id && app->has_host()) {
                    PostMessageW(app->window_handle, WM_CLOSE, 0, 0);
                    closed = true;
                }
            }
            answer = answer_json({{"closed", JsonValue(closed)}, {"id", JsonValue(request.id)}});
        }
        if (asker->has_host())
            asker->answer_window_request(request, std::move(answer));
    }
    std::erase_if(applications, [](const std::unique_ptr<WindowApplication>& app) {
        return app->child && !app->has_host() &&
               (app->window_handle == nullptr || !IsWindow(app->window_handle));
    });
}

[[nodiscard]] std::filesystem::path default_resource_root() {
    std::wstring executable(32'768U, L'\0');
    const DWORD size =
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
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
        bool watch = false;
        bool show_frame_time = false;
        std::filesystem::path performance_scenario_path;
        std::filesystem::path performance_output;
        std::filesystem::path performance_baseline;
        std::filesystem::path manifest_path;
        std::filesystem::path resources;
        const auto require_value = [&](int& index, const std::string_view option) {
            if (++index >= argument_count)
                throw std::invalid_argument(std::string(option) + " requires a value");
            return std::filesystem::path(arguments[index]);
        };
        for (int index = 1; index < argument_count; ++index) {
            const std::wstring_view argument(arguments[index]);
            if (argument == L"--help" || argument == L"-h") {
                std::cout << "usage:\n"
                             "  strata_desktop app.strata-app.json [--watch] [--uncapped] [--fps]\n"
                             "  strata_desktop [--multi-window] [--uncapped] [--fps] [resource-root]\n"
                             "  strata_desktop --performance scenario.json --output directory "
                             "[--baseline performance.json] [resource-root]\n";
                return 0;
            }
            if (argument == L"--smoke")
                smoke = true;
            else if (argument == L"--profile-showcase")
                profile_showcase = true;
            else if (argument == L"--multi-window")
                multi_window = true;
            else if (argument == L"--uncapped")
                uncapped = true;
            else if (argument == L"--watch")
                watch = true;
            else if (argument == L"--fps")
                show_frame_time = true;
            else if (argument == L"--resources")
                resources = require_value(index, "--resources");
            else if (argument == L"--performance")
                performance_scenario_path = require_value(index, "--performance");
            else if (argument == L"--output")
                performance_output = require_value(index, "--output");
            else if (argument == L"--baseline")
                performance_baseline = require_value(index, "--baseline");
            else if (argument.ends_with(L".strata-app.json") && manifest_path.empty())
                manifest_path = argument;
            else if (resources.empty())
                resources = argument;
            else
                throw std::invalid_argument("unknown desktop option");
        }
        const bool manifest_mode = !manifest_path.empty();
        const bool performance = !performance_scenario_path.empty();
        const int exclusive_modes =
            (profile_showcase ? 1 : 0) + (performance ? 1 : 0) + (manifest_mode ? 1 : 0);
        if (exclusive_modes > 1)
            throw std::invalid_argument("application manifest, showcase-profile, and performance "
                                        "modes are mutually exclusive");
        if (manifest_mode && multi_window)
            throw std::invalid_argument(
                "--multi-window is available only for the bundled showcase");
        if (!performance && (!performance_output.empty() || !performance_baseline.empty()))
            throw std::invalid_argument("--output and --baseline require --performance");
        if (performance && performance_output.empty())
            throw std::invalid_argument("--performance requires --output");

        std::optional<strata::desktop::LaunchManifest> launch_manifest =
            manifest_mode ? std::optional(strata::desktop::load_launch_manifest(manifest_path))
                          : std::nullopt;
        if (launch_manifest.has_value() &&
            launch_manifest->kind == strata::desktop::LaunchKind::custom) {
            if (watch || launch_manifest->watch) {
                throw std::invalid_argument("custom hosts own their preview lifecycle; "
                                            "launch.watch and --watch require a generic manifest");
            }
            if (!resources.empty() || uncapped || smoke) {
                throw std::invalid_argument(
                    "--resources, --uncapped, and --smoke are generic-runner options");
            }
            return strata::desktop::run_custom_host(*launch_manifest);
        }

        const bool application_mode = launch_manifest.has_value();
        if (application_mode) {
            watch = watch || launch_manifest->watch;
            if (resources.empty())
                resources = launch_manifest->resource_root;
        } else if (watch) {
            throw std::invalid_argument("--watch requires a generic .strata-app.json manifest");
        }
        if (smoke && !application_mode)
            multi_window = true;
        if (profile_showcase || performance || application_mode)
            multi_window = false;
        if (performance)
            uncapped = true;
        const bool headless = smoke || profile_showcase;
        if (resources.empty())
            resources = default_resource_root();
        const std::optional<strata::desktop::PerformanceScenario> performance_scenario =
            performance ? std::optional(
                              strata::desktop::load_performance_scenario(performance_scenario_path))
                        : std::nullopt;
        const std::optional<strata::headless::Scenario> application_launch =
            application_mode ? std::optional(launch_manifest->application) : std::nullopt;
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

        // Every window of a manifest application, the ones its documents open too, answers the
        // manifest's actions and shows its snapshots.
        WindowDefaults window_defaults{
            .vsync = !uncapped && !headless,
            .performance = performance,
            .configure_application =
                [&application_launch](strata::desktop::ApplicationHost& application) {
                    if (!application_launch.has_value())
                        return;
                    for (const std::string& action_id : application_launch->actions) {
                        application.bindings().on(
                            action_id, [](const strata::host::ActionEvent& event) {
                                std::cout << "STRATA ACTION " << event.id
                                          << " payload=" << event.payload.json() << '\n';
                                return strata::host::ActionResult::handled;
                            });
                    }
                    for (const strata::headless::SnapshotConfig& snapshot :
                         application_launch->snapshots) {
                        application.publish(
                            snapshot.id, strata::host::Value::parse(
                                             strata::data::encode_canonical_json(snapshot.values)));
                    }
                },
        };
        strata::desktop::PerformanceStartup performance_startup;
        if (performance)
            performance_startup.started_at = std::chrono::steady_clock::now();
        const std::size_t window_count = multi_window ? 2U : 1U;
        std::vector<std::unique_ptr<WindowApplication>> applications;
        applications.reserve(window_count);
        for (std::size_t index = 0U; index < window_count; ++index) {
            auto app = std::make_unique<WindowApplication>();
            app->window_id = index == 0U ? "main" : "window-" + std::to_string(index);
            const std::wstring application_title = application_launch.has_value()
                                                       ? utf8_to_wide(launch_manifest->title)
                                                       : std::wstring{};
            const wchar_t* const title = application_launch.has_value() ? application_title.c_str()
                                         : index == 0U ? L"Strata Native Showcase — Alpha"
                                                       : L"Strata Native Showcase — Omega";
            constexpr DWORD window_style = WS_OVERLAPPEDWINDOW;
            RECT window_bounds{
                0,
                0,
                static_cast<LONG>(performance_scenario.has_value()
                                      ? performance_scenario->client_width
                                  : application_launch.has_value() ? application_launch->width
                                                                   : 1120U),
                static_cast<LONG>(performance_scenario.has_value()
                                      ? performance_scenario->client_height
                                  : application_launch.has_value() ? application_launch->height
                                                                   : 760U),
            };
            if ((performance_scenario.has_value() || application_launch.has_value()) &&
                !AdjustWindowRectExForDpi(&window_bounds, window_style, FALSE, 0U,
                                          GetDpiForSystem())) {
                throw std::runtime_error("could not resolve desktop window bounds");
            }
            const auto window_create_started = std::chrono::steady_clock::now();
            const HWND window = CreateWindowExW(
                0U, window_class_name, title, window_style, index == 0U ? CW_USEDEFAULT : 160,
                index == 0U ? CW_USEDEFAULT : 120, window_bounds.right - window_bounds.left,
                window_bounds.bottom - window_bounds.top, nullptr, nullptr, instance, app.get());
            if (window == nullptr) {
                throw std::runtime_error("could not create a native desktop window");
            }
            if (performance && index == 0U) {
                performance_startup.window_create_nanos =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - window_create_started)
                        .count();
            }
            app->window_handle = window;
            bool start_maximized = false;
            if (!performance && !headless) {
                app->placement_path = strata::desktop::win32::window_placement_path(
                    application_launch.has_value()
                        ? "application:" + application_launch->application_id + "#" +
                              std::to_string(index)
                        : "showcase#" + std::to_string(index));
                start_maximized =
                    strata::desktop::win32::restore_window_placement(window, app->placement_path);
            }
            if (performance_scenario.has_value()) {
                RECT exact_bounds{
                    0,
                    0,
                    static_cast<LONG>(performance_scenario->client_width),
                    static_cast<LONG>(performance_scenario->client_height),
                };
                if (!AdjustWindowRectExForDpi(&exact_bounds, window_style, FALSE, 0U,
                                              GetDpiForWindow(window)) ||
                    !SetWindowPos(window, nullptr, 0, 0, exact_bounds.right - exact_bounds.left,
                                  exact_bounds.bottom - exact_bounds.top,
                                  SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
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
                    config.text_antialiasing =
                        application_launch->text_antialiasing == "grayscale"
                            ? strata::desktop::TextAntialiasing::grayscale
                        : application_launch->text_antialiasing == "subpixel"
                            ? strata::desktop::TextAntialiasing::subpixel
                            : strata::desktop::TextAntialiasing::system;
                    if (!application_launch->fonts.empty()) {
                        config.fonts.clear();
                        for (const strata::headless::FontConfig& font : application_launch->fonts) {
                            config.fonts.push_back({font.id, font.resource, font.system});
                        }
                    }
                    if (!application_launch->images.empty()) {
                        config.images.clear();
                        for (const strata::headless::ImageConfig& image :
                             application_launch->images) {
                            config.images.push_back({
                                image.id,
                                image.resource,
                                image.sampling == 0U ? strata::desktop::ImageSampling::nearest
                                                     : strata::desktop::ImageSampling::linear,
                            });
                        }
                    }
                    std::vector<std::string> immutable_resources;
                    if (!config.schemas_resource.empty()) {
                        immutable_resources.push_back(config.schemas_resource);
                    }
                    for (const strata::desktop::FontResource& font : config.fonts) {
                        immutable_resources.push_back(font.resource);
                    }
                    for (const strata::desktop::ImageResource& image : config.images) {
                        immutable_resources.push_back(image.resource);
                    }
                    const std::vector<std::filesystem::path> extension_search_paths =
                        config.extension_search_paths;
                    app->application_host = std::make_unique<strata::desktop::ApplicationHost>(
                        window, resources, std::move(config),
                        strata::desktop::ApplicationOptions{
                            .vsync = !uncapped && !headless,
                            .asynchronous_shader_compilation = !watch,
                        });
                    window_defaults.configure_application(*app->application_host);
                    app->application_host->activate();
                    if (watch) {
                        app->preview = std::make_unique<strata::desktop::PreviewSession>(
                            launch_manifest->path, resources, *app->application_host,
                            std::move(immutable_resources), extension_search_paths);
                    }
                } else {
                    const std::string instance_label =
                        performance
                            ? "Performance benchmark " + std::to_string(GetCurrentProcessId())
                        : index == 0U ? "Independent window alpha 7"
                                      : "Independent window omega 9";
                    app->host = std::make_unique<strata::desktop::Host>(
                        window, resources, instance_label,
                        strata::desktop::HostOptions{
                            .vsync = !uncapped && !headless,
                            .performance_hud = !performance,
                            .profile_sampling = !performance,
                        });
                    if (smoke)
                        app->host->key(VK_F7);
                }
                if (performance && index == 0U) {
                    performance_startup.host_create_nanos =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - host_create_started)
                            .count();
                }
            } catch (...) {
                DestroyWindow(window);
                throw;
            }
            app->registered = true;
            ++active_window_count;
            if (!headless && (!performance || performance_scenario->require_visible)) {
                ShowWindow(window, performance       ? SW_SHOWNOACTIVATE
                                   : start_maximized ? SW_SHOWMAXIMIZED
                                                     : SW_SHOW);
                UpdateWindow(window);
                if (performance_scenario.has_value() && performance_scenario->require_foreground) {
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
                    "could not make the performance window foreground; no objective run was made");
            }
            // The scenario frames the first window; the pump frames any its documents opened.
            WindowApplication* const measured = applications.front().get();
            auto pump_messages = [&applications, &exit_code, &window_defaults, measured]() {
                if (!dispatch_pending_messages(exit_code))
                    return false;
                for (const auto& app : applications) {
                    if (app.get() != measured && app->has_host() && app->failure.empty())
                        app->frame();
                }
                serve_window_requests(applications, window_defaults);
                return measured->host != nullptr &&
                       std::ranges::all_of(applications,
                                           [](const auto& app) { return app->failure.empty(); });
            };
            strata::desktop::PerformanceRunner runner(applications.front()->window_handle,
                                                      *applications.front()->host,
                                                      *performance_scenario, performance_output,
                                                      performance_baseline, performance_startup);
            const bool valid = runner.run(pump_messages);
            exit_code = valid ? 0 : 2;
            running = false;
        }
        bool profile_started = false;
        std::size_t profile_frames_after_open = 0U;
        // --fps reports the host loop's own cadence in the title bar: the mean interval between
        // presented frames over the last half second. With --uncapped that is the full cost of a
        // frame; without it the interval is the display's.
        std::wstring frame_time_title;
        if (show_frame_time && !applications.empty()) {
            frame_time_title.resize(256U);
            frame_time_title.resize(static_cast<std::size_t>(
                GetWindowTextW(applications.front()->window_handle, frame_time_title.data(),
                               static_cast<int>(frame_time_title.size()))));
        }
        auto frame_time_window_started = std::chrono::steady_clock::now();
        std::size_t frame_time_window_frames = 0U;
        while (running) {
            if (!dispatch_pending_messages(exit_code))
                break;
            for (const auto& app : applications) {
                if (!app->has_host())
                    continue;
                try {
                    app->frame();
                } catch (const std::exception& error) {
                    app->failed(error);
                    running = false;
                    exit_code = 1;
                    break;
                }
            }
            if (running)
                serve_window_requests(applications, window_defaults);
            if (show_frame_time && running) {
                ++frame_time_window_frames;
                const auto now = std::chrono::steady_clock::now();
                const double seconds =
                    std::chrono::duration<double>(now - frame_time_window_started).count();
                if (seconds >= 0.5) {
                    const double frames_per_second =
                        static_cast<double>(frame_time_window_frames) / seconds;
                    wchar_t suffix[64]{};
                    static_cast<void>(swprintf_s(suffix, L"  —  %.2f ms  ·  %.0f fps",
                                                 1000.0 / frames_per_second, frames_per_second));
                    SetWindowTextW(applications.front()->window_handle,
                                   (frame_time_title + suffix).c_str());
                    std::cout << "STRATA FRAME " << 1000.0 / frames_per_second << " ms "
                              << frames_per_second << " fps" << std::endl;
                    frame_time_window_started = now;
                    frame_time_window_frames = 0U;
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
                if (applications.size() != 2U || !applications[0]->host->smoke_identity_bound() ||
                    !applications[1]->host->smoke_identity_bound() ||
                    applications[0]->host->smoke_fingerprint() ==
                        applications[1]->host->smoke_fingerprint()) {
                    throw std::runtime_error(
                        "desktop isolation smoke did not observe distinct per-window host state");
                }
                std::cout << "STRATA_NATIVE_DESKTOP_SMOKE_READY windows=2 isolatedPackets=true\n";
                running = false;
            }
            if (!headless && !uncapped) {
                // A window that drew waited for the display in its present, which paces what
                // moves; waiting here as well would hold every frame for a second refresh. Only
                // when no window drew does the loop wait, for input: a frame's length while
                // something is still to do, longer when nothing is, waking for timers and async
                // completions.
                const bool drew = std::ranges::any_of(
                    applications, [](const auto& app) { return app->presented(); });
                if (!drew) {
                    const bool idle = std::ranges::all_of(
                        applications, [](const auto& app) { return app->idle(); });
                    static_cast<void>(MsgWaitForMultipleObjectsEx(
                        0U, nullptr, idle ? 50U : 16U, QS_ALLINPUT, MWMO_INPUTAVAILABLE));
                }
            }
        }
        // The windows documents opened go first: they share their opener's model.
        for (const auto& app : applications) {
            if (app->child)
                app->reset_host();
        }
        for (const auto& app : applications)
            app->reset_host();
        for (const auto& app : applications) {
            if (app->window_handle != nullptr && IsWindow(app->window_handle)) {
                DestroyWindow(app->window_handle);
            }
        }
        for (const auto& app : applications) {
            if (!app->failure.empty())
                throw std::runtime_error(app->failure);
        }
        return exit_code;
    } catch (const std::exception& error) {
        std::cerr << "strata_desktop: " << error.what() << '\n';
        return 1;
    }
}
