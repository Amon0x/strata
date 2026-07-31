#include "core/allocator.hpp"

#include <atomic>
#include <cstdint>
#include <limits>
#include <new>

namespace strata::core {
namespace {

struct AtomicAllocationStatistics final {
    std::atomic<std::uint64_t> current_bytes{0U};
    std::atomic<std::uint64_t> peak_bytes{0U};
    std::atomic<std::uint64_t> total_bytes{0U};
    std::atomic<std::uint64_t> live_allocations{0U};
    std::atomic<std::uint64_t> peak_live_allocations{0U};
    std::atomic<std::uint64_t> allocation_count{0U};
    std::atomic<std::uint64_t> deallocation_count{0U};
};

void raise_peak(std::atomic<std::uint64_t>& peak, const std::uint64_t candidate) noexcept {
    std::uint64_t observed = peak.load(std::memory_order_relaxed);
    while (observed < candidate &&
           !peak.compare_exchange_weak(
               observed,
               candidate,
               std::memory_order_relaxed,
               std::memory_order_relaxed
           )) {}
}

void record_allocation(
    AtomicAllocationStatistics& statistics,
    const std::size_t size
) noexcept {
    const std::uint64_t bytes = static_cast<std::uint64_t>(size);
    const std::uint64_t current =
        statistics.current_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    const std::uint64_t live =
        statistics.live_allocations.fetch_add(1U, std::memory_order_relaxed) + 1U;
    static_cast<void>(statistics.total_bytes.fetch_add(bytes, std::memory_order_relaxed));
    static_cast<void>(statistics.allocation_count.fetch_add(1U, std::memory_order_relaxed));
    raise_peak(statistics.peak_bytes, current);
    raise_peak(statistics.peak_live_allocations, live);
}

void record_deallocation(
    AtomicAllocationStatistics& statistics,
    const std::size_t size
) noexcept {
    const std::uint64_t bytes = static_cast<std::uint64_t>(size);
    static_cast<void>(statistics.current_bytes.fetch_sub(bytes, std::memory_order_relaxed));
    static_cast<void>(statistics.live_allocations.fetch_sub(1U, std::memory_order_relaxed));
    static_cast<void>(statistics.deallocation_count.fetch_add(1U, std::memory_order_relaxed));
}

[[nodiscard]] AllocationStatistics snapshot(
    const AtomicAllocationStatistics& statistics
) noexcept {
    return AllocationStatistics{
        statistics.current_bytes.load(std::memory_order_relaxed),
        statistics.peak_bytes.load(std::memory_order_relaxed),
        statistics.total_bytes.load(std::memory_order_relaxed),
        statistics.live_allocations.load(std::memory_order_relaxed),
        statistics.peak_live_allocations.load(std::memory_order_relaxed),
        statistics.allocation_count.load(std::memory_order_relaxed),
        statistics.deallocation_count.load(std::memory_order_relaxed),
    };
}

} // namespace

class AllocationTelemetry final {
public:
    void allocated(const std::size_t size, const AllocationDomain domain) noexcept {
        record_allocation(routed_, size);
        if (domain == AllocationDomain::arena) record_allocation(arena_, size);
    }

    void deallocated(const std::size_t size, const AllocationDomain domain) noexcept {
        record_deallocation(routed_, size);
        if (domain == AllocationDomain::arena) record_deallocation(arena_, size);
    }

    [[nodiscard]] AllocatorStatistics statistics() const noexcept {
        return AllocatorStatistics{snapshot(routed_), snapshot(arena_)};
    }

private:
    AtomicAllocationStatistics routed_;
    AtomicAllocationStatistics arena_;
};

namespace {

[[nodiscard]] void* default_allocate(
    void*,
    const std::size_t size,
    const std::size_t alignment
) noexcept {
    return ::operator new(size, std::align_val_t(alignment), std::nothrow);
}

void default_deallocate(
    void*,
    void* allocation,
    const std::size_t,
    const std::size_t alignment
) noexcept {
    ::operator delete(allocation, std::align_val_t(alignment));
}

[[nodiscard]] bool is_power_of_two(const std::size_t value) noexcept {
    return value != 0U && (value & (value - 1U)) == 0U;
}

} // namespace

HostAllocator::HostAllocator(const strata_allocator& allocator)
    : allocator_(allocator), telemetry_(std::make_shared<AllocationTelemetry>()) {}

void* HostAllocator::allocate(
    const std::size_t size,
    const std::size_t alignment,
    const AllocationDomain domain
) const noexcept {
    if (size == 0U || !is_power_of_two(alignment) || allocator_.allocate == nullptr) {
        return nullptr;
    }
    void* allocation = nullptr;
    try {
        allocation = allocator_.allocate(allocator_.user_data, size, alignment);
    } catch (...) {
        return nullptr;
    }
    if (allocation != nullptr) telemetry_->allocated(size, domain);
    return allocation;
}

void HostAllocator::deallocate(
    void* allocation,
    const std::size_t size,
    const std::size_t alignment,
    const AllocationDomain domain
) const noexcept {
    if (allocation != nullptr && allocator_.deallocate != nullptr) {
        telemetry_->deallocated(size, domain);
        try {
            allocator_.deallocate(allocator_.user_data, allocation, size, alignment);
        } catch (...) {
            // Foreign allocator callbacks must never unwind through the C ABI.
        }
    }
}

const strata_allocator& HostAllocator::abi() const noexcept {
    return allocator_;
}

AllocatorStatistics HostAllocator::statistics() const noexcept {
    return telemetry_->statistics();
}

HostMemoryResource::HostMemoryResource(HostAllocator allocator) noexcept
    : allocator_(allocator) {}

void* HostMemoryResource::do_allocate(const std::size_t bytes, const std::size_t alignment) {
    void* allocation = allocator_.allocate(bytes, alignment, AllocationDomain::arena);
    if (allocation == nullptr) {
        throw std::bad_alloc();
    }
    return allocation;
}

void HostMemoryResource::do_deallocate(
    void* allocation,
    const std::size_t bytes,
    const std::size_t alignment
) {
    allocator_.deallocate(allocation, bytes, alignment, AllocationDomain::arena);
}

bool HostMemoryResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

strata_allocator default_allocator() noexcept {
    return strata_allocator{
        sizeof(strata_allocator),
        nullptr,
        &default_allocate,
        &default_deallocate,
    };
}

bool valid_allocator(const strata_allocator& allocator) noexcept {
    if (allocator.struct_size < sizeof(strata_allocator)) {
        return false;
    }
    return allocator.allocate != nullptr && allocator.deallocate != nullptr;
}

} // namespace strata::core
