#pragma once
#include <SDL.h>
#include <cstdint>
#include <optional>
#include <strata/strata.h>
#include <string>

namespace strata::preview {
class Services final {
  public:
    explicit Services(bool ime_owner = true) : ime_owner_(ime_owner) {}
    double coordinate_scale = 1.0;
    [[nodiscard]] strata_clipboard_adapter clipboard();
    [[nodiscard]] strata_ime_adapter ime();
    /** Promotes/deactivates a Session's staged IME state; retired Sessions cannot stop its successor. */
    void set_ime_owner(bool owner) noexcept;

  private:
    std::string clipboard_text_;
    bool ime_owner_ = true;
    bool ime_active_ = false;
    std::optional<strata_rect> caret_;
    void publish_caret() const noexcept;
    static strata_status read(void*, strata_string_view*) noexcept;
    static strata_status write(void*, strata_string_view) noexcept;
    static strata_status active(void*, std::uint32_t) noexcept;
    static strata_status caret(void*, strata_rect) noexcept;
};
} // namespace strata::preview
