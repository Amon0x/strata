#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <strata/desktop.hpp>
#include <strata/strata.h>

#include "renderer.hpp"

struct HWND__;
using HWND = HWND__*;

namespace strata::desktop {

/** What one frame did, counted: the work a change should be proportional to. */
struct DesktopFrameWork final {
    std::uint64_t described_nodes = 0U;
    std::uint64_t evaluated_expressions = 0U;
    std::uint64_t layout_measured_nodes = 0U;
    std::uint64_t layout_arranged_nodes = 0U;
    std::uint64_t render_visited_nodes = 0U;
    std::uint64_t render_built_fragments = 0U;
    std::uint64_t pointer_geometry_rebuilds = 0U;
    std::uint64_t rasterized_glyphs = 0U;
    std::uint64_t raster_nanos = 0U;
};

struct DesktopFrameSample final {
    std::uint64_t frame_index = 0U;
    std::int64_t frame_time_nanos = 0;
    std::int64_t total_nanos = 0;
    std::int64_t core_nanos = 0;
    std::int64_t submit_nanos = 0;
    std::int64_t tooling_nanos = 0;
    std::int64_t present_nanos = 0;
    /** The GPU's time for the layer, as last measured (a few frames behind). */
    std::int64_t gpu_nanos = 0;
    std::uint64_t input_events = 0U;
    std::uint64_t emitted_events = 0U;
    std::uint64_t render_commands = 0U;
    std::uint64_t packet_bytes = 0U;
    std::uint64_t draw_calls = 0U;
    std::uint64_t batches = 0U;
    std::uint64_t vertices = 0U;
    std::uint64_t blur_passes = 0U;
    DesktopFrameWork work;
    bool had_draws = false;
    bool presented = true;
    /** Nothing the window shows changed, so it was not drawn again: an idle frame, not a fault. */
    bool unchanged = false;
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
    /** Files dragged from the shell: FILE_DRAG, FILE_DROP or FILE_DRAG_LEAVE, at client pixels. */
    void files(std::uint32_t kind, double x, double y, const std::string& paths);
    /** Window messages the platform layer answers (custom title bar, tray icon); else nothing. */
    [[nodiscard]] std::optional<std::intptr_t>
    window_message(std::uint32_t message, std::uintptr_t word, std::intptr_t long_value);
    /** The pointer shape the interactive layer asks for; nullopt when no layer is live. */
    [[nodiscard]] std::optional<std::uint32_t> pointer_cursor() noexcept;
    void frame();
    /** The last frame drew and presented the window: something it shows changed. */
    [[nodiscard]] bool presented() const noexcept;
    /**
     * Nothing is left to do until input or a host change: every shown layer is idle and nothing
     * on screen animates by itself. The loop may wait for input instead of framing.
     */
    [[nodiscard]] bool idle() const noexcept;
    /** The windows the documents asked to open or close since the last call. */
    [[nodiscard]] std::vector<WindowRequest> take_window_requests();
    /** Answers a window request with `json`, at the next frame. */
    void answer_window_request(const WindowRequest& request, std::string json);
    /**
     * The host of another window of this application: it shares this one's model and runs
     * `request.root`, an overlay of the showcase module, with the platform services of its own.
     */
    [[nodiscard]] std::unique_ptr<Host> open_window(HWND window, const WindowRequest& request);

    [[nodiscard]] double scale() const noexcept;
    [[nodiscard]] const DesktopFrameSample& last_frame_sample() const noexcept;
    [[nodiscard]] DesktopHostInfo performance_info() const;
    /** The next frame's pixels, read from the swap chain (Renderer::capture_next_frame). */
    void capture_next_frame() noexcept;
    [[nodiscard]] std::optional<FrameCapture> take_capture();
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
    explicit Host(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::desktop
