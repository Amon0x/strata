#include "runtime/services.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace strata::runtime {

RuntimeServices::RuntimeServices(PublishedDiagnosticSink diagnostic_sink)
    : diagnostic_sink_(std::move(diagnostic_sink)) {}

void RuntimeServices::publish_active_diagnostic(RuntimeDiagnostic diagnostic) {
    const RuntimeDiagnosticRecord& record =
        diagnostics_.append(frame_index_, std::move(diagnostic));
    if (!diagnostic_sink_)
        return;
    try {
        diagnostic_sink_(record, diagnostics_.dropped_count());
    } catch (...) {
        /* Diagnostic observers cannot break the runtime frame boundary. */
    }
}

std::uint64_t RuntimeServices::begin_frame() {
    if (frame_active_)
        throw std::logic_error("runtime frame is already active");
    if (frame_index_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("runtime service frame index exhausted");
    }
    ++frame_index_;
    frame_active_ = true;

    for (RuntimeDiagnostic& diagnostic : pending_diagnostics_) {
        publish_active_diagnostic(std::move(diagnostic));
    }
    pending_diagnostics_.clear();

    std::vector<ScheduledTask> tasks;
    tasks.swap(pending_tasks_);
    for (ScheduledTask& task : tasks) {
        try {
            task.callback();
        } catch (const std::exception& exception) {
            publish_active_diagnostic(RuntimeDiagnostic{
                "STRATA.RUNTIME.FRAME_BOUNDARY_TASK_FAILED",
                "Frame boundary task '" + task.id + "' failed: " + exception.what(),
                {},
                std::nullopt,
                DiagnosticSeverity::error,
                std::nullopt,
            });
        } catch (...) {
            publish_active_diagnostic(RuntimeDiagnostic{
                "STRATA.RUNTIME.FRAME_BOUNDARY_TASK_FAILED",
                "Frame boundary task '" + task.id + "' failed: unknown native exception",
                {},
                std::nullopt,
                DiagnosticSeverity::error,
                std::nullopt,
            });
        }
    }
    return frame_index_;
}

void RuntimeServices::end_frame() noexcept {
    frame_active_ = false;
}

void RuntimeServices::report(RuntimeDiagnostic diagnostic) {
    pending_diagnostics_.push_back(std::move(diagnostic));
}

void RuntimeServices::publish_current_frame(RuntimeDiagnostic diagnostic) {
    if (!frame_active_) {
        throw std::logic_error("current-frame diagnostic publication requires an active frame");
    }
    publish_active_diagnostic(std::move(diagnostic));
}

RuntimeDiagnosticsSnapshot RuntimeServices::diagnostics_snapshot() const {
    return diagnostics_.snapshot(frame_index_);
}

void RuntimeServices::clear_diagnostics() noexcept {
    pending_diagnostics_.clear();
    diagnostics_.clear();
}

void RuntimeServices::schedule_frame_boundary_task(std::string id, FrameBoundaryTask task) {
    if (id.empty())
        throw std::invalid_argument("frame boundary task id must not be empty");
    if (!task)
        throw std::invalid_argument("frame boundary task callback must not be empty");
    pending_tasks_.push_back(ScheduledTask{std::move(id), std::move(task)});
}

bool RuntimeServices::open_profile_section(const std::string_view name) {
    if (name.empty())
        throw std::invalid_argument("profiler section name must not be empty");
    if (frame_active_)
        return true;
    report(RuntimeDiagnostic{
        "STRATA.PROFILER.MISUSE",
        "section '" + std::string(name) +
            "' opened outside the active profiler frame owner thread; sample was ignored",
        {},
        std::nullopt,
        DiagnosticSeverity::warning,
        std::nullopt,
    });
    return false;
}

std::uint64_t RuntimeServices::frame_index() const noexcept {
    return frame_index_;
}
bool RuntimeServices::frame_active() const noexcept {
    return frame_active_;
}
bool RuntimeServices::has_pending_frame_work() const noexcept {
    return !pending_diagnostics_.empty() || !pending_tasks_.empty();
}

} // namespace strata::runtime
