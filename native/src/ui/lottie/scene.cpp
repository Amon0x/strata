#include "ui/lottie/scene.hpp"

#include "ui/lottie/geometry.hpp"
#include "ui/render/path_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <variant>

namespace strata::ui::lottie {
namespace {

namespace model = resource::lottie;

/** How far a flattened curve may stray from the true one, in logical pixels. */
constexpr double flatness = 0.08;
/** The deepest precomposition nesting a frame follows. */
constexpr std::size_t maximum_depth = 24U;

template <typename... Visitors> struct Overloaded : Visitors... {
    using Visitors::operator()...;
};
template <typename... Visitors> Overloaded(Visitors...) -> Overloaded<Visitors...>;

[[nodiscard]] Point point(const model::Vec2 value) noexcept {
    return Point{value.x, value.y};
}

[[nodiscard]] std::uint8_t channel(const double value) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

/**
 * A transform's matrix at `frame`: anchor, scale, skew, rotation and position, applied in that
 * order (After Effects' order), with `extra_rotation` degrees more (auto-orient).
 */
[[nodiscard]] Affine transform_matrix(const model::Transform& transform, const double frame,
                                      const double extra_rotation = 0.0) {
    const Point anchor = point(transform.anchor.point(frame));
    const Point position = point(transform.position_at(frame));
    const Point scale = point(transform.scale.point(frame));
    const double rotation = transform.rotation.scalar(frame) + extra_rotation;
    const double skew = transform.skew.scalar(frame);
    Affine result = Affine::translate(position.x, position.y) * Affine::rotate(rotation);
    if (skew != 0.0) {
        const double axis = transform.skew_axis.scalar(frame);
        const double shear = std::tan(-skew * std::numbers::pi / 180.0);
        result = result * Affine::rotate(-axis) * Affine{1.0, 0.0, shear, 1.0, 0.0, 0.0} *
                 Affine::rotate(axis);
    }
    return result * Affine::scale(scale.x / 100.0, scale.y / 100.0) *
           Affine::translate(-anchor.x, -anchor.y);
}

/** A gradient in the coordinates of the outline it paints, before it is normalized. */
struct LocalGradient final {
    bool radial = false;
    Point start;
    Point end;
    std::vector<GradientStop> stops;
};

/** One fill or stroke ready to draw: an outline in its own coordinates and how it maps out. */
struct Drawable final {
    Affine to_widget;
    CubicPath path;
    bool stroke = false;
    RenderColor color{0U, 0U, 0U, 0U};
    std::optional<LocalGradient> gradient;
    PathFillRule rule = PathFillRule::nonzero;
    StrokeStyle style;
};

/** A paint's single representative colour and alpha (a gradient's mean), 0-1 channels. */
struct Tone final {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double alpha = 0.0;
};

[[nodiscard]] Tone tone(const Drawable& drawable) noexcept {
    if (!drawable.gradient.has_value() || drawable.gradient->stops.empty()) {
        return Tone{drawable.color.red / 255.0, drawable.color.green / 255.0,
                    drawable.color.blue / 255.0, drawable.color.alpha / 255.0};
    }
    Tone mean;
    for (const GradientStop& stop : drawable.gradient->stops) {
        mean.red += stop.color.red / 255.0;
        mean.green += stop.color.green / 255.0;
        mean.blue += stop.color.blue / 255.0;
        mean.alpha += stop.color.alpha / 255.0;
    }
    const double count = static_cast<double>(drawable.gradient->stops.size());
    return Tone{mean.red / count, mean.green / count, mean.blue / count, mean.alpha / count};
}

/**
 * What a mask stack or a track matte lets through, as a function of which of its polygon sets a
 * point is inside. Its sets are in widget coordinates.
 */
struct Constraint final {
    enum class Kind { mask, alpha, alpha_inverted, luma, luma_inverted, inside };
    struct MaskTerm final {
        std::size_t path = 0U;
        std::optional<std::size_t> band;
        bool grow = false;
        char mode = 'a';
        bool inverted = false;
        double opacity = 1.0;
    };
    /** One matte shape, in paint order from the bottom: its set's alpha and colour. */
    struct MatteTerm final {
        double alpha = 1.0;
        double red = 0.0;
        double green = 0.0;
        double blue = 0.0;
    };
    Kind kind = Kind::mask;
    std::vector<PolygonSet> sets;
    std::vector<MaskTerm> masks;
    std::vector<MatteTerm> matte;
    bool start_full = false;

