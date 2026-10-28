#pragma once

#include "gpu/blend.hpp"
#include "gpu/blur_shaders.hpp"
#include "gpu/clip.hpp"
#include "gpu/effect_policy.hpp"
#include "gpu/groups.hpp"
#include "gpu/effect_shaders.hpp"
#include "gpu/png.hpp"
#include "gpu/shaders.hpp"
#include "resources.hpp"
#include "shader.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
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
/** Descriptor set: frame, rounded-clip and presentation-group buffers; two images; a sampler. */
inline constexpr std::uint32_t descriptor_bindings = 6;
inline constexpr VkDescriptorType descriptor_type(std::uint32_t binding) noexcept {
    return binding < 3   ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
           : binding < 5 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                         : VK_DESCRIPTOR_TYPE_SAMPLER;
}
inline Target target_of(Image& image) {
    return {image.image, image.view, image.format, image.width, image.height, image.layout};
}
/**
 * A pipeline's blend state: the packet modes, plus multiply over a rounded clip, which composites
 * the clip coverage like straight alpha.
 */
enum class Blend : std::uint8_t {
    opaque,
    straight_alpha,
    premultiplied_alpha,
    additive,
    multiply,
    rounded_multiply,
};
inline constexpr std::array<Blend, 6> blends{
    Blend::opaque,   Blend::straight_alpha, Blend::premultiplied_alpha,
    Blend::additive, Blend::multiply,       Blend::rounded_multiply,
};
} // namespace detail

struct Renderer::Impl {
    /**
     * Recording never waits for the frame just submitted: each frame owns its command buffer,
     * fence, upload arena, descriptor pools, framebuffers and retirements, and a slot is reused
     * only after its own fence (three frames earlier) has signalled.
     */
    static constexpr std::size_t frames_in_flight = 3U;
    struct Frame {
        VkCommandPool commands{};
        VkCommandBuffer command{};
        VkFence fence{};
        std::unique_ptr<UploadArena> arena;
        std::vector<VkDescriptorPool> pools;
        std::size_t pool_index = 0;
        std::vector<VkFramebuffer> framebuffers;
        std::vector<std::unique_ptr<Image>> retired_images;
        std::vector<std::unique_ptr<Buffer>> retired_buffers;
        bool pending = false;
    };
    /** Draws into one target share a render pass until another operation needs the target. */
    struct OpenPass {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        std::uint32_t width = 0, height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
    };
    Device device;
    std::array<Frame, frames_in_flight> frames;
    std::size_t frame_index = 0;
    std::optional<std::size_t> last_submitted;
    bool recording = false;
    std::optional<OpenPass> open_pass;
    UploadArena* arena = nullptr;
    VkCommandBuffer command{};
    VkPipelineCache pipeline_cache{};
    VkDescriptorSetLayout descriptors{};
    VkPipelineLayout layout{};
    VkSampler nearest{}, linear{};
    std::map<VkFormat, VkRenderPass> render_passes;
    /** Keyed by program, blend, format and whether it draws a full-target triangle. */
    std::map<std::tuple<std::string, Blend, VkFormat, bool>, VkPipeline, std::less<>> pipelines;
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
    /** Host-written geometry: one copy per frame slot, refreshed when the layer's content moves. */
    struct SlotGeometry {
        std::unique_ptr<Buffer> vertices, indices;
        std::uint64_t generation = 0;
    };
    /** The layer's current geometry, patched on the CPU exactly as a single GPU buffer would be. */
    struct Geometry {
        std::array<SlotGeometry, frames_in_flight> slots;
        std::vector<std::uint8_t> vertices;
        std::vector<std::uint8_t> indices;
        std::uint64_t generation = 0;
        std::uint64_t epoch = 0;
        bool seen = false;
    };
    std::map<std::string, Geometry, std::less<>> geometry;
    /** One effect's retained output: only its clipped bounds, at their device-pixel origin. */
    struct CachedEffect {
        host::EffectBatch batch;
        host::Scissor clip;
        double time = 0;
        double logical_width = 0, logical_height = 0;
        std::uint32_t width = 0, height = 0;
        /** Something the sample read has changed since it was taken (see gpu::EffectChanges). */
        bool stale = false;
        /** The render() call that last composited this sample; others are evicted after it. */
        std::uint64_t used = 0;
        std::unique_ptr<Image> output;
    };
    /** Keyed like D3D11: layer, batch source order, and whether it filters isolated content. */
    using EffectKey = std::tuple<std::string, std::uint32_t, bool>;
    std::map<EffectKey, CachedEffect> cached_effects;
    std::string active_layer;
    std::uint64_t render_count = 0;
    std::unique_ptr<Image> white;
    /** Presentation groups bound for the current layer; the identity table when it has none. */
    std::unique_ptr<Buffer> identity_groups;
    gpu::PresentationGroupConstants packed_groups;
    Slice groups{};
    /**
     * Intermediate images, matched by size and format so differently sized effect regions reuse
     * their own images frame after frame. An image serves one use per frame and is released after
     * scratch_lifetime frames without one.
     */
    struct Scratch {
        std::unique_ptr<Image> image;
        std::uint64_t used = 0;
    };
    static constexpr std::uint64_t scratch_lifetime = 120U;
    std::vector<Scratch> scratch;
    std::uint64_t scratch_frame = 0;
    std::vector<std::uint32_t> vertex, fullscreen;
    bool poisoned = false;
    double logical_width{}, logical_height{}, seconds{};
    RenderLayerTelemetry telemetry;

