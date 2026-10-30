#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace strata::host {

/**
 * Vector text (draw modes 13 and 14, STRATA_RENDER_PACKET_VERSION_24): each pixel's coverage by a
 * glyph measured from the glyph's outline, which the core flattens once into straight segments in
 * font units, merged where its contours overlap and grouped into horizontal bands. The pixel's
 * square is mapped into font units through how far a device pixel moves there along x and y, so a
 * glyph is measured exactly where it lands at any scale, turn or shear; nothing is rasterized for
 * it. vectorTextCoverage in gpu/shaders.hpp computes exactly what vector_text_coverage does here,
 * which every CPU backend draws vector text with.
 */

/** FreeType's default LCD filter, weighting a pixel's thirds and two either side per subpixel. */
inline constexpr std::array<double, 5U> vector_lcd_filter{8.0 / 256.0, 77.0 / 256.0, 86.0 / 256.0,
                                                          77.0 / 256.0, 8.0 / 256.0};

/** The integral over [x0, x0 + width] of the part of the column right of x, from x0. */
[[nodiscard]] inline double vector_column_area(const double x, const double x0,
                                               const double width) noexcept {
    const double x1 = x0 + width;
    if (x <= x0)
        return width * (x - x0);
    if (x >= x1)
        return width * width * 0.5;
    return x1 * (x - x0) - (x * x - x0 * x0) * 0.5;
}

/**
 * The signed area a segment from (ax, ay) to (bx, by) contributes to the column [x0, x0 + width]
 * between heights `low` and `high`: the integral, along its part in that height range, of the
 * column's width to its right. Summed over a closed outline, the area inside it.
 */
[[nodiscard]] inline double vector_segment_area(const double ax, const double ay, const double bx,
                                                const double by, const double x0,
                                                const double width, const double low,
                                                const double high) noexcept {
    if (ay == by)
        return 0.0;
    if (std::max(ay, by) <= low || std::min(ay, by) >= high)
        return 0.0;
    const double y_from = std::clamp(ay, low, high);
    const double y_to = std::clamp(by, low, high);
    const double slope = (bx - ax) / (by - ay);
    const double x_from = ax + (y_from - ay) * slope;
    const double x_to = ax + (y_to - ay) * slope;
    const double span = x_to - x_from;
    const double average =
        std::abs(span) < 1.0e-9
            ? x0 + width - std::clamp(x_from, x0, x0 + width)
            : (vector_column_area(x_to, x0, width) - vector_column_area(x_from, x0, width)) / span;
    return (y_to - y_from) * average;
}

/**
 * One glyph as the buffers hold it: its record's bounds and band count, its whole segments
 * (x0 y0 x1 y1), which a turned pixel reads, its bands (band_count + 1 offsets into its pieces)
 * and the pieces themselves: each band's segments cut to its heights, which an upright pixel
 * reads as they are.
 */
struct VectorGlyphView final {
    float left = 0.0F;
    float bottom = 0.0F;
    float right = 0.0F;
    float top = 0.0F;
    std::uint32_t band_count = 0U;
    std::span<const float> segments;
    std::span<const std::uint32_t> bands;
    std::span<const float> pieces;
};

/** Where a pixel is in a glyph's font units, and how far moving a device pixel goes there. */
struct VectorSample final {
    /** The pixel's centre: on the device (y down), and in font units (y up). */
    double device_x = 0.0;
    double device_y = 0.0;
    double glyph_x = 0.0;
    double glyph_y = 0.0;
    /** Font units a device pixel right, and a device pixel down. */
    double along_x_x = 0.0;
    double along_x_y = 0.0;
    double along_y_x = 0.0;
    double along_y_y = 0.0;
};

/**
 * A pixel's coverage by a glyph: gray coverage in every channel when `subpixel` is 0, else
 * coverage per subpixel (1 red first, 2 blue first), each the pixel's thirds weighted by
 * vector_lcd_filter, alpha the largest. A glyph drawn at its laid-out size is a variant of it
 * already fitted to the pixel grid for that size (font::fit_vector_glyph), measured the same way.
 */
