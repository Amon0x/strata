#pragma once

#include <strata/strata.h>

#include <string>
#include <vector>

namespace strata::host {

struct SelectedExtensions final {
    std::vector<strata_widget_extension> widgets;
    std::vector<strata_behavior_extension> behaviors;
    strata_surface_extension_bundle bundle{};

    [[nodiscard]] const strata_surface_extension_bundle* pointer() noexcept;
};

[[nodiscard]] SelectedExtensions select_extensions(const std::vector<std::string>& package_ids);

/** Compiler/runtime declaration documents projected from the same package definitions. */
[[nodiscard]] std::vector<std::string> package_schemas(const std::vector<std::string>& package_ids);

} // namespace strata::host
