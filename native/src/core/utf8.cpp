#include "core/utf8.hpp"

#include <cstddef>
#include <cstdint>

namespace strata::core {
namespace {

[[nodiscard]] bool decode(
    const std::string_view value,
    std::size_t& index,
    std::uint32_t& code_point
) noexcept {
    const auto lead = static_cast<std::uint8_t>(value[index]);
    if (lead <= 0x7FU) {
        code_point = lead;
        ++index;
        return true;
    }

    std::size_t continuation_count = 0U;
    std::uint32_t minimum = 0U;
    if ((lead & 0xE0U) == 0xC0U) {
        continuation_count = 1U;
        code_point = lead & 0x1FU;
        minimum = 0x80U;
    } else if ((lead & 0xF0U) == 0xE0U) {
        continuation_count = 2U;
        code_point = lead & 0x0FU;
        minimum = 0x800U;
    } else if ((lead & 0xF8U) == 0xF0U) {
        continuation_count = 3U;
        code_point = lead & 0x07U;
        minimum = 0x10000U;
    } else {
        return false;
    }
    if (continuation_count > value.size() - index - 1U) return false;
    for (std::size_t offset = 1U; offset <= continuation_count; ++offset) {
        const auto continuation = static_cast<std::uint8_t>(value[index + offset]);
        if ((continuation & 0xC0U) != 0x80U) return false;
        code_point = (code_point << 6U) | (continuation & 0x3FU);
    }
    if (code_point < minimum || code_point > 0x10FFFFU ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
        return false;
    }
    index += continuation_count + 1U;
    return true;
}

[[nodiscard]] bool unicode_white_space(const std::uint32_t value) noexcept {
    return (value >= 0x0009U && value <= 0x000DU) || value == 0x0020U ||
           value == 0x0085U || value == 0x00A0U || value == 0x1680U ||
           (value >= 0x2000U && value <= 0x200AU) || value == 0x2028U ||
           value == 0x2029U || value == 0x202FU || value == 0x205FU ||
           value == 0x3000U;
}

} // namespace

bool valid_utf8(const std::string_view value) noexcept {
    std::size_t index = 0U;
    while (index < value.size()) {
        std::uint32_t code_point = 0U;
        if (!decode(value, index, code_point)) return false;
    }
    return true;
}

Utf8Blankness utf8_blankness(const std::string_view value) noexcept {
    std::size_t index = 0U;
    bool blank = true;
    while (index < value.size()) {
        std::uint32_t code_point = 0U;
        if (!decode(value, index, code_point)) return Utf8Blankness::malformed;
        blank = blank && unicode_white_space(code_point);
    }
    return blank ? Utf8Blankness::blank : Utf8Blankness::non_blank;
}

std::optional<std::string_view> trim_utf8_white_space(
    const std::string_view value
) noexcept {
    std::size_t index = 0U;
    std::size_t first_non_white_space = value.size();
    std::size_t last_non_white_space = 0U;
    while (index < value.size()) {
        const std::size_t code_point_start = index;
        std::uint32_t code_point = 0U;
        if (!decode(value, index, code_point)) return std::nullopt;
        if (!unicode_white_space(code_point)) {
            if (first_non_white_space == value.size()) {
                first_non_white_space = code_point_start;
            }
            last_non_white_space = index;
        }
    }
    if (first_non_white_space == value.size()) return value.substr(value.size());
    return value.substr(
        first_non_white_space,
        last_non_white_space - first_non_white_space
    );
}

} // namespace strata::core
