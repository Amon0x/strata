#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace strata::resource::lottie {

/**
 * A Lottie (Bodymovin) animation, parsed once into typed properties so a frame is evaluated by
 * arithmetic alone: no JSON is read after parsing. Coordinates are the composition's own units
 * (y down), times are frames, colours are 0-1 channels and percentages stay percentages, exactly
 * as After Effects exports them. Expressions, text, 3D, effects, blend modes and image layers are
 * not part of the model: their layers are skipped.
 */
struct Vec2 final {
    double x = 0.0;
    double y = 0.0;
    [[nodiscard]] friend bool operator==(const Vec2&, const Vec2&) = default;
};

/**
 * One cubic Bezier outline as Lottie stores it: vertices, and each vertex's in and out tangent
 * relative to it. The segment from vertex k to vertex k+1 runs through v[k] + out[k] and
 * v[k+1] + in[k+1]; a closed outline also runs from the last vertex back to the first.
 */
struct BezierShape final {
    std::vector<Vec2> vertices;
    std::vector<Vec2> in_tangents;
    std::vector<Vec2> out_tangents;
    bool closed = false;
    [[nodiscard]] friend bool operator==(const BezierShape&, const BezierShape&) = default;
};

/**
 * One side of a keyframe's temporal easing: the control point of the unit cubic Bezier from (0, 0)
 * to (1, 1), per dimension (one entry applies to every dimension).
 */
struct EaseHandle final {
    std::vector<double> x;
    std::vector<double> y;
};

/** The eased progress through a keyframe segment at linear progress `t`: y of the curve at x = t.
 */
[[nodiscard]] double cubic_ease(double out_x, double out_y, double in_x, double in_y,
                                double t) noexcept;

/**
 * A keyframe and the segment it starts. `out` and `in` are the segment's easing (Lottie stores
 * both on the segment's first keyframe); a hold keeps `start` until the next keyframe. A spatial
 * segment (a position whose `to`/`ti` tangents are not zero) moves along the cubic from `start`
 * through `start + spatial_out` and `end + spatial_in` to `end`, at a speed even along its arc.
 */
struct Keyframe final {
    double time = 0.0;
    std::vector<double> start;
    std::vector<double> end;
    bool hold = false;
    EaseHandle out;
    EaseHandle in;
    bool spatial = false;
    Vec2 spatial_out;
    Vec2 spatial_in;
    /** A spatial segment's arc length at evenly spaced curve parameters, from 0. */
    std::vector<double> arc_lengths;
};

/** A property of a fixed number of components, constant or keyframed. */
class Property final {
  public:
    Property() = default;
    explicit Property(std::vector<double> constant);

    [[nodiscard]] bool animated() const noexcept {
        return !keyframes_.empty();
    }
    [[nodiscard]] std::size_t dimension() const noexcept {
        return constant_.size();
    }
    /** Writes the value at `frame` into `out`, whose size is the dimension. */
    void evaluate(double frame, std::span<double> out) const noexcept;
    [[nodiscard]] double scalar(double frame) const noexcept;
    [[nodiscard]] Vec2 point(double frame) const noexcept;
    /** The value at `frame`, sized as its keyframes are (a gradient's stops may vary). */
    [[nodiscard]] std::vector<double> values(double frame) const;

    std::vector<double> constant_;
    std::vector<Keyframe> keyframes_;
};

struct ShapeKeyframe final {
    double time = 0.0;
    BezierShape start;
    BezierShape end;
    bool hold = false;
    EaseHandle out;
    EaseHandle in;
};

/** An outline, constant or keyframed vertex by vertex. */
class ShapeProperty final {
  public:
    [[nodiscard]] BezierShape evaluate(double frame) const;
    [[nodiscard]] bool animated() const noexcept {
        return !keyframes_.empty();
    }

    BezierShape constant_;
    std::vector<ShapeKeyframe> keyframes_;
};

/**
 * A layer's or group's transform. A missing property is its identity value. Position may be
 * authored as one point or as separate x and y properties.
 */
struct Transform final {
    Property anchor{{0.0, 0.0}};
    Property position{{0.0, 0.0}};
    bool split_position = false;
    Property position_x{{0.0}};
    Property position_y{{0.0}};
    Property scale{{100.0, 100.0}};
    Property rotation{{0.0}};
    Property opacity{{100.0}};
    Property skew{{0.0}};
    Property skew_axis{{0.0}};
    /** A repeater's per-copy opacities, in percent. */
    Property start_opacity{{100.0}};
    Property end_opacity{{100.0}};

    [[nodiscard]] Vec2 position_at(double frame) const noexcept;
};

/** Path direction: 3 reverses a parametric shape (it matters to trims and to winding). */
inline constexpr int reversed_direction = 3;

struct PathItem final {
    ShapeProperty shape;
    int direction = 1;
};
struct RectItem final {
    Property position{{0.0, 0.0}};
    Property size{{0.0, 0.0}};
    Property roundness{{0.0}};
    int direction = 1;
};
struct EllipseItem final {
    Property position{{0.0, 0.0}};
    Property size{{0.0, 0.0}};
    int direction = 1;
};
/** A star (`star` true) or polygon. Roundness is a percentage. */
struct PolystarItem final {
    bool star = true;
    Property position{{0.0, 0.0}};
    Property points{{5.0}};
    Property rotation{{0.0}};
    Property outer_radius{{0.0}};
    Property inner_radius{{0.0}};
    Property outer_roundness{{0.0}};
    Property inner_roundness{{0.0}};
    int direction = 1;
};

enum class FillRule { nonzero, even_odd };
enum class LineCap { butt, round, square };
enum class LineJoin { miter, round, bevel };

