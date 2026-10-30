#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <strata/raster_job.hpp>
#include <strata/strata.h>

namespace strata {
class Surface;
}

namespace strata::host {

/** Maximum balanced CONTENT-effect nesting accepted by the packet and bundled renderers. */
inline constexpr std::size_t maximum_content_effect_depth = 8U;
inline constexpr std::size_t maximum_rounded_clip_depth = 16U;

inline constexpr std::uint32_t resource_create = 0U;
inline constexpr std::uint32_t resource_upload = 1U;
inline constexpr std::uint32_t resource_release = 2U;
/** Image creation: decoded, straight-alpha RGBA8 rows (see strata.h). */
inline constexpr std::uint32_t resource_image = 3U;
/**
 * A region of an atlas rasterized by the backend from a raster job (raster_job.hpp): every texel
 * of the region is replaced, as an upload of the same region would replace it.
 */
inline constexpr std::uint32_t resource_raster = 4U;
inline constexpr std::uint32_t packet_flag_geometry_payload = 1U;
inline constexpr std::uint32_t packet_flag_geometry_patches = 2U;
/**
 * Refresh rate of an effect that requests none: zero, evaluated every frame. Only an effect that
 * asks for a rate retains and reuses its filtered sample between refreshes.
 */
inline constexpr double default_effect_refresh_rate = 0.0;

/** Highest presentation group index. A vertex's z selects its group; zero is the identity. */
inline constexpr std::uint32_t maximum_presentation_group = 511U;

inline constexpr std::uint32_t texture_format_r8 = 0U;
inline constexpr std::uint32_t texture_format_rgba8 = 1U;
/**
 * An R8 atlas sampled bilinearly rather than texel-exact: shape coverage masks, which presentation
 * groups and rotated draws resample. Glyph pages stay texture_format_r8.
 */
inline constexpr std::uint32_t texture_format_r8_filtered = 2U;
inline constexpr std::uint32_t texture_sampling_nearest = 0U;
inline constexpr std::uint32_t texture_sampling_linear = 1U;

/**
 * One ordered GPU-resource mutation decoded from the current packet. An image's format is
 * texture_format_rgba8 and its bytes are width * height straight-alpha RGBA8 texels.
 */
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
    /** A raster operation's job, validated against its region and format by the decoder. */
    RasterJob raster;
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
    /**
     * 0 the rounded rectangle, 1 an ellipse, 2 a regular polygon and 3 a star of `sides`, each
     * filling the rectangle; `fade` ramps coverage out over that many logical pixels toward the
     * top, right, bottom and left edges.
     */
    std::uint32_t shape = 0U;
    double sides = 0.0;
    std::array<double, 4U> fade{};
    [[nodiscard]] friend bool operator==(const RoundedClip&, const RoundedClip&) = default;
};

/** Bytes of one packet vertex record; the vertex stream is a whole number of these. */
inline constexpr std::size_t packet_vertex_bytes = 88U;

/** How a draw batch blends, already adapted to its rounded clips (STRATA_RENDER_BLEND_*). */
enum class DrawBlend : std::uint8_t {
    opaque = STRATA_RENDER_BLEND_OPAQUE,
    straight_alpha = STRATA_RENDER_BLEND_STRAIGHT_ALPHA,
    premultiplied_alpha = STRATA_RENDER_BLEND_PREMULTIPLIED_ALPHA,
    additive = STRATA_RENDER_BLEND_ADDITIVE,
    multiply = STRATA_RENDER_BLEND_MULTIPLY,
    clipped_multiply = STRATA_RENDER_BLEND_CLIPPED_MULTIPLY,
};
inline constexpr std::size_t draw_blend_count = 6U;

/**
 * How a draw applies its rounded-clip coverage (STRATA_RENDER_CLIP_COVERAGE_*). The values are
 * the shaders' roundedClipMode.
 */
enum class ClipCoverage : std::uint8_t {
    straight_alpha = STRATA_RENDER_CLIP_COVERAGE_STRAIGHT_ALPHA,
    premultiplied_alpha = STRATA_RENDER_CLIP_COVERAGE_PREMULTIPLIED_ALPHA,
    hard = STRATA_RENDER_CLIP_COVERAGE_HARD,
    multiply = STRATA_RENDER_CLIP_COVERAGE_MULTIPLY,
};

/**
 * Indexed geometry submission. Vertex records use the fixed packet_vertex_bytes layout. The
 * material is an index into RenderPacket::materials.
 */
struct DrawBatch final {
    std::uint32_t source_order = 0U;
    Scissor scissor;
    std::uint16_t material = 0U;
    DrawBlend blend = DrawBlend::straight_alpha;
    ClipCoverage clip_coverage = ClipCoverage::hard;
    std::optional<std::string> texture;
    std::uint32_t base_vertex = 0U;
    std::uint32_t first_index = 0U;
    std::uint32_t index_count = 0U;
    std::vector<RoundedClip> rounded_clips;
    [[nodiscard]] friend bool operator==(const DrawBatch&, const DrawBatch&) = default;
};

