#include "window_control.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace strata::desktop {
namespace {

using data::JsonValue;

[[nodiscard]] std::optional<double> number_field(const JsonValue& payload,
                                                 const std::string_view name) {
    const JsonValue* const value = payload.find(name);
    if (value == nullptr)
        return std::nullopt;
    if (value->number() != nullptr && std::isfinite(*value->number()))
        return *value->number();
    if (value->integer() != nullptr)
        return static_cast<double>(*value->integer());
    return std::nullopt;
}

[[nodiscard]] std::optional<bool> flag_field(const JsonValue& payload,
                                             const std::string_view name) {
    const JsonValue* const value = payload.find(name);
    return value != nullptr && value->boolean() != nullptr ? std::optional(*value->boolean())
                                                           : std::nullopt;
}

[[nodiscard]] std::wstring wide(const std::string_view value) {
    if (value.empty())
        return {};
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    if (size > 0)
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
                            size);
    return result;
}

/** The window's size less its client's: borders, the title bar, and any custom chrome. */
[[nodiscard]] SIZE frame_size(const HWND window) {
    RECT outer{};
    RECT client{};
    GetWindowRect(window, &outer);
    GetClientRect(window, &client);
    return SIZE{(outer.right - outer.left) - (client.right - client.left),
                (outer.bottom - outer.top) - (client.bottom - client.top)};
}

[[nodiscard]] int pixels(const double logical, const double scale) {
    return static_cast<int>(std::lround(logical * scale));
}

} // namespace

WindowControl::WindowControl(const HWND window) : window_(window) {}

WindowControl::~WindowControl() = default;

double WindowControl::dpi_scale() const noexcept {
    const UINT dpi = GetDpiForWindow(window_);
    return dpi == 0U ? 1.0 : static_cast<double>(dpi) / 96.0;
}

bool WindowControl::hidden() const noexcept {
    return hidden_;
}

bool WindowControl::fullscreen() const noexcept {
    return fullscreen_;
}

