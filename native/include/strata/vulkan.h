#ifndef STRATA_VULKAN_H
#define STRATA_VULKAN_H

#include <strata/adapter.h>
#include <vulkan/vulkan.h>

#if defined(_WIN32) && defined(STRATA_VULKAN_SHARED)
#if defined(STRATA_VULKAN_BUILD)
#define STRATA_VULKAN_API __declspec(dllexport)
#else
#define STRATA_VULKAN_API __declspec(dllimport)
#endif
#else
#define STRATA_VULKAN_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct strata_vulkan_presenter strata_vulkan_presenter;

/** Borrowed Vulkan 1.1+ graphics device/queue. Serialize all access to this queue.
 * Set struct_size and zero reserved fields. The device must outlive the presenter.
 */
typedef struct strata_vulkan_device {
    size_t struct_size;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    uint32_t reserved;
} strata_vulkan_device;

typedef uint32_t strata_vulkan_target_load_action;
#define STRATA_VULKAN_TARGET_PRESERVE UINT32_C(0)
#define STRATA_VULKAN_TARGET_CLEAR UINT32_C(1)

typedef strata_status (*strata_vulkan_program_source_fn)(void* user_data,
                                                         strata_string_view resource_id,
                                                         strata_bytes_view* out_source);

typedef struct strata_vulkan_presenter_options {
    size_t struct_size;
    uint32_t reserved;
    void* program_source_user_data;
    strata_vulkan_program_source_fn load_program_source;
} strata_vulkan_presenter_options;

/** Host-owned single-sample image/view with color-attachment and transfer src/dst usage.
 * Supported formats: RGBA8/BGRA8 UNORM and RGBA16F. See vulkan-hosting.md for queue ownership.
 */
typedef struct strata_vulkan_render_target {
    size_t struct_size;
    VkImage image;
    VkImageView view;
    VkFormat format;
    VkImageLayout initial_layout;
    VkImageLayout final_layout;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    double logical_width;
    double logical_height;
} strata_vulkan_render_target;

typedef struct strata_vulkan_frame_options {
    size_t struct_size;
    strata_vulkan_target_load_action load_action;
    uint32_t reserved;
    float clear_color[4];
} strata_vulkan_frame_options;

typedef struct strata_vulkan_render_telemetry {
    uint64_t blur_passes;
    uint64_t effect_passes;
} strata_vulkan_render_telemetry;

typedef struct strata_vulkan_presented_frame {
    size_t struct_size;
    strata_surface_frame_info surface;
    size_t packet_bytes;
    strata_vulkan_render_telemetry rendering;
} strata_vulkan_presented_frame;

STRATA_VULKAN_API strata_adapter_result strata_vulkan_presenter_create(
    strata_runtime* runtime, const strata_vulkan_device* device,
    const strata_vulkan_presenter_options* options, strata_vulkan_presenter** out_presenter);
STRATA_VULKAN_API void strata_vulkan_presenter_destroy(strata_vulkan_presenter* presenter);
STRATA_VULKAN_API strata_adapter_result
strata_vulkan_presenter_synchronize_programs(strata_vulkan_presenter* presenter);
STRATA_VULKAN_API strata_adapter_result strata_vulkan_presenter_reload_program_source(
    strata_vulkan_presenter* presenter, strata_string_view resource_id, strata_bytes_view source,
    uint32_t* out_matched);
STRATA_VULKAN_API strata_adapter_result strata_vulkan_presenter_attach(
    strata_vulkan_presenter* presenter, strata_string_view layer_id, strata_surface* surface);
/** Submit host writes first. This call submits to the borrowed queue and waits for its fence.
 * The target is left in final_layout. The host retains window/swapchain/presentation ownership.
 */
STRATA_VULKAN_API strata_adapter_result strata_vulkan_presenter_present(
    strata_vulkan_presenter* presenter, strata_string_view layer_id, strata_surface* surface,
    const strata_vulkan_render_target* target, int64_t time_nanoseconds,
    const strata_vulkan_frame_options* options, strata_vulkan_presented_frame* out_frame);
STRATA_VULKAN_API strata_adapter_result
strata_vulkan_presenter_detach(strata_vulkan_presenter* presenter, strata_string_view layer_id);
STRATA_VULKAN_API void strata_vulkan_presenter_discard(strata_vulkan_presenter* presenter,
                                                       strata_string_view layer_id);
STRATA_VULKAN_API strata_adapter_result strata_vulkan_presenter_attached(
    const strata_vulkan_presenter* presenter, strata_string_view layer_id, uint32_t* out_attached);
STRATA_VULKAN_API strata_adapter_result
strata_vulkan_presenter_release_target(strata_vulkan_presenter* presenter);

#ifdef __cplusplus
}
#endif

#endif
