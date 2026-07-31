#pragma once

#include <filesystem>

#include "scenario.hpp"

namespace strata::headless {

/** Executes one deterministic scenario and writes captures plus result.json into output_root. */
void run_scenario(const Scenario& scenario, const std::filesystem::path& resource_root,
                  const std::filesystem::path& output_root);

} // namespace strata::headless
