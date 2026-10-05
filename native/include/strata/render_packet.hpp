#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <strata/strata.h>

namespace strata {
class Surface;
}

namespace strata::host {

/** Maximum balanced CONTENT-effect nesting accepted by the packet and bundled renderers. */
inline constexpr std::size_t maximum_content_effect_depth = 4U;
inline constexpr std::size_t maximum_rounded_clip_depth = 16U;

inline constexpr std::uint32_t resource_create = 0U;
inline constexpr std::uint32_t resource_upload = 1U;
inline constexpr std::uint32_t resource_release = 2U;
inline constexpr std::uint32_t resource_encoded_image = 3U;
inline constexpr std::uint32_t packet_flag_geometry_payload = 1U;
inline constexpr std::uint32_t packet_flag_geometry_patches = 2U;
inline constexpr double default_effect_refresh_rate = 240.0;

/** Highest presentation group index. A vertex's z selects its group; zero is the identity. */
inline constexpr std::uint32_t maximum_presentation_group = 511U;

inline constexpr std::uint32_t texture_format_r8 = 0U;
inline constexpr std::uint32_t texture_format_rgba8 = 1U;
/** Encodings of a resource_encoded_image payload; either way the texture is RGBA8. */
inline constexpr std::uint32_t texture_encoding_png = 0U;
inline constexpr std::uint32_t texture_encoding_rgba8 = 1U;
inline constexpr std::uint32_t texture_sampling_nearest = 0U;
inline constexpr std::uint32_t texture_sampling_linear = 1U;

/** One ordered GPU-resource mutation decoded from the current packet. */
struct ResourceOperation final {
    std::uint32_t kind = 0U;
    std::string texture;
    std::uint32_t format = 0U;
    std::uint32_t sampling = 0U;
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<std::uint8_t> bytes;
};

struct Scissor final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    [[nodiscard]] friend bool operator==(const Scissor&, const Scissor&) = default;
};

