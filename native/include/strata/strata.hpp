#pragma once

#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <strata/adapters.hpp>
#include <strata/config.hpp>
#include <strata/diagnostic.hpp>
#include <strata/input.hpp>
#include <strata/profiler.hpp>
#include <strata/strata.h>

namespace strata {

namespace host {
class Bindings;
}

class AbiError final : public std::runtime_error {
public:
    AbiError(
        const strata_result result,
        const std::string_view operation,
        std::optional<Diagnostic> diagnostic = std::nullopt
    ) : std::runtime_error(describe(result, operation, diagnostic)),
        status_(result.status),
        diagnostic_id_(result.diagnostic_id),
        diagnostic_(std::move(diagnostic)) {}

    [[nodiscard]] strata_status status() const noexcept { return status_; }
    [[nodiscard]] std::uint64_t diagnostic_id() const noexcept { return diagnostic_id_; }
    [[nodiscard]] const std::optional<Diagnostic>& diagnostic() const noexcept {
        return diagnostic_;
    }

private:
    [[nodiscard]] static std::string describe(
        const strata_result result,
        const std::string_view operation,
        const std::optional<Diagnostic>& diagnostic
    ) {
        std::string message(operation);
        message += " failed: ";
        message += status_name(result.status);
        if (diagnostic.has_value()) {
            if (!diagnostic->code.empty()) {
                message += " [";
                message += diagnostic->code;
                message += ']';
            }
            if (!diagnostic->message.empty()) {
                message += " ";
                message += diagnostic->message;
            }
        } else if (result.diagnostic_id != 0U) {
            message += " (diagnostic ";
            message += std::to_string(result.diagnostic_id);
            message += ')';
        }
        return message;
    }

