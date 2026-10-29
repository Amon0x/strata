#include "ui/layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

#include "ui/layout/detail_algorithms.hpp"

namespace strata::ui {
using namespace layout_detail;

std::size_t masonry_column_count(const LayoutStyle& style, const double available) noexcept {
    // Without a column count or minimum, columns are at least this wide.
    constexpr double default_minimum_column_width = 240.0;
    std::size_t count = style.masonry_columns.value_or(0U);
    if (std::isfinite(available) &&
        (style.min_column_width.has_value() || !style.masonry_columns.has_value())) {
        const double minimum = style.min_column_width.value_or(default_minimum_column_width);
        const double fitting = std::floor((available + style.gap.x) / (minimum + style.gap.x));
        const std::size_t derived =
            fitting >= 1.0 ? static_cast<std::size_t>(std::min(fitting, 4096.0)) : 1U;
        count = count == 0U ? derived : std::min(count, derived);
    }
    return std::max<std::size_t>(count, 1U);
}

MasonryLayoutResolution resolve_masonry_layout(const std::size_t columns, const double column_width,
                                               const std::vector<double>& heights,
                                               const Point gap) {
    MasonryLayoutResolution result;
    result.columns = std::max<std::size_t>(columns, 1U);
    result.column_width = std::max(0.0, column_width);
    result.offsets.reserve(heights.size());
    // Where each column's next child would start. A linear scan for the shortest is cheaper than a
    // heap for the handful of columns a box has, and keeps ties on the leftmost column.
    std::vector<double> bottoms(result.columns, 0.0);
    std::vector<bool> used(result.columns, false);
    for (const double height : heights) {
        std::size_t shortest = 0U;
        for (std::size_t column = 1U; column < bottoms.size(); ++column) {
            if (bottoms[column] < bottoms[shortest])
                shortest = column;
        }
        result.offsets.push_back(Point{
            static_cast<double>(shortest) * (result.column_width + gap.x),
            bottoms[shortest],
        });
        bottoms[shortest] += std::max(0.0, height) + gap.y;
        used[shortest] = true;
    }
    double tallest = 0.0;
    for (std::size_t column = 0U; column < bottoms.size(); ++column) {
        // The last child of a column is not followed by a gap.
        if (used[column])
            tallest = std::max(tallest, bottoms[column] - gap.y);
    }
    const double spans = static_cast<double>(result.columns);
    result.intrinsic_size = Size{
        result.column_width * spans + gap.x * (spans - 1.0),
        tallest,
    };
    return result;
}

Constraints Constraints::unbounded() noexcept { return Constraints{0.0, infinity, 0.0, infinity}; }

Constraints Constraints::fixed(const double width, const double height) {
    if (!finite_non_negative(width) || !finite_non_negative(height)) {
        throw std::invalid_argument("fixed layout constraints must be finite and non-negative");
    }
    return Constraints{width, width, height, height};
}

Constraints Constraints::loosen() const noexcept { return Constraints{0.0, max_width, 0.0, max_height}; }

Constraints Constraints::deflate(const Edges& edges_value) const noexcept {
    return Constraints{
        std::max(0.0, min_width - edges_value.horizontal()),
        subtract_finite(max_width, edges_value.horizontal()),
        std::max(0.0, min_height - edges_value.vertical()),
        subtract_finite(max_height, edges_value.vertical()),
    };
}

Size Constraints::constrain(Size size) const noexcept {
    size.width = std::clamp(size.width, min_width, max_width);
    size.height = std::clamp(size.height, min_height, max_height);
    return size;
}

void Constraints::validate() const {
    const bool valid_width = finite_non_negative(min_width) &&
                             (finite_non_negative(max_width) || max_width == infinity) &&
                             max_width >= min_width;
    const bool valid_height = finite_non_negative(min_height) &&
                              (finite_non_negative(max_height) || max_height == infinity) &&
                              max_height >= min_height;
    if (!valid_width || !valid_height) throw std::invalid_argument("layout constraints are invalid");
}

void LayoutEnvironment::validate() const {
    if (!finite_non_negative(viewport.width) || !finite_non_negative(viewport.height) ||
        !std::isfinite(viewport.x) || !std::isfinite(viewport.y) ||
        !std::isfinite(scale) || scale <= 0.0 ||
        !finite_non_negative(safe_insets.left) || !finite_non_negative(safe_insets.top) ||
        !finite_non_negative(safe_insets.right) || !finite_non_negative(safe_insets.bottom)) {
        throw std::invalid_argument("layout environment contains invalid geometry or scale");
    }
}

const LayoutRecord* LayoutResult::find(const std::uint64_t identity) const noexcept {
    const auto found = records.find(identity);
    return found != records.end() ? &found->second : nullptr;
}
} // namespace strata::ui
