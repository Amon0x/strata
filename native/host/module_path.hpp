#pragma once

#include <string>
#include <string_view>

namespace strata::host {

/** Resolves one logical module import without permitting absolute or root-escaping identities. */
[[nodiscard]] std::string resolve_module_id(std::string_view importer,
                                            std::string_view import_path);

} // namespace strata::host
