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
        if (argument_count == 3 &&
            std::string_view(arguments[1]) == "--write-registry") {
            strata::tools::write_builtin_registry(std::filesystem::path(arguments[2]));
            return 0;
        }
        if (argument_count == 5 &&
            std::string_view(arguments[1]) == "--write-cpp-contract") {
            strata::tools::write_cpp_contract(
                std::filesystem::path(arguments[2]),
                arguments[3],
                std::filesystem::path(arguments[4])
            );
            return 0;
        }
    } catch (const std::exception& exception) {
        std::cerr << "strata_authoring: " << exception.what() << '\n';
        return 4;
    }
    std::cerr
        << "usage: strata_authoring <--check|--write> <project-root>\n"
           "       strata_authoring --write-registry <output.json>\n"
           "       strata_authoring --write-cpp-contract <application.schemas.json> "
           "<cpp-namespace> <output.hpp>\n";
    return 1;
}
