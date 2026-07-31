#pragma once

#include <optional>
#include <string>

#include "compiler/ast.hpp"
#include "compiler/diagnostic.hpp"
#include "compiler/schema.hpp"
#include "compiler/semantic.hpp"
#include "data/json.hpp"
#include "data/json_view.hpp"

namespace strata::compiler {

inline constexpr std::int64_t portable_ir_version = 1;

struct PortableIrResult final {
    std::optional<data::JsonValue> unit;
    std::vector<Diagnostic> diagnostics;
};

/** Lowers parser-independent immutable JSON IR and validates its required structural invariants. */
[[nodiscard]] PortableIrResult lower_portable_ir(
    const File& file,
    const SchemaRegistry& registry,
    const std::map<std::string, ValidatedAnimation, std::less<>>& animations
);
void validate_portable_ir(const data::JsonValue& unit);
void validate_portable_ir(data::JsonView unit);
void validate_portable_ir(const data::FrozenJsonDocument& unit);

} // namespace strata::compiler