    [[nodiscard]] double evaluate(const std::span<const std::uint8_t> inside) const noexcept {
        switch (kind) {
        case Kind::inside:
            return inside.empty() || inside[0] != 0U ? 1.0 : 0.0;
        case Kind::mask: {
            double coverage = start_full ? 1.0 : 0.0;
            for (const MaskTerm& term : masks) {
                bool in = inside[term.path] != 0U;
                if (term.band.has_value()) {
                    const bool band = inside[*term.band] != 0U;
                    in = term.grow ? in || band : in && !band;
                }
                double value = in ? 1.0 : 0.0;
                if (term.inverted)
                    value = 1.0 - value;
                value *= term.opacity;
                switch (term.mode) {
                case 's':
                    coverage *= 1.0 - value;
                    break;
                case 'i':
                    coverage *= value;
                    break;
                case 'l':
                    coverage = std::max(coverage, value);
                    break;
                case 'd':
                    coverage = std::min(coverage, value);
                    break;
                case 'f':
                    coverage = std::abs(coverage - value);
                    break;
                default:
                    coverage = coverage + value - coverage * value;
                    break;
                }
            }
            return coverage;
        }
        default:
            break;
        }
        // A matte: its layer composited over nothing, then its alpha or luminance.
        double alpha = 0.0;
        double red = 0.0;
        double green = 0.0;
        double blue = 0.0;
        for (std::size_t index = 0U; index < matte.size(); ++index) {
            if (inside[index] == 0U)
                continue;
            const MatteTerm& term = matte[index];
            red = term.red * term.alpha + red * (1.0 - term.alpha);
            green = term.green * term.alpha + green * (1.0 - term.alpha);
            blue = term.blue * term.alpha + blue * (1.0 - term.alpha);
            alpha = term.alpha + alpha * (1.0 - term.alpha);
        }
        const double luma = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
        switch (kind) {
        case Kind::alpha:
            return alpha;
        case Kind::alpha_inverted:
            return 1.0 - alpha;
        case Kind::luma:
            return luma;
        case Kind::luma_inverted:
            return 1.0 - luma;
        default:
            return 1.0;
        }
    }
};

using Constraints = std::vector<const Constraint*>;

[[nodiscard]] double local_tolerance(const Affine& to_widget) noexcept {
    const double scale = to_widget.mean_scale();
    return scale > 1.0e-9 ? flatness / scale : flatness;
}

[[nodiscard]] std::vector<std::vector<Point>> polygons(const std::vector<PathContour>& contours) {
    std::vector<std::vector<Point>> result;
    result.reserve(contours.size());
    for (const PathContour& contour : contours) {
        if (contour.points.size() >= 3U)
            result.push_back(contour.points);
    }
    return result;
}

/** What a drawable covers, as polygons in its own coordinates: a fill's area, a stroke's band. */
[[nodiscard]] PolygonSet coverage(const Drawable& drawable) {
    const double tolerance = local_tolerance(drawable.to_widget);
    PolygonSet result;
    if (!drawable.stroke) {
        result.polygons = polygons(flatten(drawable.path, tolerance));
        result.even_odd = drawable.rule == PathFillRule::evenodd;
        return result;
    }
    result.polygons =
        polygons(stroke_outline(flatten(drawable.path, tolerance), drawable.style, tolerance));
    return result;
}

void transform_set(PolygonSet& set, const Affine& map) {
    for (std::vector<Point>& polygon : set.polygons) {
        for (Point& vertex : polygon)
            vertex = map.apply(vertex);
    }
}

/**
 * The regions of a drawable's coverage the constraints let through, in its own coordinates,
 * grouped by how much they let through.
 */
[[nodiscard]] std::vector<SweepLevel> constrained(const Drawable& drawable,
                                                  const Constraints& constraints) {
    const std::optional<Affine> to_local = drawable.to_widget.inverse();
    if (!to_local.has_value())
        return {};
    std::vector<PolygonSet> sets;
    sets.push_back(coverage(drawable));
    std::vector<std::size_t> offsets;
    for (const Constraint* constraint : constraints) {
        offsets.push_back(sets.size());
        for (const PolygonSet& set : constraint->sets) {
            PolygonSet local = set;
            transform_set(local, *to_local);
            sets.push_back(std::move(local));
        }
    }
    return sweep(sets, [&](const std::span<const std::uint8_t> inside) {
        if (inside[0] == 0U)
            return 0.0;
        double value = 1.0;
        for (std::size_t index = 0U; index < constraints.size() && value > 0.0; ++index) {
            value *= constraints[index]->evaluate(
                inside.subspan(offsets[index], constraints[index]->sets.size()));
        }
        return value;
    });
}

/** The paint of a drawable for an outline drawn in `frame`, a square over its local bounds. */
[[nodiscard]] Paint paint(const Drawable& drawable, const Rect frame, const double scale_alpha) {
    if (!drawable.gradient.has_value()) {
        RenderColor color = drawable.color;
        color.alpha = static_cast<std::uint8_t>(std::lround(color.alpha * scale_alpha));
        return Paint(color);
    }
    const LocalGradient& local = *drawable.gradient;
    const auto normalized = [&frame](const Point value) {
        return Point{(value.x - frame.x) / frame.width, (value.y - frame.y) / frame.height};
    };
    Gradient gradient;
    gradient.stops = local.stops;
    for (GradientStop& stop : gradient.stops)
        stop.color.alpha = static_cast<std::uint8_t>(std::lround(stop.color.alpha * scale_alpha));
    const double reach = std::hypot(local.end.x - local.start.x, local.end.y - local.start.y);
    if (gradient.stops.size() < 2U || reach <= 1.0e-9)
        return Paint(gradient.stops.empty() ? RenderColor{0U, 0U, 0U, 0U}
                                            : gradient.stops.back().color);
    if (local.radial) {
        gradient.kind = GradientKind::radial;
        gradient.center = normalized(local.start);
        gradient.radius_x = reach / frame.width;
        gradient.radius_y = reach / frame.height;
    } else {
        gradient.kind = GradientKind::linear;
        gradient.start = normalized(local.start);
        gradient.end = normalized(local.end);
    }
    return Paint(std::move(gradient));
}

/**
 * Draws one outline (in local units) as a path command under `to_widget`: normalized into a
 * square over its control points, so a gradient laid out in that square keeps its true angles.
 */
void emit_path(std::vector<RenderCommand>& output, const Affine& to_widget, const Path& outline,
               const Drawable& drawable, const bool as_stroke, const double scale_alpha) {
    double left = std::numeric_limits<double>::infinity();
    double top = std::numeric_limits<double>::infinity();
    double right = -std::numeric_limits<double>::infinity();
    double bottom = -std::numeric_limits<double>::infinity();
    const auto include = [&](const Point value) {
        left = std::min(left, value.x);
        top = std::min(top, value.y);
        right = std::max(right, value.x);
        bottom = std::max(bottom, value.y);
    };
    for (const PathSegment& segment : outline.segments()) {
        if (segment.verb == PathVerb::close)
            continue;
        include(segment.to);
        if (segment.verb == PathVerb::quadratic || segment.verb == PathVerb::cubic)
            include(segment.control_a);
        if (segment.verb == PathVerb::cubic)
            include(segment.control_b);
    }
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) ||
        !std::isfinite(bottom)) {
        return;
    }
    if (as_stroke) {
        const double factor = drawable.style.join == PathJoin::miter
                                  ? std::max(drawable.style.miter_limit, 1.0)
                              : drawable.style.cap == PathCap::square ? std::numbers::sqrt2
                                                                      : 1.0;
        const double reach = drawable.style.width * 0.5 * factor;
        left -= reach;
        top -= reach;
        right += reach;
        bottom += reach;
    }
    const double side = std::max({right - left, bottom - top, 1.0e-3});
    const Rect frame{(left + right - side) * 0.5, (top + bottom - side) * 0.5, side, side};
    const auto normalized = [&frame](const Point value) {
        return Point{(value.x - frame.x) / frame.width, (value.y - frame.y) / frame.height};
    };
    Path local;
    for (const PathSegment& segment : outline.segments()) {
        switch (segment.verb) {
        case PathVerb::move:
            local.move_to(normalized(segment.to));
            break;
        case PathVerb::line:
            local.line_to(normalized(segment.to));
            break;
        case PathVerb::quadratic:
            local.quadratic_to(normalized(segment.control_a), normalized(segment.to));
            break;
        case PathVerb::cubic:
            local.cubic_to(normalized(segment.control_a), normalized(segment.control_b),
                           normalized(segment.to));
            break;
        case PathVerb::close:
            local.close();
            break;
        }
    }
    PathShape shape;
    shape.path = std::move(local);
    if (as_stroke) {
        shape.stroke = paint(drawable, frame, scale_alpha);
        shape.stroke_style = drawable.style;
    } else {
        shape.fill = paint(drawable, frame, scale_alpha);
        shape.fill_rule = drawable.stroke ? PathFillRule::nonzero : drawable.rule;
    }
    output.emplace_back(TransformPushRenderCommand{to_widget.a, to_widget.c, to_widget.e,
                                                   to_widget.b, to_widget.d, to_widget.f});
    output.emplace_back(PathRenderCommand{frame, std::move(shape)});
    output.emplace_back(TransformPopRenderCommand{});
}

