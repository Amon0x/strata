#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <string>

namespace strata::win32 {

/**
 * The name Strata gives a Windows virtual key: lower-case words for the named keys ("pageup",
 * "f5"), and the character a key carries unshifted on a US layout for letters, digits and
 * punctuation ("a", "7", "=", "/"), so shortcuts such as "ctrl+=" and "ctrl+/" match. The number
 * pad's digits and operators name the same characters as the main keys. A key without a name is
 * "win32:" and its code.
 */
[[nodiscard]] inline std::string key_name(const std::uint32_t key) {
    switch (key) {
    case VK_TAB:
        return "tab";
    case VK_RETURN:
        return "enter";
    case VK_SPACE:
        return "space";
    case VK_ESCAPE:
        return "escape";
    case VK_BACK:
        return "backspace";
    case VK_DELETE:
        return "delete";
    case VK_LEFT:
        return "left";
    case VK_RIGHT:
        return "right";
    case VK_UP:
        return "up";
    case VK_DOWN:
        return "down";
    case VK_HOME:
        return "home";
    case VK_END:
        return "end";
    case VK_PRIOR:
        return "pageup";
    case VK_NEXT:
        return "pagedown";
    case VK_INSERT:
        return "insert";
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return "shift";
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return "control";
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return "alt";
    case VK_LWIN:
    case VK_RWIN:
        return "super";
    case VK_APPS:
        return "contextmenu";
    case VK_OEM_PLUS:
        return "=";
    case VK_OEM_MINUS:
    case VK_SUBTRACT:
        return "-";
    case VK_ADD:
        return "+";
    case VK_MULTIPLY:
        return "*";
    case VK_DIVIDE:
    case VK_OEM_2:
        return "/";
    case VK_OEM_COMMA:
        return ",";
    case VK_OEM_PERIOD:
    case VK_DECIMAL:
        return ".";
    case VK_OEM_1:
        return ";";
    case VK_OEM_3:
        return "`";
    case VK_OEM_4:
        return "[";
    case VK_OEM_5:
        return "\\";
    case VK_OEM_6:
        return "]";
    case VK_OEM_7:
        return "'";
    default:
        if (key >= 'A' && key <= 'Z')
            return std::string(1U, static_cast<char>('a' + key - 'A'));
        if (key >= '0' && key <= '9')
            return std::string(1U, static_cast<char>(key));
        if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
            return std::string(1U, static_cast<char>('0' + key - VK_NUMPAD0));
        if (key >= VK_F1 && key <= VK_F24)
            return "f" + std::to_string(key - VK_F1 + 1U);
        return "win32:" + std::to_string(key);
    }
}

} // namespace strata::win32
