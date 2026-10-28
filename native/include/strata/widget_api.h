#ifndef STRATA_WIDGET_API_H
#define STRATA_WIDGET_API_H

#include <strata/strata.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The function table an extension package calls back into Strata through. A host passes it to the
 * package's strata_extension_plugin_query_v2 entry point, so a package library binds to no Strata
 * symbol by name and links nothing from strata_c. The table is versioned on its own: a newer table
 * only appends members, which a package compiled against an older version never reads, and the
 * core ABI version of the descriptors a package returns is negotiated separately.
 *
 * Every member has the contract of the exported strata_widget_* / strata_behavior_* function of
 * the same name, which remain available for packages built before the table existed. The four
 * per-context accessor families (strata_widget_{input,render,semantics,subtargets}_property_* and
 * _retained_*) are one family here that takes the context's kind.
 */

#define STRATA_WIDGET_API_VERSION_1 UINT32_C(1)
#define STRATA_WIDGET_API_VERSION_CURRENT STRATA_WIDGET_API_VERSION_1

typedef uint32_t strata_widget_context_kind;

#define STRATA_WIDGET_CONTEXT_INPUT UINT32_C(1)
#define STRATA_WIDGET_CONTEXT_RENDER UINT32_C(2)
#define STRATA_WIDGET_CONTEXT_SEMANTICS UINT32_C(3)
#define STRATA_WIDGET_CONTEXT_SUBTARGETS UINT32_C(4)

