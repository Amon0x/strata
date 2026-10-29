#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "data/json.hpp"
#include "runtime/value.hpp"
#include "ui/layout.hpp"
#include "ui/paint.hpp"
#include "ui/path.hpp"
#include "ui/typography.hpp"

namespace strata::resource {
class SvgImageRegistry;
}

namespace strata::ui {

class InputRouter;
class MotionRuntime;
class TextEngine;
class CommandIndex;
class BehaviorRegistry;
class MaterialRegistry;
class WidgetRegistry;

/**
 * The render vocabulary and the authored value vocabulary describe colour identically, so they
 * share one type. A paint reaching a render command never needs a conversion hop.
 */
using RenderColor = runtime::ColorValue;

struct CornerRadii final {
    double top_left = 0.0;
    double top_right = 0.0;
    double bottom_right = 0.0;
    double bottom_left = 0.0;
    [[nodiscard]] static CornerRadii all(double value) noexcept;
    [[nodiscard]] double largest() const noexcept;
    [[nodiscard]] friend bool operator==(const CornerRadii&, const CornerRadii&) = default;
};

/**
 * An authored `radius`: a number rounds every corner; a map names corners (`topLeft`, `topRight`,
 * `bottomRight`, `bottomLeft`), sides (`top`, `bottom`, `left`, `right`) or `all`, most specific
 * first. Negative and non-finite values are zero; anything else is not a radius.
 */
[[nodiscard]] std::optional<CornerRadii> corner_radii_value(const runtime::Value* value) noexcept;

struct RenderBorder final {
    double width = 0.0;
    RenderColor color;
    bool inside = true;
    [[nodiscard]] friend bool operator==(const RenderBorder&, const RenderBorder&) = default;
};

struct TextureRegion final {
    double u = 0.0;
    double v = 0.0;
    double width = 1.0;
    double height = 1.0;
    [[nodiscard]] friend bool operator==(const TextureRegion&, const TextureRegion&) = default;
};

/**
 * A font id shared by the glyphs that name it. Every glyph of a run carries its font, and ids such
 * as "strata:fonts/default" are too long to store inline, so copies share one immutable string
 * rather than allocating per glyph. Compares by content, like the string it stands for.
 */
class SharedFontId final {
  public:
    SharedFontId() = default;
    SharedFontId(const std::string_view value)
        : value_(value.empty() ? nullptr : std::make_shared<const std::string>(value)) {}
    SharedFontId(const std::string& value) : SharedFontId(std::string_view(value)) {}
    SharedFontId(const char* const value) : SharedFontId(std::string_view(value)) {}

    [[nodiscard]] std::string_view view() const noexcept {
        return value_ != nullptr ? std::string_view(*value_) : std::string_view{};
    }
    [[nodiscard]] std::string str() const { return std::string(view()); }
    [[nodiscard]] bool empty() const noexcept { return value_ == nullptr; }
    operator std::string_view() const noexcept { return view(); }

    [[nodiscard]] friend bool operator==(const SharedFontId& left,
                                         const SharedFontId& right) noexcept {
        return left.value_ == right.value_ || left.view() == right.view();
    }

  private:
    std::shared_ptr<const std::string> value_;
};

struct LogicalGlyph final {
    SharedFontId font_id;
    std::uint32_t glyph_id = 0U;
    std::uint32_t code_point = 0U;
    std::size_t text_start_offset = 0U;
    std::size_t text_end_offset = 0U;
    double x = 0.0;
    double baseline = 0.0;
    double advance = 0.0;
    double x_placement = 0.0;
    double y_placement = 0.0;
    double y_advance = 0.0;
    std::uint32_t font_style_flags = 0U;
    [[nodiscard]] friend bool operator==(const LogicalGlyph&, const LogicalGlyph&) = default;
};

/** Immutable shared glyph payload; cached fragments can be composed without deep-copying runs. */
class LogicalGlyphRun final {
  public:
    using const_iterator = std::vector<LogicalGlyph>::const_iterator;