[[nodiscard]] Path trapezoid_path(const std::span<const Trapezoid> region) {
    Path result;
    for (const Trapezoid& shape : region) {
        result.move_to(Point{shape.top_left, shape.top});
        result.line_to(Point{shape.top_right, shape.top});
        result.line_to(Point{shape.bottom_right, shape.bottom});
        result.line_to(Point{shape.bottom_left, shape.bottom});
        result.close();
    }
    return result;
}

/** Where a frame's drawing goes: render commands, or the shapes of a matte. */
class Sink {
  public:
    Sink() = default;
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    virtual ~Sink() = default;
    virtual void push_opacity(double opacity) = 0;
    virtual void pop_opacity() = 0;
    /** An axis-aligned clip in widget coordinates. */
    virtual void push_clip(Rect clip) = 0;
    virtual void pop_clip() = 0;
    virtual void draw(const Drawable& drawable, const Constraints& constraints) = 0;
};

class CommandSink final : public Sink {
  public:
    explicit CommandSink(std::vector<RenderCommand>& output) : output_(output) {}

    void push_opacity(const double opacity) override {
        output_.emplace_back(OpacityPushRenderCommand{opacity});
    }
    void pop_opacity() override {
        output_.emplace_back(OpacityPopRenderCommand{});
    }
    void push_clip(const Rect clip) override {
        output_.emplace_back(ClipPushRenderCommand{clip});
    }
    void pop_clip() override {
        output_.emplace_back(ClipPopRenderCommand{});
    }

    void draw(const Drawable& drawable, const Constraints& constraints) override {
        if (drawable.path.empty())
            return;
        if (constraints.empty()) {
            emit_path(output_, drawable.to_widget, to_path(drawable.path), drawable,
                      drawable.stroke, 1.0);
            return;
        }
        for (const SweepLevel& level : constrained(drawable, constraints)) {
            emit_path(output_, drawable.to_widget, trapezoid_path(level.trapezoids), drawable,
                      false, level.value);
        }
    }

  private:
    std::vector<RenderCommand>& output_;
};

/** Gathers what a matte layer draws into a matte constraint, one polygon set a paint. */
class MatteSink final : public Sink {
  public:
    explicit MatteSink(Constraint& matte) : matte_(matte) {}

    void push_opacity(const double opacity) override {
        opacity_.push_back((opacity_.empty() ? 1.0 : opacity_.back()) * opacity);
    }
    void pop_opacity() override {
        if (!opacity_.empty())
            opacity_.pop_back();
    }
    void push_clip(const Rect clip) override {
        Constraint inside;
        inside.kind = Constraint::Kind::inside;
        inside.sets.push_back(
            PolygonSet{{{Point{clip.x, clip.y}, Point{clip.right(), clip.y},
                         Point{clip.right(), clip.bottom()}, Point{clip.x, clip.bottom()}}},
                       false});
        clips_.push_back(std::make_unique<Constraint>(std::move(inside)));
    }
    void pop_clip() override {
        if (!clips_.empty())
            clips_.pop_back();
    }

    void draw(const Drawable& drawable, const Constraints& constraints) override {
        if (drawable.path.empty())
            return;
        const double opacity = opacity_.empty() ? 1.0 : opacity_.back();
        const Tone paint_tone = tone(drawable);
        const auto add = [&](PolygonSet set, const double level) {
            transform_set(set, drawable.to_widget);
            matte_.sets.push_back(std::move(set));
            matte_.matte.push_back(Constraint::MatteTerm{paint_tone.alpha * opacity * level,
                                                         paint_tone.red, paint_tone.green,
                                                         paint_tone.blue});
        };
        Constraints all = constraints;
        for (const std::unique_ptr<Constraint>& clip : clips_)
            all.push_back(clip.get());
        if (all.empty()) {
            add(coverage(drawable), 1.0);
            return;
        }
        for (const SweepLevel& level : constrained(drawable, all))
            add(PolygonSet{trapezoid_polygons(level.trapezoids), false}, level.value);
    }

  private:
    Constraint& matte_;
    std::vector<double> opacity_;
    std::vector<std::unique_ptr<Constraint>> clips_;
};

/** Paths in a shape layer while its contents are evaluated: an outline and where it lives. */
struct Entry final {
    CubicPath path;
    Affine to_layer;
};

/** A fill or stroke and the paths above it that it paints, with its group's place and opacity. */
struct Operation final {
    const model::SolidPaintItem* solid = nullptr;
    const model::GradientPaintItem* gradient = nullptr;
    std::vector<std::size_t> entries;
    Affine to_layer;
    double opacity = 1.0;
};

