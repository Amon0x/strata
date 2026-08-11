#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/diagnostic.hpp"
#include "ui/render/material_registry.hpp"

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void material_warning_suppression_rearms_after_clear() {
    std::vector<strata::runtime::RuntimeDiagnostic> published;
    strata::ui::MaterialRegistry materials(
        std::vector<std::string>{"known"},
        [&](strata::runtime::RuntimeDiagnostic diagnostic) {
            published.push_back(std::move(diagnostic));
        }
    );
    const strata::ui::MaterialState unknown{"missing"};

    static_cast<void>(materials.validate_state(unknown));
    static_cast<void>(materials.validate_state(unknown));
    require(published.size() == 1U, "material warning was not once-suppressed");
    materials.clear_diagnostics();
    static_cast<void>(materials.validate_state(unknown));
    require(published.size() == 2U, "material warning suppression was not rearmed by clear");
    require(
        published.back().code == "STRATA.RENDER2D.MATERIAL_UNKNOWN",
        "rearmed material warning changed diagnostic identity"
    );
}

} // namespace

int strata_test_resource_host_residual() {
    try {
        material_warning_suppression_rearms_after_clear();
        std::cout << "resource/host residual tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "resource/host residual tests failed: " << error.what() << '\n';
        return 1;
    }
}
