#pragma once

#include <cstdint>

namespace strata::runtime {

/** Runtime-local cache invalidation epochs. Hosts merge these with per-surface scale state. */
struct RuntimeGenerationSnapshot final {
    std::uint64_t style_resources = 0U;
    std::uint64_t font_resources = 0U;
    std::uint64_t image_resources = 0U;
    std::uint64_t shader_resources = 0U;
    std::uint64_t material_resources = 0U;

    [[nodiscard]] friend bool operator==(
        const RuntimeGenerationSnapshot&,
        const RuntimeGenerationSnapshot&
    ) = default;
};

} // namespace strata::runtime
