#include "capture_renderer.hpp"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "software_renderer.hpp"
#if defined(STRATA_HAS_VULKAN)
#include <strata/vulkan.hpp>
#include "vulkan_renderer.hpp"
#endif
#if defined(_WIN32)
#include <strata/d3d11.hpp>
#include "d3d11_renderer.hpp"
#endif

namespace strata::headless {

namespace {

/**
 * Draws nothing: the core frames, encodes and decodes its packets exactly as with any backend, so
 * benchmarks and core-only runs measure Strata without a GPU or rasterizer in the loop.
 */
class NullRenderer final : public CaptureRenderer {
  public:
    void resize(const std::uint32_t framebuffer_width, const std::uint32_t framebuffer_height,
                double, double) override {
        width_ = framebuffer_width;
        height_ = framebuffer_height;
    }
    void set_clear_color(std::array<std::uint8_t, 4U>) noexcept override {}
    void declare_material(std::string_view, std::string_view) override {}
    void release_programs(std::string_view) override {}
    void declare_effect_pass(std::string_view, std::uint32_t, std::uint32_t, double,
                             std::uint32_t, std::uint32_t, std::uint32_t,
                             std::string_view) override {}
    void render(const host::RenderPacket&, std::int64_t) override {}
    void consume_resources(const host::RenderPacket&) override {}
    [[nodiscard]] std::string_view backend() const noexcept override { return "none"; }
    [[nodiscard]] std::uint32_t width() const noexcept override { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept override { return height_; }
    [[nodiscard]] std::span<const std::uint8_t> pixels() const noexcept override { return {}; }
    [[nodiscard]] const std::vector<std::string>& material_fallbacks() const noexcept override {
        return fallbacks_;
    }

  private:
    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
    std::vector<std::string> fallbacks_;
};

} // namespace

void use_default_shader_cache() {
#if defined(_WIN32) || defined(STRATA_HAS_VULKAN)
    std::filesystem::path directory;
#if defined(_WIN32)
    wchar_t* value = nullptr;
    std::size_t count = 0U;
    const errno_t status = _wdupenv_s(&value, &count, L"LOCALAPPDATA");
    const std::unique_ptr<wchar_t, decltype(&std::free)> owned(value, &std::free);
    if (status != 0 || value == nullptr || *value == L'\0')
        return;
    directory = std::filesystem::path(value) / "Strata" / "shaders";
#else
    if (const char* cache = std::getenv("XDG_CACHE_HOME"); cache != nullptr && *cache != '\0')
        directory = std::filesystem::path(cache) / "strata" / "shaders";
    else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
        directory = std::filesystem::path(home) / ".cache" / "strata" / "shaders";
    else
        return;
#endif
#if defined(STRATA_HAS_VULKAN)
    vulkan::set_shader_cache_directory(directory);
#endif
#if defined(_WIN32)
    d3d11::set_shader_cache_directory(std::move(directory));
#endif
#endif
}

std::unique_ptr<CaptureRenderer> create_capture_renderer(const std::string_view backend) {
    if (backend == "none") {
        return std::make_unique<NullRenderer>();
    }
    if (backend == "vulkan") {
#if defined(STRATA_HAS_VULKAN)
        return std::make_unique<VulkanRenderer>();
#else
        throw std::invalid_argument("this build does not include the Vulkan backend");
#endif
    }
    if (backend == "reference") {
        return std::make_unique<SoftwareRenderer>();
    }
    if (backend == "d3d11") {
#if defined(_WIN32)
        return std::make_unique<D3D11Renderer>();
#else
        throw std::invalid_argument("the d3d11 capture backend is available only on Windows");
#endif
    }
    throw std::invalid_argument("unknown headless render backend '" + std::string(backend) + "'");
}

} // namespace strata::headless
