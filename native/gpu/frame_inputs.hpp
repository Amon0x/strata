#pragma once

#include <array>
#include <string_view>

namespace strata::gpu {

/** Conservative source classification: frame inputs can change a retained draw without a UI commit. */
[[nodiscard]] inline bool uses_frame_inputs(const std::string_view source) noexcept {
    constexpr std::array<std::string_view, 6U> names{
        "materialTime", "materialPointer", "effectTime", "effectPointer", "frameSeconds", "framePointer"};
    for (const std::string_view name : names) {
        if (source.find(name) != std::string_view::npos)
            return true;
    }
    return false;
}

} // namespace strata::gpu
