#include "runner.hpp"

#include "session.hpp"

namespace strata::headless {

void run_scenario(const Scenario& scenario, const std::filesystem::path& resource_root,
                  const std::filesystem::path& output_root) {
    Session session(scenario, resource_root, output_root);
    for (const ScenarioStep& step : scenario.steps)
        session.execute(step);
    session.write_result();
    session.close();
}

} // namespace strata::headless
