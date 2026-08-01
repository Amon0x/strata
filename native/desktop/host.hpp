#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <strata/strata.h>

struct HWND__;
using HWND = HWND__*;

namespace strata::desktop {

struct DesktopFrameSample final {
    std::uint64_t frame_index = 0U;
    std::int64_t frame_time_nanos = 0;
    std::int64_t total_nanos = 0;
    std::int64_t core_nanos = 0;
    std::int64_t submit_nanos = 0;
    std::int64_t tooling_nanos = 0;
    std::int64_t present_nanos = 0;
    std::uint64_t input_events = 0U;
    std::uint64_t emitted_events = 0U;
    std::uint64_t render_commands = 0U;
    std::uint64_t packet_bytes = 0U;
    std::uint64_t draw_calls = 0U;
    std::uint64_t batches = 0U;
    std::uint64_t vertices = 0U;
    std::uint64_t blur_passes = 0U;
    bool had_draws = false;
    bool presented = true;
};

struct DesktopHostInfo final {
    std::string adapter;
    std::string driver_version;
    std::uint32_t vendor_id = 0U;
    std::uint32_t device_id = 0U;
    std::uint64_t dedicated_video_memory = 0U;
    std::uint32_t framebuffer_width = 0U;
    std::uint32_t framebuffer_height = 0U;
    double logical_width = 0.0;
    double logical_height = 0.0;
    double scale = 1.0;
    bool vsync = true;
};

struct HostOptions final {
    bool vsync = true;
    bool restore_window_geometry = true;
    bool performance_hud = true;
    bool profile_sampling = true;
};

/** One isolated public-ABI runtime/surface bound to one native desktop window. */
class Host final {
public:
    Host(
        HWND window,
        std::filesystem::path resource_root,
        std::string instance_label,
        HostOptions options = {}
    );
    ~Host();

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    void resize(std::uint32_t framebuffer_width, std::uint32_t framebuffer_height, double scale);
    void pointer(std::uint32_t kind, std::int32_t button, double x, double y);
    void scroll(double x, double y, double delta_x, double delta_y);
    void key(
        std::uint32_t virtual_key,
        std::uint32_t action = STRATA_KEY_PRESS
    );
    void text(std::string utf8);
    void ime_preedit(std::string utf8, std::size_t selection_start, std::size_t selection_end);
    void cancel_interactions() noexcept;
    /** Persists the completed native move/resize immediately instead of relying on shutdown. */
    void persist_window_geometry();
    void frame();

    [[nodiscard]] double scale() const noexcept;
    [[nodiscard]] const DesktopFrameSample& last_frame_sample() const noexcept;
    [[nodiscard]] DesktopHostInfo performance_info() const;
    /** Canonical active showcase frame used only to resolve scripted interaction targets. */
    [[nodiscard]] std::string performance_frame_json();
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
