#pragma once
#include "resources.hpp"
#include <atomic>
namespace strata::vulkan::detail {
/** Standalone device and readback target for headless tests. Embedders use their own Device. */
class Offscreen final {
  public:
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    Device device;
    std::unique_ptr<Image> image;
    std::unique_ptr<Buffer> readback;
    std::atomic_uint validation_errors{0};
    std::string device_name;
    explicit Offscreen(bool validation = false);
    ~Offscreen();
    Offscreen(const Offscreen&) = delete;
    Offscreen& operator=(const Offscreen&) = delete;
    void resize(std::uint32_t width, std::uint32_t height);
    RenderTarget target(double width, double height) const;
    std::vector<std::uint8_t> pixels();

  private:
    void destroy() noexcept;
};
} // namespace strata::vulkan::detail
