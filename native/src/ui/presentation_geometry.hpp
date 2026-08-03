#pragma once

#include <optional>
#include <string_view>

#include "ui/layout.hpp"
#include "ui/motion/model.hpp"

namespace strata::ui {

class MotionRuntime;
class RetainedNode;

/**
 * Affine presentation geometry shared by rendering, hit testing, and inspection.
 *
 * Layout records remain in stable logical coordinates. Presentation-only motion is composed
 * separately so animation never invalidates measurement merely to keep pointer and inspection
 * geometry aligned with pixels. Scale is centered on the arranged border bounds; its pivot
 * compensation is baked into the returned translation.
 */
[[nodiscard]] MotionTransform local_presentation_transform(
    const RetainedNode& node,
    const MotionRuntime& motion,
    const Rect& bounds
) noexcept;

/** Effective local opacity shared by rendering and presentation-aware input hit testing. */
[[nodiscard]] double local_presentation_opacity(
    const RetainedNode& node,
    const MotionRuntime* motion
) noexcept;

[[nodiscard]] MotionTransform concatenate_presentation_transform(
    const MotionTransform& parent,
    const MotionTransform& local
) noexcept;

[[nodiscard]] Rect transform_presentation_bounds(
    const Rect& bounds,
    const MotionTransform& transform
) noexcept;

/** Maps an absolute clip into the coordinate space active before a transform push. */
[[nodiscard]] Rect inverse_presentation_bounds(
    const Rect& bounds,
    const MotionTransform& transform
) noexcept;
[[nodiscard]] Point inverse_presentation_point(
    Point point,
    const MotionTransform& transform
) noexcept;

[[nodiscard]] std::optional<double> visual_number(
    const RetainedNode& node,
    std::string_view name
) noexcept;

[[nodiscard]] const runtime::Value* visual_value(
    const RetainedNode& node,
    std::string_view name
) noexcept;

} // namespace strata::ui
