#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "data/json.hpp"

namespace strata::tools {

/** Generates one self-contained C++23 host contract from a validated application schema. */
[[nodiscard]] std::string render_cpp_contract(
    const data::JsonValue& application_schema,
    std::string_view cpp_namespace,
    std::string_view source_name
);

void write_cpp_contract(
    const std::filesystem::path& application_schema_path,
    std::string_view cpp_namespace,
    const std::filesystem::path& output_path
);

} // namespace strata::tools
