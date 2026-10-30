#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
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

/**
 * A border stroked inside its shape's edge. Its paint is any paint a fill takes: a colour, or a
 * linear, radial or conic gradient laid over the bordered shape's own bounds, so a ring of colour
 * follows the shape's rounded corners.
 */
struct RenderBorder final {
    double width = 0.0;
    Paint paint;
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

/**
 * How an isolated layer (a CONTENT effect) combines with what is already drawn under it, once its
 * filters and passes have run. `source_over` is ordinary compositing. The blend modes from
 * `multiply` to `luminosity` are the W3C compositing blend modes, composited source-over. The
 * destination operators keep what is under the layer, scaled by the layer's alpha or by the
 * luminance of its premultiplied colour (inverted for `_out`), and draw nothing of the layer
 * itself: a matte. The channel operators combine premultiplied colour channel by channel,
 * alpha included (a coverage mask's lighten, darken and difference). `plus` adds
 * premultiplied colour, clamped (plus-lighter, After Effects' Add).
 */
enum class LayerComposite : std::uint32_t {
    source_over = 0U,
    multiply = 1U,
    screen = 2U,
    overlay = 3U,
    darken = 4U,
    lighten = 5U,
    color_dodge = 6U,
    color_burn = 7U,
    hard_light = 8U,
    soft_light = 9U,
    difference = 10U,
    exclusion = 11U,
    hue = 12U,
    saturation = 13U,
    color = 14U,
    luminosity = 15U,
    destination_in = 16U,
    destination_out = 17U,
    destination_in_luminance = 18U,
    destination_out_luminance = 19U,
    channel_maximum = 20U,
    channel_minimum = 21U,
    channel_difference = 22U,
    plus = 23U,
};
inline constexpr std::uint32_t layer_composite_count = 24U;

/**
 * One built-in filter an isolated layer runs over its pixels, in order, before it composites.
 * Colours are straight (unpremultiplied) 0-1 channels; lengths are logical pixels in the space the
 * layer is drawn in.
 *  - `blur` {radius x, radius y}: a Gaussian blur reaching that far each way, as BlurRegion's
 *    radius does (sigma a third of it); either may be zero.
 *  - `color_matrix` {20}: each of red, green, blue and alpha becomes a row of five weights
 *    applied to (r, g, b, a, 1).
 *  - `gradient_map` {mix, count, then count stops of offset, r, g, b}: the colour's Rec. 709
 *    luminance picks a colour from the stops (up to five), mixed into the colour by `mix`.
 *  - `levels` {20}: per channel (red, green, blue, alpha) input black, input white, gamma, output
 *    black and output white: (x - in black) / (in white - in black), clamped, raised to 1/gamma,
 *    mapped onto output black to white.
 *  - `drop_shadow` {r, g, b, opacity, offset x, offset y, radius, shadow only}: the layer's alpha
 *    in that colour, blurred by radius and moved by the offset, drawn under the layer (or alone).
 */
enum class LayerFilterKind : std::uint32_t {
    blur = 1U,
    color_matrix = 2U,
    gradient_map = 3U,
    levels = 4U,
    drop_shadow = 5U,
};
/** The most values one filter carries, and the most filters one layer runs. */
inline constexpr std::size_t maximum_layer_filter_values = 24U;
inline constexpr std::size_t maximum_layer_filters = 8U;

struct LayerFilter final {
    LayerFilterKind kind = LayerFilterKind::blur;
    std::vector<double> values;
    [[nodiscard]] friend bool operator==(const LayerFilter&, const LayerFilter&) = default;
};

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
    /**
     * A CONTENT effect's built-in processing: the filters its pixels run through after its
     * declared passes, and how it then composites. A layer that composites other than
     * source-over is evaluated every frame (its refresh rate is ignored).
     */
    LayerComposite composite = LayerComposite::source_over;
    std::vector<LayerFilter> filters{};
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
/**
 * What a run of text adds to its glyphs: a gradient across it (`gradient`, two or more colours,
 * left to right, in place of its colour), an outline around each glyph, a glow beyond it, and a
 * shadow under it. Outline, glow and shadow read the glyphs' distance field, so a run with any of
 * them rasterizes as MSDF; each reaches about as far as that field does.
 */
struct TextEffects final {
    std::vector<RenderColor> gradient;
    double outline_width = 0.0;
    RenderColor outline_color{0U, 0U, 0U, 0U};
    double glow_radius = 0.0;
    RenderColor glow_color{0U, 0U, 0U, 0U};
    double shadow_x = 0.0;
    double shadow_y = 0.0;
    double shadow_blur = 0.0;
    RenderColor shadow_color{0U, 0U, 0U, 0U};
    [[nodiscard]] bool shadowed() const noexcept {
        return shadow_color.alpha != 0U;
    }
    /** Whether it needs the glyphs' distance field: an outline, a glow or a shadow. */
    [[nodiscard]] bool distance() const noexcept {
        return (outline_width > 0.0 && outline_color.alpha != 0U) ||
               (glow_radius > 0.0 && glow_color.alpha != 0U) || shadowed();
    }
    [[nodiscard]] friend bool operator==(const TextEffects&, const TextEffects&) = default;
};

struct TextRunRenderCommand final {
    Point origin;
    RenderColor color;
    double pixel_size = 12.0;
    LogicalGlyphRun glyphs;
    FontRasterization font_rasterization = FontRasterization::grayscale;
    /** Conservative local-space bounds used to reject clipped runs before atlas warmup. */
    std::optional<Rect> cull_bounds = std::nullopt;
    TextEffects effects{};
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
/**
 * The shape a clip cuts and how its edges fade. `kind` 0 is the rounded rectangle its radii give;
 * 1 an ellipse filling the rectangle; 2 a regular polygon of `sides` sides and 3 a star of
 * `sides` points, both filling it. `fade` (top, right, bottom, left, in logical pixels) ramps the
 * content out toward each edge: a list that fades at the top and bottom as it scrolls.
 */
/**
 * A clip's outline: 0 a rounded box, 1 an ellipse filling it, 2 a regular polygon and 3 a star of
 * `sides`, 4 a sector or ring of the ellipse filling it, whose start and sweep (degrees, clockwise
 * from right) and inner radius (a fraction of the outer) ride in the clip's corner radii.
 */
struct ClipShape final {
    std::uint32_t kind = 0U;
    double sides = 0.0;
    std::array<double, 4U> fade{};
    [[nodiscard]] bool plain() const noexcept {
        return kind == 0U && fade[0] <= 0.0 && fade[1] <= 0.0 && fade[2] <= 0.0 && fade[3] <= 0.0;
    }
    [[nodiscard]] friend bool operator==(const ClipShape&, const ClipShape&) = default;
};

struct ClipPushRenderCommand final {
    Rect rect;
    CornerRadii radii;
    ClipShape shape{};
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
    /** A group turned in depth divides by w = project_x * x + project_y * y + project_w. */
    double project_x = 0.0;
    double project_y = 0.0;
    double project_w = 1.0;
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

/**
 * A command stream as runs of shared, immutable storage: what a frame drew anew, and runs of
 * earlier frames' storage it reuses as they were (a retained subtree), so reusing drawing never
 * copies it. Read as a vector: by index, in order, and compared.
 */
class RenderCommandView final {
  public:
    struct Segment final {
        std::shared_ptr<const std::vector<RenderCommand>> storage;
        std::size_t begin = 0U;
        std::size_t end = 0U;
        [[nodiscard]] std::size_t size() const noexcept {
            return end - begin;
        }
        [[nodiscard]] friend bool operator==(const Segment&, const Segment&) = default;
    };

