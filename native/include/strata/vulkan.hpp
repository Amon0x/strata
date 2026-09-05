#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <strata/strata.h>
#include <string>
#include <string_view>
#include <vulkan/vulkan.h>

namespace strata {
class Runtime;
class Surface;
} // namespace strata
namespace strata::host {
struct RenderPacket;
}

namespace strata::vulkan {

/** Borrowed Vulkan 1.1+ handles. All calls and queue access must be externally serialized. */
struct Device final {
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
};

enum class TargetLoadAction { preserve, clear };

/** Single-sampled, color/transfer-src/transfer-dst image owned by the host.
 * The host must submit earlier writes before render(), and use final_layout afterwards.
 * Image ownership must already belong to Device::queue_family; no ownership transfer is implied.
 */
struct RenderTarget final {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    std::uint32_t framebuffer_width = 0;
    std::uint32_t framebuffer_height = 0;
    double logical_width = 0;
    double logical_height = 0;
    VkImageLayout initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout final_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
};
struct FrameOptions final {
    TargetLoadAction load_action = TargetLoadAction::preserve;
    std::array<float, 4> clear_color{};
    double time_seconds = 0;
};
struct RenderLayerTelemetry final {
    std::uint64_t blur_passes = 0;
    std::uint64_t effect_passes = 0;
};
struct PresentedFrame final {
    strata_surface_frame_info surface{};
    std::size_t packet_bytes = 0;
    RenderLayerTelemetry rendering;
};
using ProgramSourceLoader = std::function<std::string(std::string_view)>;
struct PresenterOptions final {
    ProgramSourceLoader load_program_source;
};

/** GPU packet renderer. Owns no instance, device, window, or swapchain.
 * Submits to the borrowed graphics queue and waits for its own fence before returning.
 * This synchronous boundary makes target/resource destruction safe after a call; it is not a
 * command-buffer recording API. Calls must occur outside the host's active render pass.
 */
class Renderer final {
  public:
    explicit Renderer(Device device);
    ~Renderer();
    Renderer(Renderer&&) noexcept;
    Renderer& operator=(Renderer&&) noexcept;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void declare_material(std::string_view id, std::string_view hlsl_source);
    void declare_effect_pass(std::string_view effect_id, std::uint32_t index, std::uint32_t kind,
                             double radius, std::uint32_t downsample,
                             std::uint32_t radius_parameter, std::uint32_t downsample_parameter,
                             std::string_view hlsl_source);
    [[nodiscard]] RenderLayerTelemetry render(std::string_view layer_id,
                                              const host::RenderPacket& packet,
                                              const RenderTarget& target,
                                              FrameOptions options = {});
    void consume_resources(const host::RenderPacket& packet);
    void release_layer(std::string_view layer_id) noexcept;
    void release_target();
    /** Prepare all currently declared blend/effect variants for this target format at load time.
     * Repeating preparation is cheap; call again after program reload or format changes.
     * Returns the number of newly created pipelines. Does not allocate surface resources.
     */
    std::size_t prepare(VkFormat format);
    /** Optional, checksummed, device/driver-specific disk cache. Missing, stale or damaged
     * files return false. Save replaces the file atomically; filesystem failures return false.
     * Serialize access, including file access, with other renderer calls.
     */
    [[nodiscard]] bool load_pipeline_cache(const std::filesystem::path& path);
    [[nodiscard]] bool save_pipeline_cache(const std::filesystem::path& path) const;
    [[nodiscard]] std::size_t pipeline_count() const noexcept;


  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** Ordered Surface lifecycle adapter with the same shader declarations as the D3D11 presenter. */
class Presenter final {
  public:
    Presenter(Runtime& runtime, Device device, PresenterOptions options = {});
    Presenter(strata_runtime* runtime, Device device, PresenterOptions options = {});
    ~Presenter();
    Presenter(Presenter&&) noexcept;
    Presenter& operator=(Presenter&&) noexcept;
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;
    void synchronize_programs();
    [[nodiscard]] bool reload_program_source(std::string_view resource_id, std::string_view source);
    void attach(std::string_view layer_id, Surface& surface);
    void attach(std::string_view layer_id, strata_surface* surface);
    [[nodiscard]] PresentedFrame present(std::string_view layer_id, Surface& surface,
                                         const RenderTarget& target, std::int64_t time_nanoseconds,
                                         FrameOptions options = {});
    [[nodiscard]] PresentedFrame present(std::string_view layer_id, strata_surface* surface,
                                         const RenderTarget& target, std::int64_t time_nanoseconds,
                                         FrameOptions options = {});
    void detach(std::string_view layer_id);
    void discard(std::string_view layer_id) noexcept;
    [[nodiscard]] bool attached(std::string_view layer_id) const noexcept;
    void release_target();
    /** Prepare all currently declared blend/effect variants for this target format at load time.
     * Repeating preparation is cheap; call again after program reload or format changes.
     * Returns the number of newly created pipelines. Does not allocate surface resources.
     */
    std::size_t prepare(VkFormat format);
    /** Optional, checksummed, device/driver-specific disk cache. Missing, stale or damaged
     * files return false. Save replaces the file atomically; filesystem failures return false.
     * Serialize access, including file access, with other renderer calls.
     */
    [[nodiscard]] bool load_pipeline_cache(const std::filesystem::path& path);
    [[nodiscard]] bool save_pipeline_cache(const std::filesystem::path& path) const;
    [[nodiscard]] std::size_t pipeline_count() const noexcept;


  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace strata::vulkan
