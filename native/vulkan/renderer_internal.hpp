#pragma once

#include "gpu/blur_shaders.hpp"
#include "gpu/clip.hpp"
#include "gpu/effect_policy.hpp"
#include "gpu/effect_shaders.hpp"
#include "gpu/groups.hpp"
#include "gpu/isolation.hpp"
#include "gpu/layer_shaders.hpp"
#include "gpu/shaders.hpp"
#include "resources.hpp"
#include "shader.hpp"
#include <algorithm>
#include <cmath>
#include <future>
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
    /** The pointer in framebuffer pixels, and 1 while it is there. */
    float pointer[3]{};
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
        data.inverse_x[i][3] = static_cast<float>(c.shape);
        data.inverse_y[i][3] = static_cast<float>(c.sides);
        for (std::size_t j = 0; j < 4; ++j)
            data.fade[i][j] = static_cast<float>(c.fade[j]);
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
/** A pipeline's blend state is a packet draw blend (already adapted to rounded clips). */
using Blend = host::DrawBlend;
inline constexpr std::array<Blend, host::draw_blend_count> blends{
    Blend::opaque,   Blend::straight_alpha, Blend::premultiplied_alpha,
    Blend::additive, Blend::multiply,       Blend::clipped_multiply,
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
        /** Storage-buffer sets of raster jobs, in pools of their own. */
        std::vector<VkDescriptorPool> raster_pools;
        std::size_t raster_pool_index = 0;
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
    /** The packet's material and effect tables resolved for the layer being rendered. */
    std::vector<std::string_view> layer_materials;
    std::vector<const std::map<std::uint32_t, Pass>*> layer_effects;
    std::map<std::string, std::unique_ptr<Image>, std::less<>> textures;
    /** Host-written geometry: one copy per frame slot, refreshed when the layer's content moves. */
    struct SlotGeometry {
        std::unique_ptr<Buffer> vertices, indices;
        std::uint64_t generation = 0;
    };
    /** Which epoch each frame slot of a layer holds; the decoder owns the geometry itself. */
    struct Geometry {
        std::array<SlotGeometry, frames_in_flight> slots;
        std::uint64_t generation = 0;
        std::uint64_t epoch = 0;
        bool seen = false;
    };
    std::map<std::string, Geometry, std::less<>> geometry;
    /** One effect's retained output: only its clipped bounds, at their device-pixel origin. */
    struct CachedEffect {
        /** The decoder's sample revision (host::EffectBatch::sample_revision) it was taken at. */
        std::uint64_t revision = 0;
        host::Scissor clip;
        double time = 0;
        double logical_width = 0, logical_height = 0;
        std::uint32_t width = 0, height = 0;
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
    /**
     * Raster job kernels (gpu/raster_shaders.hpp), made on the first raster job: accumulate, stroke,
     * resolve_coverage, resolve_composite and finish, over six storage buffers.
     */
    VkDescriptorSetLayout raster_descriptors{};
    VkPipelineLayout raster_layout{};
    std::array<VkPipeline, 7U> raster_pipelines{};
    /** GPU-only work buffers the kernels leave zeroed, and the region they write. */
    std::unique_ptr<Buffer> raster_cells, raster_sums, raster_output;
    /** The points of the art layer being drawn, its program's output. */
    std::unique_ptr<Buffer> raster_points;
    /**
     * Art programs by source hash: SPIR-V compiling in the background once a layer needs it,
     * then its pipeline, which writes the layer's points.
     */
    struct ArtProgram {
        std::shared_future<std::vector<std::uint32_t>> code;
        VkPipeline pipeline = VK_NULL_HANDLE;
        bool failed = false;
    };
    std::map<std::size_t, ArtProgram> art_programs;
    /** Art jobs, run again each frame; `seconds` is the frame time each last ran at. */
    struct LiveArt {
        host::ResourceOperation operation;
        double seconds = 0.0;
    };
    std::vector<LiveArt> live_art;
    bool poisoned = false;
    double logical_width{}, logical_height{}, seconds{};
    std::optional<std::array<double, 2>> pointer;
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
    /** Rasterizes a raster operation's job into its region of `image` (vulkan/raster.cpp). */
    void rasterize(const host::ResourceOperation& operation, Image& image, double time = 0.0);
    /** A raster job and the atlas it fills, one of a batch. */
    struct RasterWork {
        const host::ResourceOperation* operation = nullptr;
        Image* image = nullptr;
    };
    /** Rasterizes every job of `batch` at once: each kernel once for all of them. */
    void rasterize(std::span<const RasterWork> batch, double time);
    /** An art layer's pipelines when they are ready, or null while compiling or when it failed. */
    VkPipeline art_kernel(const std::string& source);
    /** Runs every live art job at this frame's time. */
    void run_live_art();
    /** Drops the live art jobs a write over (part of) a texture replaces. */
    void forget_live_art(const host::ResourceOperation& operation, bool whole);
    void create_raster_kernels();
    /** Starts compiling the raster kernels in the background, so no frame waits on them. */
    void prepare_raster_kernels();
    /** A zeroed GPU-only storage buffer of at least `bytes`, replacing `buffer` if smaller. */
    void reserve_raster_buffer(std::unique_ptr<Buffer>& buffer, VkDeviceSize bytes, bool zeroed);
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
              std::span<const host::RoundedClip> clips = {}, double physical_radius_x = -1.0,
              double physical_radius_y = -1.0);
    /**
     * Hands a temporary image back for another use this frame once everything that reads it has
     * been recorded; its next use transitions it, and every transition is a full barrier.
     */
    void recycle(Image& image);
    /** The constants every layer program shares for `effect` on `target`. */
    gpu::LayerConstants layer_constants(const host::EffectBatch& effect, const Target& target);
    /**
     * Runs a content layer's built-in filters over `current` within `region`, returning the
     * image holding the result; temporaries it takes are added to `taken`.
     */
    Image* run_filters(const host::EffectBatch& effect, const Target& destination, Image* current,
                       host::Scissor region, std::vector<Image*>& taken);
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
    std::pair<Slice, Slice> upload_geometry(const host::RenderPacket& packet);
    RenderLayerTelemetry render(const host::RenderPacket& packet, const RenderTarget& output,
                                FrameOptions options);
};
} // namespace strata::vulkan