    strata_status status_;
    std::uint64_t diagnostic_id_;
    std::optional<Diagnostic> diagnostic_;
};

inline void require_ok(const strata_result result, const std::string_view operation) {
    if (result.status != STRATA_STATUS_OK) throw AbiError(result, operation);
}

[[nodiscard]] inline strata_scale_policy_config scale_policy_defaults(
    const strata_scale_policy_kind kind
) {
    strata_scale_policy_config policy{};
    policy.struct_size = sizeof(policy);
    require_ok(strata_scale_policy_defaults(kind, &policy), "scale policy defaults");
    return policy;
}

[[nodiscard]] inline strata_scale_context resolve_scale_context(
    const strata_scale_policy_config& policy,
    const std::int64_t framebuffer_width,
    const std::int64_t framebuffer_height
) {
    strata_scale_context context{};
    context.struct_size = sizeof(context);
    require_ok(
        strata_resolve_scale_context(
            &policy,
            framebuffer_width,
            framebuffer_height,
            &context
        ),
        "scale context resolution"
    );
    return context;
}

[[nodiscard]] inline strata_theme_tokens theme_tokens_defaults() {
    strata_theme_tokens tokens{};
    tokens.struct_size = sizeof(tokens);
    require_ok(strata_theme_tokens_defaults(&tokens), "theme token defaults");
    return tokens;
}

[[nodiscard]] inline strata_theme_visual_style theme_visual_style_defaults() {
    strata_theme_visual_style style{};
    style.struct_size = sizeof(style);
    require_ok(strata_theme_visual_style_defaults(&style), "theme visual-style defaults");
    return style;
}

[[nodiscard]] inline strata_theme_text_visual_style theme_text_visual_style_defaults() {
    strata_theme_text_visual_style style{};
    style.struct_size = sizeof(style);
    require_ok(strata_theme_text_visual_style_defaults(&style), "theme text-visual defaults");
    return style;
}

[[nodiscard]] inline strata_theme_text_layout_style theme_text_layout_style_defaults() {
    strata_theme_text_layout_style style{};
    style.struct_size = sizeof(style);
    require_ok(strata_theme_text_layout_style_defaults(&style), "theme text-layout defaults");
    return style;
}

[[nodiscard]] inline strata_theme_layout_style theme_layout_style_defaults() {
    strata_theme_layout_style style{};
    style.struct_size = sizeof(style);
    require_ok(strata_theme_layout_style_defaults(&style), "theme layout defaults");
    return style;
}

[[nodiscard]] inline strata_theme_layout_size theme_layout_size(
    const strata_theme_layout_size_kind kind = STRATA_THEME_SIZE_AUTO,
    const double value = 0.0
) noexcept {
    strata_theme_layout_size result{};
    result.struct_size = sizeof(result);
    result.kind = kind;
    result.value = value;
    return result;
}

[[nodiscard]] inline strata_theme_layout_size theme_layout_clamp(
    const strata_theme_layout_size* minimum,
    const strata_theme_layout_size* preferred,
    const strata_theme_layout_size* maximum
) noexcept {
    strata_theme_layout_size result = theme_layout_size(STRATA_THEME_SIZE_CLAMP);
    result.minimum = minimum;
    result.preferred = preferred;
    result.maximum = maximum;
    return result;
}

[[nodiscard]] inline strata_theme_animation_set theme_animation_set_defaults() {
    strata_theme_animation_set set{};
    set.struct_size = sizeof(set);
    require_ok(strata_theme_animation_set_defaults(&set), "theme animation-set defaults");
    return set;
}

[[nodiscard]] inline strata_theme_motion_easing theme_motion_easing(
    const strata_theme_motion_easing_kind kind = STRATA_THEME_MOTION_EASING_LINEAR
) noexcept {
    strata_theme_motion_easing result{};
    result.struct_size = sizeof(result);
    result.kind = kind;
    result.x2 = 1.0;
    result.y2 = 1.0;
    return result;
}

[[nodiscard]] inline strata_theme_motion_easing theme_cubic_bezier(
    const double x1,
    const double y1,
    const double x2,
    const double y2
) noexcept {
    strata_theme_motion_easing result = theme_motion_easing(
        STRATA_THEME_MOTION_EASING_CUBIC_BEZIER
    );
    result.x1 = x1;
    result.y1 = y1;
    result.x2 = x2;
    result.y2 = y2;
    return result;
}

[[nodiscard]] inline strata_theme_motion_timing theme_motion_timing(
    const strata_string_view name,
    const std::int64_t duration_nanoseconds
) noexcept {
    strata_theme_motion_timing result{};
    result.struct_size = sizeof(result);
    result.name = name;
    result.duration_nanoseconds = duration_nanoseconds;
    result.easing = theme_motion_easing();
    result.repeat_kind = STRATA_THEME_MOTION_REPEAT_NONE;
    result.fill_mode = STRATA_THEME_MOTION_FILL_BOTH;
    return result;
}

[[nodiscard]] inline strata_theme_animation_spec theme_named_animation(
    const strata_string_view name
) noexcept {
    return strata_theme_animation_spec{STRATA_THEME_ANIMATION_NAMED, 0U, name, nullptr};
}

[[nodiscard]] inline strata_theme_animation_spec theme_inline_animation(
    const strata_theme_declared_animation* animation
) noexcept {
    return strata_theme_animation_spec{
        STRATA_THEME_ANIMATION_INLINE, 0U, strata_string_view{}, animation
    };
}

class Surface;

namespace detail {

[[nodiscard]] inline std::string copied(const strata_string_view value) {
    return value.size == 0U ? std::string{} : std::string(value.data, value.size);
}

[[nodiscard]] inline std::optional<std::string> optional_copied(
    const strata_string_view value
) {
    return value.size == 0U ? std::nullopt : std::optional<std::string>(copied(value));
}

[[nodiscard]] inline DiagnosticSeverity diagnostic_severity(
    const strata_diagnostic_severity severity
) noexcept {
    switch (severity) {
    case STRATA_DIAGNOSTIC_INFO: return DiagnosticSeverity::info;
    case STRATA_DIAGNOSTIC_WARNING: return DiagnosticSeverity::warning;
    case STRATA_DIAGNOSTIC_ERROR: return DiagnosticSeverity::error;
    case STRATA_DIAGNOSTIC_FATAL: return DiagnosticSeverity::fatal;
    default: return DiagnosticSeverity::error;
    }
}

[[nodiscard]] inline Diagnostic copy_diagnostic(const strata_diagnostic& value) {
    Diagnostic result;
    result.id = value.id;
    result.severity = diagnostic_severity(value.severity);
    result.code = copied(value.code);
    result.message = copied(value.message);
    result.source_id = optional_copied(value.source_id);
    if (result.source_id.has_value()) {
        result.range = SourceRange{
            value.range.byte_start,
            value.range.byte_end,
            value.range.line_start,
            value.range.column_start,
            value.range.line_end,
            value.range.column_end,
        };
    }
    result.occurrence_count = value.occurrence_count;
    result.sequence = value.sequence;
    result.first_frame_index = value.first_frame_index;
    result.frame_index = value.frame_index;
    result.dropped_count = value.dropped_count;
    result.component_path = optional_copied(value.component_path);
    result.expected = optional_copied(value.expected);
    return result;
}

struct DiagnosticsCapture final {
    DiagnosticsSnapshot value;
    bool emitted = false;
    bool failed = false;
};

inline void capture_diagnostics(
    void* const user_data,
    const strata_diagnostics_snapshot* const snapshot
) noexcept {
    auto& capture = *static_cast<DiagnosticsCapture*>(user_data);
    if (snapshot == nullptr) {
        capture.failed = true;
        return;
    }
    try {
        capture.value.frame_index = snapshot->frame_index;
        capture.value.dropped_count = snapshot->dropped_count;
        capture.value.records.clear();
        capture.value.records.reserve(snapshot->record_count);
        for (std::size_t index = 0U; index < snapshot->record_count; ++index) {
            capture.value.records.push_back(copy_diagnostic(snapshot->records[index]));
        }
        capture.emitted = true;
    } catch (...) {
        capture.failed = true;
    }
}

[[nodiscard]] inline ProfilerCounter copy_profiler_counter(
    const strata_profiler_counter& value
) {
    return ProfilerCounter{copied(value.name), value.value};
}

[[nodiscard]] inline ProfilerSection copy_profiler_section(
    const strata_profiler_section& value
) {
    return ProfilerSection{
        value.id,
        value.parent_id,
        copied(value.name),
        copied(value.path),
        value.sample_count,
        value.last_sample_frame_index,
        value.last_nanos,
        value.average_nanos,
        value.p50_nanos,
        value.p95_nanos,
        value.p99_nanos,
        value.maximum_nanos,
    };
}

[[nodiscard]] inline ProfilerSpike copy_profiler_spike(
    const strata_profiler_spike& value
) {
    ProfilerSpike result;
    result.section_id = value.section_id;
    result.parent_id = value.parent_id;
    result.section_name = copied(value.section_name);
    result.section_path = copied(value.section_path);
    if (value.stack_depth != 0U) {
        result.stack_section_ids.assign(
            value.stack_section_ids,
            value.stack_section_ids + value.stack_depth
        );
    }
    result.frame_index = value.frame_index;
    result.duration_nanoseconds = value.duration_nanos;
    result.absolute_threshold_nanoseconds = value.absolute_threshold_nanos;
    result.effective_threshold_nanoseconds = value.effective_threshold_nanos;
    result.baseline_multiplier = value.baseline_multiplier;
    result.rolling_average_nanoseconds = value.rolling_average_nanos;
    result.has_rolling_average = value.has_rolling_average != 0U;
    result.counters.reserve(value.counter_count);
    for (std::size_t index = 0U; index < value.counter_count; ++index) {
        result.counters.push_back(copy_profiler_counter(value.counters[index]));
    }
    return result;
}

struct ProfilerCapture final {
    ProfilerSnapshot value;
    bool emitted = false;
    bool failed = false;
};

inline void capture_profiler(
    void* const user_data,
    const strata_profiler_snapshot* const snapshot
) noexcept {
    auto& capture = *static_cast<ProfilerCapture*>(user_data);
    if (snapshot == nullptr) {
        capture.failed = true;
        return;
    }
    try {
        capture.value.scope = snapshot->scope == STRATA_PROFILER_SCOPE_SURFACE
            ? ProfilerScope::surface
            : ProfilerScope::runtime;
        capture.value.scope_id = copied(snapshot->scope_id);
        capture.value.frame_index = snapshot->frame_index;
        capture.value.capture_enabled = snapshot->capture_enabled != 0U;
        capture.value.dropped_section_samples = snapshot->dropped_section_samples;
        capture.value.dropped_timing_samples = snapshot->dropped_timing_samples;
        capture.value.dropped_spikes = snapshot->dropped_spikes;
        capture.value.sections.clear();
        capture.value.sections.reserve(snapshot->section_count);
        for (std::size_t index = 0U; index < snapshot->section_count; ++index) {
            capture.value.sections.push_back(copy_profiler_section(snapshot->sections[index]));
        }
        capture.value.counters.clear();
        capture.value.counters.reserve(snapshot->counter_count);
        for (std::size_t index = 0U; index < snapshot->counter_count; ++index) {
            capture.value.counters.push_back(copy_profiler_counter(snapshot->counters[index]));
        }
        capture.value.spikes.clear();
        capture.value.spikes.reserve(snapshot->spike_count);
        for (std::size_t index = 0U; index < snapshot->spike_count; ++index) {
            capture.value.spikes.push_back(copy_profiler_spike(snapshot->spikes[index]));
        }
        capture.emitted = true;
    } catch (...) {
        capture.failed = true;
    }
}

[[nodiscard]] inline std::optional<Diagnostic> find_diagnostic(
    const strata_runtime* const runtime,
    const std::uint64_t id
) noexcept {
    if (runtime == nullptr || id == 0U) return std::nullopt;
    DiagnosticsCapture capture;
    const strata_diagnostics_snapshot_sink sink{
        sizeof(strata_diagnostics_snapshot_sink),
        &capture,
        &capture_diagnostics,
    };
    const strata_result read = strata_runtime_read_diagnostics(runtime, &sink);
    if (read.status != STRATA_STATUS_OK || capture.failed || !capture.emitted) {
        return std::nullopt;
    }
    const auto found = std::ranges::find(capture.value.records, id, &Diagnostic::id);
    if (found == capture.value.records.end()) return std::nullopt;
    try {
        return *found;
    } catch (...) {
        return std::nullopt;
    }
}

struct RuntimeCallbacks final {
    explicit RuntimeCallbacks(
        RuntimeOptions options,
        const strata_diagnostic_sink forwarded_diagnostic = {}
    ) : options(std::move(options)),
        forwarded_diagnostic(forwarded_diagnostic) {}
    RuntimeOptions options;
    strata_diagnostic_sink forwarded_diagnostic{};
    mutable std::mutex diagnostic_mutex;
    std::vector<Diagnostic> diagnostics;
};

inline std::int64_t runtime_clock(void* const user_data) noexcept {
    const auto& callbacks = *static_cast<RuntimeCallbacks*>(user_data);
    try {
        return callbacks.options.clock();
    } catch (...) {
        return 0;
    }
}

inline void runtime_diagnostic(
    void* const user_data,
    const strata_diagnostic* const diagnostic
) noexcept {
    auto& callbacks = *static_cast<RuntimeCallbacks*>(user_data);
    if (diagnostic == nullptr) return;
    if (callbacks.forwarded_diagnostic.emit != nullptr &&
        callbacks.forwarded_diagnostic.struct_size >= sizeof(strata_diagnostic_sink)) {
        try {
            callbacks.forwarded_diagnostic.emit(
                callbacks.forwarded_diagnostic.user_data,
                diagnostic
            );
        } catch (...) {
            // A foreign diagnostic observer cannot unwind through the C ABI.
        }
    }
    try {
        const Diagnostic owned = copy_diagnostic(*diagnostic);
        {
            const std::scoped_lock lock(callbacks.diagnostic_mutex);
            const auto retained = std::ranges::find(
                callbacks.diagnostics, owned.id, &Diagnostic::id
            );
            if (retained != callbacks.diagnostics.end()) {
                *retained = owned;
            } else {
                constexpr std::size_t maximum_retained_diagnostics = 512U;
                if (callbacks.diagnostics.size() == maximum_retained_diagnostics) {
                    callbacks.diagnostics.erase(callbacks.diagnostics.begin());
                }
                callbacks.diagnostics.push_back(owned);
            }
        }
        if (callbacks.options.diagnostic) callbacks.options.diagnostic(owned);
    } catch (...) {
        // A diagnostic observer cannot fail the ABI operation that produced the diagnostic.
    }
}

[[nodiscard]] inline std::optional<Diagnostic> find_diagnostic(
    const RuntimeCallbacks* const callbacks,
    const std::uint64_t id
) noexcept {
    if (callbacks == nullptr || id == 0U) return std::nullopt;
    const std::scoped_lock lock(callbacks->diagnostic_mutex);
    const auto found = std::ranges::find(callbacks->diagnostics, id, &Diagnostic::id);
    if (found == callbacks->diagnostics.end()) return std::nullopt;
    try {
        return *found;
    } catch (...) {
        return std::nullopt;
    }
}

struct ResourceBridge final {
    explicit ResourceBridge(ResourceAdapter adapter) : adapter(std::move(adapter)) {}
    ResourceAdapter adapter;
    std::vector<std::uint8_t> borrowed;
};

inline strata_status resource_load(
    void* const user_data,
    const strata_string_view resource_id,
    strata_bytes_view* const out_bytes
) noexcept {
    if (user_data == nullptr || out_bytes == nullptr) return STRATA_STATUS_INVALID_ARGUMENT;
    auto& bridge = *static_cast<ResourceBridge*>(user_data);
    try {
        const std::optional<std::vector<std::uint8_t>> loaded = bridge.adapter.load(
            std::string_view(resource_id.data, resource_id.size)
        );
        if (!loaded.has_value()) return STRATA_STATUS_NOT_FOUND;
        bridge.borrowed = *loaded;
        *out_bytes = strata_bytes_view{bridge.borrowed.data(), bridge.borrowed.size()};
        return STRATA_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return STRATA_STATUS_OUT_OF_MEMORY;
    } catch (...) {
        return STRATA_STATUS_INTERNAL_ERROR;
    }
}

struct DurableBridge final {
    explicit DurableBridge(DurableStoreAdapter adapter) : adapter(std::move(adapter)) {}
    DurableStoreAdapter adapter;
    std::vector<std::uint8_t> borrowed;
};

inline strata_status durable_load(
    void* const user_data,
    const strata_string_view application_id,
    strata_bytes_view* const out_bytes
) noexcept {
    if (user_data == nullptr || out_bytes == nullptr) return STRATA_STATUS_INVALID_ARGUMENT;
    auto& bridge = *static_cast<DurableBridge*>(user_data);
    try {
        const auto loaded = bridge.adapter.load(
            std::string_view(application_id.data, application_id.size)
        );
        if (!loaded.has_value()) return STRATA_STATUS_NOT_FOUND;
        bridge.borrowed = *loaded;
        *out_bytes = strata_bytes_view{bridge.borrowed.data(), bridge.borrowed.size()};
        return STRATA_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return STRATA_STATUS_OUT_OF_MEMORY;
    } catch (...) {
        return STRATA_STATUS_INTERNAL_ERROR;
    }
}

inline strata_status durable_write(
    void* const user_data,
    const strata_string_view application_id,
    const strata_bytes_view bytes
) noexcept {
    if (user_data == nullptr) return STRATA_STATUS_INVALID_ARGUMENT;
    auto& bridge = *static_cast<DurableBridge*>(user_data);
    try {
        bridge.adapter.write(
            std::string_view(application_id.data, application_id.size),
            std::span<const std::uint8_t>(bytes.data, bytes.size)
        );
        return STRATA_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return STRATA_STATUS_OUT_OF_MEMORY;
    } catch (...) {
        return STRATA_STATUS_INTERNAL_ERROR;
    }
}

struct AsyncBridge final {
    explicit AsyncBridge(AsyncHostAdapter adapter) : adapter(std::move(adapter)) {}
    AsyncHostAdapter adapter;
};

inline strata_status async_begin(
    void* const user_data,
    const std::uint64_t request_id,
    const strata_string_view binding,
    const strata_string_view owner,
    const strata_string_view payload_json
) noexcept {
    if (user_data == nullptr) return STRATA_STATUS_INVALID_ARGUMENT;
    auto& bridge = *static_cast<AsyncBridge*>(user_data);
    try {
        bridge.adapter.begin(AsyncRequest{
            request_id,
            copied(binding),
            copied(owner),
            copied(payload_json),
        });
        return STRATA_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return STRATA_STATUS_OUT_OF_MEMORY;
    } catch (...) {
        return STRATA_STATUS_INTERNAL_ERROR;
    }
}

inline void async_cancel(void* const user_data, const std::uint64_t request_id) noexcept {
    if (user_data == nullptr) return;
    auto& bridge = *static_cast<AsyncBridge*>(user_data);
    try {
        bridge.adapter.cancel(request_id);
    } catch (...) {
        // Cancellation is an ownership notification and cannot cross the C ABI with an exception.
    }
}

struct ClipboardBridge final {
    explicit ClipboardBridge(ClipboardAdapter adapter) : adapter(std::move(adapter)) {}
    ClipboardAdapter adapter;
    std::string borrowed;
};

inline strata_status clipboard_read(
    void* const user_data,
    strata_string_view* const out_text
) noexcept {
    if (user_data == nullptr || out_text == nullptr) return STRATA_STATUS_INVALID_ARGUMENT;
    auto& bridge = *static_cast<ClipboardBridge*>(user_data);
    try {
        const std::optional<std::string> text = bridge.adapter.read();
        if (!text.has_value()) return STRATA_STATUS_NOT_FOUND;
        bridge.borrowed = *text;
        *out_text = strata_string_view{bridge.borrowed.data(), bridge.borrowed.size()};
        return STRATA_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return STRATA_STATUS_OUT_OF_MEMORY;
    } catch (...) {
        return STRATA_STATUS_INTERNAL_ERROR;
    }
}

inline strata_status clipboard_write(
    void* const user_data,
    const strata_string_view text
) noexcept {
    if (user_data == nullptr) return STRATA_STATUS_INVALID_ARGUMENT;
    auto& bridge = *static_cast<ClipboardBridge*>(user_data);
    try {
        bridge.adapter.write(std::string_view(text.data, text.size));
        return STRATA_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return STRATA_STATUS_OUT_OF_MEMORY;
    } catch (...) {
        return STRATA_STATUS_INTERNAL_ERROR;
    }
}

struct ImeBridge final {
    explicit ImeBridge(ImeAdapter adapter) : adapter(std::move(adapter)) {}
    ImeAdapter adapter;
};

inline strata_status ime_set_active(void* const user_data, const std::uint32_t active) noexcept {
    if (user_data == nullptr) return STRATA_STATUS_INVALID_ARGUMENT;
    auto& bridge = *static_cast<ImeBridge*>(user_data);
    try {
        bridge.adapter.set_active(active != 0U);
        return STRATA_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return STRATA_STATUS_OUT_OF_MEMORY;
    } catch (...) {
        return STRATA_STATUS_INTERNAL_ERROR;
    }
}

inline strata_status ime_set_cursor_rect(
    void* const user_data,
    const strata_rect rect
) noexcept {
    if (user_data == nullptr) return STRATA_STATUS_INVALID_ARGUMENT;
    auto& bridge = *static_cast<ImeBridge*>(user_data);
    try {
        bridge.adapter.set_cursor_rect(Rect{rect.x, rect.y, rect.width, rect.height});
        return STRATA_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return STRATA_STATUS_OUT_OF_MEMORY;
    } catch (...) {
        return STRATA_STATUS_INTERNAL_ERROR;
    }
}

struct EffectBridge final {
    explicit EffectBridge(EffectAdapter adapter) : adapter(std::move(adapter)) {}
    EffectAdapter adapter;
};

inline strata_status effect_emit(
    void* const user_data,
    const strata_string_view effect_id,
    const strata_string_view payload_json
) noexcept {
    if (user_data == nullptr) return STRATA_STATUS_INVALID_ARGUMENT;
    auto& bridge = *static_cast<EffectBridge*>(user_data);
    try {
        bridge.adapter.emit(EffectRequest{copied(effect_id), copied(payload_json)});
        return STRATA_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return STRATA_STATUS_OUT_OF_MEMORY;
    } catch (...) {
        return STRATA_STATUS_INTERNAL_ERROR;
    }
}

struct SourceLoaderBridge final {
    const SourceActivation::Loader* load = nullptr;
    std::optional<SourceModule> borrowed;
};

inline strata_status source_load(
    void* const user_data,
    const strata_string_view importer_source_id,
    const strata_string_view import_path,
    strata_module_source* const out_source
) noexcept {
    if (user_data == nullptr || out_source == nullptr) return STRATA_STATUS_INVALID_ARGUMENT;
    auto& bridge = *static_cast<SourceLoaderBridge*>(user_data);
    try {
        bridge.borrowed = (*bridge.load)(
            std::string_view(importer_source_id.data, importer_source_id.size),
            std::string_view(import_path.data, import_path.size)
        );
        if (!bridge.borrowed.has_value()) return STRATA_STATUS_NOT_FOUND;
        *out_source = strata_module_source{
            sizeof(strata_module_source),
            strata_string_view{
                bridge.borrowed->source_id.data(), bridge.borrowed->source_id.size()
            },
            strata_string_view{bridge.borrowed->text.data(), bridge.borrowed->text.size()},
        };
        return STRATA_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return STRATA_STATUS_OUT_OF_MEMORY;
    } catch (...) {
        return STRATA_STATUS_INTERNAL_ERROR;
    }
}

struct RuntimeControl final {
    explicit RuntimeControl(
        strata_runtime* value,
        std::shared_ptr<RuntimeCallbacks> callbacks = {}
    ) noexcept : value(value), callbacks(std::move(callbacks)) {}
    ~RuntimeControl() {
        if (value != nullptr) static_cast<void>(strata_runtime_release(value));
    }
    RuntimeControl(const RuntimeControl&) = delete;
    RuntimeControl& operator=(const RuntimeControl&) = delete;

