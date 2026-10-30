#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "font/opentype.hpp"

namespace strata::font {

/** A font of the operating system's, by the id text gives it ("system:" and its file name). */
struct SystemFont final {
    std::string id;
    OpenTypeFont face;
};

/**
 * Where system fonts are found. By default the platform's own: the Windows font folder, or the
 * usual font directories elsewhere. An application's fonts come first; these only cover what they
 * do not (an emoji, Arabic, Han, a symbol).
 */
void set_system_font_directories(std::vector<std::filesystem::path> directories);

/**
 * A system font that covers `code_point`, loaded the first time one is needed. The fonts known to
 * cover a script are tried first (Segoe UI Emoji for emoji, say); the rest are read one at a time
 * only when those fail, keeping just their coverage. Nothing when no font covers it.
 */
[[nodiscard]] std::optional<SystemFont> system_font_for(std::uint32_t code_point);

/** Starts listing the system's fonts in the background; the first fallback then finds them listed.
 */
void prepare_system_fonts();

/** The system font with this file name (`bahnschrift.ttf`, any case); nothing when none is. */
[[nodiscard]] std::optional<SystemFont> system_font_named(std::string_view file);

/**
 * The system font a design names: by its PostScript or full name (`Arial-BoldMT`, `Arial Bold`),
 * else the face of `family` (any case) nearest `weight` (100-900) and slant. Nothing when no
 * installed font has that name or family. Every font is read for its names once, the first time
 * any is asked for.
 */
[[nodiscard]] std::optional<SystemFont>
system_font_family(std::string_view name, std::string_view family, int weight, bool italic);

} // namespace strata::font
