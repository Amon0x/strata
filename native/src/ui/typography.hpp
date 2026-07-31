#pragma once

#include <cstdint>

namespace strata::ui {

/** Size-specific grayscale masks are the ordinary UI path; MSDF is an explicit scaling path. */
enum class FontRasterization : std::uint8_t {
    grayscale,
    msdf,
};

} // namespace strata::ui