struct ShapeState final {
    double frame = 0.0;
    /** Flatness in layer units, for merged paths. */
    double layer_tolerance = flatness;
    std::vector<Entry> entries;
    /** Top first, as the layer lists them. */
    std::vector<Operation> operations;
};

/** The step a repeater takes `count` times (a fraction of a step for a fractional count). */
[[nodiscard]] Affine repeater_matrix(const model::Transform& transform, const double frame,
                                     const double count) {
    const Point anchor = point(transform.anchor.point(frame));
    const Point position = point(transform.position_at(frame));
    const Point scale = point(transform.scale.point(frame));
    const double rotation = transform.rotation.scalar(frame);
    const auto step = [&](const double amount) {
        return Affine::translate(position.x * amount, position.y * amount) *
               Affine::translate(anchor.x, anchor.y) * Affine::rotate(rotation * amount) *
               Affine::scale(1.0 + (scale.x / 100.0 - 1.0) * amount,
                             1.0 + (scale.y / 100.0 - 1.0) * amount) *
               Affine::translate(-anchor.x, -anchor.y);
    };
    const double whole = std::floor(count);
    const double fraction = count - whole;
    Affine result;
    const Affine unit = step(1.0);
    const std::optional<Affine> back = unit.inverse();
    const int steps = static_cast<int>(std::clamp(whole, -1000.0, 1000.0));
    for (int index = 0; index < std::abs(steps); ++index) {
        if (steps > 0)
            result = result * unit;
        else if (back.has_value())
            result = result * *back;
    }
    if (fraction > 0.0)
        result = result * step(fraction);
    return result;
}

[[nodiscard]] std::vector<std::size_t> evaluate_group(const std::vector<model::ShapeItem>& items,
                                                      const Affine& to_layer, double opacity,
                                                      ShapeState& state, std::size_t depth);

void merge_paths(const model::MergeItem& merge, std::vector<std::size_t>& above,
                 const Affine& to_layer, ShapeState& state) {
    if (above.empty())
        return;
    const std::optional<Affine> from_layer = to_layer.inverse();
    if (!from_layer.has_value())
        return;
    // Every path above, in this group's coordinates.
    std::vector<CubicPath> paths;
    for (const std::size_t index : above) {
        CubicPath path = state.entries[index].path;
        transform(path, *from_layer * state.entries[index].to_layer);
        paths.push_back(std::move(path));
    }
    Entry merged;
    merged.to_layer = to_layer;
    if (merge.mode <= 1 || merge.mode > 5) {
        // Merge: the outlines together, filled as one.
        for (CubicPath& path : paths)
            merged.path.insert(merged.path.end(), path.begin(), path.end());
    } else {
        const double scale = to_layer.mean_scale();
        const double tolerance = state.layer_tolerance / (scale > 1.0e-9 ? scale : 1.0);
        std::vector<PolygonSet> sets;
        for (const CubicPath& path : paths)
            sets.push_back(PolygonSet{polygons(flatten(path, tolerance)), false});
        const int mode = merge.mode;
        const std::vector<SweepLevel> levels =
            sweep(sets, [mode](const std::span<const std::uint8_t> inside) {
                std::size_t count = 0U;
                for (const std::uint8_t in : inside)
                    count += in != 0U ? 1U : 0U;
                switch (mode) {
                case 2:
                    return count > 0U ? 1.0 : 0.0;
                case 3:
                    return inside[0] != 0U && count == 1U ? 1.0 : 0.0;
                case 4:
                    return count == inside.size() ? 1.0 : 0.0;
                default:
                    return count % 2U == 1U ? 1.0 : 0.0;
                }
            });
        for (const SweepLevel& level : levels) {
            for (const std::vector<Point>& loop : region_outline(level.trapezoids)) {
                Contour contour;
                contour.closed = true;
                contour.start = loop.front();
                for (std::size_t index = 1U; index <= loop.size(); ++index) {
                    const Point to = loop[index % loop.size()];
                    const Point from = loop[index - 1U];
                    contour.segments.push_back(Cubic{from, to, to});
                }
                merged.path.push_back(std::move(contour));
            }
        }
    }
    state.entries.push_back(std::move(merged));
    above.assign(1U, state.entries.size() - 1U);
}

void trim_paths(const model::TrimItem& trim_item, const std::vector<std::size_t>& above,
                ShapeState& state) {
    const double frame = state.frame;
    const std::optional<std::vector<std::pair<double, double>>> ranges =
        trim_ranges(trim_item.start.scalar(frame) / 100.0, trim_item.end.scalar(frame) / 100.0,
                    trim_item.offset.scalar(frame) / 360.0);
    if (!ranges.has_value())
        return;
    if (!trim_item.individually) {
        for (const std::size_t index : above) {
            state.entries[index].path = trim(
                state.entries[index].path, trim_item.start.scalar(frame) / 100.0,
                trim_item.end.scalar(frame) / 100.0, trim_item.offset.scalar(frame) / 360.0, false);
        }
        return;
    }
    // Individually: every path above, in order, measured as one.
    double total = 0.0;
    std::vector<std::vector<double>> lengths;
    for (const std::size_t index : above) {
        std::vector<double>& entry = lengths.emplace_back();
        for (const Contour& contour : state.entries[index].path) {
            entry.push_back(contour_length(contour));
            total += entry.back();
        }
    }
    double before = 0.0;
    for (std::size_t position = 0U; position < above.size(); ++position) {
        Entry& entry = state.entries[above[position]];
        CubicPath kept;
        for (std::size_t contour = 0U; contour < entry.path.size(); ++contour) {
            const double length_here = lengths[position][contour];
            for (const auto& [low, high] : *ranges) {
                const double from = std::max(low * total, before);
                const double to = std::min(high * total, before + length_here);
                if (to > from) {
                    Contour piece = contour_span(entry.path[contour], from - before, to - before);
                    if (!piece.segments.empty())
                        kept.push_back(std::move(piece));
                }
            }
            before += length_here;
        }
        entry.path = std::move(kept);
    }
}

