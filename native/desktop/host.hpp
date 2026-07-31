#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

struct HWND__;
using HWND = HWND__*;

namespace strata::desktop {

/** One isolated public-ABI runtime/surface bound to one native desktop window. */
class Host final {
public:
    Host(
        HWND window,
        std::filesystem::path resource_root,
        std::string instance_label,
        bool vsync = true
    );
    ~Host();

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    void resize(std::uint32_t framebuffer_width, std::uint32_t framebuffer_height, double scale);
    void pointer(std::uint32_t kind, std::int32_t button, double x, double y);
    void scroll(double x, double y, double delta_x, double delta_y);
    void key(std::uint32_t virtual_key);
    void text(std::string utf8);
    void cancel_interactions() noexcept;
    /** Persists the completed native move/resize immediately instead of relying on shutdown. */
    void persist_window_geometry();
    void frame();

    [[nodiscard]] double scale() const noexcept;
    [[nodiscard]] bool smoke_ready() const noexcept;
    [[nodiscard]] std::uint64_t smoke_fingerprint() const noexcept;
    [[nodiscard]] bool smoke_identity_bound() const noexcept;
    /** Starts a fresh host timing window for a programmatic profile scenario. */
    void reset_profile();
    /** Captures and formats the profiler without opening or automating the debug overlay. */
    [[nodiscard]] std::string profile_report();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::desktop
