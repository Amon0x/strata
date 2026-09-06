#include "input.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace strata::preview {
namespace {
KeyModifiers modifiers(const Uint16 value) {
    KeyModifiers result = KeyModifiers::none;
    if ((value & KMOD_SHIFT) != 0)
        result = result | KeyModifiers::shift;
    if ((value & KMOD_CTRL) != 0)
        result = result | KeyModifiers::control;
    if ((value & KMOD_ALT) != 0)
        result = result | KeyModifiers::alt;
    if ((value & KMOD_GUI) != 0)
        result = result | KeyModifiers::super_key;
    return result;
}
std::string key_name(const SDL_Keycode key) {
    switch (key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        return "enter";
    case SDLK_ESCAPE:
        return "escape";
    case SDLK_SPACE:
        return "space";
    case SDLK_TAB:
        return "tab";
    case SDLK_BACKSPACE:
        return "backspace";
    case SDLK_DELETE:
        return "delete";
    case SDLK_LEFT:
        return "left";
    case SDLK_RIGHT:
        return "right";
    case SDLK_UP:
        return "up";
    case SDLK_DOWN:
        return "down";
    case SDLK_HOME:
        return "home";
    case SDLK_END:
        return "end";
    case SDLK_PAGEUP:
        return "pageup";
    case SDLK_PAGEDOWN:
        return "pagedown";
    case SDLK_INSERT:
        return "insert";
    default:
        if (key >= SDLK_a && key <= SDLK_z)
            return std::string(1, static_cast<char>(key));
        if (key >= SDLK_0 && key <= SDLK_9)
            return std::string(1, static_cast<char>(key));
        if (key >= SDLK_F1 && key <= SDLK_F12)
            return "f" + std::to_string(key - SDLK_F1 + 1);
        return {};
    }
}
std::int32_t button(const Uint8 value) {
    switch (value) {
    case SDL_BUTTON_LEFT:
        return 0;
    case SDL_BUTTON_RIGHT:
        return 1;
    case SDL_BUTTON_MIDDLE:
        return 2;
    default:
        return static_cast<std::int32_t>(value) - 1;
    }
}
// SDL composition selections count Unicode characters; Strata uses UTF-8 byte offsets.
std::size_t byte_offset(const std::string_view text, const std::int64_t count) {
    std::size_t offset = 0;
    for (std::int64_t index = 0; index < count && offset < text.size(); ++index) {
        ++offset;
        while (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xc0U) == 0x80U)
            ++offset;
    }
    return offset;
}
} // namespace

std::optional<InputEvent> translate_input(const SDL_Event& event, const double coordinate_scale,
                                          const Point pointer, const std::int64_t timestamp) {
    const double scale = coordinate_scale > 0.0 ? coordinate_scale : 1.0;
    const auto current_modifiers = modifiers(static_cast<Uint16>(SDL_GetModState()));
    switch (event.type) {
    case SDL_MOUSEMOTION:
        return InputEvent::pointer(InputKind::pointer_move,
                                   Point{event.motion.x / scale, event.motion.y / scale}, 0, 0,
                                   current_modifiers, timestamp);
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        return InputEvent::pointer(event.type == SDL_MOUSEBUTTONDOWN ? InputKind::pointer_press
                                                                     : InputKind::pointer_release,
                                   Point{event.button.x / scale, event.button.y / scale}, 0,
                                   button(event.button.button), current_modifiers, timestamp);
    case SDL_MOUSEWHEEL: {
        const double direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0 : 1.0;
        return InputEvent::scroll(
            pointer,
            Point{event.wheel.preciseX * 40.0 * direction, event.wheel.preciseY * 40.0 * direction},
            current_modifiers, timestamp);
    }
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        std::string key = key_name(event.key.keysym.sym);
        if (key.empty())
            return std::nullopt;
        return InputEvent::key(std::move(key),
                               event.type == SDL_KEYUP ? KeyAction::release
                               : event.key.repeat != 0 ? KeyAction::repeat
                                                       : KeyAction::press,
                               modifiers(event.key.keysym.mod), timestamp);
    }
    case SDL_TEXTINPUT:
        return InputEvent::committed_text(event.text.text, timestamp);
    case SDL_TEXTEDITING:
    case SDL_TEXTEDITING_EXT: {
        const std::string_view text =
            event.type == SDL_TEXTEDITING ? event.edit.text : event.editExt.text;
        const std::int64_t start =
            std::max(0, event.type == SDL_TEXTEDITING ? event.edit.start : event.editExt.start);
        const std::int64_t length =
            std::max(0, event.type == SDL_TEXTEDITING ? event.edit.length : event.editExt.length);
        return InputEvent::preedit(std::string(text), byte_offset(text, start),
                                   byte_offset(text, start + length), timestamp);
    }
    default:
        return std::nullopt;
    }
}
} // namespace strata::preview
