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
        std::filesystem::path resource_root;
        while (command_index + 1 < argument_count) {
            const std::string_view option(arguments[command_index]);
            if (option == "--extension-path")
                extension_paths.emplace_back(arguments[command_index + 1]);
            else if (option == "--resource-root")
                resource_root = arguments[command_index + 1];
            else
                break;
            command_index += 2;
        }
        const std::string_view command =
            command_index < argument_count ? arguments[command_index] : std::string_view{};
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
                      extension_paths,
                      resource_root
                  )
                : strata::tools::check_module(
                      std::filesystem::path(arguments[command_index + 1]),
                      schemas,
                      extension_paths,
                      resource_root
                  );
        }
        // Artifacts name their resource root positionally, so the option is for checks only.
        if (remaining == 5 && resource_root.empty() &&
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
                 "  strata_compile [--extension-path <directory>]... [--resource-root <directory>] "
                 "--check-module|--check-module-json <entry.strata> "
                 "[application-schemas.json]\n"
                 "  strata_compile [--extension-path <directory>]... "
                 "--emit-artifact|--check-artifact <entry.strata> "
                 "<application-schemas.json> <resource-root> <artifact.bin>\n"
                 "Imports resolve within the resource root; a check without --resource-root keeps "
                 "them inside the entry module's folder.\n";
    return 1;
}
