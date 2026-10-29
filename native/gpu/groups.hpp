#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

#include <strata/render_packet.hpp>

namespace strata::gpu {
static_assert(host::maximum_presentation_group == 511U);

/**
 * The vertex stage's PresentationGroups constant buffer (register b2): per group index, float4
 * scale.xy/translate.xy, float4 opacity/tone/shear.xy, then float4 projection.xy (and two unused).
 * Default-constructed, every entry is the identity.
 */
struct PresentationGroupConstants final {
    static constexpr std::size_t entries = host::maximum_presentation_group + 1U;
    std::array<float, entries * 12U> values = identity();

    /** Writes a packet's table; indices past it return to the identity. */
    void assign(const std::vector<host::PresentationGroup>& groups) noexcept {
        const std::size_t written = std::max(groups.size(), used_);
        for (std::size_t index = 0U; index < written && index < entries; ++index) {
            const host::PresentationGroup group =
                index < groups.size() ? groups[index] : host::PresentationGroup{};
            float* const entry = values.data() + index * 12U;
            entry[0] = static_cast<float>(group.scale_x);
            entry[1] = static_cast<float>(group.scale_y);
            entry[2] = static_cast<float>(group.translate_x);
            entry[3] = static_cast<float>(group.translate_y);
            entry[4] = static_cast<float>(group.opacity);
            entry[5] = static_cast<float>(group.tone);
            entry[6] = static_cast<float>(group.shear_x);
            entry[7] = static_cast<float>(group.shear_y);
            entry[8] = static_cast<float>(group.project_x);
            entry[9] = static_cast<float>(group.project_y);
            entry[10] = 0.0F;
            entry[11] = 0.0F;
        }
        used_ = groups.size();
    }

  private:
    static constexpr std::array<float, entries * 12U> identity() noexcept {
        std::array<float, entries * 12U> result{};
        for (std::size_t index = 0U; index < entries; ++index) {
            result[index * 12U] = 1.0F;
            result[index * 12U + 1U] = 1.0F;
            result[index * 12U + 4U] = 1.0F;
        }
        return result;
    }

    std::size_t used_ = 0U;
};

} // namespace strata::gpu
