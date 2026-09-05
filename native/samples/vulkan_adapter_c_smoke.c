#include <strata/vulkan.h>

int main(void) {
    strata_vulkan_presenter* presenter = NULL;
    const strata_adapter_result result =
        strata_vulkan_presenter_create(NULL, NULL, NULL, &presenter);
    if (result.status != STRATA_STATUS_INVALID_ARGUMENT || presenter != NULL)
        return 1;
    strata_vulkan_presenter_destroy(NULL);
    return 0;
}
