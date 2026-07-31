#pragma once

#include <filesystem>
#include <iosfwd>

#include "scenario.hpp"

namespace strata::headless {

/** Runs the persistent newline-delimited JSON control protocol until close or input EOF. */
void run_interactive(const Scenario& scenario, const std::filesystem::path& resource_root,
                     const std::filesystem::path& output_root, std::istream& input,
                     std::ostream& output);

} // namespace strata::headless
