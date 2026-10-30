#include "ui/lottie/scene.hpp"

#include "font/system_fonts.hpp"
#include "ui/lottie/geometry.hpp"
#include "ui/render/path_geometry.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace strata::ui::lottie {
namespace {

namespace model = resource::lottie;

/** How far a flattened curve may stray from the true one, in logical pixels. */
constexpr double flatness = 0.08;
/**
 * How deep a frame follows compositions and the track mattes inside them. The loader rejects a
 * document whose precompositions nest past model::maximum_nesting, and each of those levels' mattes
 * may nest as deep again, so no document it accepts reaches this: it only stops mattes that name
 * each other (`tp`) from recursing for ever.
 */
constexpr std::size_t maximum_depth = model::maximum_nesting * 2U;

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

/**
 * What a mask stack (or the box a turned precomposition shows) lets through, as a function of
 * which of its polygon sets a point is inside. Its sets are in widget coordinates.
 */
struct Constraint final {
    enum class Kind { mask, inside };
    struct MaskTerm final {
        std::size_t path = 0U;
        std::optional<std::size_t> band;
        bool grow = false;
        char mode = 'a';
        bool inverted = false;
        double opacity = 1.0;
    };
    Kind kind = Kind::mask;
    std::vector<PolygonSet> sets;
    std::vector<MaskTerm> masks;
    bool start_full = false;

