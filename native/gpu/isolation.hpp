#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

#include <strata/render_packet.hpp>

namespace strata::gpu {

/**
 * How many isolated CONTENT layers hold a framebuffer-sized target at once. Every pass draws in
 * framebuffer coordinates, so a layer being drawn needs a whole target; a layer nested deeper than
 * this sets the shallowest one that holds a target aside (parks it) in one sized to the pixels
 * later passes read back from it, and restores it when the layers inside it close. Memory then
 * follows the layers' own sizes, however deep they nest.
 */
inline constexpr std::size_t resident_content_layers = 4U;

/** Parked targets come in steps of this many pixels each way, so animated bounds reuse one. */
inline constexpr std::uint32_t parked_target_step = 64U;

/** Renders an unused isolation target is kept for before it is released. */
inline constexpr std::uint64_t isolation_target_lifetime = 120U;

/** Device pixels [left, right) x [top, bottom) of a framebuffer. */
struct PixelBounds final {
    std::uint32_t left = 0U;
    std::uint32_t top = 0U;
    std::uint32_t right = 0U;
    std::uint32_t bottom = 0U;

    [[nodiscard]] bool empty() const noexcept {
        return left >= right || top >= bottom;
    }
    [[nodiscard]] std::uint32_t width() const noexcept {
        return empty() ? 0U : right - left;
    }
    [[nodiscard]] std::uint32_t height() const noexcept {
        return empty() ? 0U : bottom - top;
    }
    void include(const PixelBounds other) noexcept {
        if (other.empty())
            return;
        if (empty()) {
            *this = other;
            return;
        }
        left = std::min(left, other.left);
        top = std::min(top, other.top);
        right = std::max(right, other.right);
        bottom = std::max(bottom, other.bottom);
    }
};

/** A parked target's extent for `extent` pixels: rounded up to the step, at most `limit`. */
[[nodiscard]] inline std::uint32_t parked_extent(const std::uint32_t extent,
                                                 const std::uint32_t limit) noexcept {
    const std::uint64_t stepped =
        (static_cast<std::uint64_t>(std::max(extent, 1U)) + parked_target_step - 1U) /
        parked_target_step * parked_target_step;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(stepped, std::max(limit, 1U)));
}

/**
 * The pixels a blur batch reads from its target: its bounds within its scissor grown by its
 * radius, as D3D11's BlurPass and Vulkan plan them, and a pixel more for their rounding.
 */
[[nodiscard]] inline PixelBounds blur_read_bounds(const host::BlurBatch& batch,
                                                  const std::uint32_t width,
                                                  const std::uint32_t height,
                                                  const double logical_width,
                                                  const double logical_height) noexcept {
    if (!(batch.radius > 0.0) || !(batch.width > 0.0) || !(batch.height > 0.0) ||
        !(logical_width > 0.0) || !(logical_height > 0.0))
        return {};
    const double scale_x = width / logical_width;
    const double scale_y = height / logical_height;
    const double padding =
        std::clamp(std::ceil(batch.radius * std::max(scale_x, scale_y)), 1.0, 256.0) + 1.0;
    const auto edge = [](const double value, const std::uint32_t limit) {
        return static_cast<std::uint32_t>(
            std::clamp(std::isfinite(value) ? value : 0.0, 0.0, static_cast<double>(limit)));
    };
    const auto clip_x = static_cast<double>(batch.scissor.x);
    const auto clip_y = static_cast<double>(batch.scissor.y);
    const double left = std::max(std::floor(batch.x * scale_x), clip_x);
    const double top = std::max(std::floor(batch.y * scale_y), clip_y);
    const double right =
        std::min(std::ceil((batch.x + batch.width) * scale_x), clip_x + batch.scissor.width);
    const double bottom =
        std::min(std::ceil((batch.y + batch.height) * scale_y), clip_y + batch.scissor.height);
    if (!(left < right) || !(top < bottom))
        return {};
    return PixelBounds{edge(left - padding, width), edge(top - padding, height),
                       edge(right + padding, width), edge(bottom + padding, height)};
}

/**
 * The device pixels later passes read back from each isolated CONTENT layer of `batches`, one
 * entry per content_begin in order: the layer's own sample region, what the layers nested in it
 * capture of it as their backdrop, and what its backdrop effects and blurs sample. `sample` is
 * the backend's sample region of an effect batch. A layer set aside keeps only these pixels; no
 * pass reads any other once they are drawn.
 */
template <typename Sample>
[[nodiscard]] std::vector<PixelBounds>
content_read_bounds(const std::span<const host::SubmissionBatch> batches,
                    const std::uint32_t width, const std::uint32_t height,
                    const double logical_width, const double logical_height,
                    const Sample& sample) {
    std::vector<PixelBounds> result;
    std::vector<std::size_t> open;
    for (const host::SubmissionBatch& batch : batches) {
        if (const auto* effect = std::get_if<host::EffectBatch>(&batch)) {
            const PixelBounds sampled = sample(*effect);
            if (!open.empty())
                result[open.back()].include(sampled);
            if (effect->kind == host::EffectBatchKind::content_begin) {
                open.push_back(result.size());
                result.push_back(sampled);
            }
        } else if (const auto* blur = std::get_if<host::BlurBatch>(&batch)) {
            if (!open.empty()) {
                result[open.back()].include(
                    blur_read_bounds(*blur, width, height, logical_width, logical_height));
            }
        } else if (std::holds_alternative<host::ContentEffectEndBatch>(batch) && !open.empty()) {
            open.pop_back();
        }
    }
    return result;
}

} // namespace strata::gpu