    strata_runtime* value;
    std::mutex host_snapshot_mutex;
    std::uint64_t next_host_snapshot_generation = 1U;
    std::shared_ptr<RuntimeCallbacks> callbacks;
    std::shared_ptr<ResourceBridge> resource;
    std::shared_ptr<DurableBridge> durable;
    std::shared_ptr<AsyncBridge> async;
    std::shared_ptr<ClipboardBridge> clipboard;
    std::shared_ptr<ImeBridge> ime;
    std::shared_ptr<EffectBridge> effect;
};

[[nodiscard]] inline std::optional<Diagnostic> find_diagnostic(
    const RuntimeControl* const control,
    const std::uint64_t id
) noexcept {
    if (control == nullptr || id == 0U) return std::nullopt;
    if (const std::optional<Diagnostic> retained = find_diagnostic(
            control->callbacks.get(), id
        ); retained.has_value()) {
        return retained;
    }
    return find_diagnostic(control->value, id);
}

inline void clear_retained_diagnostics(RuntimeControl* const control) noexcept {
    if (control == nullptr || control->callbacks == nullptr) return;
    const std::scoped_lock lock(control->callbacks->diagnostic_mutex);
    control->callbacks->diagnostics.clear();
}

[[nodiscard]] inline std::uint64_t publish_host_snapshot(
    const std::shared_ptr<RuntimeControl>& control,
    const std::string_view id,
    const std::string_view value_json
) {
    if (control == nullptr || control->value == nullptr) {
        throw std::logic_error("host snapshot publication requires a live runtime");
    }
    const std::scoped_lock lock(control->host_snapshot_mutex);
    std::uint64_t retained_generation = 0U;
    const strata_result retained = strata_runtime_get_host_snapshot_generation(
        control->value,
        strata_string_view{id.data(), id.size()},
        &retained_generation
    );
    if (retained.status != STRATA_STATUS_NOT_FOUND) {
        if (retained.status != STRATA_STATUS_OK) {
            throw AbiError(
                retained,
                "host snapshot generation read",
                find_diagnostic(control.get(), retained.diagnostic_id)
            );
        }
        if (retained_generation == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("host snapshot generation exhausted");
        }
        control->next_host_snapshot_generation = std::max(
            control->next_host_snapshot_generation,
            retained_generation + 1U
        );
    }
    const std::uint64_t generation = control->next_host_snapshot_generation;
    if (generation == 0U || generation == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("host snapshot generation exhausted");
    }
    const strata_host_snapshot_config snapshot{
        sizeof(strata_host_snapshot_config),
        strata_string_view{id.data(), id.size()},
        generation,
        strata_string_view{value_json.data(), value_json.size()},
    };
    const strata_result published = strata_runtime_publish_host_snapshot(
        control->value, &snapshot
    );
    if (published.status != STRATA_STATUS_OK) {
        throw AbiError(
            published,
            "host snapshot publication",
            find_diagnostic(control.get(), published.diagnostic_id)
        );
    }
    control->next_host_snapshot_generation = generation + 1U;
    return generation;
}

struct ByteCapture final {
    std::vector<std::uint8_t> value;
    bool failed = false;
};

inline void capture_bytes(void* const user_data, const strata_bytes_view bytes) noexcept {
    auto& capture = *static_cast<ByteCapture*>(user_data);
    try {
        if (bytes.size == 0U) capture.value.clear();
        else capture.value.assign(bytes.data, bytes.data + bytes.size);
    } catch (...) {
        capture.failed = true;
    }
}

struct StringCapture final {
    std::string value;
    bool failed = false;
};

inline void capture_string(void* const user_data, const strata_string_view text) noexcept {
    auto& capture = *static_cast<StringCapture*>(user_data);
    try {
        if (text.size == 0U) capture.value.clear();
        else capture.value.assign(text.data, text.size);
    } catch (...) {
        capture.failed = true;
    }
}

inline void require_result(
    const strata_result result,
    const std::string_view operation,
    const RuntimeControl* const control
) {
    if (result.status == STRATA_STATUS_OK) return;
    throw AbiError(result, operation, find_diagnostic(control, result.diagnostic_id));
}

[[nodiscard]] inline ActivationInfo activation_info(const strata_activation_info& value) {
    const auto status = [&value] {
        switch (value.status) {
        case STRATA_ACTIVATION_ACTIVATED: return ActivationStatus::activated;
        case STRATA_ACTIVATION_REJECTED_GENERATION: return ActivationStatus::rejected_generation;
        case STRATA_ACTIVATION_REJECTED_COMPILE: return ActivationStatus::rejected_compile;
        case STRATA_ACTIVATION_REJECTED_UNIT: return ActivationStatus::rejected_unit;
        case STRATA_ACTIVATION_REJECTED_CAPABILITY:
            return ActivationStatus::rejected_capability;
        default: return ActivationStatus::rejected_unit;
        }
    }();
    return ActivationInfo{
        status,
        value.state_migrated != 0U,
        value.attempted_generation,
        value.has_active_generation != 0U
            ? std::optional<std::uint64_t>(value.active_generation)
            : std::nullopt,
        value.diagnostic_count,
    };
}

[[nodiscard]] inline ActionDispatchInfo dispatch_info(
    const strata_action_dispatch_info& value
) noexcept {
    const auto status = [&value] {
        switch (value.status) {
        case STRATA_ACTION_DISPATCH_NO_ACTION: return ActionDispatchStatus::no_action;
        case STRATA_ACTION_DISPATCH_HANDLED: return ActionDispatchStatus::handled;
        case STRATA_ACTION_DISPATCH_FORWARDED: return ActionDispatchStatus::forwarded;
        case STRATA_ACTION_DISPATCH_IGNORED: return ActionDispatchStatus::ignored;
        case STRATA_ACTION_DISPATCH_UNHANDLED: return ActionDispatchStatus::unhandled;
        case STRATA_ACTION_DISPATCH_FAILED: return ActionDispatchStatus::failed;
        default: return ActionDispatchStatus::failed;
        }
    }();
    return ActionDispatchInfo{status, value.handler_count};
}

[[nodiscard]] inline strata_action_dispatch_config action_dispatch(
    const ActionDispatch& action
) noexcept {
    const std::string_view source = action.source_key.has_value()
        ? std::string_view(*action.source_key)
        : std::string_view{};
    const std::string_view state_scope = action.state_scope.has_value()
        ? std::string_view(*action.state_scope)
        : std::string_view{};
    return strata_action_dispatch_config{
        sizeof(strata_action_dispatch_config),
        strata_string_view{action.action_id.data(), action.action_id.size()},
        strata_string_view{action.payload_json.data(), action.payload_json.size()},
        strata_string_view{action.event_kind.data(), action.event_kind.size()},
        strata_string_view{source.data(), source.size()},
        strata_string_view{action.event_value_json.data(), action.event_value_json.size()},
        strata_string_view{state_scope.data(), state_scope.size()},
        action.dynamic ? 1U : 0U,
        0U,
    };
}

} // namespace detail

class Snapshot final {
public:
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    Snapshot(Snapshot&& other) noexcept
        : owner_(std::move(other.owner_)), value_(std::exchange(other.value_, nullptr)) {}
    Snapshot& operator=(Snapshot&& other) noexcept {
        if (this == &other) return *this;
        reset();
        owner_ = std::move(other.owner_);
        value_ = std::exchange(other.value_, nullptr);
        return *this;
    }
    ~Snapshot() { reset(); }

