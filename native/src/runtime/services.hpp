#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/diagnostic.hpp"
#include "runtime/generations.hpp"

namespace strata::runtime {

/**
 * Per-runtime service state shared by every surface owned by an application context.
 * Diagnostics and frame-boundary work live here so opening a second surface cannot create a
 * hidden process-global channel or a second, disconnected diagnostic history.
 */
class RuntimeServices final {
public:
    using FrameBoundaryTask = std::function<void()>;
    using PublishedDiagnosticSink = std::function<void(
        const RuntimeDiagnosticRecord&,
        std::uint64_t dropped_count
    )>;

    explicit RuntimeServices(PublishedDiagnosticSink diagnostic_sink = {});

    /** Begins the next runtime frame, publishes queued diagnostics, and runs queued tasks. */
    [[nodiscard]] std::uint64_t begin_frame();
    void end_frame() noexcept;

    /** Publishes at the next frame boundary. Safe for producers running between frames. */
    void report(RuntimeDiagnostic diagnostic);
    /** Publishes in the active frame through the canonical history/sink path. */
    void publish_current_frame(RuntimeDiagnostic diagnostic);
    [[nodiscard]] RuntimeDiagnosticsSnapshot diagnostics_snapshot() const;
    /** Clears both retained history and diagnostics waiting for the next frame boundary. */
    void clear_diagnostics() noexcept;

    void schedule_frame_boundary_task(std::string id, FrameBoundaryTask task);
    /** Returns false and reports misuse when no runtime frame is active. */
    [[nodiscard]] bool open_profile_section(std::string_view name);

    void bump_style_resources();
    void bump_font_resources();
    void bump_image_resources();
    void bump_shader_resources();
    void bump_material_resources();
    void bump_resource_reload_generations();
    [[nodiscard]] RuntimeGenerationSnapshot generations() const noexcept;
    [[nodiscard]] std::uint64_t style_generation() const noexcept;
    [[nodiscard]] std::uint64_t frame_index() const noexcept;
    [[nodiscard]] bool frame_active() const noexcept;
    /** Pending diagnostics/tasks require the next Surface frame boundary to run. */
    [[nodiscard]] bool has_pending_frame_work() const noexcept;

private:
    void publish_active_diagnostic(RuntimeDiagnostic diagnostic);

    struct ScheduledTask final {
        std::string id;
        FrameBoundaryTask callback;
    };

    RuntimeDiagnosticStore diagnostics_;
    std::vector<RuntimeDiagnostic> pending_diagnostics_;
    std::vector<ScheduledTask> pending_tasks_;
    PublishedDiagnosticSink diagnostic_sink_;
    std::uint64_t frame_index_ = 0U;
    RuntimeGenerationSnapshot generations_;
    bool frame_active_ = false;
};

} // namespace strata::runtime
