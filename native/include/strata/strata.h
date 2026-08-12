#ifndef STRATA_STRATA_H
#define STRATA_STRATA_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(STRATA_C_SHARED)
#if defined(STRATA_C_BUILD)
#define STRATA_API __declspec(dllexport)
#else
#define STRATA_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(STRATA_C_SHARED)
#define STRATA_API __attribute__((visibility("default")))
#else
#define STRATA_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define STRATA_ABI_VERSION_1 UINT32_C(1)
#define STRATA_ABI_VERSION_2 UINT32_C(2)
#define STRATA_ABI_VERSION_3 UINT32_C(3)
#define STRATA_ABI_VERSION_4 UINT32_C(4)
#define STRATA_ABI_VERSION_5 UINT32_C(5)
#define STRATA_ABI_VERSION_6 UINT32_C(6)
#define STRATA_ABI_VERSION_7 UINT32_C(7)
#define STRATA_ABI_VERSION_CURRENT STRATA_ABI_VERSION_7
#define STRATA_ABI_VERSION_MINIMUM STRATA_ABI_VERSION_7

typedef uint32_t strata_status;

#define STRATA_STATUS_OK UINT32_C(0)
#define STRATA_STATUS_INVALID_ARGUMENT UINT32_C(1)
#define STRATA_STATUS_UNSUPPORTED_ABI UINT32_C(2)
#define STRATA_STATUS_UNSUPPORTED_CAPABILITY UINT32_C(3)
#define STRATA_STATUS_INVALID_UTF8 UINT32_C(4)
#define STRATA_STATUS_CLOCK_REGRESSION UINT32_C(5)
#define STRATA_STATUS_OUT_OF_MEMORY UINT32_C(6)
#define STRATA_STATUS_INVARIANT_FAILURE UINT32_C(7)
#define STRATA_STATUS_INTERNAL_ERROR UINT32_C(8)
#define STRATA_STATUS_NOT_FOUND UINT32_C(9)
#define STRATA_STATUS_COMPILE_FAILED UINT32_C(10)
#define STRATA_STATUS_ACTIVATION_REJECTED UINT32_C(11)
#define STRATA_STATUS_SERVICE_UNAVAILABLE UINT32_C(12)
#define STRATA_STATUS_INPUT_QUEUE_FULL UINT32_C(13)

typedef uint32_t strata_diagnostic_severity;

#define STRATA_DIAGNOSTIC_INFO UINT32_C(0)
#define STRATA_DIAGNOSTIC_WARNING UINT32_C(1)
#define STRATA_DIAGNOSTIC_ERROR UINT32_C(2)
#define STRATA_DIAGNOSTIC_FATAL UINT32_C(3)

typedef struct strata_result {
    strata_status status;
    uint32_t reserved;
    uint64_t diagnostic_id;
} strata_result;

/* Borrowed UTF-8. The bytes need not be NUL-terminated and are valid only for the call. */
typedef struct strata_string_view {
    const char* data;
    size_t size;
} strata_string_view;

/* Borrowed bytes. The data is valid only for the duration of the call receiving it. */
typedef struct strata_bytes_view {
    const uint8_t* data;
    size_t size;
} strata_bytes_view;

typedef struct strata_source_range {
    uint64_t byte_start;
    uint64_t byte_end;
    uint32_t line_start;
    uint32_t column_start;
    uint32_t line_end;
    uint32_t column_end;
} strata_source_range;

#define STRATA_DIAGNOSTIC_VERSION_1 UINT32_C(1)
#define STRATA_DIAGNOSTIC_VERSION_CURRENT STRATA_DIAGNOSTIC_VERSION_1

/* Every string in a diagnostic is borrowed only for the duration of the callback. */
typedef struct strata_diagnostic {
    size_t struct_size;
    uint64_t id;
    strata_diagnostic_severity severity;
    uint32_t version;
    strata_string_view code;
    strata_string_view message;
    strata_string_view source_id;
    strata_source_range range;
    uint64_t occurrence_count;
    uint64_t sequence;
    uint64_t first_frame_index;
    uint64_t frame_index;
    uint64_t dropped_count;
    strata_string_view component_path;
    strata_string_view expected;
} strata_diagnostic;

#define STRATA_DIAGNOSTICS_SNAPSHOT_VERSION_1 UINT32_C(1)
#define STRATA_DIAGNOSTICS_SNAPSHOT_VERSION_CURRENT STRATA_DIAGNOSTICS_SNAPSHOT_VERSION_1

/* The record array and every record string are borrowed only for the duration of emit. */
typedef struct strata_diagnostics_snapshot {
    size_t struct_size;
    uint32_t version;
    uint32_t reserved;
    uint64_t frame_index;
    uint64_t dropped_count;
    const strata_diagnostic* records;
    size_t record_count;
} strata_diagnostics_snapshot;

typedef void (*strata_diagnostics_snapshot_fn)(void* user_data,
                                               const strata_diagnostics_snapshot* snapshot);

typedef struct strata_diagnostics_snapshot_sink {
    size_t struct_size;
    void* user_data;
    strata_diagnostics_snapshot_fn emit;
} strata_diagnostics_snapshot_sink;

#define STRATA_PROFILER_SNAPSHOT_VERSION_1 UINT32_C(1)
#define STRATA_PROFILER_SNAPSHOT_VERSION_CURRENT STRATA_PROFILER_SNAPSHOT_VERSION_1
#define STRATA_PROFILER_HOST_FRAME_VERSION_1 UINT32_C(1)
#define STRATA_PROFILER_HOST_FRAME_VERSION_CURRENT STRATA_PROFILER_HOST_FRAME_VERSION_1

typedef uint32_t strata_profiler_scope;
#define STRATA_PROFILER_SCOPE_RUNTIME UINT32_C(0)
#define STRATA_PROFILER_SCOPE_SURFACE UINT32_C(1)

/** Host-owned render/GPU measurements attached atomically to one completed surface frame. */
typedef struct strata_profiler_host_frame {
    size_t struct_size;
    uint32_t version;
    uint32_t reserved;
    uint64_t draw_calls;
    uint64_t batches;
    uint64_t vertices;
    uint64_t texture_creations;
    uint64_t texture_creation_bytes;
    uint64_t texture_uploads;
    uint64_t texture_upload_bytes;
    uint64_t queued_texture_creations;
    uint64_t queued_texture_uploads;
    uint64_t queued_texture_upload_bytes;
    uint64_t deferred_gpu_resources;
    uint64_t deferred_gpu_resource_deletions;
    uint64_t blur_passes;
    uint64_t blur_target_width;
    uint64_t blur_target_height;
    uint64_t blur_nanos;
    int64_t submit_nanos;
} strata_profiler_host_frame;

/* All arrays and strings in profiler payloads are borrowed only for the duration of emit. */
typedef struct strata_profiler_section {
    size_t struct_size;
    uint32_t version;
    uint32_t reserved;
    uint64_t id;
    uint64_t parent_id; /* Zero means no parent; canonical section ids begin at one. */
    strata_string_view name;
    strata_string_view path;
    uint64_t sample_count;
    uint64_t last_sample_frame_index;
    int64_t last_nanos;
    int64_t average_nanos;
    int64_t p50_nanos;
    int64_t p95_nanos;
    int64_t p99_nanos;
    int64_t maximum_nanos;
} strata_profiler_section;

typedef struct strata_profiler_counter {
    size_t struct_size;
    uint32_t version;
    uint32_t reserved;
    strata_string_view name;
    uint64_t value;
} strata_profiler_counter;

typedef struct strata_profiler_spike {
    size_t struct_size;
    uint32_t version;
    uint32_t reserved;
    uint64_t section_id;
    uint64_t parent_id;
    strata_string_view section_name;
    strata_string_view section_path;
    const uint64_t* stack_section_ids;
    size_t stack_depth;
    uint64_t frame_index;
    int64_t duration_nanos;
    int64_t absolute_threshold_nanos;
    int64_t effective_threshold_nanos;
    double baseline_multiplier;
    int64_t rolling_average_nanos;
    uint32_t has_rolling_average;
    uint32_t reserved_2;
    const strata_profiler_counter* counters;
    size_t counter_count;
} strata_profiler_spike;

typedef struct strata_profiler_snapshot {
    size_t struct_size;
    uint32_t version;
    strata_profiler_scope scope;
    strata_string_view scope_id;
    uint64_t frame_index;
    uint32_t capture_enabled;
    uint32_t reserved;
    uint64_t dropped_section_samples;
    uint64_t dropped_timing_samples;
    uint64_t dropped_spikes;
    const strata_profiler_section* sections;
    size_t section_count;
    const strata_profiler_counter* counters;
    size_t counter_count;
    const strata_profiler_spike* spikes;
    size_t spike_count;
} strata_profiler_snapshot;

typedef void (*strata_profiler_snapshot_fn)(void* user_data,
                                            const strata_profiler_snapshot* snapshot);

typedef struct strata_profiler_snapshot_sink {
    size_t struct_size;
    void* user_data;
    strata_profiler_snapshot_fn emit;
} strata_profiler_snapshot_sink;

typedef void* (*strata_allocate_fn)(void* user_data, size_t size, size_t alignment);
typedef void (*strata_deallocate_fn)(void* user_data, void* allocation, size_t size,
                                     size_t alignment);

typedef struct strata_allocator {
    size_t struct_size;
    void* user_data;
    strata_allocate_fn allocate;
    strata_deallocate_fn deallocate;
} strata_allocator;

typedef int64_t (*strata_clock_now_fn)(void* user_data);

typedef struct strata_clock {
    size_t struct_size;
    void* user_data;
    strata_clock_now_fn now_nanoseconds;
} strata_clock;

typedef void (*strata_diagnostic_fn)(void* user_data, const strata_diagnostic* diagnostic);

typedef struct strata_diagnostic_sink {
    size_t struct_size;
    void* user_data;
    strata_diagnostic_fn emit;
} strata_diagnostic_sink;

typedef uint64_t strata_capabilities;

#define STRATA_CAPABILITY_CORE_LIFECYCLE (UINT64_C(1) << 0)
#define STRATA_CAPABILITY_CUSTOM_ALLOCATOR (UINT64_C(1) << 1)
#define STRATA_CAPABILITY_CALLER_CLOCK (UINT64_C(1) << 2)
#define STRATA_CAPABILITY_IMMUTABLE_SNAPSHOTS (UINT64_C(1) << 3)
#define STRATA_CAPABILITY_STABLE_IDENTITIES (UINT64_C(1) << 4)
#define STRATA_CAPABILITY_HOST_SNAPSHOTS (UINT64_C(1) << 5)
#define STRATA_CAPABILITY_VALUE_JSON (UINT64_C(1) << 6)
#define STRATA_CAPABILITY_APPLICATION_LIFECYCLE (UINT64_C(1) << 7)
#define STRATA_CAPABILITY_COMPILER_ACTIVATION (UINT64_C(1) << 8)
#define STRATA_CAPABILITY_ACTION_DISPATCH (UINT64_C(1) << 9)
#define STRATA_CAPABILITY_RESOURCE_ADAPTER (UINT64_C(1) << 10)
#define STRATA_CAPABILITY_CLIPBOARD_IME_ADAPTER (UINT64_C(1) << 11)
#define STRATA_CAPABILITY_EFFECT_ADAPTER (UINT64_C(1) << 12)
#define STRATA_CAPABILITY_SURFACE_RUNTIME (UINT64_C(1) << 13)
#define STRATA_CAPABILITY_SURFACE_RENDER_PACKET (UINT64_C(1) << 14)
#define STRATA_CAPABILITY_SURFACE_EXTENSIONS (UINT64_C(1) << 15)
#define STRATA_CAPABILITY_ALLOCATOR_TELEMETRY (UINT64_C(1) << 16)
#define STRATA_CAPABILITY_SOURCE_MAP_LOOKUP (UINT64_C(1) << 18)
#define STRATA_CAPABILITY_SURFACE_EVENT_DRAIN (UINT64_C(1) << 19)
#define STRATA_CAPABILITY_DIAGNOSTIC_SNAPSHOTS (UINT64_C(1) << 20)
#define STRATA_CAPABILITY_PROFILER_SNAPSHOTS (UINT64_C(1) << 21)
#define STRATA_CAPABILITY_SURFACE_THEMES (UINT64_C(1) << 22)
#define STRATA_CAPABILITY_COMPILED_MODULE_ACTIVATION (UINT64_C(1) << 23)
#define STRATA_CAPABILITY_DURABLE_STATE (UINT64_C(1) << 24)
#define STRATA_CAPABILITY_ASYNC_HOST_DATA (UINT64_C(1) << 25)

typedef struct strata_api_info {
    size_t struct_size;
    uint32_t abi_version;
    uint32_t minimum_abi_version;
    strata_capabilities capabilities;
} strata_api_info;

typedef struct strata_runtime_config {
    size_t struct_size;
    uint32_t abi_version;
    uint32_t reserved;
    strata_capabilities required_capabilities;
    uint64_t stable_identity_seed;
    strata_allocator allocator;
    strata_clock clock;
    strata_diagnostic_sink diagnostics;
} strata_runtime_config;

typedef struct strata_snapshot_info {
    size_t struct_size;
    uint64_t generation;
    int64_t time_nanoseconds;
    uint64_t last_stable_identity;
} strata_snapshot_info;

/*
 * Counts memory routed through the runtime's configured allocator. `routed_*` includes opaque ABI
 * handles and arena backing blocks; `arena_*` is the subset owned by the runtime scratch arena.
 * Storage owned by ordinary C++ standard-library allocators is deliberately not included.
 */
