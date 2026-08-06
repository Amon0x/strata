#include <strata/win32.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <imm.h>
#include <windowsx.h>

#include <strata/strata.hpp>

namespace strata::win32 {
namespace {

[[nodiscard]] std::int64_t steady_time() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

[[nodiscard]] std::string wide_to_utf8(const std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (size <= 0) throw std::runtime_error("Win32 text input is not valid Unicode");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            size,
            nullptr,
            nullptr
        ) != size) {
        throw std::runtime_error("Win32 text input conversion was incomplete");
    }
    return result;
}

[[nodiscard]] std::string utf8_character(const wchar_t first, const wchar_t second = 0) {
    const wchar_t characters[]{first, second};
    return wide_to_utf8(std::wstring_view(characters, second == 0 ? 1U : 2U));
}

[[nodiscard]] std::string utf8_code_point(const std::uint32_t code_point) {
    if (code_point > 0x10FFFFU ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
        return {};
    }
    if (code_point <= 0xFFFFU) {
        return utf8_character(static_cast<wchar_t>(code_point));
    }
    const std::uint32_t value = code_point - 0x10000U;
    return utf8_character(
        static_cast<wchar_t>(0xD800U + (value >> 10U)),
        static_cast<wchar_t>(0xDC00U + (value & 0x3FFU))
    );
}

[[nodiscard]] KeyModifiers current_modifiers() noexcept {
    KeyModifiers result = KeyModifiers::none;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) result = result | KeyModifiers::shift;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) result = result | KeyModifiers::control;
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) result = result | KeyModifiers::alt;
    if ((GetKeyState(VK_LWIN) & 0x8000) != 0 ||
        (GetKeyState(VK_RWIN) & 0x8000) != 0) {
        result = result | KeyModifiers::super_key;
    }
    return result;
}

[[nodiscard]] std::string key_name(const std::uint32_t key) {
    switch (key) {
    case VK_TAB: return "tab";
    case VK_RETURN: return "enter";
    case VK_SPACE: return "space";
    case VK_ESCAPE: return "escape";
    case VK_BACK: return "backspace";
    case VK_DELETE: return "delete";
    case VK_LEFT: return "left";
    case VK_RIGHT: return "right";
    case VK_UP: return "up";
    case VK_DOWN: return "down";
    case VK_HOME: return "home";
    case VK_END: return "end";
    case VK_PRIOR: return "page_up";
    case VK_NEXT: return "page_down";
    case VK_INSERT: return "insert";
    default:
        if (key >= 'A' && key <= 'Z') {
            return std::string(1U, static_cast<char>('a' + key - 'A'));
        }
        if (key >= '0' && key <= '9') return std::string(1U, static_cast<char>(key));
        return "win32:" + std::to_string(key);
    }
}

void enqueue(strata_surface* const surface, const InputEvent& event) {
    const strata_input_event native = event.native();
    strata_surface_input_batch_info info{};
    info.struct_size = sizeof(info);
    require_ok(
        strata_surface_enqueue_input(surface, &native, 1U, &info),
        "Win32 Surface input enqueue"
    );
}

void cancel_interactions(strata_surface* const surface) {
    require_ok(
        strata_surface_cancel_interactions(surface),
        "Win32 Surface interaction cancellation"
    );
}

class ImeContext final {
  public:
    explicit ImeContext(const HWND window) noexcept
        : window_(window), context_(ImmGetContext(window)) {}
    ~ImeContext() {
        if (context_ != nullptr) static_cast<void>(ImmReleaseContext(window_, context_));
    }
    ImeContext(const ImeContext&) = delete;
    ImeContext& operator=(const ImeContext&) = delete;
    [[nodiscard]] HIMC get() const noexcept { return context_; }

  private:
    HWND window_ = nullptr;
    HIMC context_ = nullptr;
};

[[nodiscard]] std::wstring composition_text(const HIMC context, const DWORD index) {
    const LONG byte_count = ImmGetCompositionStringW(context, index, nullptr, 0U);
    if (byte_count < 0 || byte_count % static_cast<LONG>(sizeof(wchar_t)) != 0) {
        throw std::runtime_error("Win32 IME text could not be read");
    }
    std::wstring result(static_cast<std::size_t>(byte_count) / sizeof(wchar_t), L'\0');
    if (byte_count != 0 && ImmGetCompositionStringW(
            context,
            index,
            result.data(),
            static_cast<DWORD>(byte_count)
        ) != byte_count) {
        throw std::runtime_error("Win32 IME text read was incomplete");
    }
    return result;
}

