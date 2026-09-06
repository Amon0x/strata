#include "services.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <strata/strata.hpp>

namespace strata::preview {
strata_clipboard_adapter Services::clipboard() {
    return {sizeof(strata_clipboard_adapter), this, &read, &write};
}
strata_ime_adapter Services::ime() {
    return {sizeof(strata_ime_adapter), this, &active, &caret};
}
strata_status Services::read(void* context, strata_string_view* output) noexcept {
    try {
        std::unique_ptr<char, decltype(&SDL_free)> text(SDL_GetClipboardText(), &SDL_free);
        if (!text)
            return STRATA_STATUS_NOT_FOUND;
        auto& self = *static_cast<Services*>(context);
        self.clipboard_text_ = text.get();
        *output = strata::view(self.clipboard_text_);
        return STRATA_STATUS_OK;
    } catch (...) {
        return STRATA_STATUS_OUT_OF_MEMORY;
    }
}
strata_status Services::write(void*, const strata_string_view text) noexcept {
    try {
        const std::string owned(text.size == 0 ? "" : text.data, text.size);
        return SDL_SetClipboardText(owned.c_str()) == 0 ? STRATA_STATUS_OK
                                                        : STRATA_STATUS_INTERNAL_ERROR;
    } catch (...) {
        return STRATA_STATUS_OUT_OF_MEMORY;
    }
}
strata_status Services::active(void*, const std::uint32_t active) noexcept {
    if (active != 0)
        SDL_StartTextInput();
    else
        SDL_StopTextInput();
    return STRATA_STATUS_OK;
}
strata_status Services::caret(void* context, const strata_rect rect) noexcept {
    const double scale = static_cast<Services*>(context)->coordinate_scale;
    const SDL_Rect target{static_cast<int>(std::floor(rect.x * scale)),
                          static_cast<int>(std::floor(rect.y * scale)),
                          std::max(1, static_cast<int>(std::ceil(rect.width * scale))),
                          std::max(1, static_cast<int>(std::ceil(rect.height * scale)))};
    SDL_SetTextInputRect(&target);
    return STRATA_STATUS_OK;
}
} // namespace strata::preview
