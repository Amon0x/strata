#include "font/vector_glyph.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include <strata/vector_text.hpp>

namespace strata::font {
namespace {

/** Bands a glyph is split into: few enough that a band is not empty, at most this many. */
constexpr std::uint32_t maximum_bands = 32U;
/** Segments a band holds on average when a glyph is split into bands. */
constexpr std::size_t segments_per_band = 6U;

void flatten(const GlyphSegment& segment, const double tolerance,
             std::vector<VectorSegment>& segments) {
    const auto emit = [&segments](const double x0, const double y0, const double x1,
                                  const double y1) {
        const VectorSegment piece{static_cast<float>(x0), static_cast<float>(y0),
                                  static_cast<float>(x1), static_cast<float>(y1)};
        if (piece.x0 != piece.x1 || piece.y0 != piece.y1)
            segments.push_back(piece);
    };
    if (segment.kind == GlyphSegmentKind::line) {
        emit(segment.from.x, segment.from.y, segment.to.x, segment.to.y);
        return;
    }
    // A quadratic strays from its chord by a quarter of |p0 - 2 p1 + p2|; split in n, by that
    // over 4 n^2.
    const double dx = segment.from.x - 2.0 * segment.control.x + segment.to.x;
    const double dy = segment.from.y - 2.0 * segment.control.y + segment.to.y;
    const double deviation = std::sqrt(dx * dx + dy * dy) * 0.25;
    const int pieces =
        std::clamp(static_cast<int>(std::ceil(std::sqrt(deviation / tolerance))), 1, 64);
    double previous_x = segment.from.x;
    double previous_y = segment.from.y;
    for (int index = 1; index <= pieces; ++index) {
        const double t = static_cast<double>(index) / static_cast<double>(pieces);
        const double u = 1.0 - t;
        const double x =
            u * u * segment.from.x + 2.0 * u * t * segment.control.x + t * t * segment.to.x;
        const double y =
            u * u * segment.from.y + 2.0 * u * t * segment.control.y + t * t * segment.to.y;
        emit(previous_x, previous_y, x, y);
        previous_x = x;
        previous_y = y;
    }
}

/** Splits the glyph's height into bands of equal height, each holding the segments it meets. */
void assign_bands(VectorGlyph& glyph) {
    glyph.band_offsets.clear();
    glyph.band_segments.clear();
    const auto sloped = static_cast<std::size_t>(
        std::count_if(glyph.segments.begin(), glyph.segments.end(),
                      [](const VectorSegment& segment) { return segment.y0 != segment.y1; }));
    const auto bands = static_cast<std::uint32_t>(
        std::clamp<std::size_t>(sloped / segments_per_band, 1U, maximum_bands));
    const double height = static_cast<double>(glyph.top) - static_cast<double>(glyph.bottom);
    const double band_height = height > 0.0 ? height / static_cast<double>(bands) : 1.0;
    std::vector<std::vector<std::uint32_t>> members(bands);
    for (std::uint32_t index = 0U; index < glyph.segments.size(); ++index) {
        const VectorSegment& segment = glyph.segments[index];
        // A horizontal segment covers no area in a band; it serves only a turned glyph.
        if (segment.y0 == segment.y1)
            continue;
        const double low = std::min(segment.y0, segment.y1) - static_cast<double>(glyph.bottom);
        const double high = std::max(segment.y0, segment.y1) - static_cast<double>(glyph.bottom);
        const auto first = static_cast<std::uint32_t>(
            std::clamp(std::floor(low / band_height), 0.0, static_cast<double>(bands - 1U)));
        const auto last = static_cast<std::uint32_t>(
            std::clamp(std::floor(high / band_height), 0.0, static_cast<double>(bands - 1U)));
        for (std::uint32_t band = first; band <= last; ++band)
            members[band].push_back(index);
    }
    glyph.band_offsets.reserve(bands + 1U);
    glyph.band_offsets.push_back(0U);
    for (const std::vector<std::uint32_t>& band : members) {
        glyph.band_segments.insert(glyph.band_segments.end(), band.begin(), band.end());
        glyph.band_offsets.push_back(static_cast<std::uint32_t>(glyph.band_segments.size()));
    }
}

/**
 * Where the grid fit puts a font-unit height, in device pixels up from a baseline on a whole
 * pixel, `scale` device pixels a font unit: each hint moves whole so its anchor lands on the
 * nearest whole pixel, never below the hint under it, and what lies between hints moves in
 * proportion.
 */
[[nodiscard]] double fitted_height(const std::vector<VectorHint>& hints, const double scale,
                                   const double font_y) {
    const double unfitted = font_y * scale;
    double previous_source = 0.0;
    double previous_target = 0.0;
    bool first = true;
    for (const VectorHint& hint : hints) {
        const double low = static_cast<double>(hint.low) * scale;
        const double high = static_cast<double>(hint.high) * scale;
        const double low_shift = std::round(low) - low;
        const double high_shift = std::round(high) - high;
        double shift = hint.anchor == 1U                             ? low_shift
                       : hint.anchor == 2U                           ? high_shift
                       : std::abs(low_shift) <= std::abs(high_shift) ? low_shift
                                                                     : high_shift;
        if (!first && low + shift < previous_target)
            shift = previous_target + (low - previous_source) - low;
        const double low_target = low + shift;
        const double high_target = high + shift;
        if (first) {
            if (unfitted <= low)
                return unfitted + shift;
        } else if (unfitted <= low) {
            const double span = low - previous_source;
            const double t = span > 0.0 ? (unfitted - previous_source) / span : 0.0;
            return previous_target + (low_target - previous_target) * t;
        }
        if (unfitted <= high)
            return unfitted + shift;
        previous_source = high;
        previous_target = high_target;
        first = false;
    }
    return first ? unfitted : unfitted + previous_target - previous_source;
}

/** The glyph's bands that meet the device heights [low, high] (y up). */
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
band_range(const VectorGlyph& glyph, const double font_low, const double font_high) noexcept {
    const std::uint32_t bands = glyph.band_count();
    const double height = static_cast<double>(glyph.top) - static_cast<double>(glyph.bottom);
    if (bands == 0U || !(height > 0.0))
        return {0U, bands};
    const double band_height = height / static_cast<double>(bands);
    const auto band = [&](const double y) {
        const double index = std::floor((y - static_cast<double>(glyph.bottom)) / band_height);
        return static_cast<std::uint32_t>(std::clamp(index, 0.0, static_cast<double>(bands - 1U)));
    };
    return {band(font_low), band(font_high) + 1U};
}

/**
 * The signed area of the outline in the device column [x0, x0 + width] x [low, high], each band
 * contributing only within its own heights, so a segment in several bands is counted once.
 */
[[nodiscard]] double column_area_between(const VectorGlyph& glyph, const VectorPlacement& placement,
                                         const double x0, const double width, const double row_low,
                                         const double row_high);

/**
 * The coverage of the device column [x0, x0 + width] x [y, y + 1]: the area of the outline within
 * it over the column's area, clamped to one (rounding can carry a full column a hair over).
 */
[[nodiscard]] double column_coverage(const VectorGlyph& glyph, const VectorPlacement& placement,
                                     const double x0, const double width, const double y) {
    return std::min(std::abs(column_area_between(glyph, placement, x0, width, y, y + 1.0)) / width,
                    1.0);
}

double column_area_between(const VectorGlyph& glyph, const VectorPlacement& placement,
                           const double x0, const double width, const double row_low,
                           const double row_high) {
    const double font_low = (row_low - placement.pen_y) / placement.scale;
    const double font_high = (row_high - placement.pen_y) / placement.scale;
    const auto [first, last] = band_range(glyph, font_low, font_high);
    const double band_height =
        (static_cast<double>(glyph.top) - static_cast<double>(glyph.bottom)) /
        static_cast<double>(std::max(1U, glyph.band_count()));
    double area = 0.0;
    for (std::uint32_t band = first; band < last; ++band) {
        // Each segment counts within its band's heights alone, cut there in font units: a cut
        // segment's pieces add up to it under any transform, so one in several bands counts once.
        const double band_bottom = band == 0U
                                       ? -std::numeric_limits<double>::max()
                                       : static_cast<double>(glyph.bottom) + band_height * band;
        const double band_top = band == glyph.band_count() - 1U
                                    ? std::numeric_limits<double>::max()
                                    : static_cast<double>(glyph.bottom) + band_height * (band + 1U);
        for (std::uint32_t index = glyph.band_offsets[band]; index < glyph.band_offsets[band + 1U];
             ++index) {
            const VectorSegment& segment = glyph.segments[glyph.band_segments[index]];
            double ax = segment.x0;
            double ay = segment.y0;
            double bx = segment.x1;
            double by = segment.y1;
            const double low = std::max(std::min(ay, by), band_bottom);
            const double high = std::min(std::max(ay, by), band_top);
            if (high <= low)
                continue;
            const double slope = (bx - ax) / (by - ay);
            const double cut_from = std::clamp(ay, low, high);
            const double cut_to = std::clamp(by, low, high);
            ax = ax + (cut_from - ay) * slope;
            bx = segment.x0 + (cut_to - segment.y0) * slope;
            ay = cut_from;
            by = cut_to;
            area += host::vector_segment_area(
                placement.pen_x + ax * placement.scale, placement.pen_y + ay * placement.scale,
                placement.pen_x + bx * placement.scale, placement.pen_y + by * placement.scale, x0,
                width, row_low, row_high);
        }
    }
    return area;
}

/** Where a segment is at height y, exactly its end at an end's height. */
[[nodiscard]] double x_at(const VectorSegment& segment, const double y) noexcept {
    if (y == static_cast<double>(segment.y0))
        return segment.x0;
    if (y == static_cast<double>(segment.y1))
        return segment.x1;
    return segment.x0 + (y - segment.y0) * (static_cast<double>(segment.x1) - segment.x0) /
                            (static_cast<double>(segment.y1) - segment.y0);
}

/**
 * The boundary of what an outline covers (nonzero winding), wound one way (inside on the left):
 * overlapping contours (a variable font's pieces, a contour crossing itself) merged, so each
 * point inside is covered once and a pixel's coverage is the area of the outline within it,
 * exactly. Between consecutive heights where a segment ends or two cross, the order of the
 * segments along a row is fixed: a segment there is boundary where it parts covered from
 * uncovered; at each such height, horizontal boundary runs where covered below differs from
 * covered above. A segment's boundary pieces in consecutive slabs are joined back into one.
 */
[[nodiscard]] std::vector<VectorSegment>
covered_boundary(const std::vector<VectorSegment>& outline) {
    std::vector<std::size_t> sloped;
    std::vector<double> heights;
    for (std::size_t index = 0U; index < outline.size(); ++index) {
        const VectorSegment& segment = outline[index];
        if (segment.y0 == segment.y1)
            continue;
        sloped.push_back(index);
        heights.push_back(segment.y0);
        heights.push_back(segment.y1);
    }
    // Where two segments cross, the order along a row changes.
    for (std::size_t first = 0U; first < sloped.size(); ++first) {
        const VectorSegment& a = outline[sloped[first]];
        const double a_low = std::min(a.y0, a.y1);
        const double a_high = std::max(a.y0, a.y1);
        const double a_left = std::min(a.x0, a.x1);
        const double a_right = std::max(a.x0, a.x1);
        for (std::size_t second = first + 1U; second < sloped.size(); ++second) {
            const VectorSegment& b = outline[sloped[second]];
            if (std::max(b.y0, b.y1) <= a_low || std::min(b.y0, b.y1) >= a_high ||
                std::max(b.x0, b.x1) < a_left || std::min(b.x0, b.x1) > a_right)
                continue;
            const double rx = static_cast<double>(a.x1) - a.x0;
            const double ry = static_cast<double>(a.y1) - a.y0;
            const double sx = static_cast<double>(b.x1) - b.x0;
            const double sy = static_cast<double>(b.y1) - b.y0;
            const double denominator = rx * sy - ry * sx;
            if (denominator == 0.0)
                continue;
            const double qx = static_cast<double>(b.x0) - a.x0;
            const double qy = static_cast<double>(b.y0) - a.y0;
            const double t = (qx * sy - qy * sx) / denominator;
            const double u = (qx * ry - qy * rx) / denominator;
            if (t > 0.0 && t < 1.0 && u > 0.0 && u < 1.0)
                heights.push_back(a.y0 + t * ry);
        }
    }
    std::ranges::sort(heights);
    heights.erase(std::ranges::unique(heights).begin(), heights.end());
    std::ranges::sort(sloped, {}, [&outline](const std::size_t index) {
        return std::min(outline[index].y0, outline[index].y1);
    });

    struct Piece final {
        double ax = 0.0;
        double ay = 0.0;
        double bx = 0.0;
        double by = 0.0;
    };
    std::vector<Piece> pieces;
    // A segment's last boundary piece: which, in which slab, and which way it runs.
    struct Open final {
        std::size_t piece = 0U;
        std::size_t slab = 0U;
        bool upward = false;
    };
    std::vector<std::optional<Open>> open(outline.size());
    struct Crossing final {
        double x = 0.0;
        std::size_t segment = 0U;
        int direction = 0;
    };
    std::vector<Crossing> row;
    std::vector<std::size_t> active;
    // Each slab's covered spans, as the boundary segments entering and leaving them.
    std::vector<std::pair<std::size_t, std::size_t>> below;
    std::vector<std::pair<std::size_t, std::size_t>> above;
    std::size_t next = 0U;
    const auto spans_at = [&outline](const std::vector<std::pair<std::size_t, std::size_t>>& spans,
                                     const double y) {
        std::vector<std::pair<double, double>> result;
        result.reserve(spans.size());
        for (const auto& [enter, leave] : spans)
            result.emplace_back(x_at(outline[enter], y), x_at(outline[leave], y));
        return result;
    };
    // Horizontal boundary at height y: covered above and not below runs right (inside on the
    // left, above); covered below and not above runs left.
    const auto horizontal = [&pieces](const std::vector<std::pair<double, double>>& under,
                                      const std::vector<std::pair<double, double>>& over,
                                      const double y) {
        std::vector<double> stops;
        for (const auto& [left, right] : under)
            stops.insert(stops.end(), {left, right});
        for (const auto& [left, right] : over)
            stops.insert(stops.end(), {left, right});
        std::ranges::sort(stops);
        stops.erase(std::ranges::unique(stops).begin(), stops.end());
        const auto inside = [](const std::vector<std::pair<double, double>>& spans,
                               const double x) {
            return std::ranges::any_of(spans, [x](const std::pair<double, double>& span) {
                return span.first < x && x < span.second;
            });
        };
        int run = 0;
        double run_from = 0.0;
        for (std::size_t index = 0U; index + 1U <= stops.size(); ++index) {
            int kind = 0;
            if (index + 1U < stops.size()) {
                const double middle = (stops[index] + stops[index + 1U]) * 0.5;
                const bool in_under = inside(under, middle);
                const bool in_over = inside(over, middle);
                kind = in_over && !in_under ? 1 : in_under && !in_over ? -1 : 0;
            }
            if (kind == run)
                continue;
            if (run != 0 && stops[index] - run_from > 1.0e-6) {
                pieces.push_back(run > 0 ? Piece{run_from, y, stops[index], y}
                                         : Piece{stops[index], y, run_from, y});
            }
            run = kind;
            run_from = stops[index];
        }
    };
    for (std::size_t slab = 0U; slab + 1U < heights.size(); ++slab) {
        const double low = heights[slab];
        const double high = heights[slab + 1U];
        const double middle = (low + high) * 0.5;
        std::erase_if(active, [&](const std::size_t index) {
            return std::max(outline[index].y0, outline[index].y1) <= low;
        });
        while (next < sloped.size() &&
               std::min(outline[sloped[next]].y0, outline[sloped[next]].y1) <= low)
            active.push_back(sloped[next++]);
        row.clear();
        for (const std::size_t index : active) {
            if (std::max(outline[index].y0, outline[index].y1) <= low)
                continue;
            row.push_back(Crossing{x_at(outline[index], middle), index,
                                   outline[index].y1 > outline[index].y0 ? 1 : -1});
        }
        std::ranges::sort(row, {}, &Crossing::x);
        above.clear();
        int winding = 0;
        std::size_t entered = 0U;
        for (const Crossing& crossing : row) {
            const int before = winding;
            winding += crossing.direction;
            if ((before == 0) == (winding == 0))
                continue;
            // Covered to its right: it runs down (inside on the left); to its left, up.
            const bool upward = winding == 0;
            if (!upward)
                entered = crossing.segment;
            else
                above.emplace_back(entered, crossing.segment);
            const VectorSegment& segment = outline[crossing.segment];
            const double low_x = x_at(segment, low);
            const double high_x = x_at(segment, high);
            std::optional<Open>& last = open[crossing.segment];
            if (last.has_value() && last->slab + 1U == slab && last->upward == upward) {
                Piece& piece = pieces[last->piece];
                if (upward) {
                    piece.bx = high_x;
                    piece.by = high;
                } else {
                    piece.ax = high_x;
                    piece.ay = high;
                }
                last->slab = slab;
                continue;
            }
            last = Open{pieces.size(), slab, upward};
            pieces.push_back(upward ? Piece{low_x, low, high_x, high}
                                    : Piece{high_x, high, low_x, low});
        }
        horizontal(spans_at(below, low), spans_at(above, low), low);
        std::swap(below, above);
        // The top of the last slab: covered below, nothing above.
        if (slab + 2U == heights.size())
            horizontal(spans_at(below, high), {}, high);
    }
    std::vector<VectorSegment> boundary;
    boundary.reserve(pieces.size());
    for (const Piece& piece : pieces) {
        boundary.push_back(VectorSegment{static_cast<float>(piece.ax), static_cast<float>(piece.ay),
                                         static_cast<float>(piece.bx),
                                         static_cast<float>(piece.by)});
    }
    return boundary;
}

} // namespace

FontHintZones font_hint_zones(const OpenTypeFont& font) {
    FontHintZones zones;
    zones.heights.push_back(0.0F);
    const auto measure = [&font, &zones](const char32_t code_point, const bool top) {
        const std::uint16_t glyph = font.glyph_id(code_point);
        if (glyph == 0U)
            return;
        const std::shared_ptr<const GlyphOutline> outline = font.glyph_outline(glyph);
        if (outline == nullptr || !outline->bounds.has_value())
            return;
        zones.heights.push_back(
            static_cast<float>(top ? outline->bounds->top : outline->bounds->bottom));
    };
    measure(U'x', true);
    measure(U'H', true);
    measure(U'd', true);
    measure(U'p', false);
    std::ranges::sort(zones.heights);
    zones.heights.erase(std::ranges::unique(zones.heights).begin(), zones.heights.end());
    return zones;
}

VectorGlyph build_vector_glyph(const GlyphOutline& outline, const std::uint16_t units_per_em,
                               const FontHintZones& zones, double tolerance) {
    VectorGlyph glyph;
    if (outline.empty() || !outline.bounds.has_value())
        return glyph;
    const double em = units_per_em > 0U ? static_cast<double>(units_per_em) : 1000.0;
    if (!(tolerance > 0.0))
        tolerance = em / 4096.0;
    // The outline flattened, then its contours merged where they overlap. A horizontal segment
    // covers no area upright (bands leave it out), but turned, it does.
    std::vector<VectorSegment> flattened;
    for (const GlyphContour& contour : outline.contours) {
        for (const GlyphSegment& segment : contour.segments)
            flatten(segment, tolerance, flattened);
    }
    glyph.segments = covered_boundary(flattened);
    const auto sloped = static_cast<std::size_t>(
        std::count_if(glyph.segments.begin(), glyph.segments.end(),
                      [](const VectorSegment& segment) { return segment.y0 != segment.y1; }));
    if (sloped == 0U)
        return VectorGlyph{};
    glyph.left = static_cast<float>(outline.bounds->left);
    glyph.bottom = static_cast<float>(outline.bounds->bottom);
    glyph.right = static_cast<float>(outline.bounds->right);
    glyph.top = static_cast<float>(outline.bounds->top);

    assign_bands(glyph);

    // Hint edges: the horizontal runs of the outline (stem and bar tops and bottoms, curve
    // extremes), an edge near one of the font's zones taken as that zone so round letters line
    // up with flat ones, and the zones the glyph spans.
    const double near_flat = std::max(tolerance * 2.0, em * 0.002);
    const double long_enough = em * 0.03;
    const double zone_reach = em * 0.025;
    struct Edge final {
        double y = 0.0;
        bool zone = false;
    };
    std::vector<Edge> edges;
    const auto add_edge = [&](const double y) {
        for (const float zone : zones.heights) {
            if (std::abs(y - static_cast<double>(zone)) <= zone_reach) {
                edges.push_back(Edge{zone, true});
                return;
            }
        }
        edges.push_back(Edge{y, false});
    };
    const auto consider = [&](const VectorSegment& segment) {
        const double dx = std::abs(static_cast<double>(segment.x1) - segment.x0);
        const double dy = std::abs(static_cast<double>(segment.y1) - segment.y0);
        if (dy <= near_flat && dx >= long_enough)
            add_edge((static_cast<double>(segment.y0) + segment.y1) * 0.5);
    };
    for (const VectorSegment& segment : glyph.segments)
        consider(segment);
    for (const float zone : zones.heights) {
        if (zone >= glyph.bottom - zone_reach && zone <= glyph.top + zone_reach)
            edges.push_back(Edge{zone, true});
    }
    std::ranges::sort(edges, {}, &Edge::y);
    // Edges closer than a small fraction of the em are one edge, a zone if either is.
    const double merge = em * 0.006;
    std::vector<Edge> merged;
    for (const Edge& edge : edges) {
        if (!merged.empty() && edge.y - merged.back().y <= merge) {
            if (edge.zone)
                merged.back() = edge;
            continue;
        }
        merged.push_back(edge);
    }
    // Neighbouring edges no farther apart than a thick stroke are a stem's bottom and top.
    const double stem = em * 0.13;
    for (std::size_t index = 0U; index < merged.size(); ++index) {
        const Edge& low = merged[index];
        if (index + 1U < merged.size() && merged[index + 1U].y - low.y <= stem) {
            const Edge& high = merged[index + 1U];
            glyph.hints.push_back(VectorHint{static_cast<float>(low.y), static_cast<float>(high.y),
                                             static_cast<std::uint8_t>(low.zone    ? 1U
                                                                       : high.zone ? 2U
                                                                                   : 0U)});
            ++index;
            continue;
        }
        glyph.hints.push_back(VectorHint{static_cast<float>(low.y), static_cast<float>(low.y),
                                         static_cast<std::uint8_t>(low.zone ? 1U : 0U)});
    }
    return glyph;
}

VectorGlyph fit_vector_glyph(const VectorGlyph& glyph, const double scale) {
    if (glyph.empty() || glyph.hints.empty() || !(scale > 0.0))
        return glyph;
    // The fit is linear between the hints' edges: a segment crossing one is cut there, and each
    // piece moves exactly.
    std::vector<double> knots;
    for (const VectorHint& hint : glyph.hints)
        knots.insert(knots.end(), {static_cast<double>(hint.low), static_cast<double>(hint.high)});
    std::ranges::sort(knots);
    knots.erase(std::ranges::unique(knots).begin(), knots.end());
    const auto fitted = [&](const double y) {
        return fitted_height(glyph.hints, scale, y) / scale;
    };
    VectorGlyph result;
    result.left = glyph.left;
    result.right = glyph.right;
    result.bottom = static_cast<float>(fitted(glyph.bottom));
    result.top = static_cast<float>(fitted(glyph.top));
    for (const VectorSegment& segment : glyph.segments) {
        const double low = std::min(segment.y0, segment.y1);
        const double high = std::max(segment.y0, segment.y1);
        double x0 = segment.x0;
        double y0 = segment.y0;
        const auto emit = [&](const double x1, const double y1) {
            const VectorSegment piece{static_cast<float>(x0), static_cast<float>(fitted(y0)),
                                      static_cast<float>(x1), static_cast<float>(fitted(y1))};
            if (piece.x0 != piece.x1 || piece.y0 != piece.y1)
                result.segments.push_back(piece);
            x0 = x1;
            y0 = y1;
        };
        if (low < high) {
            // The knots strictly inside, in the segment's own direction.
            const auto first = std::ranges::upper_bound(knots, low);
            const auto last = std::ranges::lower_bound(knots, high);
            const auto cut = [&](const double y) {
                emit(segment.x0 + (y - segment.y0) *
                                      (static_cast<double>(segment.x1) - segment.x0) /
                                      (static_cast<double>(segment.y1) - segment.y0),
                     y);
            };
            if (segment.y1 > segment.y0) {
                for (auto knot = first; knot != last; ++knot)
                    cut(*knot);
            } else {
                for (auto knot = last; knot != first; --knot)
                    cut(*(knot - 1));
            }
        }
        emit(segment.x1, segment.y1);
    }
    result.top = std::max(result.top, result.bottom);
    assign_bands(result);
    return result;
}

double vector_pixel_coverage(const VectorGlyph& glyph, const VectorPlacement& placement,
                             const double x, const double y) {
    if (glyph.empty())
        return 0.0;
    return column_coverage(glyph, placement, x, 1.0, y);
}

std::array<double, 3> vector_subpixel_coverage(const VectorGlyph& glyph,
                                               const VectorPlacement& placement, const double x,
                                               const double y) {
    std::array<double, 3> result{};
    if (glyph.empty())
        return result;
    // The pixel's thirds and two either side: each subpixel weighs its own third and two
    // neighbours each way.
    std::array<double, 7> thirds{};
    for (int index = 0; index < 7; ++index) {
        thirds[static_cast<std::size_t>(index)] = column_coverage(
            glyph, placement, x + static_cast<double>(index - 2) / 3.0, 1.0 / 3.0, y);
    }
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        double value = 0.0;
        for (std::size_t tap = 0U; tap < host::vector_lcd_filter.size(); ++tap)
            value += host::vector_lcd_filter[tap] * thirds[channel + tap];
        result[channel] = std::min(value, 1.0);
    }
    return result;
}

} // namespace strata::font
