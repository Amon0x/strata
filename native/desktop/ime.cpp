#include "ime.hpp"

#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <imm.h>

namespace strata::desktop::win32 {
namespace {

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

class ImeContext final {
public:
    explicit ImeContext(const HWND window) noexcept
        : window_(window), context_(ImmGetContext(window)) {}
    ImeContext(const ImeContext&) = delete;
    ImeContext& operator=(const ImeContext&) = delete;
    ~ImeContext() {
        if (context_ != nullptr) static_cast<void>(ImmReleaseContext(window_, context_));
    }
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

} // namespace

std::string utf8_character(const wchar_t first, const wchar_t second) {
    const wchar_t characters[]{first, second};
    return wide_to_utf8(std::wstring_view(characters, second == 0 ? 1U : 2U));
}

std::string utf8_code_point(const std::uint32_t code_point) {
    if (code_point > 0x10FFFFU || (code_point >= 0xD800U && code_point <= 0xDFFFU)) return {};
    if (code_point <= 0xFFFFU) {
        return utf8_character(static_cast<wchar_t>(code_point));
    }
    const std::uint32_t value = code_point - 0x10000U;
    return utf8_character(
        static_cast<wchar_t>(0xD800U + (value >> 10U)),
        static_cast<wchar_t>(0xDC00U + (value & 0x3FFU))
    );
}

ImeUpdate read_ime_update(const HWND window, const std::intptr_t raw_flags) {
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

} // namespace strata::desktop::win32