    class const_iterator final {
      public:
        using iterator_category = std::random_access_iterator_tag;
        using iterator_concept = std::random_access_iterator_tag;
        using value_type = RenderCommand;
        using difference_type = std::ptrdiff_t;
        using pointer = const RenderCommand*;
        using reference = const RenderCommand&;

        const_iterator() = default;
        const_iterator(const RenderCommandView* view, const std::size_t index) noexcept
            : view_(view), index_(index) {}

        [[nodiscard]] reference operator*() const noexcept {
            return view_->locate(index_, run_);
        }
        [[nodiscard]] pointer operator->() const noexcept {
            return &view_->locate(index_, run_);
        }
        [[nodiscard]] reference operator[](const difference_type offset) const noexcept {
            return (*view_)[static_cast<std::size_t>(static_cast<difference_type>(index_) + offset)];
        }
        const_iterator& operator++() noexcept {
            ++index_;
            return *this;
        }
        const_iterator operator++(int) noexcept {
            const_iterator previous = *this;
            ++index_;
            return previous;
        }
        const_iterator& operator--() noexcept {
            --index_;
            return *this;
        }
        const_iterator operator--(int) noexcept {
            const_iterator previous = *this;
            --index_;
            return previous;
        }
        const_iterator& operator+=(const difference_type offset) noexcept {
            index_ = static_cast<std::size_t>(static_cast<difference_type>(index_) + offset);
            return *this;
        }
        const_iterator& operator-=(const difference_type offset) noexcept {
            return *this += -offset;
        }
        [[nodiscard]] friend const_iterator operator+(const_iterator at,
                                                      const difference_type offset) noexcept {
            return at += offset;
        }
        [[nodiscard]] friend const_iterator operator+(const difference_type offset,
                                                      const_iterator at) noexcept {
            return at += offset;
        }
        [[nodiscard]] friend const_iterator operator-(const_iterator at,
                                                      const difference_type offset) noexcept {
            return at -= offset;
        }
        [[nodiscard]] friend difference_type operator-(const const_iterator& left,
                                                       const const_iterator& right) noexcept {
            return static_cast<difference_type>(left.index_) -
                   static_cast<difference_type>(right.index_);
        }
        [[nodiscard]] friend bool operator==(const const_iterator& left,
                                             const const_iterator& right) noexcept {
            return left.index_ == right.index_;
        }
        [[nodiscard]] friend auto operator<=>(const const_iterator& left,
                                              const const_iterator& right) noexcept {
            return left.index_ <=> right.index_;
        }

