#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

#include "compiler_check.hpp"

int main(const int argument_count, const char* const* const arguments) {
    try {
        if ((argument_count == 4 || argument_count == 5) &&
            std::string_view(arguments[1]) == "--check-module") {
            return strata::tools::check_module(
                std::filesystem::path(arguments[2]),
                std::filesystem::path(arguments[3]),
                argument_count == 5 ? std::filesystem::path(arguments[4])
                                    : std::filesystem::path{}
            );
        }
        if (argument_count == 7 &&
            (std::string_view(arguments[1]) == "--emit-artifact" ||
             std::string_view(arguments[1]) == "--check-artifact")) {
            return strata::tools::write_module_artifact(
                std::filesystem::path(arguments[2]),
                std::filesystem::path(arguments[3]),
                std::filesystem::path(arguments[4]),
                std::filesystem::path(arguments[5]),
                std::filesystem::path(arguments[6]),
                std::string_view(arguments[1]) == "--check-artifact"
            );
        }
    } catch (const std::exception& exception) {
        std::cerr << "strata_compile: " << exception.what() << '\n';
        return 4;
    }
    std::cerr << "usage:\n"
                 "  strata_compile --check-module <entry.strata> "
                 "<registry.json> [application-schemas.json]\n"
                 "  strata_compile --emit-artifact|--check-artifact <entry.strata> "
                 "<registry.json> <application-schemas.json> <resource-root> <artifact.bin>\n";
    return 1;
}
