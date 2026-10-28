#pragma once

#include <optional>
#include <string_view>

namespace strata::core {

enum class Utf8Blankness { malformed, blank, non_blank };

[[nodiscard]] bool valid_utf8(std::string_view value) noexcept;
/** Classifies UTF-8 text using the Unicode White_Space property, never byte locale rules. */
[[nodiscard]] Utf8Blankness utf8_blankness(std::string_view value) noexcept;
/** Trims the same Unicode White_Space set; malformed UTF-8 has no result. */
[[nodiscard]] std::optional<std::string_view> trim_utf8_white_space(
    std::string_view value
) noexcept;

} // namespace strata::core
