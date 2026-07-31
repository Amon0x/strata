#include <strata/extension.hpp>

#include "extensions/demo_package.hpp"

namespace strata::extension {

/**
 * Every package shipped in this build. Adding a package means adding its definition and one line
 * here; hosts and the module compiler resolve it by id without further edits.
 */
void register_builtin_packages(Registry& registry) {
    registry.add(demo_package());
}

} // namespace strata::extension
