#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
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

struct SystemFontCatalog;

/** Pins one internally synchronized directory snapshot, including work that outlives its client. */
[[nodiscard]] std::shared_ptr<SystemFontCatalog> system_font_catalog();

/**
 * Selects the directories for subsequent clients; existing catalog handles retain their snapshot.
 * Empty selects the platform defaults. Replacing a catalog never joins its preparation workers.
 */
void set_system_font_directories(std::vector<std::filesystem::path> directories);

/**
 * Resolves in stable script-preference, general-font, then sorted-file order. May perform I/O and
 * parsing: use on a worker or during deterministic preparation, never as an asynchronous UI miss.
 */
[[nodiscard]] std::optional<SystemFont>
system_font_for(const std::shared_ptr<SystemFontCatalog>& catalog, std::uint32_t code_point);

/** Prefetches the immutable file index on the shared pool, without retaining a runtime. */
void prepare_system_fonts(const std::shared_ptr<SystemFontCatalog>& catalog);

/** A case-insensitive file-name lookup; may perform I/O and parsing on a cache miss. */
[[nodiscard]] std::optional<SystemFont>
system_font_named(const std::shared_ptr<SystemFontCatalog>& catalog, std::string_view file);

/**
 * Resolves a PostScript/full name, else the nearest family weight/slant. Missing indexes are
 * prepared by this caller, never by waiting for a child job. Call only on a preparation path.
 */
[[nodiscard]] std::optional<SystemFont>
system_font_family(const std::shared_ptr<SystemFontCatalog>& catalog, std::string_view name,
                   std::string_view family, int weight, bool italic);

} // namespace strata::font
