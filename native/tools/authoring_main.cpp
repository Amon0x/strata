#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

#include "authoring.hpp"
#include "authoring_contract.hpp"

int main(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count == 3 && std::string_view(arguments[1]) == "--check") {
            return strata::tools::check_authoring(std::filesystem::path(arguments[2]));
        }
        if (argument_count == 3 && std::string_view(arguments[1]) == "--write") {
            return strata::tools::write_authoring(std::filesystem::path(arguments[2]));
        }
        if (argument_count == 6 &&
            std::string_view(arguments[1]) == "--write-cpp-contract") {
            strata::tools::write_cpp_contract(
                std::filesystem::path(arguments[2]),
                std::filesystem::path(arguments[3]),
                arguments[4],
                std::filesystem::path(arguments[5])
            );
            return 0;
        }
    } catch (const std::exception& exception) {
        std::cerr << "strata_authoring: " << exception.what() << '\n';
        return 4;
    }
    std::cerr
        << "usage: strata_authoring <--check|--write> <project-root>\n"
           "       strata_authoring --write-cpp-contract <registry.json> <application.schemas.json> "
           "<cpp-namespace> <output.hpp>\n";
    return 1;
}
