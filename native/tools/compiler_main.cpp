#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "compiler_check.hpp"

int main(const int argument_count, const char* const* const arguments) {
    try {
        int command_index = 1;
        std::vector<std::filesystem::path> extension_paths;
        while (command_index < argument_count &&
               std::string_view(arguments[command_index]) == "--extension-path") {
            if (command_index + 1 >= argument_count) break;
            extension_paths.emplace_back(arguments[command_index + 1]);
            command_index += 2;
        }
        if (command_index >= argument_count) throw std::invalid_argument("missing compiler command");
        const std::string_view command(arguments[command_index]);
        const int remaining = argument_count - command_index;
        if ((remaining == 2 || remaining == 3) &&
            (command == "--check-module" || command == "--check-module-json")) {
            const std::filesystem::path schemas = remaining == 3
                ? std::filesystem::path(arguments[command_index + 2])
                : std::filesystem::path{};
            return command == "--check-module-json"
                ? strata::tools::check_module_json(
                      std::filesystem::path(arguments[command_index + 1]),
                      schemas,
                      extension_paths
                  )
                : strata::tools::check_module(
                      std::filesystem::path(arguments[command_index + 1]),
                      schemas,
                      extension_paths
                  );
        }
        if (remaining == 5 &&
            (command == "--emit-artifact" || command == "--check-artifact")) {
            return strata::tools::write_module_artifact(
                std::filesystem::path(arguments[command_index + 1]),
                std::filesystem::path(arguments[command_index + 2]),
                std::filesystem::path(arguments[command_index + 3]),
                std::filesystem::path(arguments[command_index + 4]),
                command == "--check-artifact",
                extension_paths
            );
        }
    } catch (const std::exception& exception) {
        std::cerr << "strata_compile: " << exception.what() << '\n';
        return 4;
    }
    std::cerr << "usage:\n"
                 "  strata_compile [--extension-path <directory>]... "
                 "--check-module|--check-module-json <entry.strata> "
                 "[application-schemas.json]\n"
                 "  strata_compile [--extension-path <directory>]... "
                 "--emit-artifact|--check-artifact <entry.strata> "
                 "<application-schemas.json> <resource-root> <artifact.bin>\n";
    return 1;
}
