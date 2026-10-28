#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace strata::ui {

/** The Unicode version of the grapheme property data. */
[[nodiscard]] std::string_view grapheme_unicode_version() noexcept;

/**
 * UAX #29 extended grapheme cluster boundaries of valid UTF-8 text, as ascending byte offsets.
 * The first is always 0 and the last is always `text.size()`.
 */
[[nodiscard]] std::vector<std::size_t> grapheme_boundaries(std::string_view text);

} // namespace strata::ui
