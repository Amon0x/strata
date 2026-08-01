#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

struct HWND__;
using HWND = HWND__*;

namespace strata::desktop::win32 {

struct ImeUpdate final {
    std::optional<std::string> committed;
    std::optional<std::string> preedit;
    std::size_t selection_start = 0U;
    std::size_t selection_end = 0U;
};

[[nodiscard]] std::string utf8_character(wchar_t first, wchar_t second = 0);
[[nodiscard]] std::string utf8_code_point(std::uint32_t code_point);
[[nodiscard]] ImeUpdate read_ime_update(HWND window, std::intptr_t flags);

} // namespace strata::desktop::win32
