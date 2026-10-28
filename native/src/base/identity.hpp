#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace strata::core {

class StableIdentitySource final {
public:
    explicit StableIdentitySource(const std::uint64_t seed) noexcept : last_(seed) {}

    [[nodiscard]] std::optional<std::uint64_t> next() noexcept {
        if (last_ == std::numeric_limits<std::uint64_t>::max()) {
            return std::nullopt;
        }
        ++last_;
        return last_;
    }

    [[nodiscard]] std::uint64_t last() const noexcept {
        return last_;
    }

private:
    std::uint64_t last_;
};

class ResourceHandle final {
public:
    [[nodiscard]] static std::optional<ResourceHandle> create(
        const std::uint32_t generation,
        const std::uint32_t slot
    ) noexcept {
        if (generation == 0U || slot == 0U) {
            return std::nullopt;
        }
        return ResourceHandle((static_cast<std::uint64_t>(generation) << 32U) | slot);
    }

    [[nodiscard]] std::uint64_t value() const noexcept {
        return value_;
    }

    [[nodiscard]] std::uint32_t generation() const noexcept {
        return static_cast<std::uint32_t>(value_ >> 32U);
    }

    [[nodiscard]] std::uint32_t slot() const noexcept {
        return static_cast<std::uint32_t>(value_);
    }

private:
    explicit ResourceHandle(const std::uint64_t value) noexcept : value_(value) {}

    std::uint64_t value_;
};

} // namespace strata::core
