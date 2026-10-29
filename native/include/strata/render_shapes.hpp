#pragma once

#include <algorithm>
#include <cmath>

namespace strata::host {

/**
 * Analytic shapes in the unified pixel stage: draw modes 7 (ellipse), 8 (ring sector) and 9 (line
 * segment). Each vertex's 16 draw-data floats describe the shape in its quad's local logical
 * pixels: floats 0-1 are the quad's size and 2-3 a reference point inside it (the ellipse or
 * sector centre, the segment's start), so the local position is `uv * size - reference`.
 *
 * - Ellipse: 4-5 the radii, 6 a stroke half width (zero fills the ellipse instead).
 * - Ring sector: 4 outer and 5 inner radius, 6 start and 7 sweep in radians (clockwise on screen
 *   from +x), 8 the ends: 0 cut straight across, 1 rounded (an arc stroked with round caps).
 * - Segment: 4-5 the end relative to the start, 6 half the stroke width, 7 the caps: 0 butt,
 *   1 round, 2 square.
 *
 * Coverage is the share of the pixel the shape covers, box filtered. Each distance is Euclidean and
 * comes with the direction it grows in; a device pixel's footprint across that direction is
 * `|gx| + |gy|`, where (gx, gy) is how much the distance changes per device pixel along x and y
 * (exactly one pixel for an axis-aligned edge, the square root of two at 45 degrees). A solid edge
 * covers `clamp(0.5 - d / footprint)`. A stroke is measured from its centreline: the overlap of its
 * band with the footprint, so a hairline thinner than a pixel draws proportionally lighter instead
 * of a full pixel wide, times the solid coverage past a straight end. The GPU pixel stage and every
 * CPU backend implement exactly these functions.
 */
constexpr int draw_mode_ellipse = 7;
constexpr int draw_mode_sector = 8;
constexpr int draw_mode_segment = 9;

/**
 * Where a point is relative to one analytic shape: a signed distance to a solid edge, or, for a
 * stroke, the distance from its centreline with the half width drawn around it, and for a
 * straight-ended stroke how far past its ends the point lies. Each distance carries the local
 * direction it grows in (its sign is irrelevant).
 */
struct ShapeDistance final {
    float distance = 0.0F;
    /** Negative for a solid shape: `distance` is then its edge, not a centreline. */
    float half_width = -1.0F;
    float normal_x = 1.0F;
    float normal_y = 0.0F;
    /** Distance past a straight stroke end (negative inside); very negative when it has none. */
    float beyond = -1.0e9F;
    float along_x = 1.0F;
    float along_y = 0.0F;
};

/** A unit vector along (x, y), or +x when it has no length. */
inline void direction(float& x, float& y) noexcept {
    const float length = std::sqrt(x * x + y * y);
    if (length > 1.0e-12F) {
        x /= length;
        y /= length;
    } else {
        x = 1.0F;
        y = 0.0F;
    }
}

inline float edge_coverage(const float distance, const float footprint) noexcept {
    return std::clamp(0.5F - distance / std::max(footprint, 1.0e-6F), 0.0F, 1.0F);
}

/** The share of a footprint a band `half_width` either side of a centreline `distance` away covers. */
inline float band_coverage(const float distance, const float half_width,
                           const float footprint) noexcept {
    const float scale = 1.0F / std::max(footprint, 1.0e-6F);
    const float centre = std::abs(distance) * scale;
    const float half = half_width * scale;
    return std::clamp(std::min(centre + half, 0.5F) - std::max(centre - half, -0.5F), 0.0F, 1.0F);
}

inline ShapeDistance ellipse_distance(const float x, const float y, const float radius_x,
                                      const float radius_y) noexcept {
    ShapeDistance result;
    if (std::abs(radius_x - radius_y) <= 1.0e-4F * std::max(radius_x, radius_y)) {
        result.distance = std::sqrt(x * x + y * y) - radius_x;
        result.normal_x = x;
        result.normal_y = y;
    } else {
        const float rx = std::max(radius_x, 1.0e-4F);
        const float ry = std::max(radius_y, 1.0e-4F);
        const float k0 = std::sqrt((x / rx) * (x / rx) + (y / ry) * (y / ry));
        const float gx = x / (rx * rx);
        const float gy = y / (ry * ry);
        const float k1 = std::sqrt(gx * gx + gy * gy);
        result.distance = k0 < 1.0e-6F ? -std::min(rx, ry) : k0 * (k0 - 1.0F) / k1;
        result.normal_x = gx;
        result.normal_y = gy;
    }
    direction(result.normal_x, result.normal_y);
    return result;
}

/**
 * A ring sector. With straight ends it is a solid: the ring's radial distance intersected with the
 * wedge's. With round ends it is a stroke of the arc through the ring's middle, measured from that
 * arc, and past its ends from its end points.
 */
inline ShapeDistance sector_distance(const float x, const float y, const float outer,
                                     const float inner, const float start, const float sweep,
                                     const bool round) noexcept {
    const float radius = std::sqrt(x * x + y * y);
    const float middle = (outer + inner) * 0.5F;
    const float half_width = (outer - inner) * 0.5F;
    ShapeDistance result;
    result.normal_x = x;
    result.normal_y = y;
    direction(result.normal_x, result.normal_y);
    if (sweep >= 6.2831850F) {
        if (round || inner > 0.0F) {
            result.distance = radius - middle;
            result.half_width = half_width;
        } else {
            result.distance = radius - outer;
        }
        return result;
    }
    // Turn the point so the sector's middle lies along +x, then fold it into the upper half.
    const float mid = start + sweep * 0.5F;
    const float c = std::cos(mid);
    const float s = std::sin(mid);
    const float qx = c * x + s * y;
    const float turned_y = -s * x + c * y;
    const float fold = turned_y < 0.0F ? -1.0F : 1.0F;
    const float qy = turned_y * fold;
    const float half = sweep * 0.5F;
    const float edge_x = std::cos(half);
    const float edge_y = std::sin(half);
    const bool within = std::atan2(qy, qx) <= half;
    // A direction in the folded frame, turned back into local space.
    const auto local = [&](const float fx, const float fy) {
        const float ux = fx;
        const float uy = fy * fold;
        result.normal_x = c * ux - s * uy;
        result.normal_y = s * ux + c * uy;
        direction(result.normal_x, result.normal_y);
    };
    if (round) {
        result.half_width = half_width;
        if (within) {
            result.distance = radius - middle;
        } else {
            const float dx = qx - edge_x * middle;
            const float dy = qy - edge_y * middle;
            result.distance = std::sqrt(dx * dx + dy * dy);
            local(dx, dy);
        }
        return result;
    }
    // Distance to the wedge's boundary ray, signed positive outside the wedge.
    const float along = qx * edge_x + qy * edge_y;
    const float across = std::abs(qy * edge_x - qx * edge_y);
    const float to_ray = along >= 0.0F ? across : radius;
    const float wedge = within ? -to_ray : to_ray;
    const float ring = inner > 0.0F ? std::max(radius - outer, inner - radius) : radius - outer;
    result.distance = std::max(ring, wedge);
    if (wedge > ring && along >= 0.0F)
        local(-edge_y, edge_x);
    return result;
}

/**
 * A stroked segment from the origin to (end_x, end_y). Round caps measure from the segment itself;
 * butt and square caps from its line, with `beyond` how far past the (extended) ends the point is.
 */
inline ShapeDistance segment_distance(const float x, const float y, const float end_x,
                                      const float end_y, const float half_width,
                                      const int caps) noexcept {
    const float length = std::sqrt(end_x * end_x + end_y * end_y);
    const float dx = length > 1.0e-6F ? end_x / length : 1.0F;
    const float dy = length > 1.0e-6F ? end_y / length : 0.0F;
    const float t = x * dx + y * dy;
    ShapeDistance result;
    result.half_width = half_width;
    if (caps == 1) {
        const float along = std::clamp(t, 0.0F, length);
        result.normal_x = x - dx * along;
        result.normal_y = y - dy * along;
        result.distance =
            std::sqrt(result.normal_x * result.normal_x + result.normal_y * result.normal_y);
        if (result.distance > 1.0e-6F) {
            direction(result.normal_x, result.normal_y);
        } else {
            result.normal_x = -dy;
            result.normal_y = dx;
        }
        return result;
    }
    const float extension = caps == 2 ? half_width : 0.0F;
    result.distance = dx * y - dy * x;
    result.normal_x = -dy;
    result.normal_y = dx;
    result.beyond = std::max(-t - extension, t - length - extension);
    result.along_x = dx;
    result.along_y = dy;
    return result;
}

/** Where the analytic shape `mode` stands at local point (x, y). */
inline ShapeDistance analytic_shape_distance(const int mode, const float x, const float y,
                                             const float* data) noexcept {
    if (mode == draw_mode_ellipse) {
        ShapeDistance result = ellipse_distance(x, y, data[4], data[5]);
        if (data[6] > 0.0F)
            result.half_width = data[6];
        return result;
    }
    if (mode == draw_mode_sector)
        return sector_distance(x, y, data[4], data[5], data[6], data[7], data[8] > 0.5F);
    return segment_distance(x, y, data[4], data[5], data[6],
                            static_cast<int>(std::floor(data[7] + 0.5F)));
}

/** A device pixel's footprint across local direction (nx, ny), given the position's derivatives. */
inline float pixel_footprint(const float nx, const float ny, const float px_dx, const float py_dx,
                             const float px_dy, const float py_dy) noexcept {
    return std::abs(nx * px_dx + ny * py_dx) + std::abs(nx * px_dy + ny * py_dy);
}

/**
 * A rounded box centred on the origin, as the pixel stage's rounded rects, borders and shadows draw
 * it: `radii` top-left, top-right, bottom-right, bottom-left, each clamped to the shorter half
 * extent. The distance is exact outside and on the edge.
 */
inline ShapeDistance rounded_box_distance(const float x, const float y, const float half_width,
                                          const float half_height, const float* radii) noexcept {
    const bool right = x >= 0.0F;
    const bool bottom = y >= 0.0F;
    float radius = right ? (bottom ? radii[2] : radii[1]) : (bottom ? radii[3] : radii[0]);
    radius = std::min(std::max(radius, 0.0F), std::min(half_width, half_height));
    const float qx = std::abs(x) - half_width + radius;
    const float qy = std::abs(y) - half_height + radius;
    ShapeDistance result;
    const float outside_x = std::max(qx, 0.0F);
    const float outside_y = std::max(qy, 0.0F);
    result.distance = std::sqrt(outside_x * outside_x + outside_y * outside_y) +
                      std::min(std::max(qx, qy), 0.0F) - radius;
    if (qx > 0.0F && qy > 0.0F) {
        result.normal_x = right ? qx : -qx;
        result.normal_y = bottom ? qy : -qy;
        direction(result.normal_x, result.normal_y);
    } else if (qx >= qy) {
        result.normal_x = 1.0F;
        result.normal_y = 0.0F;
    } else {
        result.normal_x = 0.0F;
        result.normal_y = 1.0F;
    }
    return result;
}

/**
 * A rounded rect's or border's edge coverage. At the default softness (one logical pixel or less)
 * it is exact box-filtered coverage; an authored softness wider than that keeps its deliberate
 * ramp of twice that width.
 */
inline float rounded_edge_coverage(const float distance, const float footprint,
                                   const float softness) noexcept {
    if (softness <= 1.0F)
        return edge_coverage(distance, footprint);
    const float t = std::clamp((distance + softness) / (2.0F * softness), 0.0F, 1.0F);
    return 1.0F - t * t * (3.0F - 2.0F * t);
}

/**
 * The coverage of the analytic shape `mode` at quad coordinate (u, v), given how u and v change per
 * device pixel along x and y: the pixel stage's derivatives, which a CPU backend gets from its
 * triangle setup.
 */
inline float analytic_shape_coverage(const int mode, const float u, const float v,
                                     const float du_dx, const float dv_dx, const float du_dy,
                                     const float dv_dy, const float* data) noexcept {
    const float size_x = data[0];
    const float size_y = data[1];
    const ShapeDistance here =
        analytic_shape_distance(mode, u * size_x - data[2], v * size_y - data[3], data);
    const float px_dx = du_dx * size_x;
    const float py_dx = dv_dx * size_y;
    const float px_dy = du_dy * size_x;
    const float py_dy = dv_dy * size_y;
    const float footprint =
        pixel_footprint(here.normal_x, here.normal_y, px_dx, py_dx, px_dy, py_dy);
    const float coverage = here.half_width >= 0.0F
                               ? band_coverage(here.distance, here.half_width, footprint)
                               : edge_coverage(here.distance, footprint);
    if (here.beyond <= -1.0e8F)
        return coverage;
    return coverage *
           edge_coverage(here.beyond, pixel_footprint(here.along_x, here.along_y, px_dx, py_dx,
                                                      px_dy, py_dy));
}

/**
 * Coverage masks (draw mode 10) paint gradients per pixel in bands: a gradient becomes one masked
 * quad per interval between its stops, each carrying in its draw data the interval's stop offsets
 * (4 and 5), the paint (6: zero for a solid colour, otherwise 1 + kind + 2 * extend, kind 0 linear
 * and 1 radial, extend 0 clamp, 1 repeat, 2 mirror), whether the band also takes everything before
 * its start (7, bit 0: the first band) or after its end (bit 1: the last), the interval's end
 * colour (8-11, its start colour being the vertex colour) and the gradient position (12-13,
 * interpolated across the quad: the linear parameter, or the radial offset whose length is the
 * parameter). A pixel belongs to exactly the band its wrapped parameter falls in and lerps across
 * it, held at the end colours outside the stops, so the bands together draw each pixel once with
 * the exact gradient colour.
 */
constexpr int draw_mode_coverage_mask = 10;

/** The gradient parameter of a banded draw at one pixel, wrapped by its extend mode. */
[[nodiscard]] inline float gradient_band_parameter(const float paint, const float x,
                                                   const float y) noexcept {
    const int packed = static_cast<int>(std::floor(paint + 0.5F)) - 1;
    float t = (packed & 1) != 0 ? std::sqrt(x * x + y * y) : x;
    const int extend = packed >> 1;
    if (extend == 1) {
        t -= std::floor(t);
    } else if (extend == 2) {
        const float period = std::abs(std::fmod(t, 2.0F));
        t = period > 1.0F ? 2.0F - period : period;
    }
    return t;
}

/**
 * How far across its band a pixel's gradient parameter lies, in [0, 1], or a negative value when
 * the pixel belongs to another band.
 */
[[nodiscard]] inline float gradient_band_factor(const float* data) noexcept {
    const float t = gradient_band_parameter(data[6], data[12], data[13]);
    const int open = static_cast<int>(std::floor(data[7] + 0.5F));
    if (((open & 1) == 0 && !(t >= data[4])) || ((open & 2) == 0 && !(t < data[5])))
        return -1.0F;
    const float span = data[5] - data[4];
    if (!(span > 0.0F))
        return t >= data[5] ? 1.0F : 0.0F;
    return std::clamp((t - data[4]) / span, 0.0F, 1.0F);
}

} // namespace strata::host