    [[nodiscard]] double evaluate(const std::span<const std::uint8_t> inside) const noexcept {
        if (kind == Kind::inside)
            return inside.empty() || inside[0] != 0U ? 1.0 : 0.0;
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
                    // The loader rejects deeper groups; this only bounds the recursion.
                    if (depth >= model::maximum_nesting)
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

/** The id isolated Lottie layers draw under: no host declares it, so it has no passes of its own.
 */
constexpr std::string_view layer_effect_id = "strata:lottie-layer";
/** One isolated layer: how it composites, its opacity and its filters. */
struct LayerSpec final {
    LayerComposite composite = LayerComposite::source_over;
    double opacity = 1.0;
    std::vector<LayerFilter> filters;
};

[[nodiscard]] Rect expanded(const Rect rect, const double margin) noexcept {
    return Rect{rect.x - margin, rect.y - margin, rect.width + margin * 2.0,
                rect.height + margin * 2.0};
}

/**
 * Where a frame's drawing goes: render commands in widget coordinates. Layers drawn apart are
 * content effects over `bounds`: the outermost exactly the widget's box, which it clips to, and
 * the rest that box grown by how far the animation's effects reach past it, and a little more so
 * that their composites' soft bounds edge stays outside the box.
 */
class Sink final {
  public:
    Sink(std::vector<RenderCommand>& output, const Rect box, const double margin)
        : output_(output), box_(box), bounds_(expanded(box, margin + 2.0)) {}

    void push_opacity(const double opacity) {
        output_.emplace_back(OpacityPushRenderCommand{opacity});
    }
    void pop_opacity() {
        output_.emplace_back(OpacityPopRenderCommand{});
    }
    /** An axis-aligned clip in widget coordinates. */
    void push_clip(const Rect clip) {
        output_.emplace_back(ClipPushRenderCommand{clip});
    }
    void pop_clip() {
        output_.emplace_back(ClipPopRenderCommand{});
    }

    [[nodiscard]] Rect bounds() const noexcept {
        return bounds_;
    }

    /** Starts a layer drawn apart; `root` composites it to the widget's box exactly. */
    void push_layer(const LayerSpec& spec, const bool root = false) {
        EffectState effect;
        effect.id = std::string(layer_effect_id);
        effect.input = EffectInput::content;
        effect.opacity = std::clamp(spec.opacity, 0.0, 1.0);
        effect.composite = spec.composite;
        effect.filters = spec.filters;
        output_.emplace_back(ContentEffectPushRenderCommand{root ? box_ : bounds_, CornerRadii{},
                                                            std::move(effect)});
    }
    void pop_layer() {
        output_.emplace_back(ContentEffectPopRenderCommand{});
    }

    void draw(const Drawable& drawable, const Constraints& constraints) {
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

    /** An image `size` large at the origin of `to_widget`, cut to the constraints. */
    void draw_image(const std::string& texture, const Point size, const Affine& to_widget,
                    const Constraints& constraints) {
        // Geometry cannot cut an image, so under masks it is drawn apart and its mask's coverage
        // keeps what shows of it.
        const bool cut = !constraints.empty();
        if (cut)
            push_layer(LayerSpec{});
        output_.emplace_back(TransformPushRenderCommand{to_widget.a, to_widget.c, to_widget.e,
                                                        to_widget.b, to_widget.d, to_widget.f});
        output_.emplace_back(ImageRenderCommand{Rect{0.0, 0.0, size.x, size.y}, texture});
        output_.emplace_back(TransformPopRenderCommand{});
        if (!cut)
            return;
        push_layer(LayerSpec{LayerComposite::destination_in, 1.0, {}});
        fill_coverage(constraints);
        pop_layer();
        pop_layer();
    }

  private:
    /** Fills, in white, what the constraints alone let through, by how much they let through. */
    void fill_coverage(const Constraints& constraints) {
        std::vector<PolygonSet> sets;
        std::vector<std::size_t> offsets;
        for (const Constraint* constraint : constraints) {
            offsets.push_back(sets.size());
            sets.insert(sets.end(), constraint->sets.begin(), constraint->sets.end());
        }
        // A region that no constraint's set contains still counts: start from the whole bounds.
        const Rect area = bounds_;
        sets.push_back(
            PolygonSet{{{Point{area.x, area.y}, Point{area.right(), area.y},
                         Point{area.right(), area.bottom()}, Point{area.x, area.bottom()}}},
                       false});
        const std::size_t whole = sets.size() - 1U;
        const std::vector<SweepLevel> levels =
            sweep(sets, [&](const std::span<const std::uint8_t> inside) {
                if (inside[whole] == 0U)
                    return 0.0;
                double value = 1.0;
                for (std::size_t index = 0U; index < constraints.size() && value > 0.0; ++index) {
                    value *= constraints[index]->evaluate(
                        inside.subspan(offsets[index], constraints[index]->sets.size()));
                }
                return value;
            });
        Drawable white;
        white.color = RenderColor{255U, 255U, 255U, 255U};
        for (const SweepLevel& level : levels)
            emit_path(output_, Affine{}, trapezoid_path(level.trapezoids), white, false,
                      level.value);
    }

    std::vector<RenderCommand>& output_;
    Rect box_;
    Rect bounds_;
};

/** A Lottie blend mode (`bm`) as the layer composite that draws it. */
[[nodiscard]] LayerComposite blend_composite(const int mode) noexcept {
    if (mode >= 1 && mode <= 15)
        return static_cast<LayerComposite>(mode);
    return mode == 16 ? LayerComposite::plus : LayerComposite::source_over;
}

[[nodiscard]] LayerComposite matte_composite(const model::MatteMode mode) noexcept {
    switch (mode) {
    case model::MatteMode::alpha_inverted:
        return LayerComposite::destination_out;
    case model::MatteMode::luma:
        return LayerComposite::destination_in_luminance;
    case model::MatteMode::luma_inverted:
        return LayerComposite::destination_out_luminance;
    default:
        return LayerComposite::destination_in;
    }
}

/** How a mask combines with the masks before it, as a composite of coverage. */
[[nodiscard]] LayerComposite mask_composite(const char mode) noexcept {
    switch (mode) {
    case 's':
        return LayerComposite::destination_out;
    case 'i':
        return LayerComposite::destination_in;
    case 'l':
        return LayerComposite::channel_maximum;
    case 'd':
        return LayerComposite::channel_minimum;
    case 'f':
        return LayerComposite::channel_difference;
    default:
        return LayerComposite::source_over;
    }
}

/** A blur reaching `radius_x` and `radius_y` widget pixels each way. */
[[nodiscard]] LayerFilter blur_filter(const double radius_x, const double radius_y) {
    return LayerFilter{LayerFilterKind::blur, {std::max(0.0, radius_x), std::max(0.0, radius_y)}};
}

/** Levels for one channel (input black, input white, gamma, output black, output white). */
void append_levels(std::vector<double>& values, const model::Effect& effect,
                   const std::size_t first, const double time) {
    values.push_back(effect.scalar(first, time, 0.0));
    values.push_back(effect.scalar(first + 1U, time, 1.0));
    values.push_back(effect.scalar(first + 2U, time, 1.0));
    values.push_back(effect.scalar(first + 3U, time, 0.0));
    values.push_back(effect.scalar(first + 4U, time, 1.0));
}

void append_identity_levels(std::vector<double>& values) {
    values.insert(values.end(), {0.0, 1.0, 1.0, 0.0, 1.0});
}

/**
 * The layer's effects as layer filters, lengths in widget pixels (`along_x`, `along_y` and
 * `scale` carry layer lengths there), as lottie-web and Skottie draw them: a blur's sigma is 0.3
 * of its blurriness and a shadow's a quarter of its softness (a filter reaches three sigmas).
 */
[[nodiscard]] std::vector<LayerFilter> layer_filters(const model::Layer& layer, const double time,
                                                     const double along_x, const double along_y,
                                                     const double scale) {
    std::vector<LayerFilter> result;
    for (const model::Effect& effect : layer.effects) {
        if (result.size() + 2U > maximum_layer_filters)
            break;
        switch (effect.type) {
        case model::effect_gaussian_blur: {
            const double radius = std::max(0.0, effect.scalar(0U, time, 0.0)) * 0.9;
            const int dimensions = static_cast<int>(std::lround(effect.scalar(1U, time, 1.0)));
            if (radius > 0.0) {
                result.push_back(blur_filter(dimensions == 3 ? 0.0 : radius * along_x,
                                             dimensions == 2 ? 0.0 : radius * along_y));
            }
            break;
        }
        case model::effect_drop_shadow: {
            const std::array<double, 4> color = effect.color(0U, time);
            const double opacity = std::clamp(effect.scalar(1U, time, 255.0) / 255.0, 0.0, 1.0);
            const double angle = (effect.scalar(2U, time, 135.0) - 90.0) * std::numbers::pi / 180.0;
            const double distance = effect.scalar(3U, time, 5.0) * scale;
            const double radius = std::max(0.0, effect.scalar(4U, time, 0.0)) * 0.75 * scale;
            const double alone = effect.scalar(5U, time, 0.0);
            result.push_back(
                LayerFilter{LayerFilterKind::drop_shadow,
                            {color[0], color[1], color[2], opacity, distance * std::cos(angle),
                             distance * std::sin(angle), radius, alone > 0.5 ? 1.0 : 0.0}});
            break;
        }
        case model::effect_fill: {
            const std::array<double, 4> color = effect.color(2U, time);
            const double opacity = std::clamp(effect.scalar(6U, time, 1.0), 0.0, 1.0);
            result.push_back(
                LayerFilter{LayerFilterKind::color_matrix, {0.0, 0.0, 0.0, 0.0,     color[0], //
                                                            0.0, 0.0, 0.0, 0.0,     color[1], //
                                                            0.0, 0.0, 0.0, 0.0,     color[2], //
                                                            0.0, 0.0, 0.0, opacity, 0.0}});
            break;
        }
        case model::effect_tint: {
            // Black maps to one colour and white to another, by luminance, mixed in by amount.
            const std::array<double, 4> black = effect.color(0U, time);
            const std::array<double, 4> white = effect.color(1U, time);
            const double amount = std::clamp(effect.scalar(2U, time, 100.0) / 100.0, 0.0, 1.0);
            constexpr std::array<double, 3> luma{0.2126, 0.7152, 0.0722};
            std::vector<double> matrix;
            for (std::size_t row = 0U; row < 3U; ++row) {
                for (std::size_t column = 0U; column < 3U; ++column) {
                    matrix.push_back((white[row] - black[row]) * luma[column] * amount +
                                     (row == column ? 1.0 - amount : 0.0));
                }
                matrix.push_back(0.0);
                matrix.push_back(black[row] * amount);
            }
            matrix.insert(matrix.end(), {0.0, 0.0, 0.0, 1.0, 0.0});
            result.push_back(LayerFilter{LayerFilterKind::color_matrix, std::move(matrix)});
            break;
        }
        case model::effect_tritone: {
            const std::array<double, 4> highlights = effect.color(0U, time);
            const std::array<double, 4> midtones = effect.color(1U, time);
            const std::array<double, 4> shadows = effect.color(2U, time);
            const double original = std::clamp(effect.scalar(3U, time, 0.0) / 100.0, 0.0, 1.0);
            result.push_back(LayerFilter{LayerFilterKind::gradient_map,
                                         {1.0 - original, 3.0, 0.0, shadows[0], shadows[1],
                                          shadows[2], 0.5, midtones[0], midtones[1], midtones[2],
                                          1.0, highlights[0], highlights[1], highlights[2]}});
            break;
        }
        case model::effect_levels: {
            // Each channel's own levels, then the composite RGB levels over them.
            if (effect.values.size() >= 36U) {
                std::vector<double> channels;
                append_levels(channels, effect, 10U, time);
                append_levels(channels, effect, 17U, time);
                append_levels(channels, effect, 24U, time);
                append_levels(channels, effect, 31U, time);
                result.push_back(LayerFilter{LayerFilterKind::levels, std::move(channels)});
            }
            if (effect.values.size() >= 8U) {
                std::vector<double> composite;
                for (int channel_index = 0; channel_index < 3; ++channel_index)
                    append_levels(composite, effect, 3U, time);
                append_identity_levels(composite);
                result.push_back(LayerFilter{LayerFilterKind::levels, std::move(composite)});
            }
            break;
        }
        default:
            break;
        }
    }
    return result;
}

/** A Lottie font style's weight (100-900) and slant. */
[[nodiscard]] std::pair<int, bool> font_style(const std::string& style) {
    std::string lowered;
    for (const char c : style) {
        if (c != ' ' && c != '-')
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    const bool italic =
        lowered.find("italic") != std::string::npos || lowered.find("oblique") != std::string::npos;
    const auto has = [&lowered](const std::string_view word) {
        return lowered.find(word) != std::string::npos;
    };
    int weight = 400;
    if (has("thin") || has("hairline"))
        weight = 100;
    else if (has("extralight") || has("ultralight"))
        weight = 200;
    else if (has("semibold") || has("demibold"))
        weight = 600;
    else if (has("extrabold") || has("ultrabold"))
        weight = 800;
    else if (has("black") || has("heavy"))
        weight = 900;
    else if (has("light"))
        weight = 300;
    else if (has("medium"))
        weight = 500;
    else if (has("bold"))
        weight = 700;
    return {weight, italic};
}

/** A font found for a text layer, or that the system's font names are still being read. */
struct ResolvedFont final {
    std::optional<font::SystemFont> face;
    bool pending = false;
};

/**
 * The system font a composition font names, found once per process and kept: by its name, else
 * its family and style, else the platform's interface font. Finding one by name needs every
 * font's names, read on a worker the first time: until then it is pending, and the layer's text
 * waits for it (as a page's text waits for a web font) rather than the frame.
 */
[[nodiscard]] ResolvedFont resolve_font(const model::Font* const font) {
    static std::mutex mutex;
    static std::map<std::string, std::optional<font::SystemFont>, std::less<>> cache;
    const std::string key =
        font != nullptr ? font->name + "|" + font->family + "|" + font->style : std::string("|");
    const std::scoped_lock lock(mutex);
    if (const auto found = cache.find(key); found != cache.end())
        return ResolvedFont{found->second, false};
    if (font != nullptr && !font::system_font_names_ready()) {
        font::prepare_system_font_names();
        return ResolvedFont{std::nullopt, true};
    }
    std::optional<font::SystemFont> result;
    if (font != nullptr) {
        const auto [weight, italic] = font_style(font->style);
        result = font::system_font_family(font->name, font->family, weight, italic);
    }
    for (const std::string_view fallback : {"segoeui.ttf", "arial.ttf", "DejaVuSans.ttf"}) {
        if (result.has_value())
            break;
        result = font::system_font_named(fallback);
    }
    cache.emplace(key, result);
    return ResolvedFont{result, false};
}

/** The code points of UTF-8 text; malformed bytes stand for themselves. */
[[nodiscard]] std::vector<std::uint32_t> code_points(const std::string_view text) {
    std::vector<std::uint32_t> result;
    for (std::size_t index = 0U; index < text.size();) {
        const auto byte = static_cast<unsigned char>(text[index]);
        std::size_t length = 1U;
        std::uint32_t value = byte;
        if (byte >= 0xF0U && index + 3U < text.size()) {
            length = 4U;
            value = byte & 0x07U;
        } else if (byte >= 0xE0U && index + 2U < text.size()) {
            length = 3U;
            value = byte & 0x0FU;
        } else if (byte >= 0xC0U && index + 1U < text.size()) {
            length = 2U;
            value = byte & 0x1FU;
        }
        for (std::size_t next = 1U; next < length; ++next)
            value = (value << 6U) | (static_cast<unsigned char>(text[index + next]) & 0x3FU);
        result.push_back(length == 1U ? byte : value);
        index += length;
    }
    return result;
}

/** A font glyph's outline as closed cubic contours, `scale` units per font unit, y down. */
[[nodiscard]] CubicPath glyph_path(const font::OpenTypeFont& face, const std::uint16_t glyph,
                                   const double scale) {
    CubicPath result;
    const std::shared_ptr<const font::GlyphOutline> outline = face.glyph_outline(glyph);
    if (outline == nullptr)
        return result;
    const auto place = [scale](const font::GlyphPoint& point) {
        return Point{point.x * scale, -point.y * scale};
    };
    for (const font::GlyphContour& source : outline->contours) {
        if (source.segments.empty())
            continue;
        Contour contour;
        contour.closed = true;
        contour.start = place(source.segments.front().from);
        for (const font::GlyphSegment& segment : source.segments) {
            const Point from = place(segment.from);
            const Point to = place(segment.to);
            if (segment.kind == font::GlyphSegmentKind::line) {
                contour.segments.push_back(Cubic{from, to, to});
            } else {
                const Point control = place(segment.control);
                contour.segments.push_back(Cubic{Point{from.x + (control.x - from.x) * 2.0 / 3.0,
                                                       from.y + (control.y - from.y) * 2.0 / 3.0},
                                                 Point{to.x + (control.x - to.x) * 2.0 / 3.0,
                                                       to.y + (control.y - to.y) * 2.0 / 3.0},
                                                 to});
            }
        }
        result.push_back(std::move(contour));
    }
    return result;
}

/** One laid-out letter of a text layer. */
struct Letter final {
    CubicPath path;
    double x = 0.0;
    double y = 0.0;
    double advance = 0.0;
    std::size_t line = 0U;
    std::size_t word = 0U;
    /** Its index among letters (all, and not counting spaces). */
    std::size_t index = 0U;
    std::size_t index_without_spaces = 0U;
    bool space = false;
};

/**
 * How much a range selector picks the unit at `index` of `total`, as lottie-web's
 * TextSelectorProperty computes it.
 */
[[nodiscard]] double selector_amount(const model::TextSelector& selector, const double time,
                                     const double index, const double total) {
    if (!(total > 0.0))
        return 0.0;
    const double divisor = selector.units == 2 ? 1.0 : 100.0 / total;
    const double offset = selector.offset.scalar(time) / divisor;
    double start = selector.start.scalar(time) / divisor + offset;
    double end = selector.end.scalar(time) / divisor + offset;
    if (start > end)
        std::swap(start, end);
    const double ease_high = selector.ease_high.scalar(time) / 100.0;
    const double ease_low = selector.ease_low.scalar(time) / 100.0;
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 1.0;
    double y2 = 1.0;
    if (ease_low > 0.0)
        x1 = ease_low;
    else
        y1 = -ease_low;
    if (ease_high > 0.0)
        x2 = 1.0 - ease_high;
    else
        y2 = 1.0 + ease_high;
    const auto ease = [&](const double value) {
        return model::cubic_ease(x1, y1, x2, y2, std::clamp(value, 0.0, 1.0));
    };
    const double span = end - start;
    double result = 0.0;
    switch (selector.shape) {
    case 2:
        result = span == 0.0 ? (index >= end ? 1.0 : 0.0)
                             : std::clamp(0.5 / span + (index - start) / span, 0.0, 1.0);
        result = ease(result);
        break;
    case 3:
        result = span == 0.0 ? (index >= end ? 0.0 : 1.0)
                             : 1.0 - std::clamp(0.5 / span + (index - start) / span, 0.0, 1.0);
        result = ease(result);
        break;
    case 4:
        if (span != 0.0) {
            result = std::clamp(0.5 / span + (index - start) / span, 0.0, 1.0);
            result = result < 0.5 ? result * 2.0 : 1.0 - 2.0 * (result - 0.5);
        }
        result = ease(result);
        break;
    case 5:
        if (span != 0.0) {
            const double along = std::clamp(index + 0.5 - start, 0.0, span);
            const double x = -span / 2.0 + along;
            const double half = span / 2.0;
            result = std::sqrt(std::max(0.0, 1.0 - (x * x) / (half * half)));
        }
        result = ease(result);
        break;
    case 6:
        if (span != 0.0) {
            const double along = std::clamp(index + 0.5 - start, 0.0, span);
            result =
                (1.0 + std::cos(std::numbers::pi + std::numbers::pi * 2.0 * along / span)) / 2.0;
        }
        result = ease(result);
        break;
    default:
        if (index >= std::floor(start)) {
            result = index - start < 0.0
                         ? std::clamp(std::min(end, 1.0) - (start - index), 0.0, 1.0)
                         : std::clamp(end - index, 0.0, 1.0);
        }
        result = ease(result);
        break;
    }
    return result * selector.amount.scalar(time) / 100.0;
}

class Renderer final {
  public:
    Renderer(const model::Animation& animation, const double layer_tolerance, Sink& sink)
        : animation_(animation), layer_tolerance_(layer_tolerance), sink_(sink) {}

    void composition(const model::Composition& composition, const double frame,
                     const Affine& to_widget, const Constraints& inherited,
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
            layer_content(layers, position, worlds, frame, to_widget, inherited, depth);
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

    /** Whether any of the layer's active masks has a feather at `time`. */
    [[nodiscard]] static bool feathered(const model::Layer& layer, const double time) noexcept {
        return std::ranges::any_of(layer.masks, [time](const model::Mask& mask) {
            if (mask.mode == 'n')
                return false;
            const model::Vec2 feather = mask.feather.point(time);
            return std::abs(feather.x) > 1.0e-6 || std::abs(feather.y) > 1.0e-6;
        });
    }

    /** A mask's outline at `time`, closed, grown or shrunk by its expansion, in layer units. */
    [[nodiscard]] CubicPath mask_path(const model::Mask& mask, const double time,
                                      const double tolerance) const {
        CubicPath path{contour_from_bezier(mask.path.evaluate(time))};
        for (Contour& contour : path)
            contour.closed = true;
        const double expansion = mask.expansion.scalar(time);
        if (std::abs(expansion) <= 1.0e-6)
            return path;
        const std::vector<PathContour> flat = flatten(path, tolerance);
        StrokeStyle band;
        band.width = std::abs(expansion) * 2.0;
        band.join = PathJoin::round;
        band.cap = PathCap::round;
        std::vector<PathContour> closed = flat;
        for (PathContour& contour : closed)
            contour.closed = true;
        const std::array<PolygonSet, 2> sets{
            PolygonSet{polygons(flat), false},
            PolygonSet{polygons(stroke_outline(closed, band, tolerance)), false},
        };
        const bool grow = expansion > 0.0;
        CubicPath result;
        for (const SweepLevel& level :
             sweep(sets, [grow](const std::span<const std::uint8_t> inside) {
                 const bool in = inside[0] != 0U;
                 const bool ring = inside[1] != 0U;
                 return (grow ? in || ring : in && !ring) ? 1.0 : 0.0;
             })) {
            for (const std::vector<Point>& loop : region_outline(level.trapezoids)) {
                Contour contour;
                contour.closed = true;
                contour.start = loop.front();
                for (std::size_t index = 1U; index <= loop.size(); ++index) {
                    const Point to = loop[index % loop.size()];
                    const Point from = loop[index - 1U];
                    contour.segments.push_back(Cubic{from, to, to});
                }
                result.push_back(std::move(contour));
            }
        }
        return result;
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

    /**
     * The layer's masks drawn apart as a coverage layer the layer keeps only where it covers
     * (destination-in): each mask white in a layer of its own, blurred by its feather and
     * combined with those before it by its mode. A lone mask is that layer itself.
     */
    void raster_masks(const model::Layer& layer, const double time, const Affine& world) {
        std::vector<const model::Mask*> active;
        for (const model::Mask& mask : layer.masks) {
            if (mask.mode != 'n')
                active.push_back(&mask);
        }
        if (active.empty())
            return;
        const double scale = world.mean_scale();
        const double tolerance = layer_tolerance_ / (scale > 1.0e-9 ? scale : 1.0);
        const double along_x = std::hypot(world.a, world.b);
        const double along_y = std::hypot(world.c, world.d);
        const char first = active.front()->mode;
        const bool start_full = first == 's' || first == 'i' || first == 'd';
        // Feather is a Gaussian of sigma 0.38 of it (Skottie's), reaching three sigmas.
        const auto feather = [&](const model::Mask& mask) {
            const model::Vec2 value = mask.feather.point(time);
            std::vector<LayerFilter> filters;
            if (std::abs(value.x) > 1.0e-6 || std::abs(value.y) > 1.0e-6)
                filters.push_back(blur_filter(std::abs(value.x) * 1.14 * along_x,
                                              std::abs(value.y) * 1.14 * along_y));
            return filters;
        };
        const std::optional<Affine> to_layer = world.inverse();
        if (!to_layer.has_value())
            return;
        // Everything a layer may draw, with room for any blur to read past it.
        const Rect cover = expanded(sink_.bounds(), sink_.bounds().width + sink_.bounds().height);
        const auto cover_contour = [&] {
            Contour contour;
            contour.closed = true;
            const std::array<Point, 4> corners{
                Point{cover.x, cover.y}, Point{cover.right(), cover.y},
                Point{cover.right(), cover.bottom()}, Point{cover.x, cover.bottom()}};
            contour.start = to_layer->apply(corners[0]);
            for (std::size_t index = 1U; index <= corners.size(); ++index) {
                const Point from = to_layer->apply(corners[index - 1U]);
                const Point to = to_layer->apply(corners[index % corners.size()]);
                contour.segments.push_back(Cubic{from, to, to});
            }
            return contour;
        };
        const auto fill = [&](const model::Mask& mask) {
            Drawable white;
            white.to_widget = world;
            white.color = RenderColor{255U, 255U, 255U, 255U};
            white.path = mask_path(mask, time, tolerance);
            if (mask.inverted) {
                white.path.push_back(cover_contour());
                white.rule = PathFillRule::evenodd;
            }
            sink_.draw(white, {});
        };
        const auto opacity = [time](const model::Mask& mask) {
            return std::clamp(mask.opacity.scalar(time) / 100.0, 0.0, 1.0);
        };
        if (active.size() == 1U && !start_full) {
            sink_.push_layer(LayerSpec{LayerComposite::destination_in, opacity(*active.front()),
                                       feather(*active.front())});
            fill(*active.front());
            sink_.pop_layer();
            return;
        }
        sink_.push_layer(LayerSpec{LayerComposite::destination_in, 1.0, {}});
        if (start_full) {
            Drawable white;
            white.color = RenderColor{255U, 255U, 255U, 255U};
            white.path.push_back(
                rectangle_contour(Point{cover.x + cover.width * 0.5, cover.y + cover.height * 0.5},
                                  Point{cover.width, cover.height}, 0.0, 1));
            sink_.draw(white, {});
        }
        for (const model::Mask* mask : active) {
            sink_.push_layer(LayerSpec{mask_composite(mask->mode), opacity(*mask), feather(*mask)});
            fill(*mask);
            sink_.pop_layer();
        }
        sink_.pop_layer();
    }

    /** The matte layer of `layers[position]`, if it has one. */
    [[nodiscard]] static std::optional<std::size_t>
    matte_source(const std::vector<model::Layer>& layers, const std::size_t position) {
        const model::Layer& layer = layers[position];
        if (layer.matte == model::MatteMode::none)
            return std::nullopt;
        std::optional<std::size_t> source;
        if (layer.matte_parent.has_value()) {
            for (std::size_t index = 0U; index < layers.size(); ++index) {
                if (layers[index].index == layer.matte_parent)
                    source = index;
            }
        } else if (position > 0U) {
            source = position - 1U;
        }
        if (source == position)
            return std::nullopt;
        return source;
    }

    void layer_content(const std::vector<model::Layer>& layers, const std::size_t position,
                       const std::vector<Affine>& worlds, const double frame,
                       const Affine& to_widget, const Constraints& inherited,
                       const std::size_t depth) {
        const model::Layer& layer = layers[position];
        if (!visible(layer, frame))
            return;
        if (layer.type != model::LayerType::shape && layer.type != model::LayerType::solid &&
            layer.type != model::LayerType::precomp && layer.type != model::LayerType::image &&
            layer.type != model::LayerType::text) {
            return;
        }
        const double time = layer_time(layer, frame);
        const double opacity = std::clamp(layer.transform.opacity.scalar(time) / 100.0, 0.0, 1.0);
        if (opacity <= 0.0)
            return;
        const Affine world = to_widget * worlds[position];

        // What the layer draws, gathered first so that its paints can be counted.
        std::vector<Drawable> drawables;
        switch (layer.type) {
        case model::LayerType::shape:
            drawables = shapes(layer, time, world);
            break;
        case model::LayerType::text:
            drawables = text(layer, time, world);
            break;
        case model::LayerType::solid:
            if (layer.width > 0.0 && layer.height > 0.0) {
                Drawable& drawable = drawables.emplace_back();
                drawable.to_widget = world;
                drawable.path.push_back(
                    rectangle_contour(Point{layer.width * 0.5, layer.height * 0.5},
                                      Point{layer.width, layer.height}, 0.0, 1));
                drawable.color =
                    RenderColor{channel(layer.solid_color[0]), channel(layer.solid_color[1]),
                                channel(layer.solid_color[2]), 255U};
            }
            break;
        default:
            break;
        }
        const model::ImageAsset* image = layer.type == model::LayerType::image &&
                                                 layer.image.has_value() &&
                                                 *layer.image < animation_.images.size() &&
                                                 !animation_.images[*layer.image].texture.empty()
                                             ? &animation_.images[*layer.image]
                                             : nullptr;
        if (layer.type == model::LayerType::image && image == nullptr)
            return;

        const bool masked = std::ranges::any_of(
            layer.masks, [](const model::Mask& mask) { return mask.mode != 'n'; });
        // Feathered masks, and any mask over an image, are drawn apart as coverage: geometry
        // can neither blur an edge nor cut an image.
        const bool raster = masked && (feathered(layer, time) || image != nullptr);
        Constraints constraints = inherited;
        std::optional<Constraint> mask_constraint;
        if (masked && !raster) {
            mask_constraint = masks(layer, time, world);
            if (mask_constraint.has_value())
                constraints.push_back(&*mask_constraint);
        }
        const std::vector<LayerFilter> filters =
            layer_filters(layer, time, std::hypot(world.a, world.b), std::hypot(world.c, world.d),
                          world.mean_scale());
        const LayerComposite composite = blend_composite(layer.blend_mode);
        const std::optional<std::size_t> matte = matte_source(layers, position);
        const bool precomposed = layer.type == model::LayerType::precomp;
        const bool blending_inside = precomposed && layer.precomp.has_value() &&
                                     *layer.precomp < animation_.precomps.size() &&
                                     animation_.precomps[*layer.precomp].blends;
        // Opacity fades the layer as one picture: paints over each other inside it do not show
        // through each other. One paint fades the same either way, so it needs no layer.
        const bool overlapping = precomposed || drawables.size() > 1U;
        const bool apart = composite != LayerComposite::source_over || !filters.empty() ||
                           matte.has_value() || raster || blending_inside ||
                           (opacity < 1.0 && overlapping);
        if (apart) {
            // Effects act on the masked layer before its matte cuts it: with a matte they are a
            // layer of their own inside it.
            const bool split = matte.has_value() && !filters.empty();
            sink_.push_layer(
                LayerSpec{composite, opacity, split ? std::vector<LayerFilter>{} : filters});
            if (split)
                sink_.push_layer(LayerSpec{LayerComposite::source_over, 1.0, filters});
            content(layer, time, world, drawables, image, constraints, depth);
            if (raster)
                raster_masks(layer, time, world);
            if (split)
                sink_.pop_layer();
            if (matte.has_value()) {
                sink_.push_layer(LayerSpec{matte_composite(layer.matte), 1.0, {}});
                layer_content(layers, *matte, worlds, frame, to_widget, {}, depth + 1U);
                sink_.pop_layer();
            }
            sink_.pop_layer();
            return;
        }
        if (opacity < 1.0)
            sink_.push_opacity(opacity);
        content(layer, time, world, drawables, image, constraints, depth);
        if (opacity < 1.0)
            sink_.pop_opacity();
    }

    void content(const model::Layer& layer, const double time, const Affine& world,
                 const std::vector<Drawable>& drawables, const model::ImageAsset* const image,
                 const Constraints& constraints, const std::size_t depth) {
        for (const Drawable& drawable : drawables)
            sink_.draw(drawable, constraints);
        if (image != nullptr) {
            sink_.draw_image(image->texture, Point{image->width, image->height}, world,
                             constraints);
        }
        if (layer.type == model::LayerType::precomp)
            precomp(layer, time, world, constraints, depth);
    }

    [[nodiscard]] std::vector<Drawable> shapes(const model::Layer& layer, const double time,
                                               const Affine& world) const {
        std::vector<Drawable> result;
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
            result.push_back(std::move(drawable));
        }
        return result;
    }

    /** The composition's exported outline of `character` in `font`, if it has one. */
    [[nodiscard]] const model::GlyphShape* exported_glyph(const std::string& character,
                                                          const model::Font* const font) const {
        for (const model::GlyphShape& glyph : animation_.glyphs) {
            if (glyph.character == character &&
                (font == nullptr || (glyph.family == font->family && glyph.style == font->style)))
                return &glyph;
        }
        return nullptr;
    }

    /** An exported glyph's outline, its paths only, `scale` units per its own. */
    [[nodiscard]] CubicPath exported_path(const model::GlyphShape& glyph,
                                          const double scale) const {
        ShapeState state;
        state.frame = 0.0;
        const std::vector<std::size_t> paths =
            evaluate_group(glyph.shapes, Affine{}, 1.0, state, 0U);
        CubicPath result;
        for (const std::size_t index : paths) {
            CubicPath path = state.entries[index].path;
            transform(path, Affine::scale(scale, scale) * state.entries[index].to_layer);
            for (Contour& contour : path) {
                contour.closed = true;
                result.push_back(std::move(contour));
            }
        }
        return result;
    }

    /**
     * A text layer's letters as fills and strokes: its document at `time` laid out line by line
     * (point text from its origin, paragraph text wrapped in its box), each letter moved, turned,
     * scaled, faded and recoloured by the animators whose selectors pick it.
     */
    [[nodiscard]] std::vector<Drawable> text(const model::Layer& layer, const double time,
                                             const Affine& world) const {
        std::vector<Drawable> result;
        const model::TextDocument* document = layer.text.document(time);
        if (document == nullptr || document->text.empty())
            return result;
        const model::Font* font = nullptr;
        for (const model::Font& entry : animation_.fonts) {
            if (entry.name == document->font)
                font = &entry;
        }
        const double size = document->size;
        // The system's font is looked up only for letters the file exports no outline of; while
        // its names are still being read the layer draws nothing.
        std::optional<font::SystemFont> face;
        double font_scale = size / 1000.0;
        bool resolved = false;
        const auto find_face = [&]() -> bool {
            if (resolved)
                return true;
            ResolvedFont found = resolve_font(font);
            if (found.pending)
                return false;
            face = std::move(found.face);
            if (face.has_value() && face->face.units_per_em() > 0U)
                font_scale = size / static_cast<double>(face->face.units_per_em());
            resolved = true;
            return true;
        };
        std::string content = document->text;
        if (document->caps != 0) {
            for (char& c : content)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        const double tracking = document->tracking / 1000.0 * size;

        // Letters with their advances, line by line.
        std::vector<Letter> letters;
        std::vector<double> line_widths(1U, 0.0);
        std::size_t line = 0U;
        std::size_t word = 0U;
        double x = 0.0;
        std::uint16_t previous = 0U;
        bool previous_space = true;
        const double box_width = document->box_size.has_value() ? document->box_size->x : 0.0;
        std::size_t line_start = 0U;
        const auto break_line = [&] {
            line_widths.back() = std::max(0.0, x - tracking);
            line_widths.push_back(0.0);
            ++line;
            x = 0.0;
            previous = 0U;
            line_start = letters.size();
        };
        for (const std::uint32_t code_point : code_points(content)) {
            if (code_point == '\r' || code_point == '\n' || code_point == 0x03U) {
                break_line();
                previous_space = true;
                continue;
            }
            Letter letter;
            letter.space = code_point == ' ' || code_point == '\t';
            if (!letter.space && previous_space && !letters.empty())
                ++word;
            previous_space = letter.space;
            std::string character;
            {
                const std::uint32_t single = code_point;
                if (single < 0x80U) {
                    character.push_back(static_cast<char>(single));
                } else if (single < 0x800U) {
                    character.push_back(static_cast<char>(0xC0U | (single >> 6U)));
                    character.push_back(static_cast<char>(0x80U | (single & 0x3FU)));
                } else if (single < 0x10000U) {
                    character.push_back(static_cast<char>(0xE0U | (single >> 12U)));
                    character.push_back(static_cast<char>(0x80U | ((single >> 6U) & 0x3FU)));
                    character.push_back(static_cast<char>(0x80U | (single & 0x3FU)));
                } else {
                    character.push_back(static_cast<char>(0xF0U | (single >> 18U)));
                    character.push_back(static_cast<char>(0x80U | ((single >> 12U) & 0x3FU)));
                    character.push_back(static_cast<char>(0x80U | ((single >> 6U) & 0x3FU)));
                    character.push_back(static_cast<char>(0x80U | (single & 0x3FU)));
                }
            }
            if (const model::GlyphShape* exported = exported_glyph(character, font);
                exported != nullptr) {
                letter.advance = exported->width * size / 100.0;
                letter.path = exported_path(*exported, size / 100.0);
                previous = 0U;
            } else if (!find_face()) {
                return result;
            } else if (face.has_value()) {
                const font::OpenTypeFont* source = &face->face;
                std::optional<font::SystemFont> fallback;
                std::uint16_t glyph = source->glyph_id(code_point);
                if (glyph == 0U && !letter.space) {
                    fallback = font::system_font_for(code_point);
                    if (fallback.has_value()) {
                        source = &fallback->face;
                        glyph = source->glyph_id(code_point);
                    }
                }
                const double scale =
                    source->units_per_em() > 0U ? size / source->units_per_em() : font_scale;
                if (source == &face->face && previous != 0U)
                    x += source->pair_advance_adjustment(previous, glyph) * scale;
                letter.advance = source->horizontal_advance(glyph) * scale;
                letter.path = glyph_path(*source, glyph, scale);
                previous = source == &face->face ? glyph : std::uint16_t{0U};
            } else {
                letter.advance = size * 0.5;
            }
            // Paragraph text wraps at the box's width, a word at a time.
            if (box_width > 0.0 && !letter.space && x + letter.advance > box_width &&
                letters.size() > line_start) {
                std::size_t cut = letters.size();
                while (cut > line_start && !letters[cut - 1U].space)
                    --cut;
                if (cut > line_start) {
                    // Move the word started after the last space onto a new line.
                    std::vector<Letter> moved(letters.begin() + static_cast<std::ptrdiff_t>(cut),
                                              letters.end());
                    letters.resize(cut);
                    double kept = 0.0;
                    for (std::size_t index = line_start; index < cut; ++index) {
                        if (!letters[index].space)
                            kept = letters[index].x + letters[index].advance;
                    }
                    x = kept + tracking;
                    break_line();
                    for (Letter& entry : moved) {
                        entry.x = x;
                        entry.line = line;
                        x += entry.advance + tracking;
                        letters.push_back(std::move(entry));
                    }
                } else {
                    break_line();
                }
            }
            letter.x = x;
            letter.line = line;
            letter.word = word;
            x += letter.advance + tracking;
            letters.push_back(std::move(letter));
        }
        line_widths.back() = std::max(0.0, x - tracking);
        if (letters.empty())
            return result;
        std::size_t counted = 0U;
        for (std::size_t index = 0U; index < letters.size(); ++index) {
            letters[index].index = index;
            letters[index].index_without_spaces = counted;
            if (!letters[index].space)
                ++counted;
        }

        // Lines placed by justification, from the origin (point text) or in the box.
        if ((font == nullptr || font->ascent <= 0.0) && !find_face())
            return result;
        const double ascent = font != nullptr && font->ascent > 0.0 ? font->ascent / 100.0 * size
                              : face.has_value() ? face->face.ascender() * font_scale
                                                 : size * 0.75;
        for (Letter& letter : letters) {
            const double width = line_widths[letter.line];
            double shift = 0.0;
            const int justify = document->justify;
            if (document->box_size.has_value()) {
                const double room = document->box_size->x - width;
                shift = justify == 1 || justify == 4   ? room
                        : justify == 2 || justify == 5 ? room * 0.5
                                                       : 0.0;
                shift += document->box_position.x;
            } else {
                shift = justify == 1 || justify == 4   ? -width
                        : justify == 2 || justify == 5 ? -width * 0.5
                                                       : 0.0;
            }
            letter.x += shift;
            letter.y = static_cast<double>(letter.line) * document->line_height -
                       document->baseline_shift +
                       (document->box_size.has_value() ? document->box_position.y + ascent : 0.0);
        }

        const std::array<double, 3> no_color{0.0, 0.0, 0.0};
        const model::Vec2 alignment = layer.text.alignment.point(time);
        // The units each selector counts: letters, letters but spaces, words or lines.
        const auto unit_total = [&](const int based_on) {
            switch (based_on) {
            case 2:
                return static_cast<double>(counted);
            case 3:
                return static_cast<double>(letters.back().word + 1U);
            case 4:
                return static_cast<double>(letters.back().line + 1U);
            default:
                return static_cast<double>(letters.size());
            }
        };
        const auto unit_index = [](const Letter& letter, const int based_on) {
            switch (based_on) {
            case 2:
                return static_cast<double>(letter.index_without_spaces);
            case 3:
                return static_cast<double>(letter.word);
            case 4:
                return static_cast<double>(letter.line);
            default:
                return static_cast<double>(letter.index);
            }
        };
        double tracking_shift = 0.0;
        std::size_t tracking_line = 0U;
        for (Letter& letter : letters) {
            if (letter.line != tracking_line) {
                tracking_line = letter.line;
                tracking_shift = 0.0;
            }
            Point position{0.0, 0.0};
            Point anchor{0.0, 0.0};
            Point scale{1.0, 1.0};
            double rotation = 0.0;
            double alpha = 1.0;
            std::array<double, 3> fill = document->fill.value_or(no_color);
            std::array<double, 3> stroke = document->stroke.value_or(no_color);
            double stroke_width = document->stroke_width;
            double extra_tracking = 0.0;
            for (const model::TextAnimator& animator : layer.text.animators) {
                const int based_on = animator.selector.based_on;
                if (based_on == 2 && letter.space)
                    continue;
                const double amount = selector_amount(
                    animator.selector, time, unit_index(letter, based_on), unit_total(based_on));
                if (amount == 0.0)
                    continue;
                if (animator.position.has_value()) {
                    const model::Vec2 value = animator.position->point(time);
                    position.x += value.x * amount;
                    position.y += value.y * amount;
                }
                if (animator.anchor.has_value()) {
                    const model::Vec2 value = animator.anchor->point(time);
                    anchor.x += value.x * amount;
                    anchor.y += value.y * amount;
                }
                if (animator.scale.has_value()) {
                    const model::Vec2 value = animator.scale->point(time);
                    scale.x *= 1.0 + (value.x / 100.0 - 1.0) * amount;
                    scale.y *= 1.0 + (value.y / 100.0 - 1.0) * amount;
                }
                if (animator.rotation.has_value())
                    rotation += animator.rotation->scalar(time) * amount;
                if (animator.opacity.has_value())
                    alpha *= 1.0 + (animator.opacity->scalar(time) / 100.0 - 1.0) * amount;
                const auto recolor = [&](const std::optional<model::Property>& property,
                                         std::array<double, 3>& color) {
                    if (!property.has_value())
                        return;
                    std::array<double, 4> value{0.0, 0.0, 0.0, 1.0};
                    property->evaluate(time, value);
                    const double weight = std::clamp(amount, 0.0, 1.0);
                    for (std::size_t index = 0U; index < 3U; ++index)
                        color[index] += (value[index] - color[index]) * weight;
                };
                recolor(animator.fill_color, fill);
                recolor(animator.stroke_color, stroke);
                if (animator.stroke_width.has_value())
                    stroke_width += animator.stroke_width->scalar(time) * amount;
                if (animator.tracking.has_value())
                    extra_tracking += animator.tracking->scalar(time) / 1000.0 * size * amount;
            }
            letter.x += tracking_shift;
            tracking_shift += extra_tracking;
            if (letter.space || letter.path.empty() || alpha <= 0.0)
                continue;
            // A letter turns and scales about its anchor: the middle of its advance on the
            // baseline, moved by the grouping alignment (percent of its advance and size).
            const Point pivot{letter.x + letter.advance * 0.5 +
                                  alignment.x / 100.0 * letter.advance,
                              letter.y + alignment.y / 100.0 * size};
            const Affine placement =
                Affine::translate(pivot.x + position.x, pivot.y + position.y) *
                Affine::rotate(rotation) * Affine::scale(scale.x, scale.y) *
                Affine::translate(letter.x - pivot.x - anchor.x, letter.y - pivot.y - anchor.y);
            const auto paint = [&](const std::array<double, 3>& color, const bool as_stroke) {
                Drawable drawable;
                drawable.to_widget = world * placement;
                drawable.path = letter.path;
                drawable.color = RenderColor{channel(color[0]), channel(color[1]),
                                             channel(color[2]), channel(alpha)};
                drawable.stroke = as_stroke;
                if (as_stroke) {
                    drawable.style.width = stroke_width;
                    drawable.style.join = PathJoin::miter;
                    drawable.style.miter_limit = 4.0;
                }
                result.push_back(std::move(drawable));
            };
            const bool stroked = document->stroke.has_value() && stroke_width > 0.0;
            // The stroke over the fill (`of`), or under it.
            if (stroked && !document->stroke_over_fill)
                paint(stroke, true);
            if (document->fill.has_value())
                paint(fill, false);
            if (stroked && document->stroke_over_fill)
                paint(stroke, true);
        }
        return result;
    }

    void precomp(const model::Layer& layer, const double time, const Affine& world,
                 const Constraints& constraints, const std::size_t depth) {
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
                sink_.push_clip(Rect{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x),
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
        composition(animation_.precomps[*layer.precomp], inner, world, inner_constraints,
                    depth + 1U);
        if (clipped)
            sink_.pop_clip();
    }

    const model::Animation& animation_;
    double layer_tolerance_ = flatness;
    Sink& sink_;
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
    // Layers drawn apart reach past the box by as far as the animation's effects can read.
    const double margin = animation.filter_reach > 0.0
                              ? std::ceil(animation.filter_reach * to_widget.mean_scale()) + 2.0
                              : 0.0;
    Sink sink(output, clip, margin);
    Renderer renderer(animation, flatness, sink);
    // An animation that blends, or whose effects reach past what they draw, is drawn as one
    // layer the widget's box cuts: its layers blend with each other and not with the page.
    if (animation.layered) {
        sink.push_layer(LayerSpec{}, true);
        renderer.composition(animation.root, frame, to_widget, {}, 0U);
        sink.pop_layer();
        return;
    }
    sink.push_clip(clip);
    renderer.composition(animation.root, frame, to_widget, {}, 0U);
    sink.pop_clip();
}

} // namespace strata::ui::lottie
