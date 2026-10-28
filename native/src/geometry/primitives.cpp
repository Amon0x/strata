#include "geometry/primitives.hpp"

#include <algorithm>

namespace strata::geometry {

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

} // namespace strata::geometry