void WindowControl::enter_fullscreen() {
    if (fullscreen_)
        return;
    auto placement = std::make_unique<WINDOWPLACEMENT>();
    placement->length = sizeof(WINDOWPLACEMENT);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetWindowPlacement(window_, placement.get()) ||
        !GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) {
        return;
    }
    restore_placement_ = std::move(placement);
    restore_style_ = GetWindowLongPtrW(window_, GWL_STYLE);
    // Set first, so the frame calculation the style change sends finds no chrome to keep.
    fullscreen_ = true;
    SetWindowLongPtrW(window_, GWL_STYLE,
                      restore_style_ & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
    const bool topmost = (GetWindowLongPtrW(window_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    const RECT& area = monitor.rcMonitor;
    SetWindowPos(window_, topmost ? HWND_TOPMOST : HWND_TOP, area.left, area.top,
                 area.right - area.left, area.bottom - area.top,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED | (hidden_ ? 0U : SWP_SHOWWINDOW));
}

void WindowControl::leave_fullscreen() {
    if (!fullscreen_)
        return;
    fullscreen_ = false;
    SetWindowLongPtrW(window_, GWL_STYLE, restore_style_);
    if (restore_placement_ != nullptr) {
        // Hidden in the tray, it stays hidden; else it returns as it was, maximized or not.
        if (hidden_)
            restore_placement_->showCmd = SW_HIDE;
        SetWindowPlacement(window_, restore_placement_.get());
    }
    restore_placement_.reset();
    SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED |
                     SWP_NOACTIVATE);
}

JsonValue::Object WindowControl::set(const JsonValue& request) {
    const std::optional<bool> fullscreen = flag_field(request, "fullscreen");
    const std::optional<double> width = number_field(request, "width");
    const std::optional<double> height = number_field(request, "height");
    const std::optional<double> x = number_field(request, "x");
    const std::optional<double> y = number_field(request, "y");
    const bool center = flag_field(request, "center").value_or(false);
    const bool geometry =
        width.has_value() || height.has_value() || x.has_value() || y.has_value() || center;
    // Size and place belong to the window as it normally stands: a fullscreen window leaves
    // fullscreen to take them, and goes back to it unless told otherwise.
    const bool return_to_fullscreen = fullscreen_ && fullscreen != false;
    if (fullscreen_ && (fullscreen == false || geometry))
        leave_fullscreen();

    if (const JsonValue* title = request.find("title");
        title != nullptr && title->string() != nullptr)
        SetWindowTextW(window_, wide(*title->string()).c_str());

    if (const std::optional<bool> resizable = flag_field(request, "resizable");
        resizable.has_value()) {
        constexpr LONG_PTR resizing = WS_THICKFRAME | WS_MAXIMIZEBOX;
        const LONG_PTR style = GetWindowLongPtrW(window_, GWL_STYLE);
        SetWindowLongPtrW(window_, GWL_STYLE, *resizable ? style | resizing : style & ~resizing);
        SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (const std::optional<bool> on_top = flag_field(request, "alwaysOnTop"); on_top.has_value()) {
        SetWindowPos(window_, *on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    if (const std::optional<double> opacity = number_field(request, "opacity");
        opacity.has_value()) {
        opacity_ = std::clamp(*opacity, 0.0, 1.0);
        const LONG_PTR extended = GetWindowLongPtrW(window_, GWL_EXSTYLE);
        if (opacity_ < 1.0) {
            if ((extended & WS_EX_LAYERED) == 0)
                SetWindowLongPtrW(window_, GWL_EXSTYLE, extended | WS_EX_LAYERED);
            SetLayeredWindowAttributes(window_, 0U,
                                       static_cast<BYTE>(std::lround(opacity_ * 255.0)), LWA_ALPHA);
        } else if ((extended & WS_EX_LAYERED) != 0) {
            SetWindowLongPtrW(window_, GWL_EXSTYLE,
                              extended & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
            RedrawWindow(window_, nullptr, nullptr,
                         RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
        }
    }

    const std::optional<double> minimum_width = number_field(request, "minWidth");
    const std::optional<double> minimum_height = number_field(request, "minHeight");
    if (minimum_width.has_value())
        minimum_width_ = std::max(0.0, *minimum_width);
    if (minimum_height.has_value())
        minimum_height_ = std::max(0.0, *minimum_height);

    const double scale = dpi_scale();
    RECT client{};
    GetClientRect(window_, &client);
    // A smaller window than the new smallest size grows to it.
    const bool too_small =
        !IsIconic(window_) && !IsZoomed(window_) && !fullscreen_ &&
        ((minimum_width.has_value() && client.right < pixels(minimum_width_, scale)) ||
         (minimum_height.has_value() && client.bottom < pixels(minimum_height_, scale)));
    if (geometry || too_small) {
        if (IsZoomed(window_) || IsIconic(window_)) {
            ShowWindow(window_, SW_RESTORE);
            GetClientRect(window_, &client);
        }
        RECT outer{};
        GetWindowRect(window_, &outer);
        const SIZE frame = frame_size(window_);
        const int client_width =
            std::max(width.has_value() ? pixels(*width, scale) : static_cast<int>(client.right),
                     pixels(minimum_width_, scale));
        const int client_height =
            std::max(height.has_value() ? pixels(*height, scale) : static_cast<int>(client.bottom),
                     pixels(minimum_height_, scale));
        const int outer_width = client_width + frame.cx;
        const int outer_height = client_height + frame.cy;
        int left = x.has_value() ? pixels(*x, scale) : static_cast<int>(outer.left);
        int top = y.has_value() ? pixels(*y, scale) : static_cast<int>(outer.top);
        if (center) {
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            if (GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) {
                const RECT& work = monitor.rcWork;
                left = work.left + ((work.right - work.left) - outer_width) / 2;
                top = work.top + ((work.bottom - work.top) - outer_height) / 2;
            }
        }
        SetWindowPos(window_, nullptr, left, top, outer_width, outer_height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (fullscreen == true || (return_to_fullscreen && !fullscreen_))
        enter_fullscreen();
    return state();
}

JsonValue::Object WindowControl::state() const {
    const double scale = dpi_scale();
    RECT outer{};
    RECT client{};
    GetWindowRect(window_, &outer);
    GetClientRect(window_, &client);
    const auto logical = [scale](const LONG value) {
        // Two decimals: a fractional DPI scale leaves no meaningful digits past them.
        return JsonValue(std::round(static_cast<double>(value) / scale * 100.0) / 100.0);
    };
    return {
        {"alwaysOnTop", JsonValue((GetWindowLongPtrW(window_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0)},
        {"fullscreen", JsonValue(fullscreen_)},
        {"height", logical(client.bottom - client.top)},
        {"maximized", JsonValue(!fullscreen_ && IsZoomed(window_) != FALSE)},
        {"minimized", JsonValue(IsIconic(window_) != FALSE)},
        {"opacity", JsonValue(opacity_)},
        {"visible", JsonValue(IsWindowVisible(window_) != FALSE)},
        {"width", logical(client.right - client.left)},
        {"x", logical(outer.left)},
        {"y", logical(outer.top)},
    };
}

void WindowControl::show() {
    hidden_ = false;
    ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(window_);
}

void WindowControl::hide() {
    hidden_ = true;
    ShowWindow(window_, SW_HIDE);
}

std::optional<std::intptr_t> WindowControl::window_message(const std::uint32_t message,
                                                           const std::uintptr_t,
                                                           const std::intptr_t long_value) {
    if (message != WM_GETMINMAXINFO || fullscreen_ ||
        (minimum_width_ <= 0.0 && minimum_height_ <= 0.0)) {
        return std::nullopt;
    }
    auto* const limits = reinterpret_cast<MINMAXINFO*>(long_value);
    const double scale = dpi_scale();
    const SIZE frame = frame_size(window_);
    limits->ptMinTrackSize.x =
        std::max<LONG>(limits->ptMinTrackSize.x, pixels(minimum_width_, scale) + frame.cx);
    limits->ptMinTrackSize.y =
        std::max<LONG>(limits->ptMinTrackSize.y, pixels(minimum_height_, scale) + frame.cy);
    return 0;
}

} // namespace strata::desktop
