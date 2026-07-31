#include "runtime/startup.hpp"

#include <set>
#include <stdexcept>
#include <utility>

#include "core/utf8.hpp"

namespace strata::runtime {

ApplicationStartup::ApplicationStartup(DiagnosticReporter reporter)
    : reporter_(std::move(reporter)) {}

ApplicationStartupStatus ApplicationStartup::start(
    const ApplicationStartupDefinition& definition
) const {
    if (definition.id.empty() || definition.module.empty() ||
        !core::valid_utf8(definition.id) || !core::valid_utf8(definition.module)) {
        throw std::invalid_argument("application id and module must be non-empty valid UTF-8");
    }
    std::set<std::string, std::less<>> unique_layers;
    for (const std::string& layer : definition.layer_ids) {
        if (!unique_layers.insert(layer).second) {
            if (reporter_) {
                reporter_(RuntimeDiagnostic{
                    "STRATA.APPLICATION.STARTUP_FAILED",
                    "Application '" + definition.id +
                        "' could not start: application layers must use unique layer ids",
                    {},
                    "valid application module and host registrations",
                    DiagnosticSeverity::error,
                    std::nullopt,
                });
            }
            return ApplicationStartupStatus::failed;
        }
    }
    return ApplicationStartupStatus::running;
}

} // namespace strata::runtime