typedef struct strata_runtime_memory_info {
    size_t struct_size;
    uint64_t routed_current_bytes;
    uint64_t routed_peak_bytes;
    uint64_t routed_total_bytes;
    uint64_t routed_live_allocations;
    uint64_t routed_peak_live_allocations;
    uint64_t routed_allocation_count;
    uint64_t routed_deallocation_count;
    uint64_t arena_current_bytes;
    uint64_t arena_peak_bytes;
    uint64_t arena_total_bytes;
    uint64_t arena_live_allocations;
    uint64_t arena_peak_live_allocations;
    uint64_t arena_allocation_count;
    uint64_t arena_deallocation_count;
} strata_runtime_memory_info;

/* The runtime copies both strings during the call; values_json must contain one JSON object. */
typedef struct strata_host_snapshot_config {
    size_t struct_size;
    strata_string_view id;
    uint64_t generation;
    strata_string_view values_json;
} strata_host_snapshot_config;

typedef struct strata_host_snapshot_info {
    size_t struct_size;
    uint64_t generation;
    uint64_t evaluated_scalar_count;
    uint32_t has_snapshot;
    uint32_t reserved;
} strata_host_snapshot_info;

/* The UTF-8 JSON is borrowed only for the duration of emit. */
typedef void (*strata_value_json_fn)(void* user_data, strata_string_view value_json);

typedef struct strata_value_json_sink {
    size_t struct_size;
    void* user_data;
    strata_value_json_fn emit;
} strata_value_json_sink;

/* Application configuration is copied during the call. Empty schemas_json selects built-ins only.
 */
typedef struct strata_application_config {
    size_t struct_size;
    strata_string_view id;
    strata_string_view schemas_json;
    /*
     * Declaration documents of the native extension packages this application activates, applied
     * before schemas_json. Hosts project them from the same package definition that produces the
     * surface extension bundle, so widget, behavior, and action contracts cannot drift.
     */
    const strata_string_view* extension_schemas_json;
    size_t extension_schema_count;
} strata_application_config;

typedef struct strata_module_source {
    size_t struct_size;
    strata_string_view source_id;
    strata_string_view text;
} strata_module_source;

/* Returned module views are borrowed only for the callback and are copied before it returns. */
typedef strata_status (*strata_module_load_fn)(void* user_data,
                                               strata_string_view importer_source_id,
                                               strata_string_view import_path,
                                               strata_module_source* out_source);

typedef struct strata_activation_config {
    size_t struct_size;
    uint64_t generation;
    strata_string_view entry_source_id;
    strata_string_view entry_text;
    void* loader_user_data;
    strata_module_load_fn load_module;
} strata_activation_config;

/* Compact build-time artifact activation; source compilation supports explicit same-contract
 * module reactivation. */
typedef struct strata_compiled_activation_config {
    size_t struct_size;
    uint64_t generation;
    strata_bytes_view artifact;
} strata_compiled_activation_config;

typedef uint32_t strata_activation_status;

#define STRATA_ACTIVATION_ACTIVATED UINT32_C(0)
#define STRATA_ACTIVATION_REJECTED_GENERATION UINT32_C(1)
#define STRATA_ACTIVATION_REJECTED_COMPILE UINT32_C(2)
#define STRATA_ACTIVATION_REJECTED_UNIT UINT32_C(3)
#define STRATA_ACTIVATION_REJECTED_CAPABILITY UINT32_C(4)

typedef struct strata_activation_info {
    size_t struct_size;
    strata_activation_status status;
    uint32_t state_migrated;
    uint32_t has_active_generation;
    uint32_t reserved;
    uint64_t attempted_generation;
    uint64_t active_generation;
    uint64_t diagnostic_count;
} strata_activation_info;

typedef uint32_t strata_action_handler_result;

#define STRATA_ACTION_HANDLER_HANDLED UINT32_C(0)
#define STRATA_ACTION_HANDLER_FORWARDED UINT32_C(1)
#define STRATA_ACTION_HANDLER_IGNORED UINT32_C(2)

/* All views in an action call are borrowed only for the callback. JSON is canonical UTF-8. */
typedef struct strata_action_call {
    size_t struct_size;
    strata_string_view action_id;
    strata_string_view payload_json;
    strata_string_view event_kind;
    strata_string_view source_key;
    strata_string_view event_value_json;
} strata_action_call;

typedef strata_action_handler_result (*strata_action_handler_fn)(void* user_data,
                                                                 const strata_action_call* call);

typedef struct strata_action_handler_config {
    size_t struct_size;
    strata_string_view action_id;
    strata_string_view owner;
    void* user_data;
    strata_action_handler_fn invoke;
} strata_action_handler_config;

typedef struct strata_action_dispatch_config {
    size_t struct_size;
    strata_string_view action_id;
    strata_string_view payload_json;
    strata_string_view event_kind;
    strata_string_view source_key;
    strata_string_view event_value_json;
    /* Required only by declaration-owned framework state actions. */
    strata_string_view state_scope;
    /* Explicit unsafe bridge for scripting/remote action ids outside the active registry. */
    uint32_t dynamic;
    uint32_t reserved;
} strata_action_dispatch_config;

typedef uint32_t strata_action_dispatch_status;

#define STRATA_ACTION_DISPATCH_NO_ACTION UINT32_C(0)
#define STRATA_ACTION_DISPATCH_HANDLED UINT32_C(1)
#define STRATA_ACTION_DISPATCH_FORWARDED UINT32_C(2)
#define STRATA_ACTION_DISPATCH_IGNORED UINT32_C(3)
#define STRATA_ACTION_DISPATCH_UNHANDLED UINT32_C(4)
#define STRATA_ACTION_DISPATCH_FAILED UINT32_C(5)

typedef struct strata_action_dispatch_info {
    size_t struct_size;
    strata_action_dispatch_status status;
    uint32_t reserved;
    uint64_t handler_count;
} strata_action_dispatch_info;

typedef void (*strata_bytes_fn)(void* user_data, strata_bytes_view bytes);

typedef struct strata_bytes_sink {
    size_t struct_size;
    void* user_data;
    strata_bytes_fn emit;
} strata_bytes_sink;

typedef void (*strata_string_fn)(void* user_data, strata_string_view text);

typedef struct strata_string_sink {
    size_t struct_size;
    void* user_data;
    strata_string_fn emit;
} strata_string_sink;

/* Returned resource bytes are borrowed from the host and copied before load returns. */
typedef strata_status (*strata_resource_load_fn)(void* user_data, strata_string_view resource_id,
                                                 strata_bytes_view* out_bytes);

typedef struct strata_resource_adapter {
    size_t struct_size;
    void* user_data;
    strata_resource_load_fn load;
} strata_resource_adapter;

/* Missing durable data returns STRATA_STATUS_NOT_FOUND. Loaded bytes are copied before return. */
typedef strata_status (*strata_durable_load_fn)(void* user_data, strata_string_view application_id,
                                                strata_bytes_view* out_bytes);
typedef strata_status (*strata_durable_write_fn)(void* user_data, strata_string_view application_id,
                                                 strata_bytes_view bytes);
typedef struct strata_durable_store_adapter {
    size_t struct_size;
    void* user_data;
    strata_durable_load_fn load;
    strata_durable_write_fn write;
} strata_durable_store_adapter;

/*
 * Async request payload JSON is canonical and borrowed for the callback. Completion functions may
 * be called from any host thread; core adopts their copied payloads on the owner thread. After a
 * cancel callback begins, the request is stale and later completions are deterministically dropped.
 * The host must quiesce its callbacks before releasing the runtime handle.
 */
typedef strata_status (*strata_async_begin_fn)(void* user_data, uint64_t request_id,
                                               strata_string_view binding, strata_string_view owner,
                                               strata_string_view payload_json);
typedef void (*strata_async_cancel_fn)(void* user_data, uint64_t request_id);
typedef struct strata_async_host_adapter {
    size_t struct_size;
    void* user_data;
    strata_async_begin_fn begin;
    strata_async_cancel_fn cancel;
} strata_async_host_adapter;

typedef struct strata_async_progress {
    size_t struct_size;
    double completed;
    double total;
    uint32_t has_total;
    uint32_t reserved;
    strata_string_view message;
} strata_async_progress;

/* Clipboard reads are borrowed from the host and copied before read returns. */
typedef strata_status (*strata_clipboard_read_fn)(void* user_data, strata_string_view* out_text);
typedef strata_status (*strata_clipboard_write_fn)(void* user_data, strata_string_view text);

typedef struct strata_clipboard_adapter {
    size_t struct_size;
    void* user_data;
    strata_clipboard_read_fn read;
    strata_clipboard_write_fn write;
} strata_clipboard_adapter;

typedef struct strata_rect {
    double x;
    double y;
    double width;
    double height;
} strata_rect;

typedef strata_status (*strata_ime_set_active_fn)(void* user_data, uint32_t active);
typedef strata_status (*strata_ime_set_cursor_rect_fn)(void* user_data, strata_rect logical_rect);

typedef struct strata_ime_adapter {
    size_t struct_size;
    void* user_data;
    strata_ime_set_active_fn set_active;
    strata_ime_set_cursor_rect_fn set_cursor_rect;
} strata_ime_adapter;

/* Effect payload JSON is canonical UTF-8 and borrowed only for the callback. */
typedef strata_status (*strata_effect_emit_fn)(void* user_data, strata_string_view effect_id,
                                               strata_string_view payload_json);

typedef struct strata_effect_adapter {
    size_t struct_size;
    void* user_data;
    strata_effect_emit_fn emit;
} strata_effect_adapter;

typedef uint32_t strata_surface_root_role;

#define STRATA_SURFACE_ROOT_SCREEN UINT32_C(0)
#define STRATA_SURFACE_ROOT_OVERLAY UINT32_C(1)

typedef uint32_t strata_point_snap_policy;

#define STRATA_POINT_SNAP_NONE UINT32_C(0)
#define STRATA_POINT_SNAP_NEAREST UINT32_C(1)

typedef uint32_t strata_rectangle_snap_policy;

#define STRATA_RECTANGLE_SNAP_NONE UINT32_C(0)
#define STRATA_RECTANGLE_SNAP_NEAREST UINT32_C(1)
#define STRATA_RECTANGLE_SNAP_OUTWARD UINT32_C(2)

typedef uint32_t strata_scale_policy_kind;

#define STRATA_SCALE_POLICY_AUTO_FIT UINT32_C(0)
#define STRATA_SCALE_POLICY_MANUAL UINT32_C(1)
#define STRATA_SCALE_POLICY_PIXEL_PERFECT UINT32_C(2)
#define STRATA_SCALE_POLICY_FLUID UINT32_C(3)

/**
 * Portable uniform-scale policy. Initialize through strata_scale_policy_defaults, then override
 * only fields owned by the selected kind. Stretch-to-fit is deliberately unsupported.
 */
typedef struct strata_scale_policy_config {
    size_t struct_size;
    strata_scale_policy_kind kind;
    uint32_t prefer_integer_scale;
    double preferred_logical_width;
    double preferred_logical_height;
    double min_scale;
    double max_scale;
    double rational_step;
    double integer_preference_tolerance;
    double manual_multiplier;
    double asset_scale;
    double minimum_logical_width;
    double minimum_logical_height;
    int64_t minimum_integer_scale;
    int64_t maximum_integer_scale;
    strata_point_snap_policy point_snapping;
    strata_rectangle_snap_policy rectangle_snapping;
    uint32_t reserved;
} strata_scale_policy_config;

typedef struct strata_scale_context {
    size_t struct_size;
    int64_t framebuffer_width;
    int64_t framebuffer_height;
    double logical_width;
    double logical_height;
    double scale;
    uint32_t integer_scale;
    strata_point_snap_policy point_snapping;
    strata_rectangle_snap_policy rectangle_snapping;
    uint32_t reserved;
} strata_scale_context;

typedef uint32_t strata_surface_density;

#define STRATA_SURFACE_DENSITY_COMPACT UINT32_C(0)
#define STRATA_SURFACE_DENSITY_COMFORTABLE UINT32_C(1)

typedef uint32_t strata_pointer_precision;

#define STRATA_POINTER_PRECISION_NONE UINT32_C(0)
#define STRATA_POINTER_PRECISION_COARSE UINT32_C(1)
#define STRATA_POINTER_PRECISION_FINE UINT32_C(2)

typedef uint64_t strata_surface_input_capabilities;

#define STRATA_SURFACE_INPUT_POINTER (UINT64_C(1) << 0)
#define STRATA_SURFACE_INPUT_KEYBOARD (UINT64_C(1) << 1)
#define STRATA_SURFACE_INPUT_TOUCH (UINT64_C(1) << 2)
#define STRATA_SURFACE_INPUT_IME (UINT64_C(1) << 3)
#define STRATA_SURFACE_INPUT_CLIPBOARD (UINT64_C(1) << 4)
#define STRATA_SURFACE_INPUT_CONTROLLER (UINT64_C(1) << 5)

typedef struct strata_surface_environment {
    size_t struct_size;
    uint64_t generation;
    int64_t framebuffer_width;
    int64_t framebuffer_height;
    double logical_width;
    double logical_height;
    double scale;
    double safe_inset_left;
    double safe_inset_top;
    double safe_inset_right;
    double safe_inset_bottom;
    strata_point_snap_policy point_snapping;
    strata_rectangle_snap_policy rectangle_snapping;
    strata_surface_density density;
    strata_pointer_precision pointer_precision;
    strata_surface_input_capabilities input_capabilities;
    uint32_t reduced_motion;
    uint32_t reserved;
} strata_surface_environment;

