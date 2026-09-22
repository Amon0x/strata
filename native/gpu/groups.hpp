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
 * scale.xy/translate.xy then float4 opacity/padding. Default-constructed, every entry is the
 * identity.
 */
struct PresentationGroupConstants final {
    static constexpr std::size_t entries = host::maximum_presentation_group + 1U;
    std::array<float, entries * 8U> values = identity();

    /** Writes a packet's table; indices past it return to the identity. */
    void assign(const std::vector<host::PresentationGroup>& groups) noexcept {
        const std::size_t written = std::max(groups.size(), used_);
        for (std::size_t index = 0U; index < written && index < entries; ++index) {
            const host::PresentationGroup group =
                index < groups.size() ? groups[index] : host::PresentationGroup{};
            float* const entry = values.data() + index * 8U;
            entry[0] = static_cast<float>(group.scale_x);
            entry[1] = static_cast<float>(group.scale_y);
            entry[2] = static_cast<float>(group.translate_x);
            entry[3] = static_cast<float>(group.translate_y);
            entry[4] = static_cast<float>(group.opacity);
        }
        used_ = groups.size();
    }

  private:
    static constexpr std::array<float, entries * 8U> identity() noexcept {
        std::array<float, entries * 8U> result{};
        for (std::size_t index = 0U; index < entries; ++index) {
            result[index * 8U] = 1.0F;
            result[index * 8U + 1U] = 1.0F;
            result[index * 8U + 4U] = 1.0F;
        }
        return result;
    }

    std::size_t used_ = 0U;
};

} // namespace strata::gpu
