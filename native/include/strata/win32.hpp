#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include <strata/win32.h>

namespace strata {
class Surface;
}

namespace strata::win32 {

using InputClock = std::function<std::int64_t()>;

struct InputAdapterOptions final {
    InputClock clock;
    double coordinate_scale = 1.0;
    bool manage_pointer_capture = true;
    bool focus_on_pointer_press = true;
    bool consume_system_keys = false;
};

/**
 * Optional Win32 message translator for an embedding host. The host retains ownership of the
 * window and chooses the target Surface for every message.
 */
class InputAdapter final {
  public:
    explicit InputAdapter(InputAdapterOptions options = {});
    ~InputAdapter();

    InputAdapter(const InputAdapter&) = delete;
    InputAdapter& operator=(const InputAdapter&) = delete;
    InputAdapter(InputAdapter&&) noexcept;
    InputAdapter& operator=(InputAdapter&&) noexcept;

    /**
     * Translates and enqueues one Win32 message. A value is returned only when the message was
     * handled and contains the LRESULT the host should return from its window procedure.
     */
    [[nodiscard]] std::optional<std::intptr_t> handle(
        Surface& surface,
        void* window,
        std::uint32_t message,
        std::uintptr_t word_parameter,
        std::intptr_t long_parameter
    );
    [[nodiscard]] std::optional<std::intptr_t> handle(
        strata_surface* surface,
        void* window,
        std::uint32_t message,
        std::uintptr_t word_parameter,
        std::intptr_t long_parameter
    );

    void set_coordinate_scale(double scale);
    [[nodiscard]] double coordinate_scale() const noexcept;

    /** Clears partial text, tracking, and capture state without emitting Surface input. */
    void reset(void* window = nullptr) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::win32
