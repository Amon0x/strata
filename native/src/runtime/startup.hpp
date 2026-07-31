#pragma once

#include <string>
#include <vector>

#include "runtime/diagnostic.hpp"

namespace strata::runtime {

struct ApplicationStartupDefinition final {
    std::string id;
    std::string module;
    std::vector<std::string> layer_ids;
};

enum class ApplicationStartupStatus { running, failed };

/** Validates the application composition boundary before any host layer is published. */
class ApplicationStartup final {
public:
    explicit ApplicationStartup(DiagnosticReporter reporter = {});
    [[nodiscard]] ApplicationStartupStatus start(const ApplicationStartupDefinition& definition) const;

private:
    DiagnosticReporter reporter_;
};

} // namespace strata::runtime