/** A rounded clip plus the affine map from presented logical pixels into its local bounds. */
struct RoundedClip final {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    std::array<double, 4U> radii{};
    std::array<double, 6U> inverse_transform{1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    [[nodiscard]] friend bool operator==(const RoundedClip&, const RoundedClip&) = default;
};

/** Indexed geometry submission. Vertex records use the fixed 88-byte layout. */
struct DrawBatch final {
    std::uint32_t source_order = 0U;
    Scissor scissor;
    std::string material;
    std::string blend_mode;
    std::optional<std::string> texture;
    std::uint32_t base_vertex = 0U;
    std::uint32_t first_index = 0U;
    std::uint32_t index_count = 0U;
    std::vector<RoundedClip> rounded_clips;
    [[nodiscard]] friend bool operator==(const DrawBatch&, const DrawBatch&) = default;
};

/**
 * A presentation group's complete transform, opacity and tone over logical layout space for the
 * current frame. Vertices in the group are placed at position * scale + translate, with the
 * translation rounded to whole framebuffer pixels, and their material opacity is multiplied by
 * `opacity`. `tone` in [0, 1] mirrors the lightness of the group's light, unsaturated vertex
 * colours toward dark ink (see tone_color); images and shadows are not toned.
 */
struct PresentationGroup final {
    double scale_x = 1.0;
    double scale_y = 1.0;
    double translate_x = 0.0;
    double translate_y = 0.0;
    double opacity = 1.0;
    double tone = 0.0;
    [[nodiscard]] friend bool operator==(const PresentationGroup&,
                                         const PresentationGroup&) = default;
};

/**
 * A group's tone applied to one straight-alpha colour, channels in [0, 1]. Light, unsaturated
 * colours are ink: their lightness is mirrored toward dark ink in proportion to `tone`. Saturated
 * colours and dark colours are left as authored, so accents keep their hue and dark ink stays dark.
 * The GPU vertex stage and every CPU backend implement exactly this.
 */
constexpr void tone_color(float& red, float& green, float& blue, const float tone) noexcept {
    const float high = red > green ? (red > blue ? red : blue) : (green > blue ? green : blue);
    const float low = red < green ? (red < blue ? red : blue) : (green < blue ? green : blue);
    const auto unit = [](const float value) { return value < 0.0F ? 0.0F : value > 1.0F ? 1.0F : value; };
    const float ink = unit((high - 0.5F) * 4.0F) * unit(1.0F - (high - low) * 2.5F);
    const float amount = unit(tone) * ink;
    const float lift = (1.0F - high) * 0.6F;
    red += (0.043F + lift - red) * amount;
    green += (0.071F + lift - green) * amount;
    blue += (0.125F + lift - blue) * amount;
}

/**
 * The alpha a grayscale glyph's atlas coverage contributes, given the ink it is drawn in (after
 * tone), channels in [0, 1]. Blending coverage as plain alpha in an encoded framebuffer makes light
 * text on dark come out thin and dark text on light come out heavy; weighting the midtones by the
 * ink's lightness puts back what a linear-light blend would have given light ink and leaves dark ink
 * close to its outline. The unified pixel stage and every CPU backend implement exactly this.
 */
constexpr float text_coverage(const float coverage, const float red, const float green,
                              const float blue) noexcept {
    const float lightness = 0.2126F * red + 0.7152F * green + 0.0722F * blue;
    const float strength = 0.12F + 0.68F * lightness;
    const float weighted = coverage + strength * coverage * (1.0F - coverage);
    return weighted < 0.0F ? 0.0F : weighted > 1.0F ? 1.0F : weighted;
}

/** Region bounds are already presented through `group` for the decoded frame. */
struct BlurBatch final {
    std::uint32_t source_order = 0U;
    Scissor scissor;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    double radius = 0.0;
    std::uint32_t downsample = 1U;
    std::vector<RoundedClip> rounded_clips;
    std::uint32_t group = 0U;
    [[nodiscard]] friend bool operator==(const BlurBatch&, const BlurBatch&) = default;
};

enum class EffectBatchKind : std::uint32_t {
    backdrop = 2U,
    content_begin = 3U,
};

enum class EffectBackdropSource : std::uint32_t {
    current = 0U,
    surface = 1U,
};

/**
 * Bounds, radii, opacity and tone are already presented through `group` for the decoded frame.
 */
struct EffectBatch final {
    EffectBatchKind kind = EffectBatchKind::backdrop;
    EffectBackdropSource backdrop_source = EffectBackdropSource::current;
    std::uint32_t source_order = 0U;
    Scissor scissor;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    std::array<double, 4U> radii{};
    std::string effect;
    double opacity = 1.0;
    /** Maximum cached-result refresh rate; zero requests per-frame evaluation. */
    double refresh_rate = default_effect_refresh_rate;
    std::array<double, 16U> parameters{};
    std::uint32_t parameter_count = 0U;
    std::vector<RoundedClip> rounded_clips;
    std::uint32_t group = 0U;
    /**
     * Nonzero asks the backend to measure the mean colour of the backdrop this effect captures and
     * hand it back to the Surface under this token (strata_surface_report_backdrop). A backend that
     * cannot measure simply reports nothing.
     */
    std::uint64_t probe = 0U;
    /** The owning group's tone, exposed to authored passes as effectTone(). */
    double tone = 0.0;
    [[nodiscard]] friend bool operator==(const EffectBatch&, const EffectBatch&) = default;
};

struct ContentEffectEndBatch final {
    std::uint32_t source_order = 0U;
    Scissor scissor;
    std::vector<RoundedClip> rounded_clips;
    [[nodiscard]] friend bool operator==(
        const ContentEffectEndBatch&,
        const ContentEffectEndBatch&
    ) = default;
};

using SubmissionBatch = std::variant<DrawBatch, BlurBatch, EffectBatch, ContentEffectEndBatch>;

struct GeometryPatch final {
    std::uint32_t offset = 0U;
    std::vector<std::uint8_t> bytes;
};

/** Logical bounds touched by one or more retained vertex patches, including old positions. */
struct GeometryDirtyRegion final {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

/** Backend-ready render plan. Geometry is retained when a packet repeats its geometry epoch. */
struct RenderPacket final {
    std::uint64_t frame_index = 0U;
    std::uint64_t geometry_epoch = 0U;
    std::uint32_t planned_draw_count = 0U;
    std::uint32_t skipped_draw_count = 0U;
    std::vector<ResourceOperation> resources;
    std::vector<std::uint8_t> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<SubmissionBatch> batches;
    /** True only when this decoded frame supplied complete geometry rather than retained patches. */
    bool full_geometry_payload = false;
    std::vector<GeometryPatch> vertex_patches;
    std::vector<GeometryPatch> index_patches;
    bool geometry_dirty_all = false;
    std::vector<GeometryDirtyRegion> geometry_dirty_regions;
    /**
     * Presentation groups indexed by vertex group, through the highest index in use (empty when
     * none; entry zero and absent indices are the identity). A changed table marks
     * geometry_dirty_all, since grouped content moves without a geometry change.
     */
    std::vector<PresentationGroup> groups;
};

/**
 * Stateful decoder for one ordered Surface/backend packet stream. Compact packets depend on the
 * latest full geometry epoch, so consume every framed packet and reset only when discarding the
 * stream.
 */
class RenderPacketDecoder final {
  public:
    [[nodiscard]] const RenderPacket& decode(std::span<const std::uint8_t> bytes);
    void reset() noexcept;

  private:
    std::optional<RenderPacket> retained_;
    /** Group-local copies of grouped blur/effect batches, re-presented when the table changes. */
    std::vector<std::pair<std::size_t, SubmissionBatch>> grouped_batches_;
    /** Layout-space left/top/right/bottom of each group's retained vertices, by group index. */
    std::vector<std::array<double, 4U>> group_extents_;
};

struct SurfacePacketFrame final {
    strata_surface_frame_info surface{};
    const RenderPacket* packet = nullptr;
    std::size_t packet_bytes = 0U;
};

/**
 * Backend-neutral ordered packet stream for one live Surface. Backend presenters use this rather
 * than duplicating frame/read/decode and terminal release ordering.
 */
class SurfacePacketStream final {
  public:
    explicit SurfacePacketStream(Surface& surface);
    explicit SurfacePacketStream(strata_surface* surface);

    SurfacePacketStream(const SurfacePacketStream&) = delete;
    SurfacePacketStream& operator=(const SurfacePacketStream&) = delete;
    SurfacePacketStream(SurfacePacketStream&&) = delete;
    SurfacePacketStream& operator=(SurfacePacketStream&&) = delete;
    ~SurfacePacketStream();

    [[nodiscard]] SurfacePacketFrame frame(std::int64_t time_nanoseconds);
    [[nodiscard]] const RenderPacket& prepare_release();
    void acknowledge_release();
    [[nodiscard]] bool matches(const Surface& surface) const noexcept;
    [[nodiscard]] bool matches(const strata_surface* surface) const noexcept;
    [[nodiscard]] strata_surface* native_surface() const noexcept;
    void reset() noexcept;

  private:
    strata_surface* surface_ = nullptr;
    RenderPacketDecoder decoder_;
    std::vector<std::uint8_t> bytes_;
};

} // namespace strata::host
