#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace strata::base {

/**
 * Static storage whose value survives caller teardown and static destruction. Detached preparation
 * can still read process-wide catalogs then; registering their destructors would race those reads.
 * Use only for static instances. Construction follows the ordinary thread-safe static guard; shared
 * mutable values still provide their own synchronization. The wrapper adds no heap allocation.
 */
template <typename T>
class ProcessLifetime final {
  public:
    ProcessLifetime() noexcept(std::is_nothrow_default_constructible_v<T>) {
        std::construct_at(reinterpret_cast<T*>(storage_));
    }

    template <typename Factory>
        requires std::same_as<std::invoke_result_t<Factory&&>, T>
    explicit ProcessLifetime(Factory&& factory) noexcept(std::is_nothrow_invocable_v<Factory&&>) {
        // Direct prvalue construction preserves elision, including for nonmovable cache objects.
        ::new (static_cast<void*>(storage_)) T(std::invoke(std::forward<Factory>(factory)));
    }

    ProcessLifetime(const ProcessLifetime&) = delete;
    ProcessLifetime& operator=(const ProcessLifetime&) = delete;

    [[nodiscard]] T& get() noexcept {
        return *std::launder(reinterpret_cast<T*>(storage_));
    }
    [[nodiscard]] const T& get() const noexcept {
        return *std::launder(reinterpret_cast<const T*>(storage_));
    }

  private:
    alignas(T) std::byte storage_[sizeof(T)];
};

} // namespace strata::base
