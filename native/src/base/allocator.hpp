#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>

namespace strata::base {

using AllocateFunction = void* (*)(void* user_data, std::size_t size, std::size_t alignment);
using DeallocateFunction =
    void (*)(void* user_data, void* allocation, std::size_t size, std::size_t alignment);

/** Caller-owned allocation callbacks; both are required. Exceptions they throw are contained. */
struct AllocatorCallbacks final {
    void* user_data = nullptr;
    AllocateFunction allocate = nullptr;
    DeallocateFunction deallocate = nullptr;
};

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
    explicit HostAllocator(const AllocatorCallbacks& allocator);

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
    [[nodiscard]] const AllocatorCallbacks& callbacks() const noexcept;
    [[nodiscard]] AllocatorStatistics statistics() const noexcept;

private:
    AllocatorCallbacks allocator_;
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

/** Aligned global operator new/delete. */
[[nodiscard]] AllocatorCallbacks default_allocator() noexcept;
[[nodiscard]] bool valid_allocator(const AllocatorCallbacks& allocator) noexcept;

} // namespace strata::base
