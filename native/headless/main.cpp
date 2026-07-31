#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "interactive.hpp"
#include "runner.hpp"
#include "scenario.hpp"

namespace {

void usage() {
    std::cerr << "usage: strata_headless --resources <resource-root> --scenario <scenario.json> "
                 "--output <directory> [--interactive]\n";
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    try {
        std::filesystem::path resources;
        std::filesystem::path scenario_path;
        std::filesystem::path output;
        bool interactive = false;
        for (int index = 1; index < argument_count; ++index) {
            const std::string_view argument(arguments[index]);
            if (argument == "--help" || argument == "-h") {
                usage();
                return 0;
            }
            if (argument == "--interactive") {
                interactive = true;
                continue;
            }
            if (index + 1 >= argument_count) {
                throw std::invalid_argument("missing value after " + std::string(argument));
            }
            const std::filesystem::path value(arguments[++index]);
            if (argument == "--resources")
                resources = value;
            else if (argument == "--scenario")
                scenario_path = value;
            else if (argument == "--output")
                output = value;
            else
                throw std::invalid_argument("unknown headless option " + std::string(argument));
        }
        if (resources.empty() || scenario_path.empty() || output.empty()) {
            usage();
            return 2;
        }
        const strata::headless::Scenario scenario = strata::headless::load_scenario(scenario_path);
        if (interactive) {
            strata::headless::run_interactive(scenario, resources, output, std::cin, std::cout);
        } else {
            strata::headless::run_scenario(scenario, resources, output);
            std::cout << (output / "result.json").string() << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_headless: " << error.what() << '\n';
        return 1;
    }
}