/**
 * A presentation group's complete transform, opacity and tone over logical layout space for the
 * current frame. A vertex at (x, y) in the group is placed at
 * (scale_x * x + shear_x * y + translate_x, shear_y * x + scale_y * y + translate_y), with the
 * translation rounded to whole framebuffer pixels; the shears are zero unless the group rotates.
 * A group turned in depth also divides both by w = project_x * x + project_y * y + project_w
 * (perspective, interpolated perspective-correctly).
 * Its material opacity is multiplied by `opacity`. `tone` in [0, 1] mirrors the lightness of the
 * group's light, unsaturated vertex colours toward dark ink (see tone_color); images and shadows
 * are not toned.
 */
struct PresentationGroup final {
    double scale_x = 1.0;
    double scale_y = 1.0;
    double translate_x = 0.0;
    double translate_y = 0.0;
    double opacity = 1.0;
    double tone = 0.0;
    double shear_x = 0.0;
    double shear_y = 0.0;
    /** A group turned in depth divides by w = project_x * x + project_y * y + project_w. */
    double project_x = 0.0;
    double project_y = 0.0;
    double project_w = 1.0;

    /** The divisor a point of the group's content is projected by: 1 unless turned in depth. */
    [[nodiscard]] constexpr double place_w(const double x, const double y) const noexcept {
        const double w = project_x * x + project_y * y + project_w;
        return w > 1.0e-6 ? w : 1.0e-6;
    }
    /** Where the group places a point of its content. */
    [[nodiscard]] constexpr double place_x(const double x, const double y) const noexcept {
        return (scale_x * x + shear_x * y + translate_x) / place_w(x, y);
    }
    [[nodiscard]] constexpr double place_y(const double x, const double y) const noexcept {
        return (shear_y * x + scale_y * y + translate_y) / place_w(x, y);
    }
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

/** How a content effect's layer composites (STRATA_LAYER_COMPOSITE_*). */
enum class LayerComposite : std::uint32_t {
    source_over = STRATA_LAYER_COMPOSITE_SOURCE_OVER,
    multiply = STRATA_LAYER_COMPOSITE_MULTIPLY,
    screen = STRATA_LAYER_COMPOSITE_SCREEN,
    overlay = STRATA_LAYER_COMPOSITE_OVERLAY,
    darken = STRATA_LAYER_COMPOSITE_DARKEN,
    lighten = STRATA_LAYER_COMPOSITE_LIGHTEN,
    color_dodge = STRATA_LAYER_COMPOSITE_COLOR_DODGE,
    color_burn = STRATA_LAYER_COMPOSITE_COLOR_BURN,
    hard_light = STRATA_LAYER_COMPOSITE_HARD_LIGHT,
    soft_light = STRATA_LAYER_COMPOSITE_SOFT_LIGHT,
    difference = STRATA_LAYER_COMPOSITE_DIFFERENCE,
    exclusion = STRATA_LAYER_COMPOSITE_EXCLUSION,
    hue = STRATA_LAYER_COMPOSITE_HUE,
    saturation = STRATA_LAYER_COMPOSITE_SATURATION,
    color = STRATA_LAYER_COMPOSITE_COLOR,
    luminosity = STRATA_LAYER_COMPOSITE_LUMINOSITY,
    destination_in = STRATA_LAYER_COMPOSITE_DESTINATION_IN,
    destination_out = STRATA_LAYER_COMPOSITE_DESTINATION_OUT,
    destination_in_luminance = STRATA_LAYER_COMPOSITE_DESTINATION_IN_LUMINANCE,
    destination_out_luminance = STRATA_LAYER_COMPOSITE_DESTINATION_OUT_LUMINANCE,
    channel_maximum = STRATA_LAYER_COMPOSITE_CHANNEL_MAXIMUM,
    channel_minimum = STRATA_LAYER_COMPOSITE_CHANNEL_MINIMUM,
    channel_difference = STRATA_LAYER_COMPOSITE_CHANNEL_DIFFERENCE,
    plus = STRATA_LAYER_COMPOSITE_PLUS,
};
inline constexpr std::uint32_t layer_composite_count = 24U;

/** A content effect layer's built-in filter (STRATA_LAYER_FILTER_*). */
enum class LayerFilterKind : std::uint32_t {
    blur = STRATA_LAYER_FILTER_BLUR,
    color_matrix = STRATA_LAYER_FILTER_COLOR_MATRIX,
    gradient_map = STRATA_LAYER_FILTER_GRADIENT_MAP,
    levels = STRATA_LAYER_FILTER_LEVELS,
    drop_shadow = STRATA_LAYER_FILTER_DROP_SHADOW,
};
inline constexpr std::size_t maximum_layer_filters = 8U;
inline constexpr std::size_t maximum_layer_filter_values = 24U;

/**
 * One built-in filter, its lengths already presented through the batch's group (logical pixels of
 * the decoded frame). See STRATA_LAYER_FILTER_* for what each value means.
 */
struct LayerFilter final {
    LayerFilterKind kind = LayerFilterKind::blur;
    std::vector<double> values;
    [[nodiscard]] double value(const std::size_t index,
                               const double fallback = 0.0) const noexcept {
        return index < values.size() ? values[index] : fallback;
    }
    [[nodiscard]] friend bool operator==(const LayerFilter&, const LayerFilter&) = default;
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
 * The effect is an index into RenderPacket::effects.
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
    std::uint16_t effect = 0U;
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
    /**
     * How far outside its bounds, in logical pixels, the effect's output depends on what was drawn
     * before it: the core's answer from the effect's declared passes.
     */
    double read_reach = 0.0;
    /**
     * The decoder's verdict on retained samples. It changes whenever anything this effect's output
     * depends on changed since the previous packet: its id, parameters, bounds or presentation, or
     * anything drawn within read_reach of its bounds (geometry, group moves, replaced texels). A
     * backend may composite the sample it retained for this source order while the revision is
     * unchanged and the effect's refresh interval has not elapsed (gpu::effect_refresh_due);
     * otherwise it evaluates the effect again. Revisions are never reused within a decoder.
     */
    std::uint64_t sample_revision = 0U;
    /**
     * A content effect's layer: the built-in filters its pixels run through after its declared
     * passes, in order, and how it then combines with what is under it.
     */
    LayerComposite composite = LayerComposite::source_over;
    std::vector<LayerFilter> filters;
    /** The widest distance, in logical pixels, its filters read beyond a pixel (blurs, shadows). */
    [[nodiscard]] double filter_reach() const noexcept {
        double reach = 0.0;
        for (const LayerFilter& filter : filters) {
            if (filter.kind == LayerFilterKind::blur) {
                reach = std::max({reach, filter.value(0U), filter.value(1U)});
            } else if (filter.kind == LayerFilterKind::drop_shadow) {
                reach = std::max(reach, filter.value(6U) + std::max(std::abs(filter.value(4U)),
                                                                    std::abs(filter.value(5U))));
            }
        }
        return reach;
    }
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

/**
 * Logical bounds whose presented content changed since the previous packet: retained vertex
 * patches (old and new positions), presentation-group moves, and draws whose sampled texels this
 * packet's resource operations replaced.
 */
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
    /** Material and effect ids the batches name by index; retained with the batches. */
    std::vector<std::string> materials;
    std::vector<std::string> effects;
    /** True only when this decoded frame supplied complete geometry rather than retained patches. */
    bool full_geometry_payload = false;
    std::vector<GeometryPatch> vertex_patches;
    std::vector<GeometryPatch> index_patches;
    /**
     * What changed since the previous decoded packet, for backends that retain samples of what
     * they drew (effects): everything when set, otherwise only geometry_dirty_regions.
     */
    bool geometry_dirty_all = false;
    std::vector<GeometryDirtyRegion> geometry_dirty_regions;
    /**
     * Presentation groups indexed by vertex group, through the highest index in use (empty when
     * none; entry zero and absent indices are the identity). A changed table moves grouped
     * content without a geometry change: the epoch stays, and the moved groups' old and new
     * bounds are reported in geometry_dirty_regions.
     */
    std::vector<PresentationGroup> groups;
};

/** Texel dimensions of each live texture, by texture id. */
using TextureSizes = std::map<std::string, std::array<std::uint32_t, 2U>, std::less<>>;

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
    /** The previous complete packet's buffers, reused by the next complete payload. */
    std::optional<RenderPacket> spare_;
    /** Group-local copies of grouped blur/effect batches, re-presented when the table changes. */
    std::vector<std::pair<std::size_t, SubmissionBatch>> grouped_batches_;
    /** Layout-space left/top/right/bottom of each group's retained vertices, by group index. */
    std::vector<std::array<double, 4U>> group_extents_;
    /** Sizes of the stream's live textures, which place an upload within its texture. */
    TextureSizes texture_sizes_;
    /** What each effect (by source order, and whether it filters content) last sampled. */
    struct EffectSample final {
        EffectBatch signature;
        std::string id;
        std::uint64_t revision = 0U;
    };
    std::map<std::pair<std::uint32_t, bool>, EffectSample> effect_samples_;
    std::uint64_t next_sample_revision_ = 1U;
    void assign_sample_revisions(RenderPacket& packet);
};

struct SurfacePacketFrame final {
    strata_surface_frame_info surface{};
    const RenderPacket* packet = nullptr;
    std::size_t packet_bytes = 0U;
    /** The framed packet as read, valid until the stream frames again (for recording). */
    std::span<const std::uint8_t> bytes;
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