[[nodiscard]] std::vector<BYTE> composition_attributes(const HIMC context) {
    const LONG byte_count = ImmGetCompositionStringW(context, GCS_COMPATTR, nullptr, 0U);
    if (byte_count <= 0) return {};
    std::vector<BYTE> result(static_cast<std::size_t>(byte_count));
    if (ImmGetCompositionStringW(
            context,
            GCS_COMPATTR,
            result.data(),
            static_cast<DWORD>(result.size())
        ) != byte_count) {
        return {};
    }
    return result;
}

[[nodiscard]] std::size_t utf8_offset(
    const std::wstring_view value,
    std::size_t utf16_offset
) {
    utf16_offset = std::min(utf16_offset, value.size());
    if (utf16_offset != 0U && utf16_offset < value.size() &&
        value[utf16_offset] >= 0xDC00 && value[utf16_offset] <= 0xDFFF &&
        value[utf16_offset - 1U] >= 0xD800 && value[utf16_offset - 1U] <= 0xDBFF) {
        --utf16_offset;
    }
    return wide_to_utf8(value.substr(0U, utf16_offset)).size();
}

[[nodiscard]] std::pair<std::size_t, std::size_t> composition_selection(
    const HIMC context,
    const std::wstring_view value
) {
    std::size_t start = 0U;
    std::size_t end = 0U;
    const std::vector<BYTE> attributes = composition_attributes(context);
    const auto target = [](const BYTE attribute) {
        return attribute == ATTR_TARGET_CONVERTED || attribute == ATTR_TARGET_NOTCONVERTED;
    };
    const auto first = std::ranges::find_if(attributes, target);
    if (first != attributes.end()) {
        const auto finish = std::find_if_not(first, attributes.end(), target);
        start = static_cast<std::size_t>(first - attributes.begin());
        end = static_cast<std::size_t>(finish - attributes.begin());
    } else {
        const LONG cursor = ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0U);
        start = cursor < 0 ? 0U : static_cast<std::size_t>(cursor);
        end = start;
    }
    return {utf8_offset(value, start), utf8_offset(value, end)};
}

struct ImeUpdate final {
    std::optional<std::string> committed;
    std::optional<std::string> preedit;
    std::size_t selection_start = 0U;
    std::size_t selection_end = 0U;
};

[[nodiscard]] ImeUpdate read_ime_update(const HWND window, const std::intptr_t raw_flags) {
    ImeUpdate result;
    const ImeContext context(window);
    if (context.get() == nullptr) return result;
    const DWORD flags = static_cast<DWORD>(raw_flags);
    if ((flags & GCS_RESULTSTR) != 0U) {
        result.committed = wide_to_utf8(composition_text(context.get(), GCS_RESULTSTR));
    }
    if ((flags & GCS_COMPSTR) != 0U) {
        const std::wstring preedit = composition_text(context.get(), GCS_COMPSTR);
        result.preedit = wide_to_utf8(preedit);
        const auto [start, end] = composition_selection(context.get(), preedit);
        result.selection_start = start;
        result.selection_end = end;
    }
    return result;
}

} // namespace

struct InputAdapter::Impl final {
    explicit Impl(InputAdapterOptions options)
        : options(std::move(options)) {
        if (!this->options.clock) this->options.clock = &steady_time;
        set_coordinate_scale(this->options.coordinate_scale);
    }

    ~Impl() {
        reset(capture_window);
    }

    [[nodiscard]] Point point(const LPARAM value) const noexcept {
        return {
            static_cast<double>(GET_X_LPARAM(value)) / options.coordinate_scale,
            static_cast<double>(GET_Y_LPARAM(value)) / options.coordinate_scale,
        };
    }

    [[nodiscard]] std::int64_t now() const {
        return options.clock();
    }

    void enqueue_pointer(
        strata_surface* const surface,
        const InputKind kind,
        const LPARAM value,
        const std::int32_t button
    ) {
        last_position = point(value);
        enqueue(surface, InputEvent::pointer(
            kind,
            last_position,
            0,
            button,
            current_modifiers(),
            now()
        ));
    }

    void set_coordinate_scale(const double scale) {
        if (!std::isfinite(scale) || scale <= 0.0) {
            throw std::invalid_argument("Win32 input coordinate scale must be finite and positive");
        }
        options.coordinate_scale = scale;
    }

    void reset(void* const native_window) noexcept {
        HWND window = static_cast<HWND>(native_window);
        if (window == nullptr) window = capture_window;
        if (options.manage_pointer_capture && window != nullptr && GetCapture() == window) {
            static_cast<void>(ReleaseCapture());
        }
        capture_window = nullptr;
        captured_buttons = 0U;
        high_surrogate = 0;
        tracking_mouse_leave = false;
    }

