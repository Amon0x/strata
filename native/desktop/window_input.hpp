#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <windowsx.h>

#include <cstdint>
#include <optional>
#include <string>

#include <strata/strata.h>

#include "ime.hpp"

namespace strata::desktop::win32 {

/** What translating one window's input remembers between its messages. */
struct WindowInputState final {
    /** Mouse buttons held since their press, as bits: left 1, right 2, middle 4, X1 8, X2 16. */
    std::uint32_t captured_buttons = 0U;
    bool tracking_mouse_leave = false;
    /** The first half of a surrogate pair WM_CHAR delivered, awaiting its second half. */
    wchar_t high_surrogate = 0;
};

/** The system cursor for one of the `STRATA_POINTER_CURSOR_*` shapes; NONE hides the pointer. */
[[nodiscard]] inline HCURSOR system_cursor(const std::uint32_t cursor) noexcept {
    LPTSTR name = IDC_ARROW;
    switch (cursor) {
    case STRATA_POINTER_CURSOR_POINTER: name = IDC_HAND; break;
    case STRATA_POINTER_CURSOR_TEXT: name = IDC_IBEAM; break;
    // Windows has no open or closed hand; the size-all arrows are its closest move shape.
    case STRATA_POINTER_CURSOR_GRAB:
    case STRATA_POINTER_CURSOR_GRABBING:
    case STRATA_POINTER_CURSOR_MOVE: name = IDC_SIZEALL; break;
    case STRATA_POINTER_CURSOR_NOT_ALLOWED: name = IDC_NO; break;
    case STRATA_POINTER_CURSOR_RESIZE_EW: name = IDC_SIZEWE; break;
    case STRATA_POINTER_CURSOR_RESIZE_NS: name = IDC_SIZENS; break;
    case STRATA_POINTER_CURSOR_RESIZE_NESW: name = IDC_SIZENESW; break;
    case STRATA_POINTER_CURSOR_RESIZE_NWSE: name = IDC_SIZENWSE; break;
    case STRATA_POINTER_CURSOR_CROSSHAIR: name = IDC_CROSS; break;
    case STRATA_POINTER_CURSOR_WAIT: name = IDC_WAIT; break;
    case STRATA_POINTER_CURSOR_PROGRESS: name = IDC_APPSTARTING; break;
    case STRATA_POINTER_CURSOR_HELP: name = IDC_HELP; break;
    case STRATA_POINTER_CURSOR_NONE: return nullptr;
    default: break;
    }
    return LoadCursor(nullptr, name);
}

/**
 * Translates one window message into Strata input on `sink`, which offers pointer, scroll, key,
 * text, ime_preedit, cancel_interactions and pointer_cursor (nullopt when no surface shows). Returns the message result when the message is
 * input; nullopt when it is not, or when the system must still see it (a system key other than
 * F10), so the caller passes it on to DefWindowProc. Window management (size, DPI, close) stays
 * with the caller.
 */
template <typename Sink>
[[nodiscard]] std::optional<LRESULT> translate_window_input(Sink& sink, WindowInputState& state,
                                                            const HWND window, const UINT message,
                                                            const WPARAM word,
                                                            const LPARAM long_value) {
    const auto x = [long_value] { return static_cast<double>(GET_X_LPARAM(long_value)); };
    const auto y = [long_value] { return static_cast<double>(GET_Y_LPARAM(long_value)); };
    const auto key_action = [message, long_value] {
        if (message == WM_KEYUP || message == WM_SYSKEYUP)
            return static_cast<std::uint32_t>(STRATA_KEY_RELEASE);
        return static_cast<std::uint32_t>((HIWORD(long_value) & KF_REPEAT) != 0U
                                              ? STRATA_KEY_REPEAT
                                              : STRATA_KEY_PRESS);
    };
    const auto release_capture = [&state, window] {
        if (state.captured_buttons == 0U && GetCapture() == window)
            ReleaseCapture();
    };
    switch (message) {
    case WM_SETCURSOR: {
        // Only the client area shows the surface's shape; borders and captions keep their own.
        if (LOWORD(long_value) != HTCLIENT)
            return std::nullopt;
        const std::optional<std::uint32_t> cursor = sink.pointer_cursor();
        if (!cursor.has_value())
            return std::nullopt;
        SetCursor(system_cursor(*cursor));
        return TRUE;
    }
    case WM_MOUSEMOVE:
        if (!state.tracking_mouse_leave) {
            TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, HOVER_DEFAULT};
            state.tracking_mouse_leave = TrackMouseEvent(&tracking) != FALSE;
        }
        sink.pointer(STRATA_INPUT_POINTER_MOVE, 0, x(), y());
        return 0;
    case WM_MOUSELEAVE:
        state.tracking_mouse_leave = false;
        if (state.captured_buttons == 0U)
            sink.pointer(STRATA_INPUT_POINTER_CANCEL, 0, 0.0, 0.0);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN: {
        const bool first_x = GET_XBUTTON_WPARAM(word) == XBUTTON1;
        const std::int32_t button = message == WM_LBUTTONDOWN   ? 0
                                    : message == WM_RBUTTONDOWN ? 1
                                    : message == WM_MBUTTONDOWN ? 2
                                    : first_x                   ? 3
                                                                : 4;
        state.captured_buttons |= 1U << static_cast<std::uint32_t>(button);
        SetFocus(window);
        SetCapture(window);
        sink.pointer(STRATA_INPUT_POINTER_PRESS, button, x(), y());
        // An X button message reports that it was handled by returning TRUE.
        return message == WM_XBUTTONDOWN ? 1 : 0;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP: {
        const bool first_x = GET_XBUTTON_WPARAM(word) == XBUTTON1;
        const std::int32_t button = message == WM_LBUTTONUP   ? 0
                                    : message == WM_RBUTTONUP ? 1
                                    : message == WM_MBUTTONUP ? 2
                                    : first_x                 ? 3
                                                              : 4;
        sink.pointer(STRATA_INPUT_POINTER_RELEASE, button, x(), y());
        state.captured_buttons &= ~(1U << static_cast<std::uint32_t>(button));
        release_capture();
        return message == WM_XBUTTONUP ? 1 : 0;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        // Wheel positions arrive in screen coordinates.
        POINT point{GET_X_LPARAM(long_value), GET_Y_LPARAM(long_value)};
        ScreenToClient(window, &point);
        const double delta = static_cast<double>(GET_WHEEL_DELTA_WPARAM(word)) / WHEEL_DELTA;
        sink.scroll(point.x, point.y, message == WM_MOUSEHWHEEL ? delta : 0.0,
                    message == WM_MOUSEWHEEL ? delta : 0.0);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_KEYUP:
        sink.key(static_cast<std::uint32_t>(word), key_action());
        return 0;
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        sink.key(static_cast<std::uint32_t>(word), key_action());
        // Bare F10 is a system key that would enter menu-key modality. Strata owns it as an
        // application shortcut, so the system must not see it; other system keys (Alt+F4) must.
        if (word == VK_F10)
            return 0;
        return std::nullopt;
    case WM_CHAR: {
        const wchar_t value = static_cast<wchar_t>(word);
        if (value >= 0xD800 && value <= 0xDBFF) {
            state.high_surrogate = value;
            return 0;
        }
        if (value >= 0xDC00 && value <= 0xDFFF) {
            if (state.high_surrogate != 0)
                sink.text(utf8_character(state.high_surrogate, value));
            state.high_surrogate = 0;
            return 0;
        }
        state.high_surrogate = 0;
        if (value >= 0x20 && value != 0x7F)
            sink.text(utf8_character(value));
        return 0;
    }
    case WM_UNICHAR:
        // Answering UNICODE_NOCHAR with TRUE tells the sender this window takes WM_UNICHAR.
        if (word == UNICODE_NOCHAR)
            return 1;
        if (word >= 0x20U && word != 0x7FU) {
            if (std::string value = utf8_code_point(static_cast<std::uint32_t>(word));
                !value.empty()) {
                sink.text(std::move(value));
            }
        }
        return 0;
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_ENDCOMPOSITION:
        sink.ime_preedit({}, 0U, 0U);
        return 0;
    case WM_IME_COMPOSITION: {
        const ImeUpdate update = read_ime_update(window, long_value);
        if (update.committed.has_value() && !update.committed->empty())
            sink.text(*update.committed);
        if (update.preedit.has_value())
            sink.ime_preedit(*update.preedit, update.selection_start, update.selection_end);
        return 0;
    }
    case WM_IME_CHAR:
        // The composition's committed text already arrived through WM_IME_COMPOSITION.
        return 0;
    case WM_KILLFOCUS:
    case WM_CANCELMODE:
        state.captured_buttons = 0U;
        state.high_surrogate = 0;
        sink.ime_preedit({}, 0U, 0U);
        sink.cancel_interactions();
        if (GetCapture() == window)
            ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        if (state.captured_buttons != 0U) {
            state.captured_buttons = 0U;
            sink.cancel_interactions();
        }
        return 0;
    default:
        return std::nullopt;
    }
}

} // namespace strata::desktop::win32