void repeat(const model::RepeaterItem& repeater, std::vector<std::size_t>& above,
            const std::size_t first_operation, const Affine& to_layer, ShapeState& state) {
    const double frame = state.frame;
    const double copies_value = std::clamp(repeater.copies.scalar(frame), 0.0, 1000.0);
    const auto copies = static_cast<std::size_t>(std::ceil(copies_value));
    if (copies == 0U) {
        state.operations.resize(first_operation);
        above.clear();
        return;
    }
    const std::optional<Affine> from_layer = to_layer.inverse();
    if (!from_layer.has_value())
        return;
    const double offset = repeater.offset.scalar(frame);
    const double start_opacity = repeater.transform.start_opacity.scalar(frame) / 100.0;
    const double end_opacity = repeater.transform.end_opacity.scalar(frame) / 100.0;
    const std::vector<Operation> originals(state.operations.begin() +
                                               static_cast<std::ptrdiff_t>(first_operation),
                                           state.operations.end());
    const std::vector<std::size_t> original_entries = above;
    std::vector<std::vector<Operation>> blocks;
    std::vector<std::size_t> next_above;
    for (std::size_t copy = 0U; copy < copies; ++copy) {
        const Affine step =
            to_layer *
            repeater_matrix(repeater.transform, frame, static_cast<double>(copy) + offset) *
            *from_layer;
        const double alpha = copies == 1U ? start_opacity
                                          : start_opacity + (end_opacity - start_opacity) *
                                                                static_cast<double>(copy) /
                                                                static_cast<double>(copies - 1U);
        std::map<std::size_t, std::size_t> moved;
        for (const std::size_t index : original_entries) {
            Entry copied = state.entries[index];
            copied.to_layer = step * copied.to_layer;
            state.entries.push_back(std::move(copied));
            moved.emplace(index, state.entries.size() - 1U);
            next_above.push_back(state.entries.size() - 1U);
        }
        std::vector<Operation>& block = blocks.emplace_back();
        for (Operation operation : originals) {
            for (std::size_t& entry : operation.entries) {
                if (const auto found = moved.find(entry); found != moved.end())
                    entry = found->second;
            }
            operation.to_layer = step * operation.to_layer;
            operation.opacity *= alpha;
            block.push_back(std::move(operation));
        }
    }
    state.operations.resize(first_operation);
    // Operations are listed top first: copies composited above earlier ones come first.
    if (repeater.above)
        std::ranges::reverse(blocks);
    for (std::vector<Operation>& block : blocks) {
        for (Operation& operation : block)
            state.operations.push_back(std::move(operation));
    }
    above = std::move(next_above);
}

std::vector<std::size_t> evaluate_group(const std::vector<model::ShapeItem>& items,
                                        const Affine& to_layer, const double opacity,
                                        ShapeState& state, const std::size_t depth) {
    std::vector<std::size_t> above;
    const std::size_t first_operation = state.operations.size();
    const double frame = state.frame;
    const auto add = [&](Contour contour) {
        if (contour.segments.empty())
            return;
        state.entries.push_back(Entry{CubicPath{std::move(contour)}, to_layer});
        above.push_back(state.entries.size() - 1U);
    };
    for (const model::ShapeItem& item : items) {
        std::visit(
            Overloaded{
                [&](const model::GroupItem& group) {
                    if (depth >= maximum_depth)
                        return;
                    const double group_opacity = group.transform.opacity.scalar(frame) / 100.0;
                    const std::vector<std::size_t> inner = evaluate_group(
                        group.items, to_layer * transform_matrix(group.transform, frame),
                        opacity * group_opacity, state, depth + 1U);
                    above.insert(above.end(), inner.begin(), inner.end());
                },
                [&](const model::PathItem& path) {
                    Contour contour = contour_from_bezier(path.shape.evaluate(frame));
                    if (path.direction == model::reversed_direction)
                        contour = reversed(contour);
                    add(std::move(contour));
                },
                [&](const model::RectItem& rect) {
                    add(rectangle_contour(point(rect.position.point(frame)),
                                          point(rect.size.point(frame)),
                                          rect.roundness.scalar(frame), rect.direction));
                },
                [&](const model::EllipseItem& ellipse) {
                    add(ellipse_contour(point(ellipse.position.point(frame)),
                                        point(ellipse.size.point(frame)), ellipse.direction));
                },
                [&](const model::PolystarItem& star) {
                    add(polystar_contour(
                        star.star, point(star.position.point(frame)), star.points.scalar(frame),
                        star.rotation.scalar(frame), star.outer_radius.scalar(frame),
                        star.inner_radius.scalar(frame), star.outer_roundness.scalar(frame),
                        star.inner_roundness.scalar(frame), star.direction));
                },
                [&](const model::SolidPaintItem& paint) {
                    state.operations.push_back(
                        Operation{&paint, nullptr, above, to_layer, opacity});
                },
                [&](const model::GradientPaintItem& paint) {
                    state.operations.push_back(
                        Operation{nullptr, &paint, above, to_layer, opacity});
                },
                [&](const model::TrimItem& trim_item) { trim_paths(trim_item, above, state); },
                [&](const model::RoundCornersItem& corners) {
                    const double radius = corners.radius.scalar(frame);
                    for (const std::size_t index : above)
                        state.entries[index].path =
                            round_corners(state.entries[index].path, radius);
                },
                [&](const model::MergeItem& merge) { merge_paths(merge, above, to_layer, state); },
                [&](const model::RepeaterItem& repeater) {
                    repeat(repeater, above, first_operation, to_layer, state);
                },
            },
            item.content);
    }
    return above;
}