typedef struct strata_surface_font_resource {
    /* Logical id referenced by .strata styles, for example strata:fonts/mono. */
    strata_string_view id;
    /* Host resource-adapter id for the OpenType bytes. */
    strata_string_view resource_id;
} strata_surface_font_resource;

typedef uint32_t strata_image_sampling;
#define STRATA_IMAGE_SAMPLING_NEAREST UINT32_C(0)
#define STRATA_IMAGE_SAMPLING_LINEAR UINT32_C(1)

typedef struct strata_surface_image_resource {
    /* Logical image id referenced by .strata Image/icon properties. */
    strata_string_view id;
    /* Host resource-adapter id for an encoded PNG or static SVG document. */
    strata_string_view resource_id;
    /* Raster sampling mode; ignored for resolution-independent SVG images. */
    strata_image_sampling sampling;
    uint32_t reserved;
} strata_surface_image_resource;

typedef struct strata_widget_input_context strata_widget_input_context;
typedef struct strata_widget_render_context strata_widget_render_context;
typedef struct strata_widget_inspection_context strata_widget_inspection_context;
typedef struct strata_widget_semantics_context strata_widget_semantics_context;
typedef struct strata_behavior_input_context strata_behavior_input_context;
typedef struct strata_widget_value strata_widget_value;
typedef struct strata_widget_subtargets_context strata_widget_subtargets_context;

typedef struct strata_color {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;
} strata_color;

typedef uint32_t strata_widget_value_kind;
#define STRATA_WIDGET_VALUE_NULL UINT32_C(0)
#define STRATA_WIDGET_VALUE_BOOLEAN UINT32_C(1)
#define STRATA_WIDGET_VALUE_NUMBER UINT32_C(2)
#define STRATA_WIDGET_VALUE_DURATION UINT32_C(3)
#define STRATA_WIDGET_VALUE_TEXT UINT32_C(4)
#define STRATA_WIDGET_VALUE_COLOR UINT32_C(5)
#define STRATA_WIDGET_VALUE_IMAGE UINT32_C(6)
#define STRATA_WIDGET_VALUE_KEY UINT32_C(7)
#define STRATA_WIDGET_VALUE_THEME_TOKEN UINT32_C(8)
#define STRATA_WIDGET_VALUE_LIST UINT32_C(9)
#define STRATA_WIDGET_VALUE_OBJECT UINT32_C(10)

typedef struct strata_border {
    double width;
    strata_color color;
} strata_border;

typedef uint32_t strata_widget_text_alignment;
#define STRATA_WIDGET_TEXT_ALIGN_START UINT32_C(0)
#define STRATA_WIDGET_TEXT_ALIGN_CENTER UINT32_C(1)
#define STRATA_WIDGET_TEXT_ALIGN_END UINT32_C(2)

#define STRATA_THEME_MODEL_VERSION_1 UINT32_C(1)
#define STRATA_THEME_MODEL_VERSION_2 UINT32_C(2)
#define STRATA_THEME_MODEL_VERSION_3 UINT32_C(3)
#define STRATA_THEME_MODEL_VERSION_CURRENT STRATA_THEME_MODEL_VERSION_3

typedef uint32_t strata_theme_value_mode;

/* In a full section value, UNSPECIFIED and NONE both decode to null; widget motion uses
 * UNSPECIFIED to inherit/omit and NONE to explicitly disable. */
#define STRATA_THEME_VALUE_UNSPECIFIED UINT32_C(0)
#define STRATA_THEME_VALUE_SET UINT32_C(1)
#define STRATA_THEME_VALUE_NONE UINT32_C(2)

typedef struct strata_theme_optional_color {
    strata_theme_value_mode mode;
    uint32_t reserved;
    strata_color value;
} strata_theme_optional_color;

typedef struct strata_theme_optional_border {
    strata_theme_value_mode mode;
    uint32_t reserved;
    struct {
        double width;
        strata_color color;
        uint32_t inside;
        uint32_t reserved;
    } value;
} strata_theme_optional_border;

typedef struct strata_theme_optional_number {
    strata_theme_value_mode mode;
    uint32_t reserved;
    double value;
} strata_theme_optional_number;

typedef struct strata_theme_tokens {
    size_t struct_size;
    strata_color surface;
    strata_color surface_raised;
    strata_color foreground;
    strata_color muted_foreground;
    strata_color accent;
    strata_color danger;
    strata_color focus;
    double spacing_unit;
    double radius;
    double density;
    uint32_t reserved;
} strata_theme_tokens;

typedef uint32_t strata_theme_motion_easing_kind;
#define STRATA_THEME_MOTION_EASING_LINEAR UINT32_C(0)
#define STRATA_THEME_MOTION_EASING_CUBIC_IN UINT32_C(1)
#define STRATA_THEME_MOTION_EASING_CUBIC_OUT UINT32_C(2)
#define STRATA_THEME_MOTION_EASING_CUBIC_IN_OUT UINT32_C(3)
#define STRATA_THEME_MOTION_EASING_CUBIC_BEZIER UINT32_C(4)

typedef struct strata_theme_motion_easing {
    size_t struct_size;
    strata_theme_motion_easing_kind kind;
    uint32_t reserved;
    double x1;
    double y1;
    double x2;
    double y2;
} strata_theme_motion_easing;

typedef uint32_t strata_theme_motion_repeat_kind;
#define STRATA_THEME_MOTION_REPEAT_NONE UINT32_C(0)
#define STRATA_THEME_MOTION_REPEAT_COUNT UINT32_C(1)
#define STRATA_THEME_MOTION_REPEAT_FOREVER UINT32_C(2)

typedef uint32_t strata_theme_motion_fill_mode;
#define STRATA_THEME_MOTION_FILL_NONE UINT32_C(0)
#define STRATA_THEME_MOTION_FILL_FORWARDS UINT32_C(1)
#define STRATA_THEME_MOTION_FILL_BACKWARDS UINT32_C(2)
#define STRATA_THEME_MOTION_FILL_BOTH UINT32_C(3)

typedef struct strata_theme_motion_timing {
    size_t struct_size;
    strata_string_view name;
    int64_t duration_nanoseconds;
    int64_t delay_nanoseconds;
    strata_theme_motion_easing easing;
    strata_theme_motion_repeat_kind repeat_kind;
    uint32_t repeat_count;
    uint32_t reverse;
    strata_theme_motion_fill_mode fill_mode;
    uint32_t reserved;
} strata_theme_motion_timing;

typedef struct strata_theme_motion_policy {
    size_t struct_size;
    uint32_t reduced_motion;
    uint32_t reserved;
    const strata_theme_motion_timing* timings;
    size_t timing_count;
} strata_theme_motion_policy;

typedef struct strata_theme_visual_style {
    size_t struct_size;
    strata_theme_optional_color background;
    strata_color foreground;
    strata_theme_optional_border border;
    double radius;
    strata_theme_optional_color hover_overlay;
    strata_theme_optional_color active_overlay;
    strata_theme_optional_border focus_ring;
    double disabled_opacity;
    double opacity;
    double translate_x;
    double translate_y;
    double scale;
    double scale_x;
    double scale_y;
    strata_theme_optional_color track;
    strata_theme_optional_color fill;
    strata_theme_optional_color thumb;
    strata_theme_optional_color selection;
    strata_theme_optional_color scrim;
    strata_theme_optional_number indicator_size;
    strata_theme_optional_number indicator_inset;
    strata_theme_optional_number track_width;
    strata_theme_optional_number track_height;
    strata_theme_optional_number track_radius;
    strata_theme_optional_number thumb_size;
    strata_theme_optional_number thumb_radius;
    strata_theme_optional_number indicator_position;
    uint32_t reserved;
} strata_theme_visual_style;

typedef struct strata_theme_text_visual_style {
    size_t struct_size;
    strata_color color;
    strata_color hint_color;
    strata_color selection_color;
    strata_color caret_color;
    uint32_t reserved;
} strata_theme_text_visual_style;

typedef struct strata_theme_text_layout_style {
    size_t struct_size;
    strata_string_view primary_font;
    const strata_string_view* fallback_fonts;
    size_t fallback_font_count;
    double pixel_size;
    uint32_t style_flags;
    uint32_t reserved;
    strata_theme_optional_number line_height;
    double line_height_multiplier;
    double letter_spacing;
} strata_theme_text_layout_style;

typedef uint32_t strata_theme_layout_kind;
#define STRATA_THEME_LAYOUT_STACK UINT32_C(0)
#define STRATA_THEME_LAYOUT_ROW UINT32_C(1)
#define STRATA_THEME_LAYOUT_COLUMN UINT32_C(2)
#define STRATA_THEME_LAYOUT_GRID UINT32_C(3)
#define STRATA_THEME_LAYOUT_PANEL UINT32_C(4)
#define STRATA_THEME_LAYOUT_OVERLAY UINT32_C(5)
#define STRATA_THEME_LAYOUT_SPACER UINT32_C(6)
#define STRATA_THEME_LAYOUT_SCROLL UINT32_C(7)
#define STRATA_THEME_LAYOUT_PORTAL UINT32_C(8)

typedef uint32_t strata_theme_layout_size_kind;
#define STRATA_THEME_SIZE_AUTO UINT32_C(0)
#define STRATA_THEME_SIZE_CONTENT UINT32_C(1)
#define STRATA_THEME_SIZE_FIXED UINT32_C(2)
#define STRATA_THEME_SIZE_PERCENT UINT32_C(3)
#define STRATA_THEME_SIZE_FILL UINT32_C(4)
#define STRATA_THEME_SIZE_CLAMP UINT32_C(5)

typedef struct strata_theme_layout_size {
    size_t struct_size;
    strata_theme_layout_size_kind kind;
    uint32_t reserved;
    double value;
    const struct strata_theme_layout_size* minimum;
    const struct strata_theme_layout_size* preferred;
    const struct strata_theme_layout_size* maximum;
} strata_theme_layout_size;

typedef struct strata_theme_edges {
    double left;
    double top;
    double right;
    double bottom;
} strata_theme_edges;

typedef struct strata_theme_point {
    double x;
    double y;
} strata_theme_point;

typedef struct strata_theme_size {
    double width;
    double height;
} strata_theme_size;

typedef uint32_t strata_theme_layout_align;
#define STRATA_THEME_ALIGN_START UINT32_C(0)
#define STRATA_THEME_ALIGN_CENTER UINT32_C(1)
#define STRATA_THEME_ALIGN_END UINT32_C(2)
#define STRATA_THEME_ALIGN_STRETCH UINT32_C(3)
#define STRATA_THEME_ALIGN_UNSPECIFIED UINT32_MAX

typedef uint32_t strata_theme_layout_justify;
#define STRATA_THEME_JUSTIFY_START UINT32_C(0)
#define STRATA_THEME_JUSTIFY_CENTER UINT32_C(1)
#define STRATA_THEME_JUSTIFY_END UINT32_C(2)
#define STRATA_THEME_JUSTIFY_SPACE_BETWEEN UINT32_C(3)
#define STRATA_THEME_JUSTIFY_SPACE_AROUND UINT32_C(4)
#define STRATA_THEME_JUSTIFY_SPACE_EVENLY UINT32_C(5)

typedef uint32_t strata_theme_layout_axis;
#define STRATA_THEME_AXIS_HORIZONTAL UINT32_C(0)
#define STRATA_THEME_AXIS_VERTICAL UINT32_C(1)

typedef struct strata_theme_virtual_item_members {
    const strata_string_view* keys;
    size_t key_count;
} strata_theme_virtual_item_members;

typedef struct strata_theme_virtual_list {
    size_t struct_size;
    strata_theme_layout_axis axis;
    uint32_t measure_item_extents;
    double item_extent;
    size_t overscan;
    const strata_string_view* item_keys;
    size_t item_count;
    const strata_theme_virtual_item_members* item_members;
    size_t item_member_count;
    const double* item_extents;
    size_t item_extent_count;
    uint32_t reserved;
} strata_theme_virtual_list;

typedef struct strata_theme_layout_style {
    size_t struct_size;
    uint32_t participates;
    strata_theme_layout_kind kind;
    strata_theme_layout_size width;
    strata_theme_layout_size height;
    const strata_theme_layout_size* min_width;
    const strata_theme_layout_size* min_height;
    const strata_theme_layout_size* max_width;
    const strata_theme_layout_size* max_height;
    strata_theme_optional_number aspect_ratio;
    const strata_theme_size* intrinsic_size;
    strata_theme_edges padding;
    strata_theme_edges margin;
    strata_theme_point gap;
    strata_theme_layout_align align_items;
    strata_theme_layout_justify justify_content;
    strata_theme_layout_justify align_content;
    strata_theme_layout_align align_self;
    strata_theme_layout_align justify_self;
    uint32_t wrap;
    uint32_t clip;
    int32_t z_index;
    uint32_t reserved;
    const strata_theme_layout_size* grid_columns;
    size_t grid_column_count;
    const strata_theme_layout_size* grid_rows;
    size_t grid_row_count;
    const size_t* grid_column;
    const size_t* grid_row;
    size_t column_span;
    size_t row_span;
    uint32_t scroll_horizontal;
    uint32_t scroll_vertical;
    strata_theme_edges scroll_viewport_insets;
    uint32_t scroll_viewport_insets_from_inside_border;
    strata_theme_edges scroll_content_padding;
    double scrollbar_gutter;
    strata_theme_point scroll_offset;
    uint32_t pin_horizontal;
    uint32_t pin_vertical;
    const strata_theme_virtual_list* virtual_list;
    strata_string_view portal_target;
    uint32_t detach_from_parent_clip;
} strata_theme_layout_style;