    [[nodiscard]] strata_snapshot_info info() const {
        strata_snapshot_info result{};
        result.struct_size = sizeof(result);
        detail::require_result(
            strata_snapshot_get_info(value_, &result),
            "snapshot info read",
            owner_.get()
        );
        return result;
    }

private:
    friend class Runtime;
    Snapshot(std::shared_ptr<detail::RuntimeControl> owner, strata_snapshot* value) noexcept
        : owner_(std::move(owner)), value_(value) {}
    void reset() noexcept {
        if (value_ != nullptr) strata_snapshot_release(std::exchange(value_, nullptr));
        owner_.reset();
    }
    std::shared_ptr<detail::RuntimeControl> owner_;
    strata_snapshot* value_ = nullptr;
};

class ApplicationStateSnapshot final {
public:
    ApplicationStateSnapshot(const ApplicationStateSnapshot&) = delete;
    ApplicationStateSnapshot& operator=(const ApplicationStateSnapshot&) = delete;
    ApplicationStateSnapshot(ApplicationStateSnapshot&& other) noexcept
        : owner_(std::move(other.owner_)), value_(std::exchange(other.value_, nullptr)) {}
    ApplicationStateSnapshot& operator=(ApplicationStateSnapshot&& other) noexcept {
        if (this == &other) return *this;
        reset();
        owner_ = std::move(other.owner_);
        value_ = std::exchange(other.value_, nullptr);
        return *this;
    }
    ~ApplicationStateSnapshot() { reset(); }

private:
    friend class Runtime;
    ApplicationStateSnapshot(
        std::shared_ptr<detail::RuntimeControl> owner,
        strata_application_state_snapshot* value
    ) noexcept : owner_(std::move(owner)), value_(value) {}
    void reset() noexcept {
        if (value_ != nullptr) {
            strata_application_state_snapshot_release(std::exchange(value_, nullptr));
        }
        owner_.reset();
    }
    std::shared_ptr<detail::RuntimeControl> owner_;
    strata_application_state_snapshot* value_ = nullptr;
};

class Runtime final {
public:
    Runtime() : Runtime(RuntimeOptions{}) {}

