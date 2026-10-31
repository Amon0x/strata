#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace strata::vulkan::detail {

/** The execution stage is part of both the cache identity and the validated entry point. */
enum class ShaderStage : std::uint32_t { vertex, fragment, compute };

/**
 * One compilation's disk entry. Source and names are borrowed until compilation returns; the
 * directory and compiler identity are captured once. Published files are immutable snapshots.
 */
class ShaderCacheEntry final {
  public:
    ShaderCacheEntry(std::string_view source, std::string_view entry, std::string_view name,
                     ShaderStage stage, bool optimized);
    [[nodiscard]] std::optional<std::vector<std::uint32_t>> load() const;
    void store(std::span<const std::uint32_t> words) const noexcept;

  private:
    std::filesystem::path path_;
    std::array<std::uint64_t, 11U> header_{};
    std::array<std::string_view, 3U> key_;
};

} // namespace strata::vulkan::detail
