#pragma once

#include <optional>

/** Plain 2D value types shared by the compiler, layout and rendering. */
namespace strata::geometry {

struct Point final {
    double x = 0.0;
    double y = 0.0;
    [[nodiscard]] friend bool operator==(const Point&, const Point&) = default;
};

struct Size final {
    double width = 0.0;
    double height = 0.0;
    [[nodiscard]] friend bool operator==(const Size&, const Size&) = default;
};

struct Edges final {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;

    [[nodiscard]] double horizontal() const noexcept;
    [[nodiscard]] double vertical() const noexcept;
    [[nodiscard]] friend bool operator==(const Edges&, const Edges&) = default;
};

struct Rect final {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    [[nodiscard]] double right() const noexcept;
    [[nodiscard]] double bottom() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] Rect deflate(const Edges& edges) const noexcept;
    /** Clip composition preserves an active zero-area rectangle instead of meaning "no clip". */
    [[nodiscard]] Rect clip_intersection(const Rect& other) const noexcept;
    [[nodiscard]] std::optional<Rect> intersection(const Rect& other) const noexcept;
    [[nodiscard]] friend bool operator==(const Rect&, const Rect&) = default;
};

} // namespace strata::geometry
