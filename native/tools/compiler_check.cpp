#include "compiler_check.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "compiler/compile.hpp"
#include "compiler/artifact.hpp"
#include "compiler/diagnostic.hpp"
#include "compiler/module.hpp"
#include "compiler/schema.hpp"
#include "data/json.hpp"
#include "host/extensions.hpp"
#include "resource/resource.hpp"

namespace strata::tools {
namespace {

[[nodiscard]] bool is_within(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate
) {
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    for (; root_part != root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end() || *candidate_part != *root_part) return false;
    }
    return true;
}

[[nodiscard]] std::filesystem::path canonical_file(
    const std::filesystem::path& path,
    const std::string_view label
) {
    std::error_code error;
    const std::filesystem::path result = std::filesystem::weakly_canonical(path, error);
    if (error || !std::filesystem::is_regular_file(result, error) || error) {
        throw std::runtime_error(std::string(label) + " does not resolve to a regular file");
    }
    return result;
}

[[nodiscard]] std::string load_file(const std::filesystem::path& path) {
    return resource::load_utf8_resource(
        path.parent_path(),
        resource::ResourceId::parse(path.filename().generic_string())
    );
}

[[nodiscard]] std::vector<std::uint8_t> load_bytes(
    const std::filesystem::path& path
) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "could not read compiled module artifact: " + path.generic_string()
        );
    }
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    );
}

[[nodiscard]] std::filesystem::path canonical_directory(
    const std::filesystem::path& path,
    const std::string_view label
) {
    std::error_code error;
    const std::filesystem::path result = std::filesystem::weakly_canonical(path, error);
    if (error || !std::filesystem::is_directory(result, error) || error) {
        throw std::runtime_error(std::string(label) + " does not resolve to a directory");
    }
    return result;
}

[[nodiscard]] std::string resource_id(
    const std::filesystem::path& root,
    const std::filesystem::path& path
) {
    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(path, root, error);
    if (error || relative.empty() || relative.is_absolute() ||
        relative.generic_string().starts_with("../")) {
        throw std::runtime_error("module is outside the declared resource root");
    }
    return relative.generic_string();
}

[[nodiscard]] compiler::ModuleSource load_source(
    const std::filesystem::path& path,
    const std::optional<std::filesystem::path>& source_root
) {
    return compiler::ModuleSource{
        source_root.has_value() ? resource_id(*source_root, path) : path.generic_string(),
        load_file(path),
    };
}

struct ModuleCompilation final {
    std::filesystem::path entry;
    compiler::CompileResult result;
    std::vector<std::string> extension_schemas;
};

[[nodiscard]] ModuleCompilation compile_module(
    const std::filesystem::path& entry_path,
    const std::filesystem::path& schemas_path,
    const std::optional<std::filesystem::path>& source_root,
    const std::vector<std::filesystem::path>& extension_paths
) {
    const std::filesystem::path entry = canonical_file(entry_path, "entry module");
    const std::filesystem::path module_root = entry.parent_path();

    compiler::SchemaRegistry registry = compiler::SchemaRegistry::builtins();
    std::vector<std::string> extension_schemas;
    if (!schemas_path.empty()) {
        const std::filesystem::path schemas_file = canonical_file(
            schemas_path,
            "application schema declarations"
        );
        const data::JsonValue schemas = data::parse_json(load_file(schemas_file));
        /*
         * Native extension packages declare their widgets, behaviors, and action contracts once in
         * C++; the application only names the packages it activates, so schema and runtime
         * registration cannot drift.
         */
        if (const data::JsonValue* const packages = schemas.find("extensionPackages");
            packages != nullptr) {
            if (packages->array() == nullptr) {
                throw std::runtime_error("extensionPackages must be an array of package ids");
            }
            std::vector<std::string> package_ids;
            package_ids.reserve(packages->array()->size());
            for (const data::JsonValue& package : *packages->array()) {
                if (package.string() == nullptr) {
                    throw std::runtime_error("extensionPackages entries must be package id strings");
                }
                package_ids.push_back(*package.string());
            }
            const host::SelectedExtensions extensions = host::select_extensions(
                package_ids,
                extension_paths
            );
            extension_schemas = extensions.schemas();
            for (const std::string& schema : extension_schemas) {
                registry.apply_scenario_declarations(data::parse_json(schema));
            }
        }
        registry.apply_scenario_declarations(schemas);
    }

    compiler::CompileResult result = compiler::compile_program(
        load_source(entry, source_root),
        [&module_root, &source_root](
            const std::string_view importer,
            const std::string_view import_path
        ) {
            if (import_path.empty() || import_path.contains('\\') || import_path.contains('\0')) {
                throw compiler::ModuleLoadError("Import path is not a portable relative path.");
            }
            const std::filesystem::path importer_path = source_root.has_value()
                ? *source_root / std::filesystem::path(importer)
                : std::filesystem::path(importer);
            std::error_code error;
            const std::filesystem::path imported = std::filesystem::weakly_canonical(
                importer_path.parent_path() / std::filesystem::path(import_path),
                error
            );
            if (error || !is_within(module_root, imported) ||
                !std::filesystem::is_regular_file(imported, error) || error) {
                throw compiler::ModuleLoadError(
                    "Import '" + std::string(import_path) +
                        "' does not resolve to a readable module inside the application root.",
                    imported.generic_string()
                );
            }
            return load_source(imported, source_root);
        },
        registry
    );
    return ModuleCompilation{entry, std::move(result), std::move(extension_schemas)};
}