    InputAdapterOptions options;
    Point last_position;
    std::uint32_t captured_buttons = 0U;
    wchar_t high_surrogate = 0;
    bool tracking_mouse_leave = false;
    HWND capture_window = nullptr;
};

InputAdapter::InputAdapter(InputAdapterOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

InputAdapter::~InputAdapter() = default;
InputAdapter::InputAdapter(InputAdapter&&) noexcept = default;
InputAdapter& InputAdapter::operator=(InputAdapter&&) noexcept = default;

std::optional<std::intptr_t> InputAdapter::handle(
    Surface& surface,
    void* const native_window,
    const std::uint32_t message,
    const std::uintptr_t raw_word_parameter,
    const std::intptr_t raw_long_parameter
) {
    return handle(
        surface.native_handle(),
        native_window,
        message,
        raw_word_parameter,
        raw_long_parameter
    );
}

std::optional<std::intptr_t> InputAdapter::handle(
    strata_surface* const surface,
    void* const native_window,
    const std::uint32_t message,
    const std::uintptr_t raw_word_parameter,
    const std::intptr_t raw_long_parameter
) {
    if (surface == nullptr) throw std::invalid_argument("Win32 input requires a live Surface");
    HWND window = static_cast<HWND>(native_window);
    const WPARAM word_parameter = static_cast<WPARAM>(raw_word_parameter);
    const LPARAM long_parameter = static_cast<LPARAM>(raw_long_parameter);
    switch (message) {
    case WM_MOUSEMOVE: {
        if (!impl_->tracking_mouse_leave && window != nullptr) {
            TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, HOVER_DEFAULT};
            impl_->tracking_mouse_leave = TrackMouseEvent(&tracking) != FALSE;
        }
        impl_->enqueue_pointer(surface, InputKind::pointer_move, long_parameter, 0);
        return 0;
    }
    case WM_MOUSELEAVE:
        impl_->tracking_mouse_leave = false;
        if (impl_->captured_buttons == 0U) {
            enqueue(surface, InputEvent::pointer(
                InputKind::pointer_cancel,
                impl_->last_position,
                0,
                0,
                current_modifiers(),
                impl_->now()
            ));
        }
        return 0;
    case WM_XBUTTONDOWN: {
        const bool first = GET_XBUTTON_WPARAM(word_parameter) == XBUTTON1;
        impl_->captured_buttons |= first ? 8U : 16U;
        if (window != nullptr && impl_->options.focus_on_pointer_press) SetFocus(window);
        if (window != nullptr && impl_->options.manage_pointer_capture) {
            SetCapture(window);
            impl_->capture_window = window;
        }
        impl_->enqueue_pointer(
            surface,
            InputKind::pointer_press,
            long_parameter,
            first ? 3 : 4
        );
        return 1;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        impl_->captured_buttons |= message == WM_LBUTTONDOWN ? 1U
            : message == WM_RBUTTONDOWN ? 2U : 4U;
        if (window != nullptr && impl_->options.focus_on_pointer_press) SetFocus(window);
        if (window != nullptr && impl_->options.manage_pointer_capture) {
            SetCapture(window);
            impl_->capture_window = window;
        }
        impl_->enqueue_pointer(
            surface,
            InputKind::pointer_press,
            long_parameter,
            message == WM_LBUTTONDOWN ? 0 : message == WM_RBUTTONDOWN ? 1 : 2
        );
        return 0;
    case WM_XBUTTONUP: {
        const bool first = GET_XBUTTON_WPARAM(word_parameter) == XBUTTON1;
        impl_->enqueue_pointer(
            surface,
            InputKind::pointer_release,
            long_parameter,
            first ? 3 : 4
        );
        impl_->captured_buttons &= first ? ~8U : ~16U;
        if (impl_->captured_buttons == 0U) {
            if (impl_->options.manage_pointer_capture &&
                window != nullptr && GetCapture() == window) {
                static_cast<void>(ReleaseCapture());
            }
            impl_->capture_window = nullptr;
        }
        return 1;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        impl_->enqueue_pointer(
            surface,
            InputKind::pointer_release,
            long_parameter,
            message == WM_LBUTTONUP ? 0 : message == WM_RBUTTONUP ? 1 : 2
        );
        impl_->captured_buttons &= message == WM_LBUTTONUP ? ~1U
            : message == WM_RBUTTONUP ? ~2U : ~4U;
        if (impl_->captured_buttons == 0U) {
            if (impl_->options.manage_pointer_capture &&
                window != nullptr && GetCapture() == window) {
                static_cast<void>(ReleaseCapture());
            }
            impl_->capture_window = nullptr;
        }
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        POINT position{GET_X_LPARAM(long_parameter), GET_Y_LPARAM(long_parameter)};
        if (window != nullptr) static_cast<void>(ScreenToClient(window, &position));
        const double delta = static_cast<double>(
            GET_WHEEL_DELTA_WPARAM(word_parameter)
        ) / WHEEL_DELTA;
        enqueue(surface, InputEvent::scroll(
            {
                static_cast<double>(position.x) / impl_->options.coordinate_scale,
                static_cast<double>(position.y) / impl_->options.coordinate_scale,
            },
            {
                message == WM_MOUSEHWHEEL ? delta : 0.0,
                message == WM_MOUSEWHEEL ? delta : 0.0,
            },
            current_modifiers(),
            impl_->now()
        ));
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        const bool released = message == WM_KEYUP || message == WM_SYSKEYUP;
        const KeyAction action = released
            ? KeyAction::release
            : (HIWORD(long_parameter) & KF_REPEAT) != 0U
                ? KeyAction::repeat
                : KeyAction::press;
        enqueue(surface, InputEvent::key(
            key_name(static_cast<std::uint32_t>(word_parameter)),
            action,
            current_modifiers(),
            impl_->now()
        ));
        const bool system = message == WM_SYSKEYDOWN || message == WM_SYSKEYUP;
        return system && !impl_->options.consume_system_keys
            ? std::nullopt
            : std::optional<std::intptr_t>(0);
    }
    case WM_CHAR: {
        const wchar_t value = static_cast<wchar_t>(word_parameter);
        if (value >= 0xD800 && value <= 0xDBFF) {
            impl_->high_surrogate = value;
            return 0;
        }
        std::string text;
        if (value >= 0xDC00 && value <= 0xDFFF) {
            if (impl_->high_surrogate != 0) {
                text = utf8_character(impl_->high_surrogate, value);
            }
        } else if (value >= 0x20 && value != 0x7F) {
            text = utf8_character(value);
        }
        impl_->high_surrogate = 0;
        if (!text.empty()) {
            enqueue(surface, InputEvent::committed_text(
                std::move(text),
                impl_->now()
            ));
        }
        return 0;
    }
    case WM_UNICHAR:
        if (word_parameter == UNICODE_NOCHAR) return 1;
        if (word_parameter >= 0x20U && word_parameter != 0x7FU) {
            if (std::string text = utf8_code_point(static_cast<std::uint32_t>(word_parameter));
                !text.empty()) {
                enqueue(surface, InputEvent::committed_text(
                    std::move(text),
                    impl_->now()
                ));
            }
        }
        return 0;
    case WM_IME_STARTCOMPOSITION:
        enqueue(surface, InputEvent::preedit({}, 0U, 0U, impl_->now()));
        return 0;
    case WM_IME_COMPOSITION: {
        const ImeUpdate update = read_ime_update(window, raw_long_parameter);
        if (update.committed.has_value() && !update.committed->empty()) {
            enqueue(surface, InputEvent::committed_text(
                *update.committed,
                impl_->now()
            ));
        }
        if (update.preedit.has_value()) {
            enqueue(surface, InputEvent::preedit(
                *update.preedit,
                update.selection_start,
                update.selection_end,
                impl_->now()
            ));
        }
        return 0;
    }
    case WM_IME_ENDCOMPOSITION:
        enqueue(surface, InputEvent::preedit({}, 0U, 0U, impl_->now()));
        return 0;
    case WM_IME_CHAR:
        return 0;
    case WM_KILLFOCUS:
    case WM_CANCELMODE:
        cancel_interactions(surface);
        impl_->reset(native_window);
        return 0;
    case WM_CAPTURECHANGED:
        impl_->capture_window = nullptr;
        if (impl_->captured_buttons != 0U) {
            impl_->captured_buttons = 0U;
            cancel_interactions(surface);
        }
        return 0;
    default:
        return std::nullopt;
    }
}

void InputAdapter::set_coordinate_scale(const double scale) {
    impl_->set_coordinate_scale(scale);
}

double InputAdapter::coordinate_scale() const noexcept {
    return impl_->options.coordinate_scale;
}

void InputAdapter::reset(void* const window) noexcept {
    impl_->reset(window);
}

} // namespace strata::win32