    explicit Runtime(RuntimeOptions options) {
        if (!options.clock) throw std::invalid_argument("Runtime requires a monotonic clock");
        auto callbacks = std::make_shared<detail::RuntimeCallbacks>(std::move(options));
        strata_runtime_config config{};
        config.struct_size = sizeof(config);
        config.abi_version = STRATA_ABI_VERSION_CURRENT;
        config.required_capabilities = callbacks->options.required_capabilities;
        config.stable_identity_seed = callbacks->options.stable_identity_seed;
        config.allocator = callbacks->options.allocator;
        if (config.allocator.allocate != nullptr || config.allocator.deallocate != nullptr) {
            config.allocator.struct_size = sizeof(config.allocator);
            config.required_capabilities |= STRATA_CAPABILITY_CUSTOM_ALLOCATOR;
        }
        config.clock = strata_clock{
            sizeof(strata_clock), callbacks.get(), &detail::runtime_clock,
        };
        config.diagnostics = strata_diagnostic_sink{
            sizeof(strata_diagnostic_sink), callbacks.get(), &detail::runtime_diagnostic,
        };
        strata_runtime* value = nullptr;
        const strata_result created = strata_runtime_create(&config, &value);
        if (created.status != STRATA_STATUS_OK) {
            throw AbiError(
                created,
                "runtime creation",
                detail::find_diagnostic(callbacks.get(), created.diagnostic_id)
            );
        }
        try {
            control_ = std::make_shared<detail::RuntimeControl>(value, std::move(callbacks));
        } catch (...) {
            static_cast<void>(strata_runtime_release(value));
            throw;
        }
    }

    explicit Runtime(const strata_runtime_config& config) {
        if (config.diagnostics.struct_size != 0U &&
            config.diagnostics.struct_size < sizeof(strata_diagnostic_sink)) {
            strata_runtime* value = nullptr;
            const strata_result created = strata_runtime_create(&config, &value);
            if (created.status != STRATA_STATUS_OK) {
                throw AbiError(created, "runtime creation");
            }
            try {
                control_ = std::make_shared<detail::RuntimeControl>(value);
            } catch (...) {
                static_cast<void>(strata_runtime_release(value));
                throw;
            }
            return;
        }
        auto callbacks = std::make_shared<detail::RuntimeCallbacks>(
            RuntimeOptions{},
            config.diagnostics
        );
        strata_runtime_config bridged = config;
        bridged.diagnostics = strata_diagnostic_sink{
            sizeof(strata_diagnostic_sink),
            callbacks.get(),
            &detail::runtime_diagnostic,
        };
        strata_runtime* value = nullptr;
        const strata_result created = strata_runtime_create(&bridged, &value);
        if (created.status != STRATA_STATUS_OK) {
            throw AbiError(
                created,
                "runtime creation",
                detail::find_diagnostic(callbacks.get(), created.diagnostic_id)
            );
        }
        try {
            control_ = std::make_shared<detail::RuntimeControl>(
                value,
                std::move(callbacks)
            );
        } catch (...) {
            static_cast<void>(strata_runtime_release(value));
            throw;
        }
    }

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) noexcept = default;
    Runtime& operator=(Runtime&&) noexcept = default;

    [[nodiscard]] strata_runtime* native_handle() const noexcept {
        return control_ != nullptr ? control_->value : nullptr;
    }
    [[nodiscard]] bool live() const noexcept { return native_handle() != nullptr; }

    [[nodiscard]] std::uint64_t next_identity() {
        std::uint64_t identity = 0U;
        require(strata_runtime_next_identity(native_handle(), &identity), "identity allocation");
        return identity;
    }

    [[nodiscard]] Snapshot snapshot() {
        strata_snapshot* value = nullptr;
        require(
            strata_runtime_create_snapshot(native_handle(), &value),
            "runtime snapshot creation"
        );
        return Snapshot(control_, value);
    }

    [[nodiscard]] ApplicationStateSnapshot application_state() {
        strata_application_state_snapshot* value = nullptr;
        require(
            strata_runtime_create_application_state_snapshot(native_handle(), &value),
            "application state snapshot creation"
        );
        return ApplicationStateSnapshot(control_, value);
    }

    [[nodiscard]] bool restore_application_state(
        const ApplicationStateSnapshot& snapshot
    ) {
        std::uint32_t changed = 0U;
        require(
            strata_runtime_restore_application_state(
                native_handle(), snapshot.value_, &changed
            ),
            "application state restoration"
        );
        return changed != 0U;
    }

    [[nodiscard]] DiagnosticsSnapshot diagnostics() const {
        detail::DiagnosticsCapture capture;
        const strata_diagnostics_snapshot_sink sink{
            sizeof(strata_diagnostics_snapshot_sink),
            &capture,
            &detail::capture_diagnostics,
        };
        require(
            strata_runtime_read_diagnostics(native_handle(), &sink),
            "runtime diagnostics read"
        );
        if (capture.failed || !capture.emitted) {
            throw std::runtime_error("runtime diagnostics callback failed");
        }
        return std::move(capture.value);
    }

    void clear_diagnostics() {
        require(strata_runtime_clear_diagnostics(native_handle()), "runtime diagnostics clear");
        detail::clear_retained_diagnostics(control_.get());
    }

    [[nodiscard]] ProfilerSnapshot profiler() const {
        detail::ProfilerCapture capture;
        const strata_profiler_snapshot_sink sink{
            sizeof(strata_profiler_snapshot_sink),
            &capture,
            &detail::capture_profiler,
        };
        require(strata_runtime_read_profiler(native_handle(), &sink), "runtime profiler read");
        if (capture.failed || !capture.emitted) {
            throw std::runtime_error("runtime profiler callback failed");
        }
        return std::move(capture.value);
    }

    void set_profiler_capture(const bool enabled) {
        require(
            strata_runtime_set_profiler_capture(native_handle(), enabled ? 1U : 0U),
            "runtime profiler capture"
        );
    }

    [[nodiscard]] strata_host_snapshot_info host_snapshot_info() const {
        strata_host_snapshot_info info{};
        info.struct_size = sizeof(info);
        require(
            strata_runtime_get_host_snapshot_info(native_handle(), &info),
            "host snapshot info read"
        );
        return info;
    }

    [[nodiscard]] std::optional<std::string> host_value_json(
        const std::string_view path
    ) {
        detail::StringCapture capture;
        const strata_value_json_sink sink{
            sizeof(strata_value_json_sink), &capture, &detail::capture_string,
        };
        const strata_result result = strata_runtime_read_host_value_json(
            native_handle(),
            strata_string_view{path.data(), path.size()},
            &sink
        );
        if (result.status == STRATA_STATUS_NOT_FOUND) return std::nullopt;
        require(result, "host value read");
        if (capture.failed) throw std::bad_alloc();
        return std::move(capture.value);
    }

    [[nodiscard]] std::string active_unit_json() {
        detail::StringCapture capture;
        const strata_value_json_sink sink{
            sizeof(strata_value_json_sink), &capture, &detail::capture_string,
        };
        require(
            strata_runtime_read_active_unit_json(native_handle(), &sink),
            "active unit read"
        );
        if (capture.failed) throw std::bad_alloc();
        return std::move(capture.value);
    }

    [[nodiscard]] strata_runtime_memory_info memory_info() const {
        strata_runtime_memory_info info{};
        info.struct_size = sizeof(info);
        require(
            strata_runtime_get_memory_info(native_handle(), &info),
            "runtime memory telemetry"
        );
        return info;
    }

    /** The materials this application declares a source for on one backend. */
    [[nodiscard]] std::vector<MaterialDeclaration> material_declarations(
        const std::string_view backend
    ) const {
        const strata_string_view backend_view{backend.data(), backend.size()};
        std::size_t count = 0U;
        require(
            strata_runtime_read_material_declarations(
                native_handle(), backend_view, nullptr, 0U, &count
            ),
            "material declaration count"
        );
        if (count == 0U) return {};
        std::vector<strata_material_declaration> native(count);
        require(
            strata_runtime_read_material_declarations(
                native_handle(), backend_view, native.data(), native.size(), &count
            ),
            "material declaration read"
        );
        std::vector<MaterialDeclaration> declarations;
        declarations.reserve(count);
        for (std::size_t index = 0U; index < count; ++index) {
            declarations.push_back(MaterialDeclaration{
                detail::copied(native[index].id),
                detail::copied(native[index].blend_mode),
                detail::copied(native[index].fallback),
                detail::copied(native[index].source),
            });
        }
        return declarations;
    }

    /** Ordered passes for every render effect declared by this application. */
    [[nodiscard]] std::vector<EffectPassDeclaration> effect_pass_declarations(
        const std::string_view backend
    ) const {
        const strata_string_view backend_view{backend.data(), backend.size()};
        std::size_t count = 0U;
        require(
            strata_runtime_read_effect_pass_declarations(
                native_handle(), backend_view, nullptr, 0U, &count
            ),
            "effect declaration count"
        );
        if (count == 0U) return {};
        std::vector<strata_effect_pass_declaration> native(count);
        require(
            strata_runtime_read_effect_pass_declarations(
                native_handle(), backend_view, native.data(), native.size(), &count
            ),
            "effect declaration read"
        );
        std::vector<EffectPassDeclaration> declarations;
        declarations.reserve(count);
        for (std::size_t index = 0U; index < count; ++index) {
            declarations.push_back(EffectPassDeclaration{
                detail::copied(native[index].effect_id),
                native[index].index,
                native[index].kind == STRATA_EFFECT_PASS_SHADER
                    ? EffectPassKind::shader
                    : native[index].kind == STRATA_EFFECT_PASS_SHADOW
                        ? EffectPassKind::shadow
                        : EffectPassKind::blur,
                native[index].radius,
                native[index].downsample,
                native[index].radius_parameter == STRATA_EFFECT_PARAMETER_NONE
                    ? std::nullopt
                    : std::optional(native[index].radius_parameter),
                native[index].downsample_parameter == STRATA_EFFECT_PARAMETER_NONE
                    ? std::nullopt
                    : std::optional(native[index].downsample_parameter),
                detail::copied(native[index].source),
            });
        }
        return declarations;
    }

