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

double Edges::horizontal() const noexcept { return left + right; }
double Edges::vertical() const noexcept { return top + bottom; }

double Rect::right() const noexcept { return x + width; }
double Rect::bottom() const noexcept { return y + height; }
bool Rect::empty() const noexcept { return width <= 0.0 || height <= 0.0; }

Rect Rect::deflate(const Edges& edges_value) const noexcept {
    return Rect{
        x + edges_value.left,
        y + edges_value.top,
        std::max(0.0, width - edges_value.horizontal()),
        std::max(0.0, height - edges_value.vertical()),
    };
}

Rect Rect::clip_intersection(const Rect& other) const noexcept {
    const double left = std::max(x, other.x);
    const double top = std::max(y, other.y);
    const double right_value = std::min(right(), other.right());
    const double bottom_value = std::min(bottom(), other.bottom());
    return Rect{
        left,
        top,
        std::max(0.0, right_value - left),
        std::max(0.0, bottom_value - top),
    };
}

std::optional<Rect> Rect::intersection(const Rect& other) const noexcept {
    const Rect result = clip_intersection(other);
    return result.empty() ? std::nullopt : std::optional<Rect>(result);
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