    explicit Impl(Device d) : device(d) {}
    ~Impl();
    Frame& frame() noexcept {
        return frames[frame_index];
    }
    /** Destroys a GPU object only after every frame that may still read it has completed. */
    void retire(std::unique_ptr<Image> image);
    void retire(std::unique_ptr<Buffer> buffer);
    void wait_idle();
    void release_frame(Frame& frame);
    /** Drops cached effect output for one layer, or for every layer when none is named. */
    void drop_effects(std::optional<std::string_view> layer = std::nullopt);
    void end_pass();
    void initialize();
    void add_program(const std::string& name, const std::string& source);
    void prune_programs();
    void begin();
    void finish();
    void clear(Image& image, std::array<float, 4> color = {});
    Image& temporary(std::uint32_t width, std::uint32_t height, VkFormat format);
    void barrier(Target& target, VkImageLayout next);
    /** Copies `region` of `source` into the same place of `destination`. */
    void copy(Target& source, Image& destination, host::Scissor region, std::int32_t destination_x,
              std::int32_t destination_y);
    /** A target-sized copy of `source` holding only `region`, which is all its reader samples. */
    Image& capture(Target& source, host::Scissor region);
    void resources(const host::RenderPacket& packet);
    VkRenderPass render_pass(VkFormat format);
    VkPipeline pipeline(std::string_view program, Blend blend, VkFormat format, bool full);
    VkDescriptorSet descriptor_set(Slice constants, const gpu::RoundedClipConstants& clips,
                                   Image& source, Image& backdrop);
    void draw(Target& target, std::string_view program, Blend blend, Slice constants,
              const gpu::RoundedClipConstants& clips, Image& source, Image& backdrop,
              host::Scissor scissor, const host::DrawBatch* batch = nullptr, Slice vertices = {},
              Slice indices = {});
    /**
     * Blurs `area` of `image` in place, as D3D11's BlurPass does: only `area` grown by the radius
     * is read, at 1/downsample resolution, and only `area` is written, composited over the
     * unblurred pixels through `clips`.
     */
    void blur(Target& image, host::Scissor area, double physical_radius, std::uint32_t downsample,
              std::span<const host::RoundedClip> clips = {});
    static void validate_target(const RenderTarget& target, const FrameOptions& options);
    /** Device pixels per logical pixel on `target`; blur radii are authored in logical pixels. */
    [[nodiscard]] double pixel_scale(const Target& target) const noexcept {
        return std::max(static_cast<double>(target.width) / logical_width,
                        static_cast<double>(target.height) / logical_height);
    }
    gpu::EffectConstants effect_constants(const host::EffectBatch& effect, const Target& target);
    host::Scissor bounds(const host::EffectBatch& effect, const Target& target);
    const std::map<std::uint32_t, Pass>& passes(const host::EffectBatch& effect) const;
    [[nodiscard]] double widest_blur(const host::EffectBatch& effect) const;
    /** How far outside its bounds, in logical pixels, the effect's output depends on its input. */
    [[nodiscard]] double read_reach(const host::EffectBatch& effect) const;
    /** Device pixels of `target` the effect samples: its bounds grown by the D3D11 reach. */
    [[nodiscard]] host::Scissor sample_region(const host::EffectBatch& effect,
                                              const Target& target) const;
    /** The retained sample the effect may composite now instead of refreshing, if any. */
    [[nodiscard]] CachedEffect* reusable(const host::EffectBatch& effect, bool content,
                                         const Target& destination);
    /**
     * Composites one effect into `destination`, refreshing it first unless its retained sample
     * is still due. `source` is the isolated content of a content effect, the captured Surface of
     * a SURFACE backdrop effect, or null to sample `destination` itself; `backdrop` is what
     * authored passes read as the backdrop when it differs from the source.
     */
    void effect(Target& destination, const host::EffectBatch& batch, bool content, Image* source,
                Image* backdrop);
    /** Marks this layer's retained samples stale where the packet's changes reach them. */
    void invalidate_effects(const host::RenderPacket& packet);
    std::pair<Slice, Slice> upload_geometry(const host::RenderPacket& packet);
    RenderLayerTelemetry render(const host::RenderPacket& packet, const RenderTarget& output,
                                FrameOptions options);
};
} // namespace strata::vulkan
