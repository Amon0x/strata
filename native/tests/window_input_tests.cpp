#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "window_input.hpp"

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

/** Records what the translator forwards, in order. */
struct RecordingSink final {
    struct Pointer final {
        std::uint32_t kind = 0U;
        std::int32_t button = 0;
        double x = 0.0;
        double y = 0.0;
    };
    std::vector<Pointer> pointers;
    std::vector<std::pair<double, double>> scrolls;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> keys;
    std::vector<std::string> texts;
    std::vector<std::string> preedits;
    int cancels = 0;

    void pointer(const std::uint32_t kind, const std::int32_t button, const double x,
                 const double y) {
        pointers.push_back(Pointer{kind, button, x, y});
    }
    void scroll(double, double, const double delta_x, const double delta_y) {
        scrolls.emplace_back(delta_x, delta_y);
    }
    void key(const std::uint32_t virtual_key, const std::uint32_t action) {
        keys.emplace_back(virtual_key, action);
    }
    void text(std::string value) { texts.push_back(std::move(value)); }
    void ime_preedit(std::string value, std::size_t, std::size_t) {
        preedits.push_back(std::move(value));
    }
    void cancel_interactions() noexcept { ++cancels; }

};

// Mouse buttons press, capture and release; X buttons report TRUE; the wheel is in steps.
void test_pointer_and_capture(const HWND window) {
    using strata::desktop::win32::translate_window_input;
    RecordingSink sink;
    strata::desktop::win32::WindowInputState state;
    const auto send = [&](const UINT message, const WPARAM word, const LPARAM long_value) {
        return translate_window_input(sink, state, window, message, word, long_value);
    };
    check(send(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 20)) == std::optional<LRESULT>(0),
          "a left press was not handled");
    check(sink.pointers.size() == 1U && sink.pointers[0].kind == STRATA_INPUT_POINTER_PRESS &&
              sink.pointers[0].button == 0 && sink.pointers[0].x == 10.0 &&
              sink.pointers[0].y == 20.0,
          "a left press was not forwarded with its button and position");
    check(state.captured_buttons == 1U && GetCapture() == window,
          "a press did not capture the pointer");
    check(send(WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON2), MAKELPARAM(1, 1)) ==
              std::optional<LRESULT>(1),
          "an X button press did not report TRUE");
    check(sink.pointers.back().button == 4 && state.captured_buttons == (1U | 16U),
          "the second X button was not mapped to button 4");
    static_cast<void>(send(WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2), MAKELPARAM(1, 1)));
    check(state.captured_buttons == 1U && GetCapture() == window,
          "releasing one of two held buttons released the capture");
    static_cast<void>(send(WM_LBUTTONUP, 0, MAKELPARAM(12, 22)));
    check(state.captured_buttons == 0U && GetCapture() != window &&
              sink.pointers.back().kind == STRATA_INPUT_POINTER_RELEASE,
          "releasing the last button kept the capture");

    static_cast<void>(send(WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-2 * WHEEL_DELTA)), 0));
    static_cast<void>(send(WM_MOUSEHWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0));
    check(sink.scrolls.size() == 2U && sink.scrolls[0] == std::pair(0.0, -2.0) &&
              sink.scrolls[1] == std::pair(1.0, 0.0),
          "wheel messages were not converted to vertical and horizontal steps");

    static_cast<void>(send(WM_LBUTTONDOWN, MK_LBUTTON, 0));
    static_cast<void>(send(WM_CAPTURECHANGED, 0, 0));
    check(state.captured_buttons == 0U && sink.cancels == 1,
          "losing the capture mid-press did not cancel the interaction");
}

// Keys carry press, repeat and release; F10 is kept from the system; text decodes surrogates.
void test_keys_and_text(const HWND window) {
    using strata::desktop::win32::translate_window_input;
    RecordingSink sink;
    strata::desktop::win32::WindowInputState state;
    const auto send = [&](const UINT message, const WPARAM word, const LPARAM long_value) {
        return translate_window_input(sink, state, window, message, word, long_value);
    };
    static_cast<void>(send(WM_KEYDOWN, 'A', 0));
    static_cast<void>(send(WM_KEYDOWN, 'A', static_cast<LPARAM>(KF_REPEAT) << 16));
    static_cast<void>(send(WM_KEYUP, 'A', 0));
    check(sink.keys.size() == 3U && sink.keys[0].second == STRATA_KEY_PRESS &&
              sink.keys[1].second == STRATA_KEY_REPEAT && sink.keys[2].second == STRATA_KEY_RELEASE,
          "key press, repeat and release were not distinguished");
    check(send(WM_SYSKEYDOWN, VK_F10, 0) == std::optional<LRESULT>(0),
          "F10 reached the system, which would enter menu mode");
    check(!send(WM_SYSKEYDOWN, VK_F4, 0).has_value() && sink.keys.back().first == VK_F4,
          "a system key other than F10 was kept from the system or not forwarded");

    // U+1F600 arrives as a surrogate pair; control characters are not text.
    static_cast<void>(send(WM_CHAR, 0xD83D, 0));
    static_cast<void>(send(WM_CHAR, 0xDE00, 0));
    static_cast<void>(send(WM_CHAR, 0x08, 0));
    static_cast<void>(send(WM_CHAR, L'x', 0));
    check(sink.texts.size() == 2U && sink.texts[0] == "\xF0\x9F\x98\x80" && sink.texts[1] == "x",
          "WM_CHAR text was not decoded into UTF-8 or control characters leaked through");
    check(send(WM_UNICHAR, UNICODE_NOCHAR, 0) == std::optional<LRESULT>(1),
          "WM_UNICHAR support was not advertised");
    static_cast<void>(send(WM_UNICHAR, 0x1F600, 0));
    check(sink.texts.back() == "\xF0\x9F\x98\x80", "WM_UNICHAR was not decoded into UTF-8");

    static_cast<void>(send(WM_CHAR, 0xD83D, 0));
    static_cast<void>(send(WM_KILLFOCUS, 0, 0));
    check(state.high_surrogate == 0 && sink.cancels == 1 && !sink.preedits.empty(),
          "losing focus did not drop a pending surrogate, preedit and interaction");
    check(!send(WM_PAINT, 0, 0).has_value(), "a non-input message was claimed as input");
}

} // namespace

int main() {
    const HWND window = CreateWindowExW(0, L"STATIC", L"strata-window-input-test", WS_POPUP, 0, 0,
                                        64, 64, nullptr, nullptr, GetModuleHandleW(nullptr),
                                        nullptr);
    if (window == nullptr) {
        std::cerr << "strata_window_input_tests: could not create a test window\n";
        return 1;
    }
    int status = 0;
    try {
        test_pointer_and_capture(window);
        test_keys_and_text(window);
        std::cout << "strata_window_input_tests: OK\n";
    } catch (const std::exception& error) {
        std::cerr << "strata_window_input_tests: " << error.what() << '\n';
        status = 1;
    }
    DestroyWindow(window);
    return status;
}