    LogicalGlyphRun();
    LogicalGlyphRun(std::vector<LogicalGlyph> glyphs);
    LogicalGlyphRun(std::initializer_list<LogicalGlyph> glyphs);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const LogicalGlyph& front() const;
    [[nodiscard]] const LogicalGlyph& operator[](std::size_t index) const noexcept;
    [[nodiscard]] const_iterator begin() const noexcept;
    [[nodiscard]] const_iterator end() const noexcept;
    friend bool operator==(const LogicalGlyphRun& left, const LogicalGlyphRun& right) noexcept;

  private:
    std::shared_ptr<const std::vector<LogicalGlyph>> glyphs_;
};

struct MaterialParameter final {
    std::string name;
    runtime::Value value;
    [[nodiscard]] friend bool operator==(const MaterialParameter&,
                                         const MaterialParameter&) = default;
};

struct MaterialState final {
    std::string id;
    std::string blend_mode = "straight_alpha";
    double opacity = 1.0;
    std::vector<MaterialParameter> parameters{};
    [[nodiscard]] friend bool operator==(const MaterialState&, const MaterialState&) = default;
};

enum class EffectInput : std::uint32_t {
    backdrop = 0U,
    content = 1U,
    shape = 2U,
};

enum class EffectBackdropSource : std::uint32_t {
    current = 0U,
    surface = 1U,
};

/**
 * Refresh rate of an effect that requests none: zero, which means evaluated every frame. A
 * retained sample is reused only when the display outpaces the requested rate, so a default cap
 * would make every effect pay for the cache while rarely hitting it.
 */
inline constexpr double default_effect_refresh_rate = 0.0;

struct EffectState final {
    std::string id;
    EffectInput input = EffectInput::backdrop;
    EffectBackdropSource backdrop_source = EffectBackdropSource::current;
    double opacity = 1.0;
    double refresh_rate = default_effect_refresh_rate;
    bool refresh_rate_valid = true;
    std::vector<MaterialParameter> parameters{};
    std::array<double, 16U> packed_parameters{};
    std::uint32_t packed_parameter_count = 0U;
    /**
     * Nonzero asks the host to measure the backdrop this effect captures and report it under this
     * token (Surface::report_backdrop). Only BACKDROP effects carry one.
     */
    std::uint64_t probe = 0U;
    /**
     * How far outside its bounds, in logical pixels, the effect's output depends on what was drawn
     * before it, from its declared passes (see effect_read_reach). Hosts retain an effect's sample
     * only while nothing within this reach changes.
     */
    double read_reach = 0.0;
    [[nodiscard]] friend bool operator==(const EffectState&, const EffectState&) = default;
};

struct SolidRectRenderCommand final {
    Rect bounds;
    Paint fill;
    [[nodiscard]] friend bool operator==(const SolidRectRenderCommand&,
                                         const SolidRectRenderCommand&) = default;
};
struct RoundedRectRenderCommand final {
    Rect bounds;
    CornerRadii radii;
    Paint fill;
    std::optional<RenderBorder> border;
    double softness = 1.0;
    [[nodiscard]] friend bool operator==(const RoundedRectRenderCommand&,
                                         const RoundedRectRenderCommand&) = default;
};
struct BorderRenderCommand final {
    Rect bounds;
    RenderBorder border;
    CornerRadii radii;
    [[nodiscard]] friend bool operator==(const BorderRenderCommand&,
                                         const BorderRenderCommand&) = default;
};
struct ImageRenderCommand final {
    Rect bounds;
    std::string texture;
    TextureRegion source;
    RenderColor tint{255U, 255U, 255U, 255U};
    [[nodiscard]] friend bool operator==(const ImageRenderCommand&,
                                         const ImageRenderCommand&) = default;
};
struct NinePatchRenderCommand final {
    Rect bounds;
    std::string texture;
    TextureRegion source;
    Edges source_insets;
    Edges destination_insets;
    RenderColor tint{255U, 255U, 255U, 255U};
    [[nodiscard]] friend bool operator==(const NinePatchRenderCommand&,
                                         const NinePatchRenderCommand&) = default;
};
struct TextRunRenderCommand final {
    Point origin;
    RenderColor color;
    double pixel_size = 12.0;
    LogicalGlyphRun glyphs;
    FontRasterization font_rasterization = FontRasterization::grayscale;
    /** Conservative local-space bounds used to reject clipped runs before atlas warmup. */
    std::optional<Rect> cull_bounds = std::nullopt;
    [[nodiscard]] friend bool operator==(const TextRunRenderCommand&,
                                         const TextRunRenderCommand&) = default;
};

/** One portable custom-mesh vertex. x/y are normalized to the command bounds. */
struct MeshVertex final {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double u = 0.0;
    double v = 0.0;
    RenderColor color{255U, 255U, 255U, 255U};
    [[nodiscard]] friend bool operator==(const MeshVertex&, const MeshVertex&) = default;
};

struct MeshGeometry final {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    [[nodiscard]] friend bool operator==(const MeshGeometry&, const MeshGeometry&) = default;
};

struct CustomMeshRenderCommand final {
    Rect bounds;
    std::string mesh;
    MeshGeometry geometry;
    std::optional<std::string> texture;
    std::optional<MaterialState> material;
    double opacity = 1.0;
    [[nodiscard]] friend bool operator==(const CustomMeshRenderCommand&,
                                         const CustomMeshRenderCommand&) = default;
};
/** One authored vector shape: an outline plus how it is filled, stroked, or both. */
/**
 * An outline a GPU program generates (WidgetRenderScope::art): HLSL defining
 * `float2 artPoint(uint index, uint count)` in the shape's own 0-1 space, how many points it has,
 * whether the last joins the first (or the points pair off into lines of their own,
 * `segments`), and the sixteen parameters `artParameter(index)` reads. The
 * backend draws it again each frame with `artTime()`, so a picture that moves costs no CPU work.
 * It is decorative and GPU-only: hit testing sees only its bounds, and the software renderer
 * leaves it out.
 */
struct ArtOutline final {
    std::string source;
    std::uint32_t count = 0U;
    bool closed = false;
    bool segments = false;
    std::array<float, 16U> parameters{};
    [[nodiscard]] friend bool operator==(const ArtOutline&, const ArtOutline&) = default;
};

struct PathRenderCommand final {
    Rect bounds;
    PathShape shape;
    /**
     * Further shapes in the same frame, drawn after `shape` as one picture with it: where their
     * edges meet, each pixel is split between them by area, so nothing behind shows through the
     * seam (WidgetRenderScope::shapes joins shapes that share an edge).
     */
    std::vector<PathShape> joined;
    /** Art: the outline comes from this program rather than `shape`'s path. */
    std::optional<ArtOutline> art;
    [[nodiscard]] friend bool operator==(const PathRenderCommand&,
                                         const PathRenderCommand&) = default;
};
struct BlurRegionRenderCommand final {
    Rect bounds;
    double radius = 0.0;
    std::size_t downsample = 1U;
    [[nodiscard]] friend bool operator==(const BlurRegionRenderCommand&,
                                         const BlurRegionRenderCommand&) = default;
};
struct ShadowRenderCommand final {
    /** The shadow's silhouette: the source bounds moved by `offset`. */
    Rect bounds;
    CornerRadii radii;
    RenderColor color;
    double radius = 0.0;
    double spread = 0.0;
    /** Where the shadow sits relative to its source, whose own silhouette stays uncovered. */
    Point offset{};
    [[nodiscard]] friend bool operator==(const ShadowRenderCommand&,
                                         const ShadowRenderCommand&) = default;
};
/** Filters the framebuffer already rendered behind this node. */
struct BackdropEffectRenderCommand final {
    Rect bounds;
    CornerRadii radii;
    EffectState effect;
    [[nodiscard]] friend bool operator==(const BackdropEffectRenderCommand&,
                                         const BackdropEffectRenderCommand&) = default;
};
/** Begins an isolated subtree whose pixels are filtered when the matching end is reached. */
struct ContentEffectPushRenderCommand final {
    Rect bounds;
    CornerRadii radii;
    EffectState effect;
    [[nodiscard]] friend bool operator==(const ContentEffectPushRenderCommand&,
                                         const ContentEffectPushRenderCommand&) = default;
};
struct ContentEffectPopRenderCommand final {
    [[nodiscard]] friend bool operator==(ContentEffectPopRenderCommand,
                                         ContentEffectPopRenderCommand) = default;
};
struct ClipPushRenderCommand final {
    Rect rect;
    CornerRadii radii;
    [[nodiscard]] friend bool operator==(const ClipPushRenderCommand&,
                                         const ClipPushRenderCommand&) = default;
};
struct ClipPopRenderCommand final {
    [[nodiscard]] friend bool operator==(ClipPopRenderCommand, ClipPopRenderCommand) = default;
};
struct TransformPushRenderCommand final {
    double m00 = 1.0;
    double m01 = 0.0;
    double m02 = 0.0;
    double m10 = 0.0;
    double m11 = 1.0;
    double m12 = 0.0;
    [[nodiscard]] friend bool operator==(const TransformPushRenderCommand&,
                                         const TransformPushRenderCommand&) = default;
};
struct TransformPopRenderCommand final {
    [[nodiscard]] friend bool operator==(TransformPopRenderCommand,
                                         TransformPopRenderCommand) = default;
};
struct MaterialPushRenderCommand final {
    MaterialState material;
    [[nodiscard]] friend bool operator==(const MaterialPushRenderCommand&,
                                         const MaterialPushRenderCommand&) = default;
};
struct MaterialPopRenderCommand final {
    [[nodiscard]] friend bool operator==(MaterialPopRenderCommand,
                                         MaterialPopRenderCommand) = default;
};
/**
 * Multiplies the opacity of everything drawn in its scope. The scoped commands keep their own
 * colours, so a fade leaves their prepared geometry reusable and only the opacity changes.
 */
struct OpacityPushRenderCommand final {
    double opacity = 1.0;
    [[nodiscard]] friend bool operator==(const OpacityPushRenderCommand&,
                                         const OpacityPushRenderCommand&) = default;
};
struct OpacityPopRenderCommand final {
    [[nodiscard]] friend bool operator==(OpacityPopRenderCommand,
                                         OpacityPopRenderCommand) = default;
};
/**
 * A presentation group: everything in scope is encoded in layout space and placed by the group's
 * per-frame transform and opacity (RenderCommandBuffer::groups), applied on the GPU. Animating a
 * group therefore changes only its table entry, never the scoped commands or their geometry.
 */
struct GroupPushRenderCommand final {
    std::uint32_t group = 0U;
    [[nodiscard]] friend bool operator==(GroupPushRenderCommand,
                                         GroupPushRenderCommand) = default;
};
struct GroupPopRenderCommand final {
    [[nodiscard]] friend bool operator==(GroupPopRenderCommand, GroupPopRenderCommand) = default;
};
/** Stable 1-based group indices; zero means "no group" (identity). */
inline constexpr std::uint32_t maximum_render_groups = 511U;
/** A group's affine transform and opacity relative to its parent group. */
struct RenderGroup final {
    std::uint32_t index = 0U;
    std::uint32_t parent = 0U;
    double scale_x = 1.0;
    double scale_y = 1.0;
    double translate_x = 0.0;
    double translate_y = 0.0;
    double opacity = 1.0;
    /** The group's tone in [0, 1]; negative inherits the parent group's tone. */
    double tone = -1.0;
    /** The off-diagonal terms of a rotated group: x' += shear_x * y and y' += shear_y * x. */
    double shear_x = 0.0;
    double shear_y = 0.0;
    [[nodiscard]] friend bool operator==(const RenderGroup&, const RenderGroup&) = default;
};

using RenderCommand =
    std::variant<SolidRectRenderCommand, RoundedRectRenderCommand, BorderRenderCommand,
                 ImageRenderCommand, NinePatchRenderCommand, TextRunRenderCommand,
                 CustomMeshRenderCommand, PathRenderCommand, BlurRegionRenderCommand,
                 ShadowRenderCommand, BackdropEffectRenderCommand, ContentEffectPushRenderCommand,
                 ContentEffectPopRenderCommand, ClipPushRenderCommand, ClipPopRenderCommand,
                 TransformPushRenderCommand, TransformPopRenderCommand, MaterialPushRenderCommand,
                 MaterialPopRenderCommand, OpacityPushRenderCommand, OpacityPopRenderCommand,
                 GroupPushRenderCommand, GroupPopRenderCommand>;

/** Returns a backend-independent command with every opacity-bearing payload multiplied. */
[[nodiscard]] RenderCommand render_command_with_opacity(RenderCommand command, double opacity);

class RenderCommandBuffer final {
  public:
    void append(RenderCommand command);
    void append(const std::vector<RenderCommand>& commands);
    void clear() noexcept;
    [[nodiscard]] const std::vector<RenderCommand>& commands() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    /**
     * Whether both buffers view one stream. Storage is copy-on-write, so shared storage means
     * equal commands without comparing them.
     */
    [[nodiscard]] bool shares_storage(const RenderCommandBuffer& other) const noexcept {
        return commands_ == other.commands_;
    }
    /** This frame's presentation groups, ordered by index. Not part of command equality. */
    void set_groups(std::vector<RenderGroup> groups);
    [[nodiscard]] const std::vector<RenderGroup>& groups() const noexcept;