      private:
        const RenderCommandView* view_ = nullptr;
        std::size_t index_ = 0U;
        /** The run the last read found: iteration walks the runs in order. */
        mutable std::size_t run_ = 0U;
    };
    using iterator = const_iterator;
    using value_type = RenderCommand;
    using size_type = std::size_t;

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0U;
    }
    /** The command at `index`: a search of the runs. */
    [[nodiscard]] const RenderCommand& operator[](std::size_t index) const noexcept {
        std::size_t run = 0U;
        return locate(index, run);
    }
    [[nodiscard]] const RenderCommand& front() const noexcept {
        return (*this)[0U];
    }
    [[nodiscard]] const RenderCommand& back() const noexcept {
        return (*this)[size_ - 1U];
    }
    [[nodiscard]] const_iterator begin() const noexcept {
        return const_iterator(this, 0U);
    }
    [[nodiscard]] const_iterator end() const noexcept {
        return const_iterator(this, size_);
    }
    [[nodiscard]] const std::vector<Segment>& segments() const noexcept {
        return segments_;
    }
    /** Commands `begin` to `end` as the runs of storage they are in. */
    [[nodiscard]] std::vector<Segment> slice(std::size_t begin, std::size_t end) const;
    [[nodiscard]] std::vector<RenderCommand> to_vector() const;
    /**
     * The indices whose commands differ between two streams of one length, in order: runs both
     * read from the same storage at the same place are equal without comparing them.
     */
    [[nodiscard]] std::vector<std::size_t> differences(const RenderCommandView& other) const;

    friend bool operator==(const RenderCommandView& left, const RenderCommandView& right);
    friend bool operator==(const RenderCommandView& left, const std::vector<RenderCommand>& right);

  private:
    friend class RenderCommandBuffer;
    void push(Segment segment);
    /** The command at `index`, looking in `run` and the one after it before searching. */
    [[nodiscard]] const RenderCommand& locate(std::size_t index, std::size_t& run) const noexcept;

    std::vector<Segment> segments_;
    /** Each run's first index in the stream. */
    std::vector<std::size_t> offsets_;
    std::size_t size_ = 0U;
};

class RenderCommandBuffer final {
  public:
    RenderCommandBuffer() = default;
    /** A copy reads the same runs; it appends into storage of its own, never into these. */
    RenderCommandBuffer(const RenderCommandBuffer& other);
    RenderCommandBuffer& operator=(const RenderCommandBuffer& other);
    RenderCommandBuffer(RenderCommandBuffer&&) noexcept = default;
    RenderCommandBuffer& operator=(RenderCommandBuffer&&) noexcept = default;

    void append(RenderCommand command);
    void append(const std::vector<RenderCommand>& commands);
    /** Runs of commands drawn before, read where they are: nothing is copied. */
    void append(std::span<const RenderCommandView::Segment> segments);
    void clear() noexcept;
    [[nodiscard]] const RenderCommandView& commands() const noexcept {
        return view_;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return view_.size();
    }
    /** Whether both buffers read the same runs: equal commands without comparing them. */
    [[nodiscard]] bool shares_storage(const RenderCommandBuffer& other) const noexcept {
        return view_.segments_ == other.view_.segments_;
    }
    /** This frame's presentation groups, ordered by index. Not part of command equality. */
    void set_groups(std::vector<RenderGroup> groups);
    [[nodiscard]] const std::vector<RenderGroup>& groups() const noexcept;

  private:
    /** Makes the stream's last run one this buffer may append to. */
    void open_tail();

    RenderCommandView view_;
    /**
     * The storage this buffer appends to. Only this buffer appends, and only past what any run
     * reads, so every run anyone holds stays as it was.
     */
    std::shared_ptr<std::vector<RenderCommand>> tail_;
    /** The last run is `tail_`'s, ending where it does. */
    bool tail_open_ = false;
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
