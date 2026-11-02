#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <strata/presentation.hpp>
#include <vulkan/vulkan.h>

namespace strata::vulkan {

/**
 * Platform surface creation for an owned Vulkan presentation driver. The callback transfers its
 * VkSurfaceKHR to the driver. It runs during GPU-owner initialization; hosts whose window API is
 * main-thread-only marshal that one operation to their window owner and keep pumping until it
 * completes. The window must outlive Presentation::stopped(). No runtime is involved.
 */
struct WindowSurface final {
    std::vector<std::string> instance_extensions;
    std::function<VkSurfaceKHR(VkInstance)> create;
};

struct WindowOptions final {
    bool vsync = true;
    bool validation = false;
    std::array<std::uint8_t, 4U> clear_color{9U, 11U, 15U, 255U};
};

/**
 * Creates an owned instance, surface, graphics/present device, swap chain and packet renderer.
 * Invoke in a Presentation factory so creation, consumption and destruction share its GPU owner.
 * Requires one graphics queue family that can present to the platform surface. Ordinary frames
 * never read back pixels; only an explicit capture does. A zero-sized viewport suspends drawing.
 */
[[nodiscard]] std::unique_ptr<host::PresentationDriver>
create_window_driver(WindowSurface surface, WindowOptions options = {});

} // namespace strata::vulkan
