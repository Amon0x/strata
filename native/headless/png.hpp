#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

namespace strata::headless {

/** Writes deterministic RGBA8 PNG data using filter-none, stored DEFLATE blocks. */
void write_png(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
               std::span<const std::uint8_t> rgba);

} // namespace strata::headless