    void configure_application(const ApplicationOptions& options) {
        std::vector<strata_string_view> extension_schemas;
        extension_schemas.reserve(options.extension_schemas_json.size());
        for (const std::string& schema : options.extension_schemas_json) {
            extension_schemas.push_back(strata_string_view{schema.data(), schema.size()});
        }
        const strata_application_config config{
            sizeof(strata_application_config),
            strata_string_view{options.id.data(), options.id.size()},
            strata_string_view{options.schemas_json.data(), options.schemas_json.size()},
            extension_schemas.data(),
            extension_schemas.size(),
        };
        configure_application(config);
    }

    void configure_application(const strata_application_config& config) {
        require(
            strata_runtime_configure_application(native_handle(), &config),
            "application configuration"
        );
    }

    void set_resource_adapter(ResourceAdapter adapter) {
        if (adapter.generation == 0U || !adapter.load) {
            throw std::invalid_argument("resource adapter requires a generation and load callback");
        }
        auto bridge = std::make_shared<detail::ResourceBridge>(std::move(adapter));
        const strata_resource_adapter native{
            sizeof(strata_resource_adapter),
            bridge.get(),
            bridge->adapter.generation,
            &detail::resource_load,
        };
        require(
            strata_runtime_set_resource_adapter(native_handle(), &native),
            "resource adapter installation"
        );
        control_->resource = std::move(bridge);
    }

    [[nodiscard]] std::optional<std::vector<std::uint8_t>> resource(
        const std::string_view id
    ) {
        detail::ByteCapture capture;
        const strata_bytes_sink sink{
            sizeof(strata_bytes_sink), &capture, &detail::capture_bytes,
        };
        const strata_result result = strata_runtime_read_resource(
            native_handle(), strata_string_view{id.data(), id.size()}, &sink
        );
        if (result.status == STRATA_STATUS_NOT_FOUND) return std::nullopt;
        require(result, "resource read");
        if (capture.failed) throw std::bad_alloc();
        return std::move(capture.value);
    }

    void set_durable_store(DurableStoreAdapter adapter) {
        if (!adapter.load || !adapter.write) {
            throw std::invalid_argument("durable store requires load and write callbacks");
        }
        auto bridge = std::make_shared<detail::DurableBridge>(std::move(adapter));
        const strata_durable_store_adapter native{
            sizeof(strata_durable_store_adapter),
            bridge.get(),
            &detail::durable_load,
            &detail::durable_write,
        };
        require(
            strata_runtime_set_durable_store_adapter(native_handle(), &native),
            "durable store installation"
        );
        control_->durable = std::move(bridge);
    }

    void set_durable_store(const strata_durable_store_adapter& adapter) {
        require(
            strata_runtime_set_durable_store_adapter(native_handle(), &adapter),
            "durable store installation"
        );
    }

    void set_async_host(AsyncHostAdapter adapter) {
        if (!adapter.begin || !adapter.cancel) {
            throw std::invalid_argument("async host requires begin and cancel callbacks");
        }
        auto bridge = std::make_shared<detail::AsyncBridge>(std::move(adapter));
        const strata_async_host_adapter native{
            sizeof(strata_async_host_adapter),
            bridge.get(),
            &detail::async_begin,
            &detail::async_cancel,
        };
        require(
            strata_runtime_set_async_host_adapter(native_handle(), &native),
            "async host installation"
        );
        control_->async = std::move(bridge);
    }

    void set_async_host(const strata_async_host_adapter& adapter) {
        require(
            strata_runtime_set_async_host_adapter(native_handle(), &adapter),
            "async host installation"
        );
    }

    void set_clipboard_adapter(ClipboardAdapter adapter) {
        if (!adapter.read || !adapter.write) {
            throw std::invalid_argument("clipboard adapter requires read and write callbacks");
        }
        auto bridge = std::make_shared<detail::ClipboardBridge>(std::move(adapter));
        const strata_clipboard_adapter native{
            sizeof(strata_clipboard_adapter),
            bridge.get(),
            &detail::clipboard_read,
            &detail::clipboard_write,
        };
        require(
            strata_runtime_set_clipboard_adapter(native_handle(), &native),
            "clipboard adapter installation"
        );
        control_->clipboard = std::move(bridge);
    }

    [[nodiscard]] std::optional<std::string> clipboard_text() {
        detail::StringCapture capture;
        const strata_string_sink sink{
            sizeof(strata_string_sink), &capture, &detail::capture_string,
        };
        const strata_result result = strata_runtime_clipboard_read(native_handle(), &sink);
        if (result.status == STRATA_STATUS_NOT_FOUND) return std::nullopt;
        require(result, "clipboard read");
        if (capture.failed) throw std::bad_alloc();
        return std::move(capture.value);
    }

    void set_clipboard_text(const std::string_view text) {
        require(
            strata_runtime_clipboard_write(
                native_handle(), strata_string_view{text.data(), text.size()}
            ),
            "clipboard write"
        );
    }

    void set_ime_adapter(ImeAdapter adapter) {
        if (!adapter.set_active || !adapter.set_cursor_rect) {
            throw std::invalid_argument("IME adapter requires active and cursor callbacks");
        }
        auto bridge = std::make_shared<detail::ImeBridge>(std::move(adapter));
        const strata_ime_adapter native{
            sizeof(strata_ime_adapter),
            bridge.get(),
            &detail::ime_set_active,
            &detail::ime_set_cursor_rect,
        };
        require(
            strata_runtime_set_ime_adapter(native_handle(), &native),
            "IME adapter installation"
        );
        control_->ime = std::move(bridge);
    }

    void set_ime_active(const bool active) {
        require(
            strata_runtime_ime_set_active(native_handle(), active ? 1U : 0U),
            "IME active-state publication"
        );
    }

    void set_ime_cursor_rect(const Rect rect) {
        require(
            strata_runtime_ime_set_cursor_rect(
                native_handle(), strata_rect{rect.x, rect.y, rect.width, rect.height}
            ),
            "IME cursor publication"
        );
    }

    void set_effect_adapter(EffectAdapter adapter) {
        if (!adapter.emit) throw std::invalid_argument("effect adapter requires an emit callback");
        auto bridge = std::make_shared<detail::EffectBridge>(std::move(adapter));
        const strata_effect_adapter native{
            sizeof(strata_effect_adapter), bridge.get(), &detail::effect_emit,
        };
        require(
            strata_runtime_set_effect_adapter(native_handle(), &native),
            "effect adapter installation"
        );
        control_->effect = std::move(bridge);
    }

    void emit_effect(const EffectRequest& effect) {
        require(
            strata_runtime_emit_effect_json(
                native_handle(),
                strata_string_view{effect.id.data(), effect.id.size()},
                strata_string_view{effect.payload_json.data(), effect.payload_json.size()}
            ),
            "effect emission"
        );
    }

    [[nodiscard]] std::optional<std::string> durable_shell_value_json(
        const std::string_view key
    ) const {
        detail::StringCapture capture;
        const strata_value_json_sink sink{
            sizeof(strata_value_json_sink), &capture, &detail::capture_string,
        };
        const strata_result result = strata_runtime_read_durable_shell_value_json(
            native_handle(), strata_string_view{key.data(), key.size()}, &sink
        );
        if (result.status == STRATA_STATUS_NOT_FOUND) return std::nullopt;
        require(result, "durable shell value read");
        if (capture.failed) throw std::bad_alloc();
        return std::move(capture.value);
    }

    void write_durable_shell_value_json(
        const std::string_view key,
        const std::string_view value_json
    ) {
        require(
            strata_runtime_write_durable_shell_value_json(
                native_handle(),
                strata_string_view{key.data(), key.size()},
                strata_string_view{value_json.data(), value_json.size()}
            ),
            "durable shell value write"
        );
    }

    void flush_durable_state() {
        require(strata_runtime_flush_durable_state(native_handle()), "durable state flush");
    }

    void async_progress(const std::uint64_t request_id, const AsyncProgress& progress) {
        const strata_async_progress native{
            sizeof(strata_async_progress),
            progress.completed,
            progress.total.value_or(0.0),
            progress.total.has_value() ? 1U : 0U,
            0U,
            strata_string_view{progress.message.data(), progress.message.size()},
        };
        async_progress(request_id, native);
    }

    void async_progress(const std::uint64_t request_id, const strata_async_progress& progress) {
        require(
            strata_runtime_async_progress(native_handle(), request_id, &progress),
            "async progress publication"
        );
    }

