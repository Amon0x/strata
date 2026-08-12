#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "headless/scenario.hpp"

namespace strata::desktop {

enum class LaunchKind { generic, custom };

struct LaunchManifest final {
    LaunchKind kind = LaunchKind::generic;
    std::filesystem::path path;
    std::filesystem::path resource_root;
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::string title;
    headless::Scenario application;
    bool watch = false;
};

[[nodiscard]] LaunchManifest load_launch_manifest(const std::filesystem::path& path);
[[nodiscard]] int run_custom_host(const LaunchManifest& manifest);

} // namespace strata::desktop
