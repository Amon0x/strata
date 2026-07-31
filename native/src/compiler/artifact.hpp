#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "compiler/source_map.hpp"
#include "data/json.hpp"
#include "data/json_view.hpp"

namespace strata::compiler {

inline constexpr std::uint32_t compiled_module_artifact_version = 1U;

struct CompiledModuleArtifact final {
    data::FrozenJsonDocument unit;
    CompiledSourceMap source_map;
};

[[nodiscard]] data::JsonValue source_map_entry_json(const CompiledSourceMapEntry& entry);

[[nodiscard]] std::vector<std::uint8_t> encode_compiled_module_artifact(
    const data::JsonValue& unit,
    const CompiledSourceMap& source_map
);

[[nodiscard]] CompiledModuleArtifact decode_compiled_module_artifact(
    std::span<const std::uint8_t> artifact
);

} // namespace strata::compiler
