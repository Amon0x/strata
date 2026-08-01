#pragma once

#include "compiler/builtin_catalog.hpp"

namespace strata::compiler {

void add_builtin_types(BuiltinCatalog& catalog);
void add_builtin_properties(BuiltinCatalog& catalog);
void add_builtin_actions(BuiltinCatalog& catalog);
void add_builtin_helpers(BuiltinCatalog& catalog);
void add_builtin_primitive_widgets(BuiltinCatalog& catalog);
void add_builtin_control_widgets(BuiltinCatalog& catalog);
void add_builtin_shell_widgets(BuiltinCatalog& catalog);
void add_builtin_collection_widgets(BuiltinCatalog& catalog);

} // namespace strata::compiler
