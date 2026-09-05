#pragma once
#include <cstdint>
#include <span>
#include <vector>
namespace strata::gpu {
std::vector<std::uint8_t> decode_png(std::span<const std::uint8_t> bytes, std::uint32_t width,
                                     std::uint32_t height);
}
