#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>

#include <strata/strata.h>

namespace strata::core {

enum class AllocationDomain : std::uint8_t {
    persistent,
    arena,
};

struct AllocationStatistics final {
    std::uint64_t current_bytes = 0U;
    std::uint64_t peak_bytes = 0U;
    std::uint64_t total_bytes = 0U;
    std::uint64_t live_allocations = 0U;
    std::uint64_t peak_live_allocations = 0U;
    std::uint64_t allocation_count = 0U;
    std::uint64_t deallocation_count = 0U;
};

struct AllocatorStatistics final {
    AllocationStatistics routed;
    AllocationStatistics arena;
};

class AllocationTelemetry;

class HostAllocator final {
public:
    explicit HostAllocator(const strata_allocator& allocator);

    [[nodiscard]] void* allocate(
        std::size_t size,
        std::size_t alignment,
        AllocationDomain domain = AllocationDomain::persistent
    ) const noexcept;
    void deallocate(
        void* allocation,
        std::size_t size,
        std::size_t alignment,
        AllocationDomain domain = AllocationDomain::persistent
    ) const noexcept;
    [[nodiscard]] const strata_allocator& abi() const noexcept;
    [[nodiscard]] AllocatorStatistics statistics() const noexcept;

private:
    strata_allocator allocator_;
    std::shared_ptr<AllocationTelemetry> telemetry_;
};

class HostMemoryResource final : public std::pmr::memory_resource {
public:
    explicit HostMemoryResource(HostAllocator allocator) noexcept;

private:
    [[nodiscard]] void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* allocation, std::size_t bytes, std::size_t alignment) override;
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

    HostAllocator allocator_;
};

[[nodiscard]] strata_allocator default_allocator() noexcept;
[[nodiscard]] bool valid_allocator(const strata_allocator& allocator) noexcept;

} // namespace strata::core
