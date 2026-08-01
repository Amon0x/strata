#include "authoring.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "authoring_contract.hpp"
#include "authoring_grammar.hpp"
#include "authoring_registry.hpp"
#include "core/utf8.hpp"
#include "data/json.hpp"
#include "diagnostic_catalog.hpp"
#include "resource/resource.hpp"

namespace strata::tools {
namespace {

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("authoring artifact does not exist");
    std::ostringstream output;
    output << input.rdbuf();
    std::string text = output.str();
    if (!core::valid_utf8(text)) throw std::runtime_error("authoring artifact is not valid UTF-8");
    return text;
}

[[nodiscard]] data::JsonValue load_json(const std::filesystem::path& path) {
    return data::parse_json(resource::load_utf8_resource(
        path.parent_path(),
        resource::ResourceId::parse(path.filename().generic_string())
    ));
}

[[nodiscard]] std::map<std::filesystem::path, std::string> generated(
    const std::filesystem::path& project_root
) {
    const data::JsonValue registry = load_json(
        project_root / "src/main/resources/strata/registry-v1.json"
    );
    const data::JsonValue lexical = load_json(
        project_root / "src/main/resources/strata/lexical-v1.json"
    );
    std::map<std::filesystem::path, std::string> result{
        {"docs/generated/diagnostics.md", render_diagnostic_catalog(project_root)},
        {"docs/generated/strata-reference.md", render_reference(registry)},
        {"editor/strata-completions.json", render_completions(registry)},
        {"editor/vscode/syntaxes/strata.tmLanguage.json", render_grammar(lexical)},
    };
    constexpr std::pair<std::string_view, std::string_view> contracts[]{
        {"debug_overlay", "debug_overlay"},
        {"demo_surface", "demo_surface"},
        {"performance_hud", "performance_hud"},
        {"settings_app", "settings_app"},
        {"strata_hub", "strata_hub"},
    };
    for (const auto& [file_stem, namespace_name] : contracts) {
        const std::filesystem::path schema_path =
            project_root / "src/main/resources/assets/strata/ui" /
            (std::string(file_stem) + ".schemas.json");
        result.emplace(
            std::filesystem::path("native/generated/strata/contracts") /
                (std::string(file_stem) + ".hpp"),
            render_cpp_contract(
                registry,
                load_json(schema_path),
                "strata::contracts::" + std::string(namespace_name),
                schema_path.lexically_relative(project_root).generic_string()
            )
        );
    }
    return result;
}

void write_text(const std::filesystem::path& destination, const std::string_view text) {
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) throw std::runtime_error("could not create authoring artifact directory");
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("could not create authoring artifact");
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) throw std::runtime_error("could not write authoring artifact");
}

} // namespace

int check_authoring(const std::filesystem::path& project_root) {
    bool current = true;
    for (const auto& [relative, expected] : generated(project_root)) {
        try {
            if (read_text(project_root / relative) == expected) continue;
        } catch (const std::exception&) {
            // Report the artifact path uniformly below.
        }
        std::cerr << relative.generic_string() << " is missing or stale\n";
        current = false;
    }
    if (!current) {
        std::cerr << "Run the strata_generate_authoring CMake target.\n";
        return 10;
    }
    std::cout << "Strata native authoring artifacts are current.\n";
    return 0;
}

int write_authoring(const std::filesystem::path& project_root) {
    for (const auto& [relative, content] : generated(project_root)) {
        write_text(project_root / relative, content);
        std::cout << "Wrote " << relative.generic_string() << '\n';
    }
    return 0;
}

} // namespace strata::tools
