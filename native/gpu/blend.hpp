#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace strata::gpu {

/** The blend modes a packet draw batch may name. Backends resolve the name once per batch. */
enum class BlendMode : std::uint8_t {
    opaque,
    straight_alpha,
    premultiplied_alpha,
    additive,
    multiply,
};

[[nodiscard]] constexpr std::optional<BlendMode> blend_mode(const std::string_view name) noexcept {
    if (name == "opaque")
        return BlendMode::opaque;
    if (name == "straight_alpha")
        return BlendMode::straight_alpha;
    if (name == "premultiplied_alpha")
        return BlendMode::premultiplied_alpha;
    if (name == "additive")
        return BlendMode::additive;
    if (name == "multiply")
        return BlendMode::multiply;
    return std::nullopt;
}

} // namespace strata::gpu