/** The stops of a gradient at `frame`: colour stops and opacity stops merged, at most sixteen. */
[[nodiscard]] std::vector<GradientStop> gradient_stops(const model::GradientPaintItem& item,
                                                       const double frame, const double opacity) {
    const std::vector<double> values = item.colors.values(frame);
    const std::size_t count = item.color_stops;
    if (values.size() < count * 4U || count == 0U)
        return {};
    struct ColorStop final {
        double offset;
        double red;
        double green;
        double blue;
    };
    std::vector<ColorStop> colors;
    for (std::size_t index = 0U; index < count; ++index) {
        colors.push_back(ColorStop{values[index * 4U], values[index * 4U + 1U],
                                   values[index * 4U + 2U], values[index * 4U + 3U]});
    }
    std::vector<std::pair<double, double>> alphas;
    for (std::size_t index = count * 4U; index + 1U < values.size(); index += 2U)
        alphas.emplace_back(values[index], values[index + 1U]);
    const auto color_at = [&colors](const double offset) {
        if (offset <= colors.front().offset)
            return colors.front();
        for (std::size_t index = 1U; index < colors.size(); ++index) {
            if (offset <= colors[index].offset) {
                const ColorStop& a = colors[index - 1U];
                const ColorStop& b = colors[index];
                const double span = b.offset - a.offset;
                const double t = span > 0.0 ? (offset - a.offset) / span : 1.0;
                return ColorStop{offset, a.red + (b.red - a.red) * t,
                                 a.green + (b.green - a.green) * t, a.blue + (b.blue - a.blue) * t};
            }
        }
        return colors.back();
    };
    const auto alpha_at = [&alphas](const double offset) {
        if (alphas.empty())
            return 1.0;
        if (offset <= alphas.front().first)
            return alphas.front().second;
        for (std::size_t index = 1U; index < alphas.size(); ++index) {
            if (offset <= alphas[index].first) {
                const auto& [a_offset, a_alpha] = alphas[index - 1U];
                const auto& [b_offset, b_alpha] = alphas[index];
                const double span = b_offset - a_offset;
                const double t = span > 0.0 ? (offset - a_offset) / span : 1.0;
                return a_alpha + (b_alpha - a_alpha) * t;
            }
        }
        return alphas.back().second;
    };
    std::vector<double> offsets;
    for (const ColorStop& stop : colors)
        offsets.push_back(std::clamp(stop.offset, 0.0, 1.0));
    for (const auto& [offset, alpha] : alphas)
        offsets.push_back(std::clamp(offset, 0.0, 1.0));
    std::ranges::sort(offsets);
    offsets.erase(
        std::unique(offsets.begin(), offsets.end(),
                    [](const double a, const double b) { return std::abs(a - b) < 1.0e-6; }),
        offsets.end());
    // More stops than a paint carries: sampled evenly instead.
    if (offsets.size() > maximum_gradient_stops) {
        offsets.clear();
        for (std::size_t index = 0U; index < maximum_gradient_stops; ++index)
            offsets.push_back(static_cast<double>(index) /
                              static_cast<double>(maximum_gradient_stops - 1U));
    }
    std::vector<GradientStop> stops;
    for (const double offset : offsets) {
        const ColorStop color = color_at(offset);
        stops.push_back(GradientStop{offset, RenderColor{channel(color.red), channel(color.green),
                                                         channel(color.blue),
                                                         channel(alpha_at(offset) * opacity)}});
    }
    if (stops.size() == 1U)
        stops.push_back(GradientStop{1.0, stops.front().color});
    stops.front().offset = std::max(0.0, stops.front().offset);
    return stops;
}

[[nodiscard]] StrokeStyle stroke_style(const model::StrokeStyle& style, const double frame) {
    StrokeStyle result;
    result.width = style.width.scalar(frame);
    result.cap = style.cap == model::LineCap::round    ? PathCap::round
                 : style.cap == model::LineCap::square ? PathCap::square
                                                       : PathCap::butt;
    result.join = style.join == model::LineJoin::round   ? PathJoin::round
                  : style.join == model::LineJoin::bevel ? PathJoin::bevel
                                                         : PathJoin::miter;
    result.miter_limit = std::max(1.0, style.miter_limit);
    double period = 0.0;
    for (const model::Dash& dash : style.dashes) {
        const double length = std::max(0.0, dash.length.scalar(frame));
        if (dash.kind == model::Dash::Kind::offset) {
            result.dash_offset = length;
        } else if (result.dash.size() < 16U) {
            result.dash.push_back(length);
            period += length;
        }
    }
    if (period <= 0.0) {
        result.dash.clear();
        result.dash_offset = 0.0;
    } else if (result.dash.size() % 2U == 1U && result.dash.size() <= 8U) {
        // An odd pattern repeats to make dashes and gaps alternate, as in SVG.
        const std::vector<double> once = result.dash;
        result.dash.insert(result.dash.end(), once.begin(), once.end());
    }
    return result;
}

class Renderer final {
  public:
    Renderer(const model::Animation& animation, const double layer_tolerance)
        : animation_(animation), layer_tolerance_(layer_tolerance) {}

    void composition(const model::Composition& composition, const double frame,
                     const Affine& to_widget, const Constraints& inherited, Sink& sink,
                     const std::size_t depth) {
        if (depth > maximum_depth)
            return;
        const std::vector<model::Layer>& layers = composition.layers;
        const std::vector<Affine> worlds = world_matrices(layers, frame);
        // Bottom (last listed) to top.
        for (std::size_t position = layers.size(); position-- > 0U;) {
            const model::Layer& layer = layers[position];
            if (layer.hidden || layer.matte_source)
                continue;
            layer_content(layers, position, worlds, frame, to_widget, inherited, sink, depth);
        }
    }

  private:
    [[nodiscard]] static double layer_time(const model::Layer& layer, const double frame) noexcept {
        return (frame - layer.start_time) / layer.stretch;
    }

    [[nodiscard]] Affine local_matrix(const model::Layer& layer, const double frame) const {
        const double time = layer_time(layer, frame);
        double orient = 0.0;
        if (layer.auto_orient) {
            const model::Vec2 before = layer.transform.position_at(time - 0.05);
            const model::Vec2 after = layer.transform.position_at(time + 0.05);
            if (std::hypot(after.x - before.x, after.y - before.y) > 1.0e-9)
                orient =
                    std::atan2(after.y - before.y, after.x - before.x) * 180.0 / std::numbers::pi;
        }
        return transform_matrix(layer.transform, time, orient);
    }

