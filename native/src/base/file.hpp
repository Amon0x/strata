#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace strata::base {

/**
 * A file's bytes, read whole and unbuffered; nothing when it cannot be opened, sized or read in
 * full. A stream would copy a multi-megabyte artifact, font or image through its own small
 * buffer, several times slower than the read itself.
 */
[[nodiscard]] std::optional<std::vector<std::uint8_t>>
read_file(const std::filesystem::path& path);

} // namespace strata::base
