#pragma once
#include <SDL.h>
#include <cstdint>
#include <strata/strata.h>
#include <string>

namespace strata::preview {
class Services final {
  public:
    double coordinate_scale = 1.0;
    [[nodiscard]] strata_clipboard_adapter clipboard();
    [[nodiscard]] strata_ime_adapter ime();

  private:
    std::string clipboard_text_;
    static strata_status read(void*, strata_string_view*) noexcept;
    static strata_status write(void*, strata_string_view) noexcept;
    static strata_status active(void*, std::uint32_t) noexcept;
    static strata_status caret(void*, strata_rect) noexcept;
};
} // namespace strata::preview
