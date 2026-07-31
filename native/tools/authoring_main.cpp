#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

#include "authoring.hpp"

int main(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count == 3 && std::string_view(arguments[1]) == "--check") {
            return strata::tools::check_authoring(std::filesystem::path(arguments[2]));
        }
        if (argument_count == 3 && std::string_view(arguments[1]) == "--write") {
            return strata::tools::write_authoring(std::filesystem::path(arguments[2]));
        }
    } catch (const std::exception& exception) {
        std::cerr << "strata_authoring: " << exception.what() << '\n';
        return 4;
    }
    std::cerr << "usage: strata_authoring <--check|--write> <project-root>\n";
    return 1;
}