    /** Each layer's transform into its composition, through its parents. */
    [[nodiscard]] std::vector<Affine> world_matrices(const std::vector<model::Layer>& layers,
                                                     const double frame) const {
        std::map<std::int64_t, std::size_t> by_index;
        for (std::size_t position = 0U; position < layers.size(); ++position) {
            if (layers[position].index.has_value())
                by_index.emplace(*layers[position].index, position);
        }
        std::vector<std::optional<Affine>> resolved(layers.size());
        std::vector<Affine> result(layers.size());
        for (std::size_t position = 0U; position < layers.size(); ++position) {
            // Walk up to the first resolved ancestor (a parent chain is bounded by the layers).
            std::vector<std::size_t> chain;
            std::optional<std::size_t> current = position;
            while (current.has_value() && !resolved[*current].has_value() &&
                   chain.size() <= layers.size()) {
                chain.push_back(*current);
                const model::Layer& layer = layers[*current];
                current.reset();
                if (layer.parent.has_value()) {
                    if (const auto found = by_index.find(*layer.parent); found != by_index.end())
                        current = found->second;
                }
                if (current.has_value() && std::ranges::find(chain, *current) != chain.end()) {
                    current.reset();
                }
            }
            Affine base = current.has_value() && resolved[*current].has_value()
                              ? *resolved[*current]
                              : Affine{};
            for (std::size_t index = chain.size(); index-- > 0U;) {
                base = base * local_matrix(layers[chain[index]], frame);
                resolved[chain[index]] = base;
            }
            result[position] = resolved[position].value_or(Affine{});
        }
        return result;
    }

    [[nodiscard]] static bool visible(const model::Layer& layer, const double frame) noexcept {
        return frame >= layer.in_frame && frame < layer.out_frame;
    }

    /** The mask stack of a layer as a constraint, in widget coordinates. */
    [[nodiscard]] std::optional<Constraint> masks(const model::Layer& layer, const double time,
                                                  const Affine& to_widget) const {
        Constraint result;
        result.kind = Constraint::Kind::mask;
        const double scale = to_widget.mean_scale();
        const double tolerance = layer_tolerance_ / (scale > 1.0e-9 ? scale : 1.0);
        bool first = true;
        for (const model::Mask& mask : layer.masks) {
            if (mask.mode == 'n')
                continue;
            if (first)
                result.start_full = mask.mode == 's' || mask.mode == 'i' || mask.mode == 'd';
            first = false;
            CubicPath path{contour_from_bezier(mask.path.evaluate(time))};
            for (Contour& contour : path)
                contour.closed = true;
            const std::vector<PathContour> flat = flatten(path, tolerance);
            Constraint::MaskTerm term;
            term.mode = mask.mode;
            term.inverted = mask.inverted;
            term.opacity = std::clamp(mask.opacity.scalar(time) / 100.0, 0.0, 1.0);
            PolygonSet outline{polygons(flat), false};
            transform_set(outline, to_widget);
            term.path = result.sets.size();
            result.sets.push_back(std::move(outline));
            // Expansion grows or shrinks the mask by the band a stroke of twice its size covers.
            const double expansion = mask.expansion.scalar(time);
            if (std::abs(expansion) > 1.0e-6) {
                StrokeStyle band;
                band.width = std::abs(expansion) * 2.0;
                band.join = PathJoin::round;
                band.cap = PathCap::round;
                std::vector<PathContour> closed = flat;
                for (PathContour& contour : closed)
                    contour.closed = true;
                PolygonSet ring{polygons(stroke_outline(closed, band, tolerance)), false};
                transform_set(ring, to_widget);
                term.band = result.sets.size();
                term.grow = expansion > 0.0;
                result.sets.push_back(std::move(ring));
            }
            result.masks.push_back(term);
        }
        if (result.masks.empty())
            return std::nullopt;
        return result;
    }

    void layer_content(const std::vector<model::Layer>& layers, const std::size_t position,
                       const std::vector<Affine>& worlds, const double frame,
                       const Affine& to_widget, const Constraints& inherited, Sink& sink,
                       const std::size_t depth) {
        const model::Layer& layer = layers[position];
        if (!visible(layer, frame))
            return;
        if (layer.type != model::LayerType::shape && layer.type != model::LayerType::solid &&
            layer.type != model::LayerType::precomp) {
            return;
        }
        const double time = layer_time(layer, frame);
        const double opacity = std::clamp(layer.transform.opacity.scalar(time) / 100.0, 0.0, 1.0);
        if (opacity <= 0.0)
            return;
        const Affine world = to_widget * worlds[position];
        Constraints constraints = inherited;
        std::optional<Constraint> mask_constraint;
        if (!layer.masks.empty()) {
            mask_constraint = masks(layer, time, world);
            if (mask_constraint.has_value())
                constraints.push_back(&*mask_constraint);
        }
        std::optional<Constraint> matte_constraint;
        if (layer.matte != model::MatteMode::none) {
            matte_constraint = matte(layers, position, worlds, frame, to_widget, depth);
            constraints.push_back(&*matte_constraint);
        }
        if (opacity < 1.0)
            sink.push_opacity(opacity);
        switch (layer.type) {
        case model::LayerType::shape:
            shapes(layer, time, world, constraints, sink);
            break;
        case model::LayerType::solid: {
            if (layer.width > 0.0 && layer.height > 0.0) {
                Drawable drawable;
                drawable.to_widget = world;
                drawable.path.push_back(
                    rectangle_contour(Point{layer.width * 0.5, layer.height * 0.5},
                                      Point{layer.width, layer.height}, 0.0, 1));
                drawable.color =
                    RenderColor{channel(layer.solid_color[0]), channel(layer.solid_color[1]),
                                channel(layer.solid_color[2]), 255U};
                sink.draw(drawable, constraints);
            }
            break;
        }
        case model::LayerType::precomp:
            precomp(layer, time, world, constraints, sink, depth);
            break;
        default:
            break;
        }
        if (opacity < 1.0)
            sink.pop_opacity();
    }

