#pragma once

#include "gpu/blur_shaders.hpp"
#include "gpu/clip.hpp"
#include "gpu/effect_shaders.hpp"
#include "gpu/png.hpp"
#include "gpu/shaders.hpp"
#include "resources.hpp"
#include "shader.hpp"
#include <cmath>
#include <map>
#include <span>
#include <strata/render_packet.hpp>
#include <strata/vulkan.hpp>
#include <tuple>

namespace strata::vulkan {
using namespace detail;
namespace detail {
struct FrameData {
    float logical[2];
    float framebuffer[2];
    float seconds;
    float padding[3]{};
};
struct BlurData {
    float texel[2]{};
    float direction[2]{};
    float radius{};
    float composite{};
    float padding[2]{};
    float source_uv[4]{0, 0, 1, 1};
    float original_uv[4]{0, 0, 1, 1};
    float logical[2]{};
    float target[2]{};
};
struct Target {
    VkImage image;
    VkImageView view;
    VkFormat format;
    std::uint32_t width, height;
    VkImageLayout layout;
};
inline gpu::RoundedClipConstants clip_constants(std::span<const host::RoundedClip> clips,
                                                gpu::RoundedClipMode mode) {
    if (clips.size() > host::maximum_rounded_clip_depth)
        throw std::length_error("rounded clip stack exceeds packet limit");
    gpu::RoundedClipConstants data;
    data.count = static_cast<std::uint32_t>(clips.size());
    data.mode = static_cast<std::uint32_t>(mode);
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const auto& c = clips[i];
        data.bounds[i] = {static_cast<float>(c.x), static_cast<float>(c.y),
                          static_cast<float>(c.width), static_cast<float>(c.height)};
        for (std::size_t j = 0; j < 4; ++j)
            data.radii[i][j] = static_cast<float>(c.radii[j]);
        for (std::size_t j = 0; j < 3; ++j) {
            data.inverse_x[i][j] = static_cast<float>(c.inverse_transform[j]);
            data.inverse_y[i][j] = static_cast<float>(c.inverse_transform[j + 3]);
        }
    }
    return data;
}
inline Target target_of(Image& image) {
    return {image.image, image.view, image.format, image.width, image.height, image.layout};
}
} // namespace detail

struct Renderer::Impl {
    Device device;
    UploadArena arena;
    VkCommandPool commands{};
    VkCommandBuffer command{};
    VkFence fence{};
    VkPipelineCache pipeline_cache{};
    VkDescriptorSetLayout descriptors{};
    VkPipelineLayout layout{};
    VkSampler nearest{}, linear{};
    std::vector<VkDescriptorPool> pools;
    std::size_t pool_index = 0;
    std::map<VkFormat, VkRenderPass> render_passes;
    std::map<std::tuple<std::string, std::string, VkFormat, bool>, VkPipeline> pipelines;
    std::map<std::string, std::vector<std::uint32_t>, std::less<>> programs;
    std::map<std::string, std::string, std::less<>> materials;
    struct Pass {
        std::uint32_t kind;
        double radius;
        std::uint32_t downsample;
        std::uint32_t radius_parameter, downsample_parameter;
        std::string program;
    };
    std::map<std::string, std::map<std::uint32_t, Pass>, std::less<>> effects;
    std::map<std::string, std::unique_ptr<Image>, std::less<>> textures;
    struct Geometry {
        std::unique_ptr<Buffer> vertices, indices;
        std::uint64_t epoch = 0;
    };
    std::map<std::string, Geometry, std::less<>> geometry;
    struct CachedEffect {
        host::EffectBatch batch;
        std::uint64_t epoch = 0;
        double time = 0;
        double logical_width = 0, logical_height = 0;
        std::uint32_t width = 0, height = 0;
        std::unique_ptr<Image> output;
    };
    std::map<std::pair<std::string, std::uint64_t>, CachedEffect> cached_effects;
    std::string active_layer;
    std::uint64_t active_epoch = 0;
    std::uint64_t effect_index = 0;
    std::unique_ptr<Image> white;
    std::vector<std::unique_ptr<Image>> scratch, retired;
    std::vector<VkFramebuffer> framebuffers;
    std::size_t scratch_index = 0;
    std::vector<std::uint32_t> vertex, fullscreen;
    bool poisoned = false;
    double logical_width{}, logical_height{}, seconds{};
    RenderLayerTelemetry telemetry;

    explicit Impl(Device d) : device(d), arena(d) {}
    ~Impl();
    void clear_framebuffers();
    void initialize();
    void add_program(const std::string& name, const std::string& source);
    void prune_programs();
    void begin();
    void finish();
    void clear(Image& image, std::array<float, 4> color = {});
    Image& temporary(std::uint32_t width, std::uint32_t height, VkFormat format);
    void barrier(Target& target, VkImageLayout next);
    Image& capture(Target& source);
    void resources(const host::RenderPacket& packet);
    VkRenderPass render_pass(VkFormat format);
    VkPipeline pipeline(const std::string& program, const std::string& blend, VkFormat format,
                        bool full);
    VkDescriptorSet descriptor_set(Slice constants, const gpu::RoundedClipConstants& clips,
                                   Image& source, Image& backdrop);
    void draw(Target& target, const std::string& program, const std::string& blend, Slice constants,
              const gpu::RoundedClipConstants& clips, Image& source, Image& backdrop,
              host::Scissor scissor, const host::DrawBatch* batch = nullptr, Slice vertices = {},
              Slice indices = {});
    Image& blur(Image& source, double radius, std::uint32_t downsample);
    gpu::EffectConstants effect_constants(const host::EffectBatch& effect, const Target& target);
    host::Scissor bounds(const host::EffectBatch& effect, const Target& target);
    void effect(Target& destination, Image& source, Image& backdrop,
                const host::EffectBatch& batch);
    std::pair<Slice, Slice> upload_geometry(const host::RenderPacket& packet);
    RenderLayerTelemetry render(const host::RenderPacket& packet, const RenderTarget& output,
                                FrameOptions options);
};
} // namespace strata::vulkan