  private:
    void make_unique_storage();

    /**
     * Command streams are immutable once handed to submission planning. Copy-on-write storage
     * lets the renderer, its settled-frame cache, and the packet cache retain the same stream
     * without cloning every variant, string, and glyph run at each ownership boundary.
     */
    std::shared_ptr<std::vector<RenderCommand>> commands_ =
        std::make_shared<std::vector<RenderCommand>>();
    std::shared_ptr<const std::vector<RenderGroup>> groups_;
    /** Length of the stream last released to a retainer; sizes the next stream's storage. */
    std::size_t reserve_hint_ = 0U;
};

struct RenderOperationCounters final {
    std::size_t commands_emitted = 0U;
    std::size_t fragments_built = 0U;
    std::size_t fragments_reused = 0U;
    std::size_t nodes_visited = 0U;
    std::size_t overlays_rendered = 0U;
    std::size_t portals_rendered = 0U;
    std::size_t retained_subtrees_reused = 0U;
    std::size_t retained_subtrees_translated = 0U;
    [[nodiscard]] friend bool operator==(const RenderOperationCounters&,
                                         const RenderOperationCounters&) = default;
};

struct RenderGenerationToken final {
    std::uint64_t scale_context = 0U;
    std::uint64_t runtime_unit = 0U;
    [[nodiscard]] friend bool operator==(const RenderGenerationToken&,
                                         const RenderGenerationToken&) = default;
};

[[nodiscard]] data::JsonValue render_command_json(const RenderCommand& command);
[[nodiscard]] data::JsonValue render_commands_json(const RenderCommandBuffer& commands);

/** Retained fragment planner producing one backend-independent command stream per surface frame. */
class RenderEngine final {
  public:
    RenderEngine();
    ~RenderEngine();
    RenderEngine(const RenderEngine&) = delete;
    RenderEngine& operator=(const RenderEngine&) = delete;

    [[nodiscard]] RenderOperationCounters
    render(const RetainedTree& tree, const LayoutResult& layout, const InputRouter& input,
           const CommandIndex& commands, const WidgetRegistry& widgets,
           const BehaviorRegistry& behaviors, const MotionRuntime& motion, const TextEngine* text,
           const resource::SvgImageRegistry* svg_images, const MaterialRegistry& materials,
           const RenderGenerationToken& generations, RenderCommandBuffer& output);
    /** Captures retained widget fragments for a drag preview before the next frame mutates them. */
    [[nodiscard]] std::vector<RenderCommand> snapshot_subtree(const RetainedNode& node) const;
    void clear();

  private:
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace strata::ui