void print_diagnostic(const compiler::Diagnostic& diagnostic) {
    std::cerr << diagnostic.code;
    if (diagnostic.range.has_value()) {
        std::cerr << ' ' << diagnostic.range->source_id << ':'
                  << diagnostic.range->start.line << ':'
                  << diagnostic.range->start.column;
    }
    std::cerr << ": " << diagnostic.message << '\n';
}

[[nodiscard]] data::JsonValue json_object(
    std::initializer_list<data::JsonValue::ObjectEntry> fields
) {
    return data::JsonValue(data::JsonValue::Object(fields));
}

[[nodiscard]] const char* severity_name(
    const compiler::DiagnosticSeverity severity
) noexcept {
    switch (severity) {
    case compiler::DiagnosticSeverity::info: return "info";
    case compiler::DiagnosticSeverity::warning: return "warning";
    case compiler::DiagnosticSeverity::error: return "error";
    }
    return "error";
}

[[nodiscard]] data::JsonValue diagnostic_json(const compiler::Diagnostic& diagnostic) {
    data::JsonValue range;
    if (diagnostic.range.has_value()) {
        const compiler::SourceRange& source = *diagnostic.range;
        range = json_object({
            {"sourceId", data::JsonValue(source.source_id)},
            {"start", json_object({
                {"line", data::JsonValue(static_cast<std::int64_t>(source.start.line))},
                {"column", data::JsonValue(static_cast<std::int64_t>(source.start.column))},
                {"offset", data::JsonValue(static_cast<std::int64_t>(source.start.offset))},
            })},
            {"end", json_object({
                {"line", data::JsonValue(static_cast<std::int64_t>(source.end.line))},
                {"column", data::JsonValue(static_cast<std::int64_t>(source.end.column))},
                {"offset", data::JsonValue(static_cast<std::int64_t>(source.end.offset))},
            })},
        });
    }
    return json_object({
        {"code", data::JsonValue(diagnostic.code)},
        {"severity", data::JsonValue(severity_name(diagnostic.severity))},
        {"message", data::JsonValue(diagnostic.message)},
        {"range", std::move(range)},
        {
            "componentPath",
            diagnostic.component_path.has_value()
                ? data::JsonValue(*diagnostic.component_path)
                : data::JsonValue(),
        },
        {
            "expected",
            diagnostic.expected.has_value()
                ? data::JsonValue(*diagnostic.expected)
                : data::JsonValue(),
        },
    });
}

} // namespace

int check_module(
    const std::filesystem::path& entry_path,
    const std::filesystem::path& schemas_path,
    const std::vector<std::filesystem::path>& extension_paths
) {
    ModuleCompilation compilation = compile_module(
        entry_path, schemas_path, std::nullopt, extension_paths
    );

    for (const compiler::Diagnostic& diagnostic : compilation.result.diagnostics) {
        print_diagnostic(diagnostic);
    }
    if (!compilation.result.succeeded()) {
        std::cout << "STRATA VALIDATE: FAILED\n";
        return 2;
    }
    std::cout << "STRATA VALIDATE: OK - "
              << compilation.entry.filename().generic_string() << " (native)\n";
    return 0;
}

