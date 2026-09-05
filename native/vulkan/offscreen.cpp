#include "offscreen.hpp"
#include <cstdio>
#include <cstdlib>
#include <string_view>
namespace strata::vulkan::detail {
namespace {
VKAPI_ATTR VkBool32 VKAPI_CALL debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                     VkDebugUtilsMessageTypeFlagsEXT,
                                     const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        ++static_cast<Offscreen*>(user)->validation_errors;
    std::fprintf(stderr, "Vulkan validation: %s\n", data->pMessage);
    return VK_FALSE;
}
} // namespace
Offscreen::Offscreen(bool validation) {
    try {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Strata";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* extensions[]{VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
                                 VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
        const VkValidationFeatureEnableEXT synchronization =
            VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validation_features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        validation_features.enabledValidationFeatureCount = 1;
        validation_features.pEnabledValidationFeatures = &synchronization;
        if (validation) {
            info.enabledLayerCount = 1;
            info.ppEnabledLayerNames = &layer;
            info.enabledExtensionCount = 2;
            info.ppEnabledExtensionNames = extensions;
            info.pNext = &validation_features;
        }
        check(vkCreateInstance(&info, nullptr, &instance), "create Vulkan instance");
        if (validation) {
            VkDebugUtilsMessengerCreateInfoEXT di{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            di.pfnUserCallback = debug;
            di.pUserData = this;
            const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            if (!create)
                throw std::runtime_error("Vulkan debug utils unavailable");
            check(create(instance, &di, nullptr, &messenger), "create validation messenger");
        }
        std::uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate devices");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "read devices");
        const char* requested = std::getenv("STRATA_VULKAN_DEVICE");
        int best = -1;
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_1 ||
                (requested &&
                 std::string_view(properties.deviceName).find(requested) == std::string_view::npos))
                continue;
            std::uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> queues(families);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, queues.data());
            for (std::uint32_t i = 0; i < families; ++i)
                if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                    const int score =
                        properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                    if (score > best) {
                        device.physical_device = candidate;
                        device.queue_family = i;
                        device_name = properties.deviceName;
                        best = score;
                    }
                    break;
                }
        }
        if (!device.physical_device)
            throw std::runtime_error("no matching Vulkan 1.1 graphics device");
        float priority = 1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = device.queue_family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        check(vkCreateDevice(device.physical_device, &di, nullptr, &device.device),
              "create Vulkan device");
        vkGetDeviceQueue(device.device, device.queue_family, 0, &device.queue);
    } catch (...) {
        destroy();
        throw;
    }
}
void Offscreen::destroy() noexcept {
    if (device.device) {
        vkDeviceWaitIdle(device.device);
        readback.reset();
        image.reset();
        vkDestroyDevice(device.device, nullptr);
    }
    if (messenger) {
        const auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy_messenger)
            destroy_messenger(instance, messenger, nullptr);
    }
    if (instance)
        vkDestroyInstance(instance, nullptr);
}
Offscreen::~Offscreen() { destroy(); }
void Offscreen::resize(std::uint32_t width, std::uint32_t height) {
    if (image && image->width == width && image->height == height)
        return;
    auto next = std::make_unique<Image>(device, width, height, VK_FORMAT_R8G8B8A8_UNORM);
    auto next_readback = std::make_unique<Buffer>(
        device, static_cast<VkDeviceSize>(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    image = std::move(next);
    readback = std::move(next_readback);
}
RenderTarget Offscreen::target(double width, double height) const {
    if (!image)
        throw std::logic_error("offscreen target has not been sized");
    return {image->image, image->view,   image->format,
            image->width, image->height, width,
            height,       image->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
}
std::vector<std::uint8_t> Offscreen::pixels() {
    VkCommandPool pool{};
    VkFence fence{};
    struct Cleanup {
        VkDevice device;
        VkCommandPool* pool;
        VkFence* fence;
        ~Cleanup() {
            if (*fence)
                vkDestroyFence(device, *fence, nullptr);
            if (*pool)
                vkDestroyCommandPool(device, *pool, nullptr);
        }
    } cleanup{device.device, &pool, &fence};
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = device.queue_family;
    check(vkCreateCommandPool(device.device, &pi, nullptr, &pool), "create readback command pool");
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.commandBufferCount = 1;
    VkCommandBuffer command{};
    check(vkAllocateCommandBuffers(device.device, &ai, &command), "allocate readback command");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(command, &bi), "begin readback");
    // Renderer returned in TRANSFER_SRC_OPTIMAL, with all earlier writes made visible.
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {image->width, image->height, 1};
    vkCmdCopyImageToBuffer(command, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback->buffer, 1, &copy);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                         &barrier, 0, nullptr, 0, nullptr);
    check(vkEndCommandBuffer(command), "end readback");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(device.device, &fi, nullptr, &fence), "create readback fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    check(vkQueueSubmit(device.queue, 1, &submit, fence), "submit readback");
    check(vkWaitForFences(device.device, 1, &fence, VK_TRUE, UINT64_MAX), "wait readback");
    image->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const auto* bytes = static_cast<const std::uint8_t*>(readback->mapped);
    return {bytes, bytes + static_cast<std::size_t>(readback->size)};
}
} // namespace strata::vulkan::detail