    /** The track matte of `layers[position]`: the matte layer drawn into a constraint. */
    [[nodiscard]] Constraint matte(const std::vector<model::Layer>& layers,
                                   const std::size_t position, const std::vector<Affine>& worlds,
                                   const double frame, const Affine& to_widget,
                                   const std::size_t depth) {
        const model::Layer& layer = layers[position];
        Constraint result;
        switch (layer.matte) {
        case model::MatteMode::alpha_inverted:
            result.kind = Constraint::Kind::alpha_inverted;
            break;
        case model::MatteMode::luma:
            result.kind = Constraint::Kind::luma;
            break;
        case model::MatteMode::luma_inverted:
            result.kind = Constraint::Kind::luma_inverted;
            break;
        default:
            result.kind = Constraint::Kind::alpha;
            break;
        }
        std::optional<std::size_t> source;
        if (layer.matte_parent.has_value()) {
            for (std::size_t index = 0U; index < layers.size(); ++index) {
                if (layers[index].index == layer.matte_parent)
                    source = index;
            }
        } else if (position > 0U) {
            source = position - 1U;
        }
        if (source.has_value() && *source != position) {
            MatteSink matte_sink(result);
            layer_content(layers, *source, worlds, frame, to_widget, {}, matte_sink, depth + 1U);
        }
        return result;
    }

    void shapes(const model::Layer& layer, const double time, const Affine& world,
                const Constraints& constraints, Sink& sink) {
        ShapeState state;
        state.frame = time;
        const double scale = world.mean_scale();
        state.layer_tolerance = layer_tolerance_ / (scale > 1.0e-9 ? scale : 1.0);
        static_cast<void>(evaluate_group(layer.shapes, Affine{}, 1.0, state, 0U));
        // Operations are listed top first; the bottom one draws first.
        for (std::size_t index = state.operations.size(); index-- > 0U;) {
            const Operation& operation = state.operations[index];
            const std::optional<Affine> from_layer = operation.to_layer.inverse();
            if (!from_layer.has_value() || operation.opacity <= 0.0)
                continue;
            Drawable drawable;
            drawable.to_widget = world * operation.to_layer;
            for (const std::size_t entry : operation.entries) {
                CubicPath path = state.entries[entry].path;
                if (!(state.entries[entry].to_layer == operation.to_layer))
                    transform(path, *from_layer * state.entries[entry].to_layer);
                for (Contour& contour : path) {
                    if (!contour.segments.empty())
                        drawable.path.push_back(std::move(contour));
                }
            }
            if (drawable.path.empty())
                continue;
            if (operation.solid != nullptr) {
                const model::SolidPaintItem& item = *operation.solid;
                std::array<double, 4> color{0.0, 0.0, 0.0, 1.0};
                item.color.evaluate(time, color);
                if (item.color.dimension() < 4U)
                    color[3] = 1.0;
                const double alpha = color[3] *
                                     std::clamp(item.opacity.scalar(time) / 100.0, 0.0, 1.0) *
                                     operation.opacity;
                drawable.color = RenderColor{channel(color[0]), channel(color[1]),
                                             channel(color[2]), channel(alpha)};
                if (drawable.color.alpha == 0U)
                    continue;
                drawable.stroke = item.stroke;
                drawable.rule = item.rule == model::FillRule::even_odd ? PathFillRule::evenodd
                                                                       : PathFillRule::nonzero;
                if (item.stroke)
                    drawable.style = stroke_style(item.style, time);
            } else {
                const model::GradientPaintItem& item = *operation.gradient;
                const double alpha =
                    std::clamp(item.opacity.scalar(time) / 100.0, 0.0, 1.0) * operation.opacity;
                LocalGradient gradient;
                gradient.radial = item.radial;
                gradient.start = point(item.start.point(time));
                gradient.end = point(item.end.point(time));
                gradient.stops = gradient_stops(item, time, alpha);
                if (gradient.stops.empty())
                    continue;
                drawable.gradient = std::move(gradient);
                drawable.stroke = item.stroke;
                drawable.rule = item.rule == model::FillRule::even_odd ? PathFillRule::evenodd
                                                                       : PathFillRule::nonzero;
                if (item.stroke)
                    drawable.style = stroke_style(item.style, time);
            }
            if (drawable.stroke && !(drawable.style.width > 0.0))
                continue;
            sink.draw(drawable, constraints);
        }
    }

    void precomp(const model::Layer& layer, const double time, const Affine& world,
                 const Constraints& constraints, Sink& sink, const std::size_t depth) {
        if (!layer.precomp.has_value() || *layer.precomp >= animation_.precomps.size())
            return;
        const double inner = layer.time_remap.has_value()
                                 ? layer.time_remap->scalar(time) * animation_.frame_rate
                                 : time;
        // A precomposition shows only its own box: a clip when it stays axis-aligned, otherwise
        // a constraint like a mask.
        Constraints inner_constraints = constraints;
        std::optional<Constraint> box;
        bool clipped = false;
        if (layer.width > 0.0 && layer.height > 0.0) {
            const Point a = world.apply(Point{0.0, 0.0});
            const Point b = world.apply(Point{layer.width, layer.height});
            if (std::abs(world.b) < 1.0e-12 && std::abs(world.c) < 1.0e-12) {
                sink.push_clip(Rect{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x),
                                    std::abs(b.y - a.y)});
                clipped = true;
            } else {
                Constraint inside;
                inside.kind = Constraint::Kind::inside;
                inside.sets.push_back(PolygonSet{{{a, world.apply(Point{layer.width, 0.0}), b,
                                                   world.apply(Point{0.0, layer.height})}},
                                                 false});
                box = std::move(inside);
                inner_constraints.push_back(&*box);
            }
        }
        composition(animation_.precomps[*layer.precomp], inner, world, inner_constraints, sink,
                    depth + 1U);
        if (clipped)
            sink.pop_clip();
    }

    const model::Animation& animation_;
    double layer_tolerance_ = flatness;
};

} // namespace

void append_frame(std::vector<RenderCommand>& output, const resource::lottie::Animation& animation,
                  const double frame, const Rect viewport, const Rect clip) {
    if (!(viewport.width > 0.0) || !(viewport.height > 0.0) || !(animation.width > 0.0) ||
        !(animation.height > 0.0)) {
        return;
    }
    const Affine to_widget =
        Affine::translate(viewport.x, viewport.y) *
        Affine::scale(viewport.width / animation.width, viewport.height / animation.height);
    if (!to_widget.finite())
        return;
    output.emplace_back(ClipPushRenderCommand{clip});
    CommandSink sink(output);
    Renderer renderer(animation, flatness);
    renderer.composition(animation.root, frame, to_widget, {}, sink, 0U);
    output.emplace_back(ClipPopRenderCommand{});
}

} // namespace strata::ui::lottie