    void async_succeed(const std::uint64_t request_id, const std::string_view value_json) {
        require(
            strata_runtime_async_succeed_json(
                native_handle(), request_id,
                strata_string_view{value_json.data(), value_json.size()}
            ),
            "async success publication"
        );
    }

    void async_fail(
        const std::uint64_t request_id,
        const std::string_view message,
        const std::string_view code = {}
    ) {
        require(
            strata_runtime_async_fail(
                native_handle(), request_id,
                strata_string_view{message.data(), message.size()},
                strata_string_view{code.data(), code.size()}
            ),
            "async failure publication"
        );
    }

    /** Publishes one immutable host root with a runtime-owned monotonic generation. */
    [[nodiscard]] std::uint64_t publish_host_snapshot(
        const std::string_view id,
        const std::string_view value_json
    ) {
        return detail::publish_host_snapshot(control_, id, value_json);
    }

    [[nodiscard]] ActivationInfo activate(const SourceActivation& activation) {
        detail::SourceLoaderBridge loader{
            activation.load_module ? &activation.load_module : nullptr,
            std::nullopt,
        };
        const strata_activation_config config{
            sizeof(strata_activation_config),
            activation.generation,
            strata_string_view{
                activation.entry_source_id.data(), activation.entry_source_id.size()
            },
            strata_string_view{activation.entry_text.data(), activation.entry_text.size()},
            loader.load != nullptr ? &loader : nullptr,
            loader.load != nullptr ? &detail::source_load : nullptr,
        };
        return detail::activation_info(activate(config));
    }

    [[nodiscard]] ActivationInfo activate(
        const std::uint64_t generation,
        const std::span<const std::uint8_t> artifact
    ) {
        const strata_compiled_activation_config config{
            sizeof(strata_compiled_activation_config),
            generation,
            strata_bytes_view{artifact.data(), artifact.size()},
        };
        return detail::activation_info(activate(config));
    }

    [[nodiscard]] strata_activation_info activate(const strata_activation_config& config) {
        strata_activation_info info{};
        info.struct_size = sizeof(info);
        require(
            strata_runtime_compile_and_activate(native_handle(), &config, &info),
            "application activation"
        );
        return info;
    }

    [[nodiscard]] strata_activation_info activate(
        const strata_compiled_activation_config& config
    ) {
        strata_activation_info info{};
        info.struct_size = sizeof(info);
        require(
            strata_runtime_activate_compiled_module(native_handle(), &config, &info),
            "compiled application activation"
        );
        return info;
    }

    [[nodiscard]] ActionDispatchInfo dispatch(const ActionDispatch& action) {
        const strata_action_dispatch_config config = detail::action_dispatch(action);
        strata_action_dispatch_info info{};
        info.struct_size = sizeof(info);
        require(
            strata_runtime_dispatch_action_json(native_handle(), &config, &info),
            "runtime action dispatch"
        );
        return detail::dispatch_info(info);
    }

    [[nodiscard]] std::optional<std::string> source_map_entry_json(
        const std::string_view compiled_path
    ) {
        detail::StringCapture capture;
        const strata_value_json_sink sink{
            sizeof(strata_value_json_sink), &capture, &detail::capture_string,
        };
        const strata_result result = strata_runtime_read_source_map_entry_json(
            native_handle(),
            strata_string_view{compiled_path.data(), compiled_path.size()},
            &sink
        );
        if (result.status == STRATA_STATUS_NOT_FOUND) return std::nullopt;
        require(result, "source-map entry read");
        if (capture.failed) throw std::bad_alloc();
        return std::move(capture.value);
    }

    [[nodiscard]] std::string source_map_entries_json(
        const std::string_view source_id,
        const std::uint32_t line,
        const std::uint32_t column
    ) {
        detail::StringCapture capture;
        const strata_value_json_sink sink{
            sizeof(strata_value_json_sink), &capture, &detail::capture_string,
        };
        require(
            strata_runtime_read_source_map_entries_at_json(
                native_handle(),
                strata_string_view{source_id.data(), source_id.size()},
                line,
                column,
                &sink
            ),
            "source-map position read"
        );
        if (capture.failed) throw std::bad_alloc();
        return std::move(capture.value);
    }

    [[nodiscard]] Surface create_surface(const SurfaceOptions& options) const;
    [[nodiscard]] Surface create_surface(const strata_surface_config& config) const;

    /** Explicitly releases an empty runtime; live Surfaces make this throw and remain recoverable. */
    void close() {
        if (control_ == nullptr || control_->value == nullptr) return;
        require(strata_runtime_release(control_->value), "runtime release");
        control_->value = nullptr;
        control_.reset();
    }

private:
    friend class host::Bindings;

    void require(const strata_result result, const std::string_view operation) const {
        detail::require_result(result, operation, control_.get());
    }

    std::shared_ptr<detail::RuntimeControl> control_;
};

class Surface final {
public:
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    Surface(Surface&& other) noexcept
        : owner_(std::move(other.owner_)), value_(std::exchange(other.value_, nullptr)) {}

    Surface& operator=(Surface&& other) noexcept {
        if (this == &other) return *this;
        abandon_noexcept();
        owner_ = std::move(other.owner_);
        value_ = std::exchange(other.value_, nullptr);
        return *this;
    }

    /**
     * Explicit close remains the ordered path. A forgotten live Surface is safely abandoned and
     * emits STRATA.SURFACE.RELEASE_ABANDONED instead of terminating the process.
     */
    ~Surface() { abandon_noexcept(); }

    [[nodiscard]] strata_surface* native_handle() const noexcept { return value_; }
    [[nodiscard]] bool live() const noexcept { return value_ != nullptr; }

    [[nodiscard]] strata_surface_frame_info frame(const std::int64_t time_nanoseconds) {
        strata_surface_frame_info info{};
        info.struct_size = sizeof(info);
        require(strata_surface_frame(value_, time_nanoseconds, &info), "surface frame");
        return info;
    }

    [[nodiscard]] bool adopt_environment(const SurfaceEnvironment& environment) {
        const strata_surface_environment native = environment.native();
        std::uint32_t adopted = 0U;
        require(
            strata_surface_adopt_environment(value_, &native, &adopted),
            "surface environment adoption"
        );
        return adopted != 0U;
    }

    [[nodiscard]] InputBatchInfo enqueue(const std::span<const InputEvent> events) {
        std::vector<strata_input_event> native;
        native.reserve(events.size());
        for (const InputEvent& event : events) native.push_back(event.native());
        strata_surface_input_batch_info info{};
        info.struct_size = sizeof(info);
        require(
            strata_surface_enqueue_input(value_, native.data(), native.size(), &info),
            "surface input enqueue"
        );
        return InputBatchInfo{info.accepted_event_count, info.queued_event_count};
    }

    [[nodiscard]] InputBatchInfo enqueue(const InputEvent& event) {
        return enqueue(std::span<const InputEvent>(&event, 1U));
    }

    void cancel_interactions() {
        require(strata_surface_cancel_interactions(value_), "surface interaction cancellation");
    }

    [[nodiscard]] ActionDispatchInfo dispatch(const ActionDispatch& action) {
        const strata_action_dispatch_config config = detail::action_dispatch(action);
        strata_action_dispatch_info info{};
        info.struct_size = sizeof(info);
        require(
            strata_surface_dispatch_action_json(value_, &config, &info),
            "surface action dispatch"
        );
        return detail::dispatch_info(info);
    }

    [[nodiscard]] bool set_focus_containment(
        const std::optional<std::string_view> key
    ) {
        std::uint32_t contained = 0U;
        const std::string_view value = key.value_or(std::string_view{});
        require(
            strata_surface_set_focus_containment(
                value_, strata_string_view{value.data(), value.size()}, &contained
            ),
            "surface focus containment"
        );
        return contained != 0U;
    }

    [[nodiscard]] DiagnosticsSnapshot diagnostics() const {
        detail::DiagnosticsCapture capture;
        const strata_diagnostics_snapshot_sink sink{
            sizeof(strata_diagnostics_snapshot_sink),
            &capture,
            &detail::capture_diagnostics,
        };
        require(
            strata_surface_read_diagnostics(value_, &sink),
            "surface diagnostics read"
        );
        if (capture.failed || !capture.emitted) {
            throw std::runtime_error("surface diagnostics callback failed");
        }
        return std::move(capture.value);
    }

    void clear_diagnostics() {
        require(strata_surface_clear_diagnostics(value_), "surface diagnostics clear");
        detail::clear_retained_diagnostics(owner_.get());
    }

    [[nodiscard]] ProfilerSnapshot profiler() const {
        detail::ProfilerCapture capture;
        const strata_profiler_snapshot_sink sink{
            sizeof(strata_profiler_snapshot_sink),
            &capture,
            &detail::capture_profiler,
        };
        require(strata_surface_read_profiler(value_, &sink), "surface profiler read");
        if (capture.failed || !capture.emitted) {
            throw std::runtime_error("surface profiler callback failed");
        }
        return std::move(capture.value);
    }