typedef uint32_t strata_theme_motion_trigger;
#define STRATA_THEME_MOTION_ENTER UINT32_C(0)
#define STRATA_THEME_MOTION_EXIT UINT32_C(1)
#define STRATA_THEME_MOTION_HOVER UINT32_C(2)
#define STRATA_THEME_MOTION_PRESSED UINT32_C(3)
#define STRATA_THEME_MOTION_FOCUS UINT32_C(4)
#define STRATA_THEME_MOTION_CHECKED UINT32_C(5)
#define STRATA_THEME_MOTION_MOVE UINT32_C(6)
#define STRATA_THEME_MOTION_ANIMATE UINT32_C(7)
#define STRATA_THEME_MOTION_FOCUS_VISIBLE UINT32_C(8)
#define STRATA_THEME_MOTION_TRIGGER_UNSPECIFIED UINT32_MAX

typedef uint32_t strata_theme_motion_direction;
#define STRATA_THEME_MOTION_FORWARD UINT32_C(0)
#define STRATA_THEME_MOTION_REVERSE UINT32_C(1)
#define STRATA_THEME_MOTION_TO_TARGET UINT32_C(2)
#define STRATA_THEME_MOTION_EXPAND UINT32_C(3)
#define STRATA_THEME_MOTION_COLLAPSE UINT32_C(4)

typedef uint32_t strata_theme_motion_interaction;
#define STRATA_THEME_MOTION_INTERACTION_HOVER UINT32_C(0)
#define STRATA_THEME_MOTION_INTERACTION_PRESSED UINT32_C(1)
#define STRATA_THEME_MOTION_INTERACTION_FOCUS UINT32_C(2)
#define STRATA_THEME_MOTION_INTERACTION_FOCUS_VISIBLE UINT32_C(3)
#define STRATA_THEME_MOTION_INTERACTION_UNSPECIFIED UINT32_MAX

typedef uint32_t strata_theme_motion_property;
#define STRATA_THEME_MOTION_PROPERTY_WIDTH UINT32_C(0)
#define STRATA_THEME_MOTION_PROPERTY_HEIGHT UINT32_C(1)
#define STRATA_THEME_MOTION_PROPERTY_MIN_WIDTH UINT32_C(2)
#define STRATA_THEME_MOTION_PROPERTY_MIN_HEIGHT UINT32_C(3)
#define STRATA_THEME_MOTION_PROPERTY_MAX_WIDTH UINT32_C(4)
#define STRATA_THEME_MOTION_PROPERTY_MAX_HEIGHT UINT32_C(5)
#define STRATA_THEME_MOTION_PROPERTY_MARGIN_LEFT UINT32_C(6)
#define STRATA_THEME_MOTION_PROPERTY_MARGIN_TOP UINT32_C(7)
#define STRATA_THEME_MOTION_PROPERTY_MARGIN_RIGHT UINT32_C(8)
#define STRATA_THEME_MOTION_PROPERTY_MARGIN_BOTTOM UINT32_C(9)
#define STRATA_THEME_MOTION_PROPERTY_PADDING_LEFT UINT32_C(10)
#define STRATA_THEME_MOTION_PROPERTY_PADDING_TOP UINT32_C(11)
#define STRATA_THEME_MOTION_PROPERTY_PADDING_RIGHT UINT32_C(12)
#define STRATA_THEME_MOTION_PROPERTY_PADDING_BOTTOM UINT32_C(13)
#define STRATA_THEME_MOTION_PROPERTY_PLACEMENT_X UINT32_C(14)
#define STRATA_THEME_MOTION_PROPERTY_PLACEMENT_Y UINT32_C(15)
#define STRATA_THEME_MOTION_PROPERTY_BACKGROUND UINT32_C(16)
#define STRATA_THEME_MOTION_PROPERTY_FOREGROUND UINT32_C(17)
#define STRATA_THEME_MOTION_PROPERTY_COLOR UINT32_C(18)
#define STRATA_THEME_MOTION_PROPERTY_RADIUS UINT32_C(19)
#define STRATA_THEME_MOTION_PROPERTY_TRACK UINT32_C(20)
#define STRATA_THEME_MOTION_PROPERTY_FILL UINT32_C(21)
#define STRATA_THEME_MOTION_PROPERTY_THUMB UINT32_C(22)
#define STRATA_THEME_MOTION_PROPERTY_TRACK_RADIUS UINT32_C(23)
#define STRATA_THEME_MOTION_PROPERTY_THUMB_RADIUS UINT32_C(24)
#define STRATA_THEME_MOTION_PROPERTY_THUMB_SIZE UINT32_C(25)
#define STRATA_THEME_MOTION_PROPERTY_INDICATOR_POSITION UINT32_C(26)
#define STRATA_THEME_MOTION_PROPERTY_CLIP UINT32_C(27)
#define STRATA_THEME_MOTION_PROPERTY_OPACITY UINT32_C(28)
#define STRATA_THEME_MOTION_PROPERTY_X UINT32_C(29)
#define STRATA_THEME_MOTION_PROPERTY_Y UINT32_C(30)
#define STRATA_THEME_MOTION_PROPERTY_TRANSLATE_X UINT32_C(31)
#define STRATA_THEME_MOTION_PROPERTY_TRANSLATE_Y UINT32_C(32)
#define STRATA_THEME_MOTION_PROPERTY_SCALE UINT32_C(33)
#define STRATA_THEME_MOTION_PROPERTY_SCALE_X UINT32_C(34)
#define STRATA_THEME_MOTION_PROPERTY_SCALE_Y UINT32_C(35)

typedef uint32_t strata_theme_motion_value_kind;
#define STRATA_THEME_MOTION_VALUE_NUMBER UINT32_C(0)
#define STRATA_THEME_MOTION_VALUE_COLOR UINT32_C(1)
#define STRATA_THEME_MOTION_VALUE_BOOLEAN UINT32_C(2)

typedef struct strata_theme_motion_value {
    strata_theme_motion_value_kind kind;
    uint32_t reserved;
    double number;
    strata_color color;
    uint32_t boolean;
} strata_theme_motion_value;

typedef struct strata_theme_motion_keyframe {
    double offset;
    strata_theme_motion_value value;
    const struct strata_theme_motion_easing* easing;
} strata_theme_motion_keyframe;

typedef struct strata_theme_motion_track {
    strata_theme_motion_property property;
    uint32_t reserved;
    const strata_theme_motion_keyframe* keyframes;
    size_t keyframe_count;
} strata_theme_motion_track;

typedef struct strata_theme_declared_animation {
    size_t struct_size;
    strata_string_view name;
    strata_theme_motion_trigger trigger;
    strata_theme_motion_repeat_kind repeat_kind;
    int64_t duration_nanoseconds;
    int64_t delay_nanoseconds;
    struct strata_theme_motion_easing easing;
    uint32_t repeat_count;
    uint32_t reverse;
    strata_theme_motion_fill_mode fill_mode;
    const strata_theme_motion_track* tracks;
    size_t track_count;
    uint32_t reserved;
} strata_theme_declared_animation;

typedef uint32_t strata_theme_animation_spec_kind;
#define STRATA_THEME_ANIMATION_NAMED UINT32_C(0)
#define STRATA_THEME_ANIMATION_INLINE UINT32_C(1)

typedef struct strata_theme_animation_spec {
    strata_theme_animation_spec_kind kind;
    uint32_t reserved;
    strata_string_view name;
    const strata_theme_declared_animation* inline_animation;
} strata_theme_animation_spec;

typedef struct strata_theme_motion_attachment {
    strata_theme_motion_trigger trigger;
    strata_theme_motion_direction direction;
    strata_theme_animation_spec animation;
    uint32_t cancel_on_detach;
    strata_theme_motion_trigger continuity_trigger;
    uint32_t reserved;
} strata_theme_motion_attachment;

typedef struct strata_theme_motion_channel {
    strata_string_view id;
    strata_theme_animation_spec animation;
    strata_theme_motion_interaction interaction;
    strata_theme_value_mode state_target_mode;
    uint32_t state_target;
    uint32_t reserved;
} strata_theme_motion_channel;

typedef struct strata_theme_motion_value_channel {
    strata_string_view id;
    strata_theme_motion_property property;
    uint32_t reserved;
    strata_theme_motion_value target;
    strata_string_view timing;
} strata_theme_motion_value_channel;

typedef struct strata_theme_resolved_property_motion {
    const strata_theme_motion_property* properties;
    size_t property_count;
    strata_string_view timing;
} strata_theme_resolved_property_motion;

typedef struct strata_theme_disclosure_motion {
    uint32_t expanded;
    uint32_t reserved;
    double collapsed_extent;
    strata_string_view timing;
} strata_theme_disclosure_motion;

typedef struct strata_theme_content_size_motion {
    uint32_t animate_width;
    uint32_t animate_height;
    uint32_t clip;
    uint32_t reserved;
    strata_string_view timing;
} strata_theme_content_size_motion;

typedef struct strata_theme_animation_set {
    size_t struct_size;
    const strata_theme_motion_attachment* attachments;
    size_t attachment_count;
    const strata_theme_declared_animation* declared_animations;
    size_t declared_animation_count;
    const strata_theme_motion_channel* channels;
    size_t channel_count;
    const strata_theme_motion_value_channel* value_channels;
    size_t value_channel_count;
    const strata_theme_resolved_property_motion* resolved_properties;
    const strata_theme_disclosure_motion* disclosure;
    const strata_theme_content_size_motion* content_size;
    uint32_t reserved;
} strata_theme_animation_set;

/**
 * Typed widget-theme entry. Null sections are omitted; an all-null entry remains explicit and
 * suppresses semantic-style fallback. Empty variant means "default".
 */
typedef struct strata_theme_widget_style {
    size_t struct_size;
    strata_string_view component_type;
    strata_string_view variant;
    const strata_theme_visual_style* visual;
    const strata_theme_text_visual_style* text_visual;
    const strata_theme_text_layout_style* text_layout;
    const strata_theme_layout_style* layout;
    strata_theme_value_mode motion_mode;
    uint32_t reserved;
    const strata_theme_animation_set* motion;
} strata_theme_widget_style;

/** All data is borrowed for the register/set call and copied into the owning Surface. */
typedef struct strata_theme {
    size_t struct_size;
    uint32_t model_version;
    uint32_t reserved;
    strata_string_view name;
    strata_string_view parent;
    strata_theme_tokens tokens;
    const strata_theme_motion_policy* motion_policy;
    const strata_theme_widget_style* widget_styles;
    size_t widget_style_count;
} strata_theme;

typedef struct strata_scroll_animation_request {
    size_t struct_size;
    strata_string_view key;
    uint32_t has_x;
    uint32_t has_y;
    double x;
    double y;
    strata_string_view timing;
    uint32_t has_duration;
    uint32_t reserved;
    int64_t duration_nanoseconds;
} strata_scroll_animation_request;

typedef uint32_t strata_extension_input_result;
#define STRATA_EXTENSION_INPUT_IGNORED UINT32_C(0)
#define STRATA_EXTENSION_INPUT_CONSUMED UINT32_C(1)

typedef strata_extension_input_result (*strata_widget_activate_fn)(
    void* user_data, strata_widget_input_context* context);
typedef void (*strata_widget_present_fn)(void* user_data, strata_widget_render_context* context);
typedef strata_rect (*strata_widget_hit_bounds_fn)(void* user_data,
                                                   strata_widget_inspection_context* context);
typedef void (*strata_widget_semantics_fn)(void* user_data,
                                           strata_widget_semantics_context* context);

typedef struct strata_corner_radii {
    double top_left;
    double top_right;
    double bottom_right;
    double bottom_left;
} strata_corner_radii;

typedef struct strata_edges {
    double left;
    double top;
    double right;
    double bottom;
} strata_edges;

/** Normalized image source region; {0,0,1,1} selects the whole PNG or SVG. */
typedef struct strata_texture_region {
    double u;
    double v;
    double width;
    double height;
} strata_texture_region;

/* x and y are normalized inside the draw bounds: 0 is the leading edge, 1 the trailing edge. */
typedef struct strata_mesh_vertex {
    double x;
    double y;
    double z;
    double u;
    double v;
    strata_color color;
} strata_mesh_vertex;

/* Vertex and index storage is borrowed for the draw call and copied into the render command. */
typedef struct strata_mesh_geometry {
    size_t struct_size;
    const strata_mesh_vertex* vertices;
    size_t vertex_count;
    const uint32_t* indices;
    size_t index_count;
} strata_mesh_geometry;

typedef uint32_t strata_material_parameter_kind;
#define STRATA_MATERIAL_PARAMETER_NUMBER UINT32_C(0)
#define STRATA_MATERIAL_PARAMETER_BOOLEAN UINT32_C(1)
#define STRATA_MATERIAL_PARAMETER_TEXT UINT32_C(2)
#define STRATA_MATERIAL_PARAMETER_COLOR UINT32_C(3)

typedef struct strata_material_parameter {
    size_t struct_size;
    strata_string_view name;
    strata_material_parameter_kind kind;
    uint32_t boolean_value;
    double number;
    strata_string_view text;
    strata_color color;
} strata_material_parameter;

/**
 * Material state carried by one custom-mesh draw. The id is validated against the application
 * material contracts; an unknown id or parameter is dropped from the packet with a diagnostic.
 */
typedef struct strata_material_state {
    size_t struct_size;
    strata_string_view id;
    /* Empty blend mode selects "straight_alpha". */
    strata_string_view blend_mode;
    double opacity;
    const strata_material_parameter* parameters;
    size_t parameter_count;
} strata_material_state;

typedef struct strata_widget_key_event {
    size_t struct_size;
    /* Canonical lowercased key name shared with built-in widgets, for example "enter" or "right".
     */
    strata_string_view key;
    uint32_t modifiers;
    uint32_t reserved;
} strata_widget_key_event;

typedef strata_extension_input_result (*strata_widget_key_fn)(void* user_data,
                                                              strata_widget_input_context* context,
                                                              const strata_widget_key_event* event);

/** Work invalidated when an extension writes one declared retained field. */
typedef uint32_t strata_widget_invalidation;
#define STRATA_WIDGET_INVALIDATION_PROPERTIES UINT32_C(0)
#define STRATA_WIDGET_INVALIDATION_LAYOUT UINT32_C(1)
#define STRATA_WIDGET_INVALIDATION_STYLE UINT32_C(2)
#define STRATA_WIDGET_INVALIDATION_TEXT UINT32_C(3)
#define STRATA_WIDGET_INVALIDATION_SEMANTICS UINT32_C(4)
/* Rebuilds this widget's presentation without description reconciliation or layout. */
#define STRATA_WIDGET_INVALIDATION_PAINT UINT32_C(5)
/* Updates gesture/session bookkeeping without presentation, layout, or semantic work. */
#define STRATA_WIDGET_INVALIDATION_INPUT UINT32_C(6)

/** Downstream work admitted for one requested extension frame. */
typedef uint32_t strata_widget_frame_cost;
#define STRATA_WIDGET_FRAME_PAINT UINT32_C(0)
#define STRATA_WIDGET_FRAME_LAYOUT UINT32_C(1)

typedef struct strata_widget_frame_info {
    size_t struct_size;
    int64_t time_nanoseconds;
    int64_t delta_nanoseconds;
    uint32_t reduced_motion;
    uint32_t reserved;
} strata_widget_frame_info;

typedef void (*strata_widget_frame_fn)(void* user_data, strata_widget_input_context* context,
                                       const strata_widget_frame_info* frame);

/**
 * One retained field owned by a widget extension. Reads and writes of undeclared names fail with
 * STRATA_STATUS_NOT_FOUND instead of silently materializing untracked retained state.
 */
typedef struct strata_widget_retained_field {
    size_t struct_size;
    strata_string_view name;
    strata_widget_invalidation invalidation;
    uint32_t reserved;
} strata_widget_retained_field;

/** One widget-owned hit region; later entries win equal-z overlap. */
typedef struct strata_widget_subtarget {
    size_t struct_size;
    strata_string_view id;
    strata_rect bounds;
    int32_t z_index;
    uint32_t enabled;
    /* Stable logical collection index, or SIZE_MAX when the target is not indexed. */
    size_t index;
    /* CONTROL belongs to the owner; ITEM maps an indexed virtual semantic child. */
    uint32_t kind;
    uint32_t reserved;
} strata_widget_subtarget;

typedef void (*strata_widget_subtargets_fn)(void* user_data,
                                            strata_widget_subtargets_context* context);
#define STRATA_WIDGET_SUBTARGET_CONTROL UINT32_C(0)
#define STRATA_WIDGET_SUBTARGET_ITEM UINT32_C(1)

#define STRATA_WIDGET_SEMANTIC_CHILD_SELECTED (UINT32_C(1) << 0)
#define STRATA_WIDGET_SEMANTIC_CHILD_DISABLED (UINT32_C(1) << 1)
#define STRATA_WIDGET_SEMANTIC_CHILD_VALUE_RANGE (UINT32_C(1) << 2)

/** One independently addressable semantic child of a compound widget. */
typedef struct strata_widget_semantic_child {
    size_t struct_size;
    size_t index;
    strata_string_view role;
    strata_string_view name;
    strata_string_view value_text;
    double current;
    double minimum;
    double maximum;
    uint32_t flags;
    uint32_t reserved;
} strata_widget_semantic_child;

#define STRATA_WIDGET_EXTENSION_FOCUSABLE (UINT64_C(1) << 0)
#define STRATA_WIDGET_EXTENSION_DETACHED_OVERLAY (UINT64_C(1) << 1)
/* Presentation reads motion progress; the frame keeps this widget alive while channels animate. */
#define STRATA_WIDGET_EXTENSION_DEPENDS_ON_MOTION (UINT64_C(1) << 2)
/* Presentation reads hover/press/focus feedback and repaints when it changes. */
#define STRATA_WIDGET_EXTENSION_DEPENDS_ON_STATUS (UINT64_C(1) << 3)

/*
 * Surface extension descriptors are copied during creation. user_data remains host-owned and must
 * outlive the Surface. JSON fields contain objects whose values are copied into description/layout
 * defaults before the first retained node is materialized.
 */
typedef struct strata_widget_extension {
    size_t struct_size;
    strata_string_view type;
    uint64_t flags;
    void* user_data;
    strata_string_view description_properties_json;
    strata_string_view layout_defaults_json;
    strata_string_view popup_retained;
    strata_widget_activate_fn activate;
    strata_widget_present_fn present;
    strata_widget_present_fn overlay;
    strata_widget_hit_bounds_fn hit_bounds;
    strata_widget_key_fn key;
    strata_widget_semantics_fn semantics;
    /* Empty role keeps the framework default for an interactive extension widget. */
    strata_string_view semantics_role;
    const strata_widget_retained_field* retained_fields;
    size_t retained_field_count;
    /* Optional suffix; absent from version-1 descriptors. */
    strata_widget_subtargets_fn subtargets;
    /* Optional suffix; absent from version-1 and version-2 descriptors. */
    strata_widget_frame_fn frame;
} strata_widget_extension;
#define STRATA_WIDGET_EXTENSION_VERSION_1_SIZE offsetof(strata_widget_extension, subtargets)
#define STRATA_WIDGET_EXTENSION_VERSION_2_SIZE offsetof(strata_widget_extension, frame)

typedef uint32_t strata_extension_event_phase;
#define STRATA_EXTENSION_EVENT_CAPTURE UINT32_C(0)
#define STRATA_EXTENSION_EVENT_TARGET UINT32_C(1)
#define STRATA_EXTENSION_EVENT_BUBBLE UINT32_C(2)

typedef strata_extension_event_phase strata_behavior_event_phase;
#define STRATA_BEHAVIOR_EVENT_CAPTURE STRATA_EXTENSION_EVENT_CAPTURE
#define STRATA_BEHAVIOR_EVENT_TARGET STRATA_EXTENSION_EVENT_TARGET
#define STRATA_BEHAVIOR_EVENT_BUBBLE STRATA_EXTENSION_EVENT_BUBBLE

typedef struct strata_behavior_pointer_event {
    size_t struct_size;
    uint32_t kind;
    strata_behavior_event_phase phase;
    uint32_t modifiers;
    int32_t pointer_id;
    int32_t button;
    double x;
    double y;
    uint32_t target;
    uint32_t reserved;
    /* Optional suffix; absent from version-1 events. */
    double local_x;
    double local_y;
    double delta_x;
    double delta_y;
    int64_t timestamp_nanoseconds;
} strata_behavior_pointer_event;

typedef strata_extension_input_result (*strata_behavior_pointer_fn)(
    void* user_data, strata_behavior_input_context* context,
    const strata_behavior_pointer_event* event);

/** Wheel/trackpad event delivered to an attached behavior through routed input phases. */
typedef struct strata_behavior_scroll_event {
    size_t struct_size;
    strata_behavior_event_phase phase;
    uint32_t modifiers;
    double x;
    double y;
    double local_x;
    double local_y;
    double delta_x;
    double delta_y;
    uint32_t target;
    uint32_t reserved;
} strata_behavior_scroll_event;

typedef strata_extension_input_result (*strata_behavior_scroll_fn)(
    void* user_data, strata_behavior_input_context* context,
    const strata_behavior_scroll_event* event);

/** Committed key or focus transition delivered to an attached behavior. */
typedef struct strata_behavior_key_event {
    size_t struct_size;
    strata_behavior_event_phase phase;
    uint32_t modifiers;
    strata_string_view key;
    uint32_t target;
    uint32_t reserved;
} strata_behavior_key_event;

typedef strata_extension_input_result (*strata_behavior_key_fn)(
    void* user_data, strata_behavior_input_context* context,
    const strata_behavior_key_event* event);

typedef uint32_t strata_behavior_focus_kind;
#define STRATA_BEHAVIOR_FOCUS_GAINED UINT32_C(0)
#define STRATA_BEHAVIOR_FOCUS_LOST UINT32_C(1)

typedef struct strata_behavior_focus_event {
    size_t struct_size;
    strata_behavior_focus_kind kind;
    strata_behavior_event_phase phase;
    uint32_t target;
    uint32_t reserved;
} strata_behavior_focus_event;

typedef strata_extension_input_result (*strata_behavior_focus_fn)(
    void* user_data, strata_behavior_input_context* context,
    const strata_behavior_focus_event* event);

#define STRATA_BEHAVIOR_EXTENSION_FOCUSABLE (UINT64_C(1) << 0)
#define STRATA_BEHAVIOR_EXTENSION_ACCEPTS_POINTER (UINT64_C(1) << 1)

typedef struct strata_behavior_extension {
    size_t struct_size;
    strata_string_view id;
    uint64_t flags;
    void* user_data;
    strata_behavior_pointer_fn pointer;
} strata_behavior_extension;
#define STRATA_BEHAVIOR_EXTENSION_VERSION_1_SIZE                                                   \
    offsetof(strata_behavior_extension, pointer) + sizeof(strata_behavior_pointer_fn)

/** Optional routed behavior input kept separate so version-1 descriptor strides stay stable. */
typedef struct strata_behavior_input_extension {
    size_t struct_size;
    strata_string_view id;
    void* user_data;
    strata_behavior_scroll_fn scroll;
    strata_behavior_key_fn key;
    strata_behavior_focus_fn focus;
} strata_behavior_input_extension;

/** Widget-local pointer event. Surface coordinates remain available while capture leaves bounds. */
typedef struct strata_widget_pointer_event {
    size_t struct_size;
    uint32_t kind;
    strata_extension_event_phase phase;
    uint32_t modifiers;
    int32_t pointer_id;
    int32_t button;
    double x;
    double y;
    double local_x;
    double local_y;
    double delta_x;
    double delta_y;
    int64_t timestamp_nanoseconds;
    uint32_t target;
    uint32_t reserved;
    /* Optional suffix. Empty/SIZE_MAX means the routed region is the widget itself. */
    strata_string_view subtarget_id;
    size_t subtarget_index;
} strata_widget_pointer_event;

typedef strata_extension_input_result (*strata_widget_pointer_fn)(
    void* user_data, strata_widget_input_context* context,
    const strata_widget_pointer_event* event);
/** Widget-local wheel/trackpad event delivered through capture, target, and bubble phases. */
typedef struct strata_widget_scroll_event {
    size_t struct_size;
    strata_extension_event_phase phase;
    uint32_t modifiers;
    double x;
    double y;
    double local_x;
    double local_y;
    double delta_x;
    double delta_y;
    uint32_t target;
    uint32_t reserved;
} strata_widget_scroll_event;

typedef strata_extension_input_result (*strata_widget_scroll_fn)(
    void* user_data, strata_widget_input_context* context, const strata_widget_scroll_event* event);

/**
 * Optional input phases for one widget descriptor. This table is separate so extending widget
 * input never changes the stride of the version-1 `strata_widget_extension` descriptor array.
 */
typedef struct strata_widget_input_extension {
    size_t struct_size;
    strata_string_view type;
    void* user_data;
    strata_widget_pointer_fn pointer;
} strata_widget_input_extension;
/** Optional scroll lifecycle kept separate so version-2 pointer descriptor strides stay stable. */
typedef struct strata_widget_scroll_extension {
    size_t struct_size;
    strata_string_view type;
    void* user_data;
    strata_widget_scroll_fn scroll;
} strata_widget_scroll_extension;

typedef struct strata_surface_extension_bundle {
    size_t struct_size;
    const strata_widget_extension* widgets;
    size_t widget_count;
    const strata_behavior_extension* behaviors;
    size_t behavior_count;
    const strata_widget_input_extension* widget_inputs;
    size_t widget_input_count;
    const strata_widget_scroll_extension* widget_scrolls;
    size_t widget_scroll_count;
    const strata_behavior_input_extension* behavior_inputs;
    size_t behavior_input_count;
} strata_surface_extension_bundle;

#define STRATA_SURFACE_EXTENSION_BUNDLE_VERSION_1_SIZE                                             \
    offsetof(strata_surface_extension_bundle, widget_inputs)
#define STRATA_SURFACE_EXTENSION_BUNDLE_VERSION_2_SIZE                                             \
    offsetof(strata_surface_extension_bundle, widget_scrolls)
#define STRATA_SURFACE_EXTENSION_BUNDLE_VERSION_3_SIZE                                             \
    offsetof(strata_surface_extension_bundle, behavior_inputs)

typedef struct strata_surface_config {
    size_t struct_size;
    strata_string_view id;
    strata_surface_root_role root_role;
    uint32_t reserved;
    strata_string_view root_name;
    strata_surface_environment environment;
    /* Empty font tables disable portable text shaping for command-only/headless consumers. */
    const strata_surface_font_resource* fonts;
    size_t font_count;
    /* Optional surface-owned lifecycle extensions. */
    const strata_surface_extension_bundle* extensions;
    /* PNGs become surface textures; SVGs become backend-independent vector geometry. */
    const strata_surface_image_resource* images;
    size_t image_count;
} strata_surface_config;

typedef uint32_t strata_input_event_kind;