[[nodiscard]] inline std::array<double, 4U> vector_text_coverage(const VectorGlyphView& glyph,
                                                                 const VectorSample& sample,
                                                                 const std::uint32_t subpixel) {
    std::array<double, 4U> result{};
    const double determinant =
        sample.along_x_x * sample.along_y_y - sample.along_x_y * sample.along_y_x;
    if (std::abs(determinant) < 1.0e-12 || glyph.band_count == 0U)
        return result;
    // Font units to pixel offsets from the pixel's centre.
    const std::array<double, 4U> inverse{
        sample.along_y_y / determinant, -sample.along_y_x / determinant,
        -sample.along_x_y / determinant, sample.along_x_x / determinant};
    // The font-unit heights the pixel (and its neighbours' thirds) reaches.
    const double reach = 0.5 * std::abs(sample.along_y_y) + 1.5 * std::abs(sample.along_x_y);
    const double height = static_cast<double>(glyph.top) - glyph.bottom;
    const std::uint32_t bands = glyph.band_count;
    const double band_height = height / static_cast<double>(bands);
    const auto clamped = [bands](const double band) {
        return static_cast<int>(std::clamp(band, 0.0, static_cast<double>(bands - 1U)));
    };
    // Bands serve when a device row is a range of glyph heights (moving right on the device does
    // not change the height); a turned glyph's row crosses every band, and reads every segment.
    // A band whose bottom is the row's top holds nothing within the row: one a pixel row tall
    // on the rows is the only band its row reads.
    const bool banded = std::abs(sample.along_x_y) <= 1.0e-4 * std::abs(sample.along_y_y);
    const int first_band =
        banded ? clamped(std::floor((sample.glyph_y - reach - glyph.bottom) / band_height)) : 0;
    const int last_band =
        banded ? clamped(std::ceil((sample.glyph_y + reach - glyph.bottom) / band_height) - 1.0)
               : 0;
    const int columns = subpixel != 0U ? 7 : 1;
    std::array<double, 7U> area{};
    for (int band = first_band; band <= last_band; ++band) {
        // An upright pixel reads its bands' pieces; a turned one every whole segment.
        const std::span<const float> source = banded ? glyph.pieces : glyph.segments;
        const std::uint32_t first = banded ? glyph.bands[static_cast<std::size_t>(band)] : 0U;
        const std::uint32_t last = banded ? glyph.bands[static_cast<std::size_t>(band) + 1U]
                                          : static_cast<std::uint32_t>(glyph.segments.size() / 4U);
        for (std::uint32_t entry = first; entry < last; ++entry) {
            const double ax = source[entry * 4U];
            const double ay = source[entry * 4U + 1U];
            const double bx = source[entry * 4U + 2U];
            const double by = source[entry * 4U + 3U];
            // In pixel offsets from the pixel's centre, y down.
            const double dax =
                inverse[0] * (ax - sample.glyph_x) + inverse[1] * (ay - sample.glyph_y);
            const double day =
                inverse[2] * (ax - sample.glyph_x) + inverse[3] * (ay - sample.glyph_y);
            const double dbx =
                inverse[0] * (bx - sample.glyph_x) + inverse[1] * (by - sample.glyph_y);
            const double dby =
                inverse[2] * (bx - sample.glyph_x) + inverse[3] * (by - sample.glyph_y);
            for (int column = 0; column < columns; ++column) {
                const double column_x0 =
                    columns == 1 ? -0.5 : -0.5 + (static_cast<double>(column) - 2.0) / 3.0;
                const double width = columns == 1 ? 1.0 : 1.0 / 3.0;
                area[static_cast<std::size_t>(column)] +=
                    vector_segment_area(dax, day, dbx, dby, column_x0, width, -0.5, 0.5);
            }
        }
    }
    std::array<double, 7U> thirds{};
    for (int column = 0; column < columns; ++column) {
        const double width = columns == 1 ? 1.0 : 1.0 / 3.0;
        thirds[static_cast<std::size_t>(column)] =
            std::min(std::abs(area[static_cast<std::size_t>(column)]) / width, 1.0);
    }
    if (subpixel == 0U) {
        result.fill(thirds[0]);
        return result;
    }
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        double value = 0.0;
        for (std::size_t tap = 0U; tap < vector_lcd_filter.size(); ++tap)
            value += vector_lcd_filter[tap] * thirds[channel + tap];
        result[channel] = std::min(value, 1.0);
    }
    if (subpixel == 2U)
        std::swap(result[0], result[2]);
    result[3] = std::max({result[0], result[1], result[2]});
    return result;
}

} // namespace strata::host