int check_module_json(
    const std::filesystem::path& entry_path,
    const std::filesystem::path& schemas_path,
    const std::vector<std::filesystem::path>& extension_paths
) {
    try {
        ModuleCompilation compilation = compile_module(
            entry_path, schemas_path, std::nullopt, extension_paths
        );
        std::vector<data::JsonValue> diagnostics;
        diagnostics.reserve(compilation.result.diagnostics.size());
        for (const compiler::Diagnostic& diagnostic : compilation.result.diagnostics) {
            diagnostics.push_back(diagnostic_json(diagnostic));
        }
        std::vector<data::JsonValue> extension_schemas;
        extension_schemas.reserve(compilation.extension_schemas.size());
        for (const std::string& schema : compilation.extension_schemas) {
            extension_schemas.push_back(data::parse_json(schema));
        }
        std::cout << data::encode_json_line(json_object({
            {"format", data::JsonValue("strata.diagnostics")},
            {"version", data::JsonValue(std::int64_t{1})},
            {"succeeded", data::JsonValue(compilation.result.succeeded())},
            {"entry", data::JsonValue(compilation.entry.generic_string())},
            {"diagnostics", data::JsonValue(std::move(diagnostics))},
            {"extensionSchemas", data::JsonValue(std::move(extension_schemas))},
        }));
        return compilation.result.succeeded() ? 0 : 2;
    } catch (const std::exception& error) {
        std::vector<data::JsonValue> diagnostics;
        diagnostics.push_back(diagnostic_json(compiler::Diagnostic{
            "STRATA.TOOL.VALIDATION_FAILED",
            compiler::DiagnosticSeverity::error,
            error.what(),
            std::nullopt,
            std::nullopt,
            std::nullopt,
        }));
        std::cout << data::encode_json_line(json_object({
            {"format", data::JsonValue("strata.diagnostics")},
            {"version", data::JsonValue(std::int64_t{1})},
            {"succeeded", data::JsonValue(false)},
            {"entry", data::JsonValue(entry_path.generic_string())},
            {"diagnostics", data::JsonValue(std::move(diagnostics))},
            {"extensionSchemas", data::JsonValue(std::vector<data::JsonValue>{})},
        }));
        return 2;
    }
}

int write_module_artifact(
    const std::filesystem::path& entry_path,
    const std::filesystem::path& schemas_path,
    const std::filesystem::path& resource_root,
    const std::filesystem::path& artifact_path,
    const bool check_only,
    const std::vector<std::filesystem::path>& extension_paths
) {
    const std::filesystem::path root = canonical_directory(resource_root, "resource root");
    ModuleCompilation compilation = compile_module(
        entry_path, schemas_path, root, extension_paths
    );
    for (const compiler::Diagnostic& diagnostic : compilation.result.diagnostics) {
        print_diagnostic(diagnostic);
    }
    if (!compilation.result.succeeded()) {
        std::cout << "STRATA ARTIFACT: FAILED\n";
        return 2;
    }
    const std::vector<std::uint8_t> artifact = compiler::encode_compiled_module_artifact(
        *compilation.result.unit,
        compilation.result.source_map
    );
    if (check_only) {
        if (load_bytes(canonical_file(artifact_path, "compiled module artifact")) != artifact) {
            std::cerr << "compiled module artifact is stale: "
                      << artifact_path.generic_string() << '\n';
            return 3;
        }
        std::cout << "STRATA ARTIFACT: OK - "
                  << compilation.entry.filename().generic_string() << '\n';
        return 0;
    }
    std::ofstream output(artifact_path, std::ios::binary | std::ios::trunc);
    if (!output || !output.write(
            reinterpret_cast<const char*>(artifact.data()),
            static_cast<std::streamsize>(artifact.size())
        )) {
        throw std::runtime_error(
            "could not write compiled module artifact: " + artifact_path.generic_string()
        );
    }
    std::cout << "STRATA ARTIFACT: WROTE - " << artifact_path.generic_string() << '\n';
    return 0;
}

} // namespace strata::tools