#define STRATA_INPUT_POINTER_MOVE UINT32_C(0)
#define STRATA_INPUT_POINTER_PRESS UINT32_C(1)
#define STRATA_INPUT_POINTER_RELEASE UINT32_C(2)
#define STRATA_INPUT_POINTER_CANCEL UINT32_C(3)
#define STRATA_INPUT_SCROLL UINT32_C(4)
#define STRATA_INPUT_KEY UINT32_C(5)
#define STRATA_INPUT_TEXT UINT32_C(6)
#define STRATA_INPUT_IME_PREEDIT UINT32_C(7)
#define STRATA_INPUT_NAVIGATION UINT32_C(8)

#define STRATA_INPUT_EVENT_VERSION_2 UINT32_C(2)

typedef uint32_t strata_key_event_action;
#define STRATA_KEY_PRESS UINT32_C(0)
#define STRATA_KEY_RELEASE UINT32_C(1)
#define STRATA_KEY_REPEAT UINT32_C(2)

typedef uint32_t strata_key_modifiers;

#define STRATA_KEY_MODIFIER_SHIFT (UINT32_C(1) << 0)
#define STRATA_KEY_MODIFIER_CONTROL (UINT32_C(1) << 1)
#define STRATA_KEY_MODIFIER_ALT (UINT32_C(1) << 2)
#define STRATA_KEY_MODIFIER_SUPER (UINT32_C(1) << 3)

/* Fields unused by an event kind are ignored. Strings are copied during enqueue. */
typedef struct strata_input_event {
    size_t struct_size;
    uint32_t version;
    strata_input_event_kind kind;
    strata_key_modifiers modifiers;
    int32_t pointer_id;
    int32_t button;
    double x;
    double y;
    double delta_x;
    double delta_y;
    strata_string_view text;
    uint64_t selection_start;
    uint64_t selection_end;
    int64_t timestamp_nanoseconds;
    strata_key_event_action key_action;
    uint32_t reserved;
} strata_input_event;

typedef struct strata_surface_input_batch_info {
    size_t struct_size;
    uint64_t accepted_event_count;
    uint64_t queued_event_count;
} strata_surface_input_batch_info;

typedef struct strata_surface_frame_info {
    size_t struct_size;
    uint64_t frame_index;
    int64_t frame_time_nanoseconds;
    uint64_t processed_input_event_count;
    uint64_t emitted_event_count;
    uint64_t action_outcome_count;
    uint64_t render_command_count;
    uint64_t semantics_generation;
    uint64_t queued_input_event_count;
    uint64_t render_packet_size;
} strata_surface_frame_info;

/*
 * Packet v10 is little-endian and tightly encoded (no native padding). Numbers are IEEE-754 f64
 * bit patterns, strings are a u32 byte count followed by UTF-8, and each resource/batch record is
 * [u32 kind, u32 payload byte count, payload]:
 *
 *   bytes[8] "STRATARP", u32 version, u32 resource count, u32 batch count,
 *   u64 frame index, u64 geometry epoch, u32 flags, u32 vertex byte count, u32 index count,
 *   u32 planned draw count, u32 skipped draw count, resource records, vertex bytes,
 *   little-endian u32 indices, submission batch records.
 *
 * A vertex is 88 bytes: f32 x/y/z/u/v, u8 red/green/blue/alpha, then sixteen f32 material values.
 * Every batch payload begins with source order, a u32 framebuffer scissor, and a u32 rounded-clip
 * count. Each rounded clip contains f64 x/y/width/height, four f64 corner radii, and a six-f64
 * inverse affine transform from presented logical pixels into that clip's local space. Draw
 * payloads continue with material/blend strings, optional texture id, and
 * base-vertex/first-index/index-count. Blur payloads continue with f64 x/y/width/height/radius and
 * u32 downsample. Backdrop/content-begin effect payloads continue with f64 x/y/width/height; four
 * f64 corner radii; the
 * effect-id string; f64 opacity; f64 maximum refresh rate (zero = unbounded); u32 backdrop source
 * (0 = current framebuffer, 1 = framebuffer before this Surface); a u32 packed-parameter count;
 * and up to sixteen f64 values. Content effects require backdrop source 0.
 * Content-end carries only the common prefix. Rounded clip stacks are limited to sixteen entries;
 * authored CONTENT effect isolation is limited to four layers.
 * Resource payloads begin
 * with a texture-id string. Atlas create/upload then carry u32 format (0 = R8, 1 = RGBA8) and u32
 * x/y/width/height; upload adds u32 byte count and raw texels. Release has no additional fields.
 * Encoded texture creation carries u32 encoding (0 = PNG), u32 sampling, u32 width/height, a u32
 * byte count, and encoded bytes. STRATA_RENDER_PACKET_FLAG_GEOMETRY_PATCHES advances the epoch
 * from the immediately preceding retained epoch. Header geometry counts describe the complete
 * retained arrays; resource records are followed by vertex and index patch lists. Each list is
 * u32 count followed by [u32 byte offset, u32 byte count, raw bytes], then the complete updated
 * batch records. Vertex patches are vertex-aligned and index patches are u32-aligned. When neither
 * geometry flag is present, batch and geometry counts are zero and the decoder retains the
 * complete geometry/batch plan for the repeated epoch while still applying this packet's resources
 * and frame index. Packets form one ordered Surface/backend stream: consume every framed packet
 * and discard decoder state only when discarding the stream.
 *
 * C++ backends should prefer <strata/render_packet.hpp>, whose stateful decoder validates record
 * framing, ranges, resources, and retained epochs. STRATA_RENDER_COMMAND_* and
 * STRATA_RENDER_VALUE_* describe the optional canonical frame-JSON projection, not v10 records.
 */
#define STRATA_RENDER_PACKET_VERSION_1 UINT32_C(1)
#define STRATA_RENDER_PACKET_VERSION_2 UINT32_C(2)
#define STRATA_RENDER_PACKET_VERSION_3 UINT32_C(3)
#define STRATA_RENDER_PACKET_VERSION_4 UINT32_C(4)
#define STRATA_RENDER_PACKET_VERSION_5 UINT32_C(5)
#define STRATA_RENDER_PACKET_VERSION_6 UINT32_C(6)
#define STRATA_RENDER_PACKET_VERSION_7 UINT32_C(7)
#define STRATA_RENDER_PACKET_VERSION_8 UINT32_C(8)
#define STRATA_RENDER_PACKET_VERSION_9 UINT32_C(9)
#define STRATA_RENDER_PACKET_VERSION_10 UINT32_C(10)
#define STRATA_RENDER_PACKET_VERSION_CURRENT STRATA_RENDER_PACKET_VERSION_10
#define STRATA_RENDER_PACKET_VERTEX_STRIDE UINT32_C(88)
#define STRATA_RENDER_PACKET_FLAG_GEOMETRY_PAYLOAD UINT32_C(1)
#define STRATA_RENDER_PACKET_FLAG_GEOMETRY_PATCHES UINT32_C(2)
#define STRATA_EFFECT_DEFAULT_REFRESH_RATE 240.0
#define STRATA_EFFECT_BACKDROP_SOURCE_CURRENT UINT32_C(0)
#define STRATA_EFFECT_BACKDROP_SOURCE_SURFACE UINT32_C(1)

#define STRATA_RENDER_RESOURCE_ATLAS_CREATE UINT32_C(0)
#define STRATA_RENDER_RESOURCE_ATLAS_UPLOAD UINT32_C(1)
#define STRATA_RENDER_RESOURCE_TEXTURE_RELEASE UINT32_C(2)
#define STRATA_RENDER_RESOURCE_ENCODED_TEXTURE UINT32_C(3)

#define STRATA_RENDER_BATCH_DRAW UINT32_C(0)
#define STRATA_RENDER_BATCH_BLUR UINT32_C(1)
#define STRATA_RENDER_BATCH_BACKDROP_EFFECT UINT32_C(2)
#define STRATA_RENDER_BATCH_CONTENT_EFFECT_BEGIN UINT32_C(3)
#define STRATA_RENDER_BATCH_CONTENT_EFFECT_END UINT32_C(4)

#define STRATA_RENDER_COMMAND_SOLID_RECT UINT32_C(0)
#define STRATA_RENDER_COMMAND_ROUNDED_RECT UINT32_C(1)
#define STRATA_RENDER_COMMAND_BORDER UINT32_C(2)
#define STRATA_RENDER_COMMAND_IMAGE UINT32_C(3)
#define STRATA_RENDER_COMMAND_NINE_PATCH UINT32_C(4)
#define STRATA_RENDER_COMMAND_TEXT_RUN UINT32_C(5)
#define STRATA_RENDER_COMMAND_CUSTOM_MESH UINT32_C(6)
#define STRATA_RENDER_COMMAND_BLUR_REGION UINT32_C(7)
#define STRATA_RENDER_COMMAND_SHADOW UINT32_C(8)
#define STRATA_RENDER_COMMAND_CLIP_PUSH UINT32_C(9)
#define STRATA_RENDER_COMMAND_CLIP_POP UINT32_C(10)
#define STRATA_RENDER_COMMAND_TRANSFORM_PUSH UINT32_C(11)
#define STRATA_RENDER_COMMAND_TRANSFORM_POP UINT32_C(12)
#define STRATA_RENDER_COMMAND_MATERIAL_PUSH UINT32_C(13)
#define STRATA_RENDER_COMMAND_MATERIAL_POP UINT32_C(14)
#define STRATA_RENDER_COMMAND_BACKDROP_EFFECT UINT32_C(15)
#define STRATA_RENDER_COMMAND_CONTENT_EFFECT_PUSH UINT32_C(16)
#define STRATA_RENDER_COMMAND_CONTENT_EFFECT_POP UINT32_C(17)

#define STRATA_RENDER_VALUE_NULL UINT32_C(0)
#define STRATA_RENDER_VALUE_BOOLEAN UINT32_C(1)
#define STRATA_RENDER_VALUE_NUMBER UINT32_C(2)
#define STRATA_RENDER_VALUE_DURATION UINT32_C(3)
#define STRATA_RENDER_VALUE_STRING UINT32_C(4)
#define STRATA_RENDER_VALUE_COLOR UINT32_C(5)
#define STRATA_RENDER_VALUE_IMAGE UINT32_C(6)
#define STRATA_RENDER_VALUE_KEY UINT32_C(7)
#define STRATA_RENDER_VALUE_THEME_TOKEN UINT32_C(8)
#define STRATA_RENDER_VALUE_LIST UINT32_C(9)
#define STRATA_RENDER_VALUE_OBJECT UINT32_C(10)

/* Opaque handles. Each successful create has exactly one matching terminal release/abandon. */
typedef struct strata_runtime strata_runtime;
typedef struct strata_snapshot strata_snapshot;
typedef struct strata_action_registration strata_action_registration;
typedef struct strata_surface strata_surface;
typedef struct strata_application_state_snapshot strata_application_state_snapshot;

/*
 * Resource handles are scalar values, never pointers. Zero is invalid. The high 32 bits are a
 * runtime-local generation and the low 32 bits are a slot identity; hosts must treat the encoding
 * as opaque even though diagnostic projections may serialize both logical parts.
 */
typedef struct strata_resource_handle {
    uint64_t value;
} strata_resource_handle;

STRATA_API uint32_t strata_abi_version(void);
STRATA_API strata_capabilities strata_capability_bits(void);
STRATA_API strata_result strata_get_api_info(uint32_t requested_abi, strata_api_info* out_info);

STRATA_API strata_result strata_runtime_create(const strata_runtime_config* config,
                                               strata_runtime** out_runtime);
/* Refuses to destroy a runtime that still owns any Surface, leaving every handle recoverable. */
STRATA_API strata_result strata_runtime_release(strata_runtime* runtime);
STRATA_API strata_result strata_runtime_get_memory_info(const strata_runtime* runtime,
                                                        strata_runtime_memory_info* out_info);
STRATA_API strata_result strata_runtime_next_identity(strata_runtime* runtime,
                                                      uint64_t* out_identity);
STRATA_API strata_result strata_runtime_create_snapshot(strata_runtime* runtime,
                                                        strata_snapshot** out_snapshot);
STRATA_API strata_result strata_runtime_publish_host_snapshot(
    strata_runtime* runtime, const strata_host_snapshot_config* snapshot);
STRATA_API strata_result strata_runtime_get_host_snapshot_info(const strata_runtime* runtime,
                                                               strata_host_snapshot_info* out_info);
/* Reads the retained generation for one snapshot producer id. */
STRATA_API strata_result strata_runtime_get_host_snapshot_generation(const strata_runtime* runtime,
                                                                     strata_string_view id,
                                                                     uint64_t* out_generation);
/* Reads the canonical bounded runtime diagnostic history without materializing frame JSON. */
STRATA_API strata_result strata_runtime_read_diagnostics(
    const strata_runtime* runtime, const strata_diagnostics_snapshot_sink* sink);
/* Clears retained diagnostics, pending publication, and every owned Surface diagnostic queue. */
STRATA_API strata_result strata_runtime_clear_diagnostics(strata_runtime* runtime);
/* Typed canonical profiler history; this never materializes inspection/frame JSON. */
STRATA_API strata_result strata_runtime_read_profiler(const strata_runtime* runtime,
                                                      const strata_profiler_snapshot_sink* sink);
STRATA_API strata_result strata_runtime_set_profiler_capture(strata_runtime* runtime,
                                                             uint32_t enabled);
STRATA_API strata_result strata_runtime_read_host_value_json(strata_runtime* runtime,
                                                             strata_string_view path,
                                                             const strata_value_json_sink* sink);
STRATA_API strata_result strata_runtime_configure_application(
    strata_runtime* runtime, const strata_application_config* config);
STRATA_API strata_result strata_runtime_compile_and_activate(strata_runtime* runtime,
                                                             const strata_activation_config* config,
                                                             strata_activation_info* out_info);
