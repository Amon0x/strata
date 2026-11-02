#pragma once

#include <cstdint>
#include <stdexcept>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace strata::base {

/**
 * A walk over something nested deeper than this thread's stack can hold. Thrown while there is still
 * room to report it: running out of stack ends the process with no word about why, wherever the
 * application's own recursion happens to reach it.
 */
class StackExhausted final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/** The lowest address this thread's recursive walks may reach, leaving room to unwind and report. */
[[nodiscard]] std::uintptr_t stack_floor() noexcept;
[[noreturn]] void throw_stack_exhausted();

/** Cheap enough for a per-node walk: compares the stack pointer against a per-thread limit. */
inline void check_stack() {
#if defined(_MSC_VER)
    const auto here = reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
#else
    const auto here = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
#endif
    static thread_local const std::uintptr_t floor = stack_floor();
    if (here < floor)
        throw_stack_exhausted();
}

} // namespace strata::base
