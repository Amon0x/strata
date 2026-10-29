#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "data/json.hpp"

struct HWND__;
using HWND = HWND__*;
struct tagWINDOWPLACEMENT;

namespace strata::desktop {

/**
 * `window.set`, `window.show` and `window.hideToTray` on one top-level window: borderless
 * fullscreen on its monitor (and back to where it was), always on top, size and place in
 * device-independent pixels, a smallest size, opacity, title and resizing.
 */
class WindowControl final {
  public:
    explicit WindowControl(HWND window);
    ~WindowControl();

    WindowControl(const WindowControl&) = delete;
    WindowControl& operator=(const WindowControl&) = delete;

    /** Applies the fields `request` gives (any of window.set's) and returns the resulting state. */
    [[nodiscard]] data::JsonValue::Object set(const data::JsonValue& request);
    /** { fullscreen, alwaysOnTop, x, y, width, height, maximized, minimized, visible, opacity }. */
    [[nodiscard]] data::JsonValue::Object state() const;
    /** Shows the window (from the tray, minimized or behind others) and brings it forward. */
    void show();
    /** Hides the window and its taskbar button. */
    void hide();
    [[nodiscard]] bool hidden() const noexcept;
    [[nodiscard]] bool fullscreen() const noexcept;
    /** Messages it answers itself (the smallest size the window may take); else nothing. */
    [[nodiscard]] std::optional<std::intptr_t>
    window_message(std::uint32_t message, std::uintptr_t word, std::intptr_t long_value);

  private:
    void enter_fullscreen();
    void leave_fullscreen();
    /** Window pixels per device-independent pixel on the window's monitor. */
    [[nodiscard]] double dpi_scale() const noexcept;

    HWND window_ = nullptr;
    bool fullscreen_ = false;
    bool hidden_ = false;
    /** The window's style and placement before it went fullscreen, to return to. */
    std::intptr_t restore_style_ = 0;
    std::unique_ptr<tagWINDOWPLACEMENT> restore_placement_;
    /** The smallest client size, in device-independent pixels; zero is the system's own. */
    double minimum_width_ = 0.0;
    double minimum_height_ = 0.0;
    double opacity_ = 1.0;
};

} // namespace strata::desktop