STRATA_API strata_result strata_runtime_activate_compiled_module(
    strata_runtime* runtime, const strata_compiled_activation_config* config,
    strata_activation_info* out_info);
STRATA_API strata_result strata_runtime_read_active_unit_json(strata_runtime* runtime,
                                                              const strata_value_json_sink* sink);
/*
 * One application-declared material as a host sees it. Parameter packing is already resolved into
 * the vertex draw data, so a host needs only the shading source it implements and the policy for
 * drawing without it. The views borrow runtime-owned storage and stay valid while the runtime's
 * application is configured.
 */
typedef struct strata_material_declaration {
    size_t struct_size;
    strata_string_view id;
    strata_string_view blend_mode;
    /* Material drawn instead when this host implements no source for the material. */
    strata_string_view fallback;
    /* Resource id of the shading source for the requested backend; empty when undeclared. */
    strata_string_view source;
} strata_material_declaration;

/*
 * Enumerates the materials this application declares for one backend id (`hlsl`, `glsl`). Passing a
 * null buffer reports the count only. The declaration is backend-neutral, so supporting a further
 * backend adds a source key rather than a second contract.
 */
STRATA_API strata_result strata_runtime_read_material_declarations(
    strata_runtime* runtime, strata_string_view backend,
    strata_material_declaration* out_declarations, size_t capacity, size_t* out_count);

#define STRATA_EFFECT_INPUT_BACKDROP UINT32_C(0)
#define STRATA_EFFECT_INPUT_CONTENT UINT32_C(1)
#define STRATA_EFFECT_INPUT_SHAPE UINT32_C(2)
#define STRATA_EFFECT_PASS_BLUR UINT32_C(0)
#define STRATA_EFFECT_PASS_SHADER UINT32_C(1)
#define STRATA_EFFECT_PASS_SHADOW UINT32_C(2)
#define STRATA_EFFECT_PARAMETER_NONE UINT32_MAX

/** One pass in an application-declared render effect program. */
typedef struct strata_effect_pass_declaration {
    size_t struct_size;
    strata_string_view effect_id;
    uint32_t index;
    uint32_t kind;
    double radius;
    uint32_t downsample;
    uint32_t radius_parameter;
    uint32_t downsample_parameter;
    /* Resource id of this pass's shader for the requested backend. */
    strata_string_view source;
} strata_effect_pass_declaration;

/** Enumerates every pass of every effect for one backend as a flat ordered table. */
STRATA_API strata_result strata_runtime_read_effect_pass_declarations(
    strata_runtime* runtime, strata_string_view backend,
    strata_effect_pass_declaration* out_declarations, size_t capacity, size_t* out_count);
/* Resolves one compiled path without exporting or reparsing the whole active unit/source map. */
STRATA_API strata_result strata_runtime_read_source_map_entry_json(
    strata_runtime* runtime, strata_string_view compiled_path, const strata_value_json_sink* sink);
/* Returns the ordered active-unit entries whose authored range contains this source position. */
STRATA_API strata_result strata_runtime_read_source_map_entries_at_json(
    strata_runtime* runtime, strata_string_view source_id, uint32_t line, uint32_t column,
    const strata_value_json_sink* sink);
STRATA_API strata_result strata_runtime_register_action_handler(
    strata_runtime* runtime, const strata_action_handler_config* config,
    strata_action_registration** out_registration);
STRATA_API void strata_action_registration_release(strata_action_registration* registration);
STRATA_API strata_result strata_runtime_dispatch_action_json(
    strata_runtime* runtime, const strata_action_dispatch_config* config,
    strata_action_dispatch_info* out_info);
/* Installs the Runtime's immutable resource provider. Replacing it is rejected. */
STRATA_API strata_result strata_runtime_set_resource_adapter(
    strata_runtime* runtime, const strata_resource_adapter* adapter);
STRATA_API strata_result strata_runtime_read_resource(strata_runtime* runtime,
                                                      strata_string_view resource_id,
                                                      const strata_bytes_sink* sink);
STRATA_API strata_result strata_runtime_set_durable_store_adapter(
    strata_runtime* runtime, const strata_durable_store_adapter* adapter);
STRATA_API strata_result strata_runtime_read_durable_shell_value_json(
    strata_runtime* runtime, strata_string_view key, const strata_value_json_sink* sink);
STRATA_API strata_result strata_runtime_write_durable_shell_value_json(
    strata_runtime* runtime, strata_string_view key, strata_string_view value_json);
STRATA_API strata_result strata_runtime_flush_durable_state(strata_runtime* runtime);
STRATA_API strata_result strata_runtime_set_async_host_adapter(
    strata_runtime* runtime, const strata_async_host_adapter* adapter);
STRATA_API strata_result strata_runtime_async_progress(strata_runtime* runtime, uint64_t request_id,
                                                       const strata_async_progress* progress);
STRATA_API strata_result strata_runtime_async_succeed_json(strata_runtime* runtime,
                                                           uint64_t request_id,
                                                           strata_string_view value_json);
STRATA_API strata_result strata_runtime_async_fail(strata_runtime* runtime, uint64_t request_id,
                                                   strata_string_view message,
                                                   strata_string_view code);
STRATA_API strata_result strata_runtime_set_clipboard_adapter(
    strata_runtime* runtime, const strata_clipboard_adapter* adapter);
STRATA_API strata_result strata_runtime_clipboard_read(strata_runtime* runtime,
                                                       const strata_string_sink* text_sink);
STRATA_API strata_result strata_runtime_clipboard_write(strata_runtime* runtime,
                                                        strata_string_view text);
STRATA_API strata_result strata_runtime_set_ime_adapter(strata_runtime* runtime,
                                                        const strata_ime_adapter* adapter);
STRATA_API strata_result strata_runtime_ime_set_active(strata_runtime* runtime, uint32_t active);
STRATA_API strata_result strata_runtime_ime_set_cursor_rect(strata_runtime* runtime,
                                                            strata_rect logical_rect);
STRATA_API strata_result strata_runtime_set_effect_adapter(strata_runtime* runtime,
                                                           const strata_effect_adapter* adapter);
STRATA_API strata_result strata_runtime_emit_effect_json(strata_runtime* runtime,
                                                         strata_string_view effect_id,
                                                         strata_string_view payload_json);
STRATA_API strata_result strata_runtime_create_surface(strata_runtime* runtime,
                                                       const strata_surface_config* config,
                                                       strata_surface** out_surface);
/** Produces a complete validated policy configuration for the selected policy kind. */
STRATA_API strata_result strata_scale_policy_defaults(strata_scale_policy_kind kind,
                                                      strata_scale_policy_config* out_policy);
/** Resolves one portable, uniform logical/framebuffer mapping without host-side policy code. */
STRATA_API strata_result strata_resolve_scale_context(const strata_scale_policy_config* policy,
                                                      int64_t framebuffer_width,
                                                      int64_t framebuffer_height,
                                                      strata_scale_context* out_context);
/** Fills the frozen default theme token set. */
STRATA_API strata_result strata_theme_tokens_defaults(strata_theme_tokens* out_tokens);
STRATA_API strata_result strata_theme_visual_style_defaults(strata_theme_visual_style* out_style);
STRATA_API strata_result
strata_theme_text_visual_style_defaults(strata_theme_text_visual_style* out_style);
STRATA_API strata_result
strata_theme_text_layout_style_defaults(strata_theme_text_layout_style* out_style);
STRATA_API strata_result strata_theme_layout_style_defaults(strata_theme_layout_style* out_style);
STRATA_API strata_result strata_theme_animation_set_defaults(strata_theme_animation_set* out_set);
/** Registers/replaces a named immutable theme. */
STRATA_API strata_result strata_surface_register_theme(strata_surface* surface,
                                                       const strata_theme* theme,
                                                       uint32_t* out_changed);
/** Registers and selects the supplied root theme. */
STRATA_API strata_result strata_surface_set_theme(strata_surface* surface,
                                                  const strata_theme* theme, uint32_t* out_changed);
/** Refuses the active root and reports an unknown name as an unchanged successful operation. */
STRATA_API strata_result strata_surface_unregister_theme(strata_surface* surface,
                                                         strata_string_view name,
                                                         uint32_t* out_removed);
/** Installs a typed local ThemeScope at the keyed node; descendants inherit it. */
STRATA_API strata_result strata_surface_set_scoped_theme(strata_surface* surface,
                                                         strata_string_view node_key,
                                                         const strata_theme* theme,
                                                         uint32_t* out_changed);
STRATA_API strata_result strata_surface_clear_scoped_theme(strata_surface* surface,
                                                           strata_string_view node_key,
                                                           uint32_t* out_removed);
STRATA_API strata_result strata_surface_animate_scroll_to(
    strata_surface* surface, const strata_scroll_animation_request* request, uint32_t* out_started);
STRATA_API strata_result strata_runtime_create_application_state_snapshot(
    strata_runtime* runtime, strata_application_state_snapshot** out_snapshot);
STRATA_API strata_result strata_runtime_restore_application_state(
    strata_runtime* runtime, const strata_application_state_snapshot* snapshot,
    uint32_t* out_changed);
STRATA_API void
strata_application_state_snapshot_release(strata_application_state_snapshot* snapshot);
/*
 * Terminal ordered teardown. Prepare and synchronously consume the resource-only packet while its
 * GPU owner is alive, acknowledge that consumption, then release. Preparation alone is never a
 * delivery acknowledgement. Repeated preparation returns the same retained terminal packet;
 * ordinary Surface mutation/framing is rejected after the first successful preparation.
 */
STRATA_API strata_result strata_surface_prepare_release_packet(strata_surface* surface,
                                                               const strata_bytes_sink* sink);
STRATA_API strata_result strata_surface_acknowledge_release_packet(strata_surface* surface);
/* Refuses out-of-order release and leaves the Surface handle recoverable for retry. */
STRATA_API strata_result strata_surface_release(strata_surface* surface);
/* Explicit last resort when packet delivery is impossible; host GPU resources may remain live. */
STRATA_API strata_result strata_surface_abandon(strata_surface* surface);
STRATA_API strata_result strata_surface_adopt_environment(
    strata_surface* surface, const strata_surface_environment* environment, uint32_t* out_adopted);
STRATA_API strata_result strata_surface_enqueue_input(strata_surface* surface,
                                                      const strata_input_event* events,
                                                      size_t event_count,
                                                      strata_surface_input_batch_info* out_info);
STRATA_API strata_result strata_surface_cancel_interactions(strata_surface* surface);
STRATA_API strata_result strata_surface_dispatch_action_json(
    strata_surface* surface, const strata_action_dispatch_config* config,
    strata_action_dispatch_info* out_info);
/* Empty key clears the active focus containment scope. */
STRATA_API strata_result strata_surface_set_focus_containment(strata_surface* surface,
                                                              strata_string_view key,
                                                              uint32_t* out_contained);
STRATA_API strata_result strata_surface_frame(strata_surface* surface,
                                              int64_t frame_time_nanoseconds,
                                              strata_surface_frame_info* out_info);
/* The canonical frame JSON is borrowed only for the duration of emit. */
STRATA_API strata_result strata_surface_read_frame_json(const strata_surface* surface,
                                                        const strata_value_json_sink* sink);
/* Surface aliases operate on the owning runtime's canonical shared diagnostic history. */
STRATA_API strata_result strata_surface_read_diagnostics(
    const strata_surface* surface, const strata_diagnostics_snapshot_sink* sink);
STRATA_API strata_result strata_surface_clear_diagnostics(strata_surface* surface);
STRATA_API strata_result strata_surface_read_profiler(const strata_surface* surface,
                                                      const strata_profiler_snapshot_sink* sink);
STRATA_API strata_result strata_surface_set_profiler_capture(strata_surface* surface,
                                                             uint32_t enabled);
/* Called after submission to attach one complete genuine host boundary to the latest frame. */
STRATA_API strata_result strata_surface_record_host_frame(
    strata_surface* surface, const strata_profiler_host_frame* telemetry);
/* Drains bounded ordered lifecycle events and paired action outcomes since the prior drain. */
STRATA_API strata_result strata_surface_drain_events_json(strata_surface* surface,
                                                          const strata_value_json_sink* sink);
/* The complete packet is borrowed only for the duration of emit. */
STRATA_API strata_result strata_surface_read_render_packet(const strata_surface* surface,
                                                           const strata_bytes_sink* sink);
STRATA_API strata_result strata_surface_inspector_select(strata_surface* surface,
                                                         strata_string_view key,
                                                         uint32_t* out_selected);
STRATA_API strata_result strata_surface_inspector_pick(strata_surface* surface, double x, double y,
                                                       uint32_t* out_selected);
STRATA_API strata_result strata_surface_inspector_clear(strata_surface* surface);
STRATA_API strata_result strata_surface_read_inspector_selection_json(
    const strata_surface* surface, const strata_value_json_sink* sink);

/* Extension callback capabilities. Context pointers are valid only for their callback invocation.
 */
/* Borrowed immutable values remain valid only for the callback that returned them. */
STRATA_API strata_widget_value_kind strata_widget_value_get_kind(const strata_widget_value* value);
STRATA_API uint32_t strata_widget_value_get_boolean(const strata_widget_value* value,
                                                    uint32_t fallback);
STRATA_API double strata_widget_value_get_number(const strata_widget_value* value, double fallback);
STRATA_API uint32_t strata_widget_value_get_color(const strata_widget_value* value,
                                                  strata_color* out_color);
STRATA_API strata_string_view strata_widget_value_get_text(const strata_widget_value* value);
STRATA_API size_t strata_widget_value_list_size(const strata_widget_value* value);
STRATA_API const strata_widget_value* strata_widget_value_list_at(const strata_widget_value* value,
                                                                  size_t index);
