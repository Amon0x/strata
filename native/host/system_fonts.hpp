#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace strata::host {

/**
 * Hosts serve fonts installed on the machine under resource ids with this prefix, followed by the
 * font's file name. A Surface font may therefore name the platform's own interface face and fall
 * back to a bundled resource where it is absent, without the application shipping that face.
 */
inline constexpr std::string_view system_font_scheme = "system-font:";

/**
 * The installed font file with this name, looked up (case-insensitively) in the platform's font
 * directories, or an empty path when none has it. Only bare file names resolve: a name with a
 * directory separator is never looked up, so a font id cannot read outside the font directories.
 */
[[nodiscard]] std::filesystem::path find_system_font(std::string_view file_name);

/**
 * The first candidate that is installed, as a `system-font:` resource id, or nullopt when none is.
 * Candidates are file names in preference order.
 */
[[nodiscard]] std::optional<std::string> select_system_font(
    std::span<const std::string> candidates);

/** The bytes behind a `system-font:` resource id; nullopt when `resource_id` is not one. */
[[nodiscard]] std::optional<std::vector<std::uint8_t>> read_system_font(
    std::string_view resource_id);

} // namespace strata::host