/** A dash pattern entry: a dash, a gap, or the pattern's offset. */
struct Dash final {
    enum class Kind { dash, gap, offset } kind = Kind::dash;
    Property length{{0.0}};
};

struct StrokeStyle final {
    Property width{{1.0}};
    LineCap cap = LineCap::butt;
    LineJoin join = LineJoin::miter;
    double miter_limit = 4.0;
    std::vector<Dash> dashes;
};

/** A fill or a stroke in one colour. */
struct SolidPaintItem final {
    bool stroke = false;
    Property color{{0.0, 0.0, 0.0, 1.0}};
    Property opacity{{100.0}};
    FillRule rule = FillRule::nonzero;
    StrokeStyle style;
};

/**
 * A gradient fill or stroke. `colors` holds `color_stops` stops of (offset, r, g, b), then any
 * opacity stops as (offset, alpha) pairs.
 */
struct GradientPaintItem final {
    bool stroke = false;
    bool radial = false;
    Property start{{0.0, 0.0}};
    Property end{{0.0, 0.0}};
    std::size_t color_stops = 0U;
    Property colors;
    Property opacity{{100.0}};
    FillRule rule = FillRule::nonzero;
    StrokeStyle style;
};

/** Trim paths: `start`, `end` in percent, `offset` in degrees of a full turn. */
struct TrimItem final {
    Property start{{0.0}};
    Property end{{100.0}};
    Property offset{{0.0}};
    /** Trim each path alone (simultaneously), or all of them as one path (individually). */
    bool individually = false;
};

struct RepeaterItem final {
    Property copies{{1.0}};
    Property offset{{0.0}};
    /** Later copies drawn above earlier ones (1) or below them (2). */
    bool above = true;
    Transform transform;
};

struct RoundCornersItem final {
    Property radius{{0.0}};
};

/** Merge paths: 1 merge, 2 add, 3 subtract, 4 intersect, 5 exclude intersections. */
struct MergeItem final {
    int mode = 1;
};

struct ShapeItem;

struct GroupItem final {
    std::vector<ShapeItem> items;
    Transform transform;
};

using ShapeContent =
    std::variant<GroupItem, PathItem, RectItem, EllipseItem, PolystarItem, SolidPaintItem,
                 GradientPaintItem, TrimItem, RepeaterItem, RoundCornersItem, MergeItem>;

struct ShapeItem final {
    ShapeContent content;
};

/** A mask: `mode` is Lottie's letter (a add, s subtract, i intersect, l, d, f, n none). */
struct Mask final {
    char mode = 'a';
    bool inverted = false;
    ShapeProperty path;
    Property opacity{{100.0}};
    Property expansion{{0.0}};
};

enum class LayerType { precomp, solid, image, null, shape, text, other };

/** How a layer uses the matte above it: 1 alpha, 2 alpha inverted, 3 luma, 4 luma inverted. */
enum class MatteMode { none = 0, alpha = 1, alpha_inverted = 2, luma = 3, luma_inverted = 4 };

struct Layer final {
    LayerType type = LayerType::other;
    std::optional<std::int64_t> index;
    std::optional<std::int64_t> parent;
    double in_frame = 0.0;
    double out_frame = 0.0;
    double start_time = 0.0;
    double stretch = 1.0;
    Transform transform;
    bool auto_orient = false;
    bool hidden = false;
    /** This layer is another's matte (`td`), drawn only through it. */
    bool matte_source = false;
    MatteMode matte = MatteMode::none;
    /** The matte layer's index (`tp`); absent, the matte is the layer just above. */
    std::optional<std::int64_t> matte_parent;
    std::vector<Mask> masks;
    /** Shape layers. */
    std::vector<ShapeItem> shapes;
    /** Precomposition layers: the asset (an index into Animation::precomps), size and remap. */
    std::optional<std::size_t> precomp;
    double width = 0.0;
    double height = 0.0;
    std::optional<Property> time_remap;
    /** Solid layers: colour (0-1 channels) and size. */
    std::array<double, 3> solid_color{};
};

struct Composition final {
    std::vector<Layer> layers;
};

/** A parsed animation: its composition, its frame range and rate, and its precompositions. */
struct Animation final {
    double width = 0.0;
    double height = 0.0;
    double frame_rate = 30.0;
    double in_frame = 0.0;
    double out_frame = 0.0;
    std::string name;
    Composition root;
    std::vector<Composition> precomps;
};

/** Parses a Lottie JSON document; throws std::invalid_argument describing what is wrong. */
[[nodiscard]] Animation parse(std::string_view json);

} // namespace strata::resource::lottie

namespace strata::resource {

/**
 * The surface's Lottie animations by resource id, each loaded through the host's resource loader
 * and parsed once, the first time anything asks for it. A failure is remembered too, so a missing
 * or malformed file is read only once. Safe to call from any thread.
 */
class LottieLibrary final {
  public:
    using Loader = std::function<std::vector<std::uint8_t>(std::string_view id)>;

    explicit LottieLibrary(Loader loader);

    /** The parsed animation, or null when it could not be loaded or parsed. */
    [[nodiscard]] std::shared_ptr<const lottie::Animation> find(std::string_view id) const;
    /** Why `find` gave null for `id`, or empty when it did not (or was not asked). */
    [[nodiscard]] std::string error(std::string_view id) const;

  private:
    struct Entry final {
        std::shared_ptr<const lottie::Animation> animation;
        std::string error;
    };
    Loader loader_;
    mutable std::mutex mutex_;
    mutable std::map<std::string, Entry, std::less<>> entries_;
};

} // namespace strata::resource
