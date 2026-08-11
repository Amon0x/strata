#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

#include "core/allocator.hpp"
#include "core/arena.hpp"
#include "core/diagnostics.hpp"
#include "core/identity.hpp"
#include "core/utf8.hpp"

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

struct AllocationCounts final {
    std::size_t allocations = 0U;
    std::size_t deallocations = 0U;
};

void* allocate(void* const user_data, const std::size_t size, const std::size_t alignment) {
    auto& counts = *static_cast<AllocationCounts*>(user_data);
    void* const value = ::operator new(size, std::align_val_t(alignment), std::nothrow);
    if (value != nullptr) {
        ++counts.allocations;
    }
    return value;
}

void deallocate(
    void* const user_data,
    void* const allocation,
    const std::size_t,
    const std::size_t alignment
) {
    auto& counts = *static_cast<AllocationCounts*>(user_data);
    ++counts.deallocations;
    ::operator delete(allocation, std::align_val_t(alignment));
}

void throwing_diagnostic(void*, const strata_diagnostic*) {
    throw std::runtime_error("foreign diagnostic callback");
}

void test_utf8() {
    check(strata::core::valid_utf8("plain ASCII"), "ASCII must be valid UTF-8");
    check(strata::core::valid_utf8("\xC4\xB0stanbul \xF0\x9F\x8C\x8D"), "Unicode must be valid UTF-8");
    check(!strata::core::valid_utf8("\xC0\xAF"), "overlong UTF-8 must be rejected");
    check(!strata::core::valid_utf8("\xED\xA0\x80"), "UTF-8 surrogate must be rejected");
    check(!strata::core::valid_utf8("\xF4\x90\x80\x80"), "out-of-range UTF-8 must be rejected");
    check(!strata::core::valid_utf8("\xE2\x82"), "truncated UTF-8 must be rejected");
}

void test_arena_uses_host_allocator() {
    AllocationCounts counts;
    const strata_allocator callbacks{
        sizeof(strata_allocator),
        &counts,
        &allocate,
        &deallocate,
    };
    const strata::core::HostAllocator host_allocator(callbacks);
    strata::core::HostMemoryResource resource{host_allocator};
    strata::core::AllocatorStatistics active_statistics{};
    {
        strata::core::Arena arena(resource);
        const auto* const value = arena.create<std::uint64_t>(42U);
        check(*value == 42U, "arena object construction failed");
        active_statistics = host_allocator.statistics();
        check(active_statistics.routed.current_bytes > 0U, "routed bytes were not tracked");
        check(
            active_statistics.arena.current_bytes == active_statistics.routed.current_bytes,
            "arena allocation was not categorized"
        );
        arena.reset();
    }
    const strata::core::AllocatorStatistics released_statistics =
        host_allocator.statistics();
    check(released_statistics.routed.current_bytes == 0U, "released routed bytes remained live");
    check(released_statistics.arena.current_bytes == 0U, "released arena bytes remained live");
    check(
        released_statistics.arena.peak_bytes >= sizeof(std::uint64_t),
        "arena byte high-water mark was not retained"
    );
    check(
        released_statistics.arena.allocation_count ==
            released_statistics.arena.deallocation_count,
        "arena telemetry did not balance"
    );
    check(counts.allocations > 0U, "arena must allocate through the host allocator");
    check(counts.allocations == counts.deallocations, "arena allocations must return to their owner");
}

void test_stable_identity_and_resource_handles() {
    strata::core::StableIdentitySource identities(40U);
    check(identities.next() == 41U, "stable identity seed was not honored");
    check(identities.next() == 42U, "stable identities must be monotonic");
    strata::core::StableIdentitySource exhausted(std::numeric_limits<std::uint64_t>::max());
    check(!exhausted.next().has_value(), "stable identity overflow must fail");

    const auto handle = strata::core::ResourceHandle::create(7U, 19U);
    check(handle.has_value(), "valid resource handle was rejected");
    check(handle->generation() == 7U && handle->slot() == 19U, "resource handle parts changed");
    check(handle->value() == ((UINT64_C(7) << 32U) | UINT64_C(19)), "resource encoding changed");
    check(!strata::core::ResourceHandle::create(0U, 1U).has_value(), "zero generation must be invalid");
    check(!strata::core::ResourceHandle::create(1U, 0U).has_value(), "zero slot must be invalid");
}

void test_foreign_diagnostic_exception_is_contained() {
    const strata_diagnostic_sink sink{
        sizeof(strata_diagnostic_sink),
        nullptr,
        &throwing_diagnostic,
    };
    strata::core::Diagnostics diagnostics(sink);
    const strata_result emitted = diagnostics.emit(
        STRATA_STATUS_INTERNAL_ERROR,
        STRATA_DIAGNOSTIC_FATAL,
        "STRATA.TEST",
        "test"
    );
    check(emitted.status == STRATA_STATUS_INTERNAL_ERROR, "diagnostic status changed");
    check(emitted.diagnostic_id == 1U, "diagnostic identity must be deterministic per runtime");
}

} // namespace

int strata_test_core() {
    try {
        test_utf8();
        test_arena_uses_host_allocator();
        test_stable_identity_and_resource_handles();
        test_foreign_diagnostic_exception_is_contained();
        std::cout << "strata_core_tests: OK\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "strata_core_tests: " << exception.what() << '\n';
        return 1;
    }
}