typedef struct strata_widget_api {
    size_t struct_size;
    uint32_t api_version;
    uint32_t reserved;

    /*
     * Property and retained-field reads for any widget context. `context` is the callback's
     * strata_widget_{input,render,semantics,subtargets}_context named by `kind`; an unknown kind
     * reads as absent.
     */
    /* Nonzero when the authored property exists, independent of any typed fallback. */
    uint32_t (*has_property)(strata_widget_context_kind kind, const void* context,
        strata_string_view name);
    /* The authored property value, valid for the callback; null when absent. */
    const strata_widget_value* (*property_value)(strata_widget_context_kind kind, const void* context,
        strata_string_view name);
    double (*property_number)(strata_widget_context_kind kind, const void* context,
        strata_string_view name, double fallback);
    uint32_t (*property_boolean)(strata_widget_context_kind kind, const void* context,
        strata_string_view name, uint32_t fallback);
    strata_result (*property_text)(strata_widget_context_kind kind, const void* context,
        strata_string_view name, char* buffer, size_t capacity, size_t* out_length);
    /* Retained fields are declared by the widget; a subtargets context reads them as bytes only. */
    double (*retained_number)(strata_widget_context_kind kind, const void* context,
        strata_string_view name, double fallback);
    uint32_t (*retained_boolean)(strata_widget_context_kind kind, const void* context,
        strata_string_view name, uint32_t fallback);
    strata_result (*retained_text)(strata_widget_context_kind kind, const void* context,
        strata_string_view name, char* buffer, size_t capacity, size_t* out_length);
    strata_result (*retained_bytes)(strata_widget_context_kind kind, const void* context,
        strata_string_view name, void* buffer, size_t size);

    strata_widget_value_kind (*value_get_kind)(const strata_widget_value* value);
    uint32_t (*value_get_boolean)(const strata_widget_value* value, uint32_t fallback);
    double (*value_get_number)(const strata_widget_value* value, double fallback);
    uint32_t (*value_get_color)(const strata_widget_value* value, strata_color* out_color);
    strata_string_view (*value_get_text)(const strata_widget_value* value);
    size_t (*value_list_size)(const strata_widget_value* value);
    const strata_widget_value* (*value_list_at)(const strata_widget_value* value, size_t index);
    const strata_widget_value* (*value_object_field)(const strata_widget_value* value, strata_string_view name);

    strata_rect (*input_bounds)(const strata_widget_input_context* context);
    double (*input_scale)(const strata_widget_input_context* context);
    uint32_t (*input_claim_gesture)(strata_widget_input_context* context);
    uint32_t (*input_cancel_gesture)(strata_widget_input_context* context);
    strata_result (*input_invalidate)(strata_widget_input_context* context, strata_widget_invalidation invalidation);
    strata_result (*input_request_frame)(strata_widget_input_context* context, strata_widget_frame_cost cost);
    uint32_t (*input_cancel_frame)(strata_widget_input_context* context);
    strata_result (*input_set_retained_number)(strata_widget_input_context* context, strata_string_view name, double value);
    strata_result (*input_set_retained_boolean)(strata_widget_input_context* context, strata_string_view name, uint32_t value);
    strata_result (*input_set_retained_text)(strata_widget_input_context* context, strata_string_view name, strata_string_view value);
    strata_result (*input_set_retained_bytes)(strata_widget_input_context* context, strata_string_view name, const void* value, size_t size);
    strata_result (*input_set_target_retained_bytes)(strata_widget_input_context* context, strata_string_view target_key, strata_string_view name, const void* value, size_t size);
    strata_result (*input_emit_action_json)(strata_widget_input_context* context, strata_string_view action_id, strata_string_view payload_json, strata_string_view event_kind, strata_string_view event_value_json);
    strata_result (*input_emit_property_action_json)(strata_widget_input_context* context, strata_string_view action_property, strata_string_view event_kind, strata_string_view event_value_json);
    strata_result (*input_emit_property_color_event)(strata_widget_input_context* context, strata_string_view action_property, strata_string_view event_kind, strata_color value);
    strata_result (*input_emit_event_json)(strata_widget_input_context* context, strata_string_view event_kind, strata_string_view event_value_json);
    strata_result (*input_emit_number_event)(strata_widget_input_context* context, strata_string_view event_kind, double value);
    strata_result (*input_emit_boolean_event)(strata_widget_input_context* context, strata_string_view event_kind, uint32_t value);
    strata_result (*input_emit_text_event)(strata_widget_input_context* context, strata_string_view event_kind, strata_string_view value);

    strata_result (*behavior_input_emit_action_json)(strata_behavior_input_context* context, strata_string_view action_id, strata_string_view payload_json, strata_string_view event_kind, strata_string_view event_value_json);

    const strata_widget_value* (*render_style_value)(const strata_widget_render_context* context, strata_string_view name);
    strata_rect (*render_bounds)(const strata_widget_render_context* context);
    strata_rect (*render_root_bounds)(const strata_widget_render_context* context);
    double (*render_scale)(const strata_widget_render_context* context);
    uint32_t (*render_focused)(const strata_widget_render_context* context);
    uint32_t (*render_focus_visible)(const strata_widget_render_context* context);
    uint32_t (*render_hovered)(const strata_widget_render_context* context);
    double (*render_motion_progress)(const strata_widget_render_context* context, strata_string_view id, double fallback);
    void (*render_rounded_rect)(strata_widget_render_context* context, strata_rect bounds, double radius, strata_color fill, const strata_border* border);
    void (*render_solid_rect)(strata_widget_render_context* context, strata_rect bounds, strata_color color);
    void (*render_border)(strata_widget_render_context* context, strata_rect bounds, double radius, strata_border border);
    void (*render_text)(strata_widget_render_context* context, strata_string_view text, double x, double y, strata_color color);
    void (*render_aligned_text)(strata_widget_render_context* context, strata_string_view text, strata_rect bounds, strata_widget_text_alignment horizontal, strata_widget_text_alignment vertical, strata_color color);
    void (*render_image)(strata_widget_render_context* context, strata_rect bounds, strata_string_view image, strata_color tint, strata_texture_region source);
    void (*render_nine_patch)(strata_widget_render_context* context, strata_rect bounds, strata_string_view texture, strata_edges source_insets, strata_edges destination_insets, strata_texture_region source, strata_color tint);
    void (*render_custom_mesh)(strata_widget_render_context* context, strata_rect bounds, strata_string_view mesh, const strata_mesh_geometry* geometry, strata_string_view texture, const strata_material_state* material);
    void (*render_blur)(strata_widget_render_context* context, strata_rect bounds, double radius, uint32_t downsample);
    void (*render_shadow)(strata_widget_render_context* context, strata_rect bounds, strata_corner_radii radii, strata_color color, double radius, double spread);
    void (*render_push_clip)(strata_widget_render_context* context, strata_rect bounds);
    void (*render_pop_clip)(strata_widget_render_context* context);
    void (*render_discard)(strata_widget_render_context* context);
    strata_result (*render_text_metrics)(const strata_widget_render_context* context, strata_string_view text, double* out_width, double* out_height);
    uint32_t (*render_enabled)(const strata_widget_render_context* context);
    uint32_t (*render_active)(const strata_widget_render_context* context);

    strata_rect (*inspection_layout_bounds)(const strata_widget_inspection_context* context);

    strata_result (*semantics_set_name)(strata_widget_semantics_context* context, strata_string_view value);
    strata_result (*semantics_set_value_text)(strata_widget_semantics_context* context, strata_string_view value);
    void (*semantics_set_value_range)(strata_widget_semantics_context* context, double current, double minimum, double maximum);
    strata_result (*semantics_add_action)(strata_widget_semantics_context* context, strata_string_view action);
    strata_result (*semantics_add_child)(strata_widget_semantics_context* context, const strata_widget_semantic_child* child);
    void (*semantics_set_checked)(strata_widget_semantics_context* context, uint32_t value);
    void (*semantics_set_expanded)(strata_widget_semantics_context* context, uint32_t value);
    void (*semantics_set_selected)(strata_widget_semantics_context* context, uint32_t value);

    strata_rect (*subtargets_bounds)(const strata_widget_subtargets_context* context);
    strata_result (*subtargets_reserve)(strata_widget_subtargets_context* context, size_t capacity);
    strata_result (*subtargets_add)(strata_widget_subtargets_context* context, const strata_widget_subtarget* subtarget);
} strata_widget_api;

/*
 * The table this library implements, for a host to pass to extension packages. Fails with
 * STRATA_STATUS_UNSUPPORTED_ABI when `requested_version` is newer than the library's table.
 */
STRATA_API strata_result strata_widget_api_query(uint32_t requested_version,
                                                 const strata_widget_api** out_api)
    STRATA_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif
