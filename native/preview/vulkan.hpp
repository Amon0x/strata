#pragma once

#include <filesystem>

namespace strata::desktop {
struct LaunchManifest;
}

namespace strata::preview {
/** Runs the Session owner on SDL's main thread and the owned Vulkan target on its presentation owner. */
int run_vulkan(const desktop::LaunchManifest& manifest, bool smoke, bool uncapped,
               std::filesystem::path screenshot);
}