STRATA_API const strata_widget_value*
strata_widget_value_object_field(const strata_widget_value* value, strata_string_view name);
STRATA_API const strata_widget_value*
strata_widget_input_property_value(const strata_widget_input_context* context,
                                   strata_string_view name);
STRATA_API const strata_widget_value*
strata_widget_render_property_value(const strata_widget_render_context* context,
                                    strata_string_view name);
STRATA_API const strata_widget_value*
strata_widget_render_style_value(const strata_widget_render_context* context,
                                 strata_string_view name);
STRATA_API const strata_widget_value*
strata_widget_semantics_property_value(const strata_widget_semantics_context* context,
                                       strata_string_view name);
STRATA_API strata_rect
strata_widget_subtargets_bounds(const strata_widget_subtargets_context* context);
STRATA_API const strata_widget_value*
strata_widget_subtargets_property_value(const strata_widget_subtargets_context* context,
                                        strata_string_view name);
STRATA_API strata_result
strata_widget_subtargets_retained_bytes(const strata_widget_subtargets_context* context,
                                        strata_string_view name, void* buffer, size_t size);
STRATA_API strata_result strata_widget_subtargets_reserve(strata_widget_subtargets_context* context,
                                                          size_t capacity);
STRATA_API strata_result strata_widget_subtargets_add(strata_widget_subtargets_context* context,
                                                      const strata_widget_subtarget* subtarget);
STRATA_API strata_rect strata_widget_input_bounds(const strata_widget_input_context* context);
STRATA_API double strata_widget_input_scale(const strata_widget_input_context* context);
/* The framework retains press capture through release/cancel; claiming wins gesture arbitration. */
STRATA_API uint32_t strata_widget_input_claim_gesture(strata_widget_input_context* context);
STRATA_API uint32_t strata_widget_input_cancel_gesture(strata_widget_input_context* context);
STRATA_API strata_result strata_widget_input_invalidate(strata_widget_input_context* context,
                                                        strata_widget_invalidation invalidation);
STRATA_API strata_result strata_widget_input_request_frame(strata_widget_input_context* context,
                                                           strata_widget_frame_cost cost);
STRATA_API uint32_t strata_widget_input_cancel_frame(strata_widget_input_context* context);
STRATA_API double strata_widget_input_retained_number(const strata_widget_input_context* context,
                                                      strata_string_view name, double fallback);
STRATA_API uint32_t strata_widget_input_retained_boolean(const strata_widget_input_context* context,
                                                         strata_string_view name,
                                                         uint32_t fallback);
STRATA_API strata_result strata_widget_input_set_retained_number(
    strata_widget_input_context* context, strata_string_view name, double value);
STRATA_API strata_result strata_widget_input_set_retained_boolean(
    strata_widget_input_context* context, strata_string_view name, uint32_t value);
/*
 * Text reads copy into caller storage; out_length always receives the byte length the field needs.
 * A field larger than capacity fails with STRATA_STATUS_INVALID_ARGUMENT and leaves the buffer
 * untouched, an absent field with STRATA_STATUS_NOT_FOUND.
 */
STRATA_API strata_result strata_widget_input_retained_text(
    const strata_widget_input_context* context, strata_string_view name, char* buffer,
    size_t capacity, size_t* out_length);
STRATA_API strata_result strata_widget_input_set_retained_text(strata_widget_input_context* context,
                                                               strata_string_view name,
                                                               strata_string_view value);
/*
 * Fixed-size extension state is copied exactly. The first successful write reserves node-local
 * storage; equal-size updates reuse it and perform no runtime-routed allocation.
 */
STRATA_API strata_result strata_widget_input_retained_bytes(
    const strata_widget_input_context* context, strata_string_view name, void* buffer, size_t size);
STRATA_API strata_result strata_widget_input_set_retained_bytes(
    strata_widget_input_context* context, strata_string_view name, const void* value, size_t size);
/**
 * Updates a paint-invalidating retained byte field on another keyed extension widget.
 * The target field must be declared by its widget; equal-size updates reuse target storage.
 */
STRATA_API strata_result strata_widget_input_set_target_retained_bytes(
    strata_widget_input_context* context, strata_string_view target_key, strata_string_view name,
    const void* value, size_t size);
/** Reports whether the authored property exists, independent of its typed fallback. */
STRATA_API uint32_t strata_widget_input_has_property(const strata_widget_input_context* context,
                                                     strata_string_view name);
STRATA_API double strata_widget_input_property_number(const strata_widget_input_context* context,
                                                      strata_string_view name, double fallback);
STRATA_API uint32_t strata_widget_input_property_boolean(const strata_widget_input_context* context,
                                                         strata_string_view name,
                                                         uint32_t fallback);
STRATA_API strata_result strata_widget_input_property_text(
    const strata_widget_input_context* context, strata_string_view name, char* buffer,
    size_t capacity, size_t* out_length);
STRATA_API strata_result strata_widget_input_emit_action_json(strata_widget_input_context* context,
                                                              strata_string_view action_id,
                                                              strata_string_view payload_json,
                                                              strata_string_view event_kind,
                                                              strata_string_view event_value_json);
/**
 * Dispatches the action authored in `action_property` with a canonical JSON event value.
 * An absent optional action property is accepted and produces an unhandled local event.
 */
STRATA_API strata_result strata_widget_input_emit_property_action_json(
    strata_widget_input_context* context, strata_string_view action_property,
    strata_string_view event_kind, strata_string_view event_value_json);
STRATA_API strata_result strata_widget_input_emit_property_color_event(
    strata_widget_input_context* context, strata_string_view action_property,
    strata_string_view event_kind, strata_color value);
STRATA_API strata_result strata_widget_input_emit_event_json(strata_widget_input_context* context,
                                                             strata_string_view event_kind,
                                                             strata_string_view event_value_json);
STRATA_API strata_result strata_widget_input_emit_number_event(strata_widget_input_context* context,
                                                               strata_string_view event_kind,
                                                               double value);
STRATA_API strata_result strata_widget_input_emit_boolean_event(
    strata_widget_input_context* context, strata_string_view event_kind, uint32_t value);
STRATA_API strata_result strata_widget_input_emit_text_event(strata_widget_input_context* context,
                                                             strata_string_view event_kind,
                                                             strata_string_view value);
STRATA_API strata_result strata_behavior_input_emit_action_json(
    strata_behavior_input_context* context, strata_string_view action_id,
    strata_string_view payload_json, strata_string_view event_kind,
    strata_string_view event_value_json);
STRATA_API strata_rect strata_widget_render_bounds(const strata_widget_render_context* context);
STRATA_API strata_rect
strata_widget_render_root_bounds(const strata_widget_render_context* context);
STRATA_API double strata_widget_render_scale(const strata_widget_render_context* context);
STRATA_API uint32_t strata_widget_render_focused(const strata_widget_render_context* context);
STRATA_API uint32_t strata_widget_render_focus_visible(const strata_widget_render_context* context);
STRATA_API uint32_t strata_widget_render_hovered(const strata_widget_render_context* context);
STRATA_API double strata_widget_render_retained_number(const strata_widget_render_context* context,
                                                       strata_string_view name, double fallback);
STRATA_API uint32_t strata_widget_render_retained_boolean(
    const strata_widget_render_context* context, strata_string_view name, uint32_t fallback);
STRATA_API double strata_widget_render_motion_progress(const strata_widget_render_context* context,
                                                       strata_string_view id, double fallback);
STRATA_API void strata_widget_render_rounded_rect(strata_widget_render_context* context,
                                                  strata_rect bounds, double radius,
                                                  strata_color fill, const strata_border* border);
STRATA_API void strata_widget_render_solid_rect(strata_widget_render_context* context,
                                                strata_rect bounds, strata_color color);
STRATA_API void strata_widget_render_border(strata_widget_render_context* context,
                                            strata_rect bounds, double radius,
                                            strata_border border);
STRATA_API void strata_widget_render_text(strata_widget_render_context* context,
                                          strata_string_view text, double x, double y,
                                          strata_color color);
STRATA_API void strata_widget_render_aligned_text(strata_widget_render_context* context,
                                                  strata_string_view text, strata_rect bounds,
                                                  strata_widget_text_alignment horizontal,
                                                  strata_widget_text_alignment vertical,
                                                  strata_color color);
STRATA_API void strata_widget_render_image(strata_widget_render_context* context,
                                           strata_rect bounds, strata_string_view image,
                                           strata_color tint, strata_texture_region source);
STRATA_API void strata_widget_render_nine_patch(strata_widget_render_context* context,
                                                strata_rect bounds, strata_string_view texture,
                                                strata_edges source_insets,
                                                strata_edges destination_insets,
                                                strata_texture_region source, strata_color tint);
/*
 * Indexed triangles only: index_count must be a non-zero multiple of three and every index must
 * address a declared vertex. A draw that fails validation is dropped whole rather than failing the
 * frame, so one malformed mesh never blanks a Surface.
 */
STRATA_API void strata_widget_render_custom_mesh(strata_widget_render_context* context,
                                                 strata_rect bounds, strata_string_view mesh,
                                                 const strata_mesh_geometry* geometry,
                                                 strata_string_view texture,
                                                 const strata_material_state* material);
STRATA_API void strata_widget_render_blur(strata_widget_render_context* context, strata_rect bounds,
                                          double radius, uint32_t downsample);
STRATA_API void strata_widget_render_shadow(strata_widget_render_context* context,
                                            strata_rect bounds, strata_corner_radii radii,
                                            strata_color color, double radius, double spread);
/* Every push must be matched by one pop inside the same presentation callback. */
STRATA_API void strata_widget_render_push_clip(strata_widget_render_context* context,
                                               strata_rect bounds);
STRATA_API void strata_widget_render_pop_clip(strata_widget_render_context* context);
/** Measures one unwrapped line through the same shaping cache the paint path uses. */
STRATA_API strata_result
strata_widget_render_text_metrics(const strata_widget_render_context* context,
                                  strata_string_view text, double* out_width, double* out_height);
STRATA_API uint32_t strata_widget_render_enabled(const strata_widget_render_context* context);
STRATA_API uint32_t strata_widget_render_active(const strata_widget_render_context* context);
STRATA_API strata_result strata_widget_render_retained_text(
    const strata_widget_render_context* context, strata_string_view name, char* buffer,
    size_t capacity, size_t* out_length);
STRATA_API strata_result
strata_widget_render_retained_bytes(const strata_widget_render_context* context,
                                    strata_string_view name, void* buffer, size_t size);
STRATA_API uint32_t strata_widget_render_has_property(const strata_widget_render_context* context,
                                                      strata_string_view name);
STRATA_API double strata_widget_render_property_number(const strata_widget_render_context* context,
                                                       strata_string_view name, double fallback);
STRATA_API uint32_t strata_widget_render_property_boolean(
    const strata_widget_render_context* context, strata_string_view name, uint32_t fallback);
STRATA_API strata_result strata_widget_render_property_text(
    const strata_widget_render_context* context, strata_string_view name, char* buffer,
    size_t capacity, size_t* out_length);
STRATA_API strata_rect
strata_widget_inspection_layout_bounds(const strata_widget_inspection_context* context);

STRATA_API strata_result strata_widget_semantics_set_name(strata_widget_semantics_context* context,
                                                          strata_string_view value);
STRATA_API strata_result strata_widget_semantics_set_value_text(
    strata_widget_semantics_context* context, strata_string_view value);
STRATA_API void strata_widget_semantics_set_value_range(strata_widget_semantics_context* context,
                                                        double current, double minimum,
                                                        double maximum);
STRATA_API strata_result strata_widget_semantics_add_action(
    strata_widget_semantics_context* context, strata_string_view action);
STRATA_API strata_result strata_widget_semantics_add_child(
    strata_widget_semantics_context* context, const strata_widget_semantic_child* child);
STRATA_API void strata_widget_semantics_set_checked(strata_widget_semantics_context* context,
                                                    uint32_t value);
STRATA_API void strata_widget_semantics_set_expanded(strata_widget_semantics_context* context,
                                                     uint32_t value);
STRATA_API void strata_widget_semantics_set_selected(strata_widget_semantics_context* context,
                                                     uint32_t value);
STRATA_API double
strata_widget_semantics_retained_number(const strata_widget_semantics_context* context,
                                        strata_string_view name, double fallback);
STRATA_API uint32_t strata_widget_semantics_retained_boolean(
    const strata_widget_semantics_context* context, strata_string_view name, uint32_t fallback);
STRATA_API uint32_t strata_widget_semantics_has_property(
    const strata_widget_semantics_context* context, strata_string_view name);
STRATA_API double
strata_widget_semantics_property_number(const strata_widget_semantics_context* context,
                                        strata_string_view name, double fallback);
STRATA_API uint32_t strata_widget_semantics_property_boolean(
    const strata_widget_semantics_context* context, strata_string_view name, uint32_t fallback);

STRATA_API strata_result strata_widget_semantics_retained_text(
    const strata_widget_semantics_context* context, strata_string_view name, char* buffer,
    size_t capacity, size_t* out_length);
STRATA_API strata_result strata_widget_semantics_property_text(
    const strata_widget_semantics_context* context, strata_string_view name, char* buffer,
    size_t capacity, size_t* out_length);
STRATA_API strata_result
strata_widget_semantics_retained_bytes(const strata_widget_semantics_context* context,
                                       strata_string_view name, void* buffer, size_t size);
STRATA_API strata_result strata_snapshot_get_info(const strata_snapshot* snapshot,
                                                  strata_snapshot_info* out_info);
STRATA_API void strata_snapshot_release(strata_snapshot* snapshot);

#ifdef __cplusplus
}
#endif

#endif
