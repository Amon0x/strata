#include "base/stack_guard.hpp"

#include <algorithm>
#include <cstddef>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#elif defined(__linux__)
#include <pthread.h>
#elif defined(__APPLE__)
#include <pthread.h>
#endif

namespace strata::base {

namespace {

/** Room left for what runs below the last check: one more node's frames, unwinding, the report. */
constexpr std::size_t reserve = std::size_t{256U} * 1024U;

[[nodiscard]] std::uintptr_t floor_above(const std::uintptr_t lowest, const std::size_t size) noexcept {
    return lowest + std::min(reserve, size / 4U);
}

} // namespace

std::uintptr_t stack_floor() noexcept {
#if defined(_WIN32)
    ULONG_PTR low = 0U;
    ULONG_PTR high = 0U;
    GetCurrentThreadStackLimits(&low, &high);
    return floor_above(low, high - low);
#elif defined(__linux__)
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes) != 0)
        return 0U;
    void* lowest = nullptr;
    std::size_t size = 0U;
    const int status = pthread_attr_getstack(&attributes, &lowest, &size);
    pthread_attr_destroy(&attributes);
    return status == 0 ? floor_above(reinterpret_cast<std::uintptr_t>(lowest), size) : 0U;
#elif defined(__APPLE__)
    const pthread_t self = pthread_self();
    const auto highest = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(self));
    const std::size_t size = pthread_get_stacksize_np(self);
    return floor_above(highest - size, size);
#else
    return 0U;
#endif
}

void throw_stack_exhausted() {
    throw StackExhausted(
        "the interface is nested too deeply for this thread's stack; a component that calls itself "
        "needs a limit on how deep it goes");
}

} // namespace strata::base