    void set_profiler_capture(const bool enabled) {
        require(
            strata_surface_set_profiler_capture(value_, enabled ? 1U : 0U),
            "surface profiler capture"
        );
    }

    void record_host_frame(const ProfilerHostFrame& telemetry) {
        const strata_profiler_host_frame native = telemetry.native();
        require(
            strata_surface_record_host_frame(value_, &native),
            "surface host-frame telemetry"
        );
    }

    [[nodiscard]] std::string drain_events_json() {
        detail::StringCapture capture;
        const strata_value_json_sink sink{
            sizeof(strata_value_json_sink), &capture, &detail::capture_string,
        };
        require(strata_surface_drain_events_json(value_, &sink), "surface event drain");
        if (capture.failed) throw std::bad_alloc();
        return std::move(capture.value);
    }

    [[nodiscard]] bool inspector_select(const std::string_view key) {
        std::uint32_t selected = 0U;
        require(
            strata_surface_inspector_select(
                value_, strata_string_view{key.data(), key.size()}, &selected
            ),
            "surface inspector selection"
        );
        return selected != 0U;
    }

    [[nodiscard]] bool inspector_pick(const Point point) {
        std::uint32_t selected = 0U;
        require(
            strata_surface_inspector_pick(value_, point.x, point.y, &selected),
            "surface inspector pick"
        );
        return selected != 0U;
    }

    void inspector_clear() {
        require(strata_surface_inspector_clear(value_), "surface inspector clear");
    }

    [[nodiscard]] std::string inspector_selection_json() const {
        detail::StringCapture capture;
        const strata_value_json_sink sink{
            sizeof(strata_value_json_sink), &capture, &detail::capture_string,
        };
        require(
            strata_surface_read_inspector_selection_json(value_, &sink),
            "surface inspector read"
        );
        if (capture.failed) throw std::bad_alloc();
        return std::move(capture.value);
    }

    [[nodiscard]] std::vector<std::uint8_t> render_packet() const {
        detail::ByteCapture capture;
        const strata_bytes_sink sink{
            sizeof(strata_bytes_sink), &capture, &detail::capture_bytes,
        };
        require(strata_surface_read_render_packet(value_, &sink), "render packet read");
        if (capture.failed) throw std::bad_alloc();
        return std::move(capture.value);
    }

    /**
     * Enters terminal two-phase teardown and copies the resource-only release packet. Submit the
     * packet while the host GPU owner is alive, then acknowledge_release_packet() and close().
     * Copying this packet does not acknowledge host consumption. Repeated calls are idempotent.
     */
    [[nodiscard]] std::vector<std::uint8_t> prepare_release_packet() {
        detail::ByteCapture capture;
        const strata_bytes_sink sink{
            sizeof(strata_bytes_sink), &capture, &detail::capture_bytes,
        };
        require(
            strata_surface_prepare_release_packet(value_, &sink),
            "surface release packet preparation"
        );
        if (capture.failed) throw std::bad_alloc();
        return std::move(capture.value);
    }

    void acknowledge_release_packet() {
        require(
            strata_surface_acknowledge_release_packet(value_),
            "surface release packet acknowledgement"
        );
    }

    [[nodiscard]] std::string frame_json() const {
        detail::StringCapture capture;
        const strata_value_json_sink sink{
            sizeof(strata_value_json_sink), &capture, &detail::capture_string,
        };
        require(strata_surface_read_frame_json(value_, &sink), "frame JSON read");
        if (capture.failed) throw std::bad_alloc();
        return std::move(capture.value);
    }

    void reload_resources() {
        require(strata_surface_reload_resources(value_), "surface resource reload");
    }

    [[nodiscard]] bool register_theme(const strata_theme& theme) {
        std::uint32_t changed = 0U;
        require(
            strata_surface_register_theme(value_, &theme, &changed),
            "surface theme registration"
        );
        return changed != 0U;
    }

    [[nodiscard]] bool set_theme(const strata_theme& theme) {
        std::uint32_t changed = 0U;
        require(
            strata_surface_set_theme(value_, &theme, &changed),
            "surface root theme selection"
        );
        return changed != 0U;
    }

    [[nodiscard]] bool unregister_theme(const std::string_view name) {
        std::uint32_t removed = 0U;
        require(
            strata_surface_unregister_theme(
                value_, strata_string_view{name.data(), name.size()}, &removed
            ),
            "surface theme removal"
        );
        return removed != 0U;
    }

    [[nodiscard]] bool set_scoped_theme(
        const std::string_view node_key,
        const strata_theme& theme
    ) {
        std::uint32_t changed = 0U;
        require(
            strata_surface_set_scoped_theme(
                value_,
                strata_string_view{node_key.data(), node_key.size()},
                &theme,
                &changed
            ),
            "surface scoped theme mutation"
        );
        return changed != 0U;
    }

    [[nodiscard]] bool clear_scoped_theme(const std::string_view node_key) {
        std::uint32_t removed = 0U;
        require(
            strata_surface_clear_scoped_theme(
                value_, strata_string_view{node_key.data(), node_key.size()}, &removed
            ),
            "surface scoped theme removal"
        );
        return removed != 0U;
    }

    [[nodiscard]] bool animate_scroll_to(const strata_scroll_animation_request& request) {
        std::uint32_t started = 0U;
        require(
            strata_surface_animate_scroll_to(value_, &request, &started),
            "surface scroll animation"
        );
        return started != 0U;
    }

    [[nodiscard]] bool animate_scroll_to(
        const std::string_view key,
        const std::optional<double> x,
        const std::optional<double> y,
        const std::string_view timing = "standard",
        const std::optional<std::int64_t> duration_nanoseconds = std::nullopt
    ) {
        const strata_scroll_animation_request request{
            sizeof(strata_scroll_animation_request),
            strata_string_view{key.data(), key.size()},
            x.has_value() ? 1U : 0U,
            y.has_value() ? 1U : 0U,
            x.value_or(0.0),
            y.value_or(0.0),
            strata_string_view{timing.data(), timing.size()},
            duration_nanoseconds.has_value() ? 1U : 0U,
            0U,
            duration_nanoseconds.value_or(0),
        };
        return animate_scroll_to(request);
    }

    void close() {
        if (value_ == nullptr) return;
        require(strata_surface_release(value_), "surface release");
        value_ = nullptr;
        owner_.reset();
    }

    /** Explicitly bypasses the packet barrier when delivery is impossible. */
    void abandon() {
        if (value_ == nullptr) return;
        require(strata_surface_abandon(value_), "surface abandon");
        value_ = nullptr;
        owner_.reset();
    }

private:
    friend class Runtime;
    Surface(std::shared_ptr<detail::RuntimeControl> owner, strata_surface* value) noexcept
        : owner_(std::move(owner)), value_(value) {}

    void require(const strata_result result, const std::string_view operation) const {
        detail::require_result(
            result,
            operation,
            owner_.get()
        );
    }

    void abandon_noexcept() noexcept {
        if (value_ == nullptr) return;
        static_cast<void>(strata_surface_abandon(value_));
        value_ = nullptr;
        owner_.reset();
    }

    std::shared_ptr<detail::RuntimeControl> owner_;
    strata_surface* value_ = nullptr;
};

inline Surface Runtime::create_surface(const SurfaceOptions& options) const {
    std::vector<strata_surface_font_resource> fonts;
    fonts.reserve(options.fonts.size());
    for (const FontResource& font : options.fonts) {
        fonts.push_back(strata_surface_font_resource{
            strata_string_view{font.id.data(), font.id.size()},
            strata_string_view{font.resource_id.data(), font.resource_id.size()},
        });
    }
    std::vector<strata_surface_image_resource> images;
    images.reserve(options.images.size());
    for (const ImageResource& image : options.images) {
        images.push_back(strata_surface_image_resource{
            strata_string_view{image.id.data(), image.id.size()},
            strata_string_view{image.resource_id.data(), image.resource_id.size()},
            image.sampling == ImageSampling::nearest
                ? STRATA_IMAGE_SAMPLING_NEAREST
                : STRATA_IMAGE_SAMPLING_LINEAR,
            0U,
        });
    }
    const strata_surface_config config{
        sizeof(strata_surface_config),
        strata_string_view{options.id.data(), options.id.size()},
        options.root_role == SurfaceRootRole::screen
            ? STRATA_SURFACE_ROOT_SCREEN
            : STRATA_SURFACE_ROOT_OVERLAY,
        0U,
        strata_string_view{options.root_name.data(), options.root_name.size()},
        options.environment.native(),
        fonts.data(),
        fonts.size(),
        options.extensions,
        images.data(),
        images.size(),
    };
    return create_surface(config);
}

inline Surface Runtime::create_surface(const strata_surface_config& config) const {
    strata_surface* value = nullptr;
    require(
        strata_runtime_create_surface(native_handle(), &config, &value),
        "surface creation"
    );
    return Surface(control_, value);
}

[[nodiscard]] constexpr strata_string_view view(const std::string_view value) noexcept {
    return strata_string_view{value.data(), value.size()};
}

} // namespace strata
