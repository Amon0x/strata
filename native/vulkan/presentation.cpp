#include <strata/vulkan_presentation.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "resources.hpp"

namespace strata::vulkan {
namespace {
using detail::check;
constexpr VkClearColorValue transparent_clear{};

/** Owns WSI and every GPU object on the consuming thread; platform creation is the sole callback. */
class WindowDriver final : public host::PresentationDriver {
  public:
    WindowDriver(WindowSurface surface, const WindowOptions options)
        : options_(options) {
        clear_.float32[3] = options.clear_color[3] / 255.0F;
        for (std::size_t channel = 0U; channel < 3U; ++channel)
            clear_.float32[channel] = options.clear_color[channel] / 255.0F * clear_.float32[3];
        try {
            initialize(std::move(surface));
        } catch (...) {
            destroy();
            throw;
        }
    }
    ~WindowDriver() override { destroy(); }

    [[nodiscard]] host::PresentationInfo info() const override { return information_; }
    void declare_material(const host::MaterialSource& material) override {
        renderer_->declare_material(material.declaration.id, material.source);
    }
    void declare_effect(const host::EffectSource& effect) override {
        const auto& pass = effect.declaration;
        renderer_->declare_effect_pass(pass.effect_id, pass.index,
            static_cast<std::uint32_t>(pass.kind), pass.radius, pass.downsample,
            pass.radius_parameter.value_or(STRATA_EFFECT_PARAMETER_NONE),
            pass.downsample_parameter.value_or(STRATA_EFFECT_PARAMETER_NONE), effect.source);
    }
    void resize(const host::PresentationViewport& viewport) override {
        if (viewport.width != 0U && viewport.height != 0U &&
            (!std::isfinite(viewport.logical_width) || !std::isfinite(viewport.logical_height) ||
             viewport.logical_width <= 0.0 || viewport.logical_height <= 0.0))
            throw std::invalid_argument("Vulkan presentation requires positive logical dimensions");
        recreate_ = recreate_ || viewport.width != viewport_.width || viewport.height != viewport_.height;
        viewport_ = viewport;
    }
    void set_pointer(const std::optional<std::array<double, 2U>> position) override {
        pointer_ = position;
    }
    void set_transparent(const bool transparent) override {
        if (transparent_ != transparent) {
            transparent_ = transparent;
            recreate_ = true;
        }
    }
    void consume_resources(const host::RenderPacket& packet) override {
        renderer_->consume_resources(packet);
    }
    void release_layer(const std::string_view id) noexcept override { renderer_->release_layer(id); }

    [[nodiscard]] bool begin_frame() override {
        if (viewport_.width == 0U || viewport_.height == 0U)
            return false;
        if ((recreate_ || swapchain_ == VK_NULL_HANDLE) && !create_swapchain())
            return false;
        Frame& frame = frames_[frame_index_];
        check(vkWaitForFences(device_.device, 1U, &frame.finished, VK_TRUE, UINT64_MAX),
              "wait window frame fence");
        // A compositor may withhold its images while hidden. Finite acquisition lets queued
        // resize/close commands regain the owner rather than depending on another visible frame.
        const VkResult acquired = vkAcquireNextImageKHR(device_.device, swapchain_, 100'000'000U,
                                                        frame.available, VK_NULL_HANDLE, &image_index_);
        if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY)
            return false;
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            recreate_ = true;
            return false;
        }
        if (acquired == VK_SUBOPTIMAL_KHR)
            recreate_ = true;
        else
            check(acquired, "acquire window image");
        check(vkResetCommandPool(device_.device, frame.pool, 0U), "reset window command pool");
        begin_command(frame.commands[0]);
        Image& image = images_[image_index_];
        detail::transition(frame.commands[0], image.image,
                           image.presented ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearColorValue& color = transparent_ ? transparent_clear : clear_;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
        vkCmdClearColorImage(frame.commands[0], image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &color, 1U, &range);
        detail::transition(frame.commands[0], image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        check(vkEndCommandBuffer(frame.commands[0]), "end window clear");
        constexpr VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1U;
        submit.pWaitSemaphores = &frame.available;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1U;
        submit.pCommandBuffers = &frame.commands[0];
        check(vkQueueSubmit(device_.queue, 1U, &submit, VK_NULL_HANDLE), "submit window acquisition");
        frame_seconds_ = std::chrono::duration<double>(Clock::now() - started_).count();
        return true;
    }

    void release_programs(const std::string_view prefix) override {
        renderer_->release_programs(prefix);
    }

    [[nodiscard]] host::PresentationLayerTiming draw(
        const std::string_view id, const host::RenderPacket& packet) override {
        const Image& image = images_[image_index_];
        const RenderTarget target{image.image, image.view, format_, extent_.width, extent_.height,
                                  viewport_.logical_width, viewport_.logical_height,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
        FrameOptions options;
        options.time_seconds = frame_seconds_;
        options.pointer = pointer_;
        const RenderLayerTelemetry measured = renderer_->render(id, packet, target, options);
        host::PresentationLayerTiming result;
        result.blur_passes = measured.blur_passes;
        result.effect_passes = measured.effect_passes;
        result.live = measured.live;
        return result;
    }
    [[nodiscard]] std::vector<strata_backdrop_sample> take_backdrop_samples(std::string_view) override {
        // The low-level Vulkan renderer currently produces no CPU backdrop probes.
        return {};
    }

    [[nodiscard]] host::PresentationResult end_frame() override {
        Frame& frame = frames_[frame_index_];
        Image& image = images_[image_index_];
        begin_command(frame.commands[1]);
        if (capture_requested_) {
            const VkDeviceSize bytes = VkDeviceSize{extent_.width} * extent_.height * 4U;
            if (readback_ == nullptr || readback_->size != bytes)
                readback_ = std::make_unique<detail::Buffer>(device_, bytes,
                                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 0U, 1U};
            copy.imageExtent = {extent_.width, extent_.height, 1U};
            vkCmdCopyImageToBuffer(frame.commands[1], image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback_->buffer, 1U, &copy);
            VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = readback_->buffer;
            barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(frame.commands[1], VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, nullptr, 1U, &barrier, 0U, nullptr);
        }
        detail::transition(frame.commands[1], image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        check(vkEndCommandBuffer(frame.commands[1]), "end window presentation commands");
        check(vkResetFences(device_.device, 1U, &frame.finished), "reset window frame fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1U;
        submit.pCommandBuffers = &frame.commands[1];
        submit.signalSemaphoreCount = 1U;
        submit.pSignalSemaphores = &image.rendered;
        check(vkQueueSubmit(device_.queue, 1U, &submit, frame.finished), "submit window presentation");
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1U;
        present.pWaitSemaphores = &image.rendered;
        present.swapchainCount = 1U;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &image_index_;
        const VkResult result = vkQueuePresentKHR(device_.queue, &present);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            recreate_ = true;
        else
            check(result, "present window image");
        image.presented = true;
        if (capture_requested_) {
            check(vkWaitForFences(device_.device, 1U, &frame.finished, VK_TRUE, UINT64_MAX),
                  "wait window capture");
            capture_.emplace();
            capture_->width = extent_.width;
            capture_->height = extent_.height;
            capture_->rgba.resize(static_cast<std::size_t>(readback_->size));
            std::memcpy(capture_->rgba.data(), readback_->mapped, capture_->rgba.size());
            for (std::size_t pixel = 0U; pixel < capture_->rgba.size(); pixel += 4U) {
                auto* const rgba = capture_->rgba.data() + pixel;
                if (format_ == VK_FORMAT_B8G8R8A8_UNORM)
                    std::swap(rgba[0], rgba[2]);
                const unsigned alpha = rgba[3];
                if (alpha != 255U) {
                    for (std::size_t channel = 0U; channel < 3U; ++channel)
                        rgba[channel] = static_cast<std::uint8_t>(alpha == 0U ? 0U :
                            std::min(255U, (rgba[channel] * 255U + alpha / 2U) / alpha));
                }
            }
            capture_requested_ = false;
        }
        frame_index_ = (frame_index_ + 1U) % frames_.size();
        if (validation_errors_.load(std::memory_order_relaxed) != 0U)
            throw std::runtime_error("Vulkan window presentation reported validation errors");
        return {result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR, recreate_};
    }
    void capture_next_frame() override { capture_requested_ = true; }
    [[nodiscard]] std::optional<host::PresentationCapture> take_capture() override {
        return std::exchange(capture_, std::nullopt);
    }

  private:
    using Clock = std::chrono::steady_clock;
    struct Frame final {
        VkCommandPool pool{};
        std::array<VkCommandBuffer, 2U> commands{};
        VkSemaphore available{};
        VkFence finished{};
    };
    struct Image final {
        VkImage image{};
        VkImageView view{};
        // Reacquiring this image, unlike a rendering fence, proves its prior Present consumed
        // the semaphore. Presentation-wait semaphores therefore belong to images, not frames.
        VkSemaphore rendered{};
        bool presented = false;
    };
    static VKAPI_ATTR VkBool32 VKAPI_CALL diagnostic(
        const VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
        if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0U)
            static_cast<WindowDriver*>(user)->validation_errors_.fetch_add(1U, std::memory_order_relaxed);
        std::fprintf(stderr, "Vulkan presentation validation: %s\n", data->pMessage);
        return VK_FALSE;
    }
    static void begin_command(const VkCommandBuffer command) {
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command, &info), "begin window commands");
    }
    void initialize(WindowSurface surface) {
        if (!surface.create)
            throw std::invalid_argument("Vulkan presentation requires a platform surface factory");
        if (options_.validation)
            surface.instance_extensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        std::vector<const char*> extensions;
        extensions.reserve(surface.instance_extensions.size());
        for (const std::string& extension : surface.instance_extensions)
            extensions.push_back(extension.c_str());
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "Strata";
        application.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance.pApplicationInfo = &application;
        instance.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        instance.ppEnabledExtensionNames = extensions.data();
        constexpr const char* validation_layer = "VK_LAYER_KHRONOS_validation";
        if (options_.validation) {
            instance.enabledLayerCount = 1U;
            instance.ppEnabledLayerNames = &validation_layer;
        }
        check(vkCreateInstance(&instance, nullptr, &instance_), "create window Vulkan instance");
        if (options_.validation) {
            const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
            if (create == nullptr)
                throw std::runtime_error("Vulkan presentation debug utils unavailable");
            VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            info.pfnUserCallback = diagnostic;
            info.pUserData = this;
            check(create(instance_, &info, nullptr, &messenger_), "create window validation messenger");
        }
        surface_ = surface.create(instance_);
        if (surface_ == VK_NULL_HANDLE)
            throw std::runtime_error("Vulkan platform surface creation returned no surface");
        std::uint32_t count = 0U;
        check(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "enumerate window devices");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "read window devices");
        int best = -1;
        for (const VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_1)
                continue;
            std::uint32_t families = 0U;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> queues(families);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, queues.data());
            for (std::uint32_t family = 0U; family < families; ++family) {
                if ((queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U)
                    continue;
                VkBool32 supported = VK_FALSE;
                check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family, surface_, &supported),
                      "query window presentation support");
                if (supported == VK_FALSE)
                    continue;
                const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                if (score > best) {
                    best = score;
                    device_.physical_device = candidate;
                    device_.queue_family = family;
                    information_.adapter = properties.deviceName;
                    information_.vendor_id = properties.vendorID;
                    information_.device_id = properties.deviceID;
                    information_.driver_version = std::to_string(properties.driverVersion);
                }
                break;
            }
        }
        if (device_.physical_device == VK_NULL_HANDLE)
            throw std::runtime_error("no Vulkan 1.1 graphics queue can present to this surface");
        const float priority = 1.0F;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = device_.queue_family;
        queue.queueCount = 1U;
        queue.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(device_.physical_device, &supported);
        VkPhysicalDeviceFeatures features{};
        features.dualSrcBlend = supported.dualSrcBlend;
        device_.dual_source_blend = supported.dualSrcBlend == VK_TRUE;
        constexpr const char* swapchain_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device.queueCreateInfoCount = 1U;
        device.pQueueCreateInfos = &queue;
        device.pEnabledFeatures = &features;
        device.enabledExtensionCount = 1U;
        device.ppEnabledExtensionNames = &swapchain_extension;
        check(vkCreateDevice(device_.physical_device, &device, nullptr, &device_.device),
              "create window Vulkan device");
        vkGetDeviceQueue(device_.device, device_.queue_family, 0U, &device_.queue);
        check(vkGetPhysicalDeviceSurfacePresentModesKHR(device_.physical_device, surface_, &count, nullptr),
              "query window present modes");
        std::vector<VkPresentModeKHR> modes(count);
        check(vkGetPhysicalDeviceSurfacePresentModesKHR(device_.physical_device, surface_, &count, modes.data()),
              "read window present modes");
        if (!options_.vsync) {
            if (std::ranges::find(modes, VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end())
                present_mode_ = VK_PRESENT_MODE_IMMEDIATE_KHR;
            else if (std::ranges::find(modes, VK_PRESENT_MODE_MAILBOX_KHR) != modes.end())
                present_mode_ = VK_PRESENT_MODE_MAILBOX_KHR;
        }
        information_.vsync = present_mode_ == VK_PRESENT_MODE_FIFO_KHR;
        renderer_ = std::make_unique<Renderer>(device_);
        for (Frame& frame : frames_) {
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.queueFamilyIndex = device_.queue_family;
            check(vkCreateCommandPool(device_.device, &pool, nullptr, &frame.pool),
                  "create window command pool");
            VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            commands.commandPool = frame.pool;
            commands.commandBufferCount = static_cast<std::uint32_t>(frame.commands.size());
            check(vkAllocateCommandBuffers(device_.device, &commands, frame.commands.data()),
                  "allocate window commands");
            VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            check(vkCreateSemaphore(device_.device, &semaphore, nullptr, &frame.available),
                  "create window acquisition semaphore");
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            check(vkCreateFence(device_.device, &fence, nullptr, &frame.finished),
                  "create window frame fence");
        }
    }

    [[nodiscard]] bool create_swapchain() {
        VkSurfaceCapabilitiesKHR capabilities{};
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_.physical_device, surface_, &capabilities),
              "query window capabilities");
        VkExtent2D extent = capabilities.currentExtent;
        if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
            extent.width = std::clamp(viewport_.width, capabilities.minImageExtent.width,
                                      capabilities.maxImageExtent.width);
            extent.height = std::clamp(viewport_.height, capabilities.minImageExtent.height,
                                       capabilities.maxImageExtent.height);
        }
        if (extent.width == 0U || extent.height == 0U)
            return false;
        constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ((capabilities.supportedUsageFlags & usage) != usage)
            throw std::runtime_error("Vulkan window images require color and transfer usage");
        std::uint32_t count = 0U;
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(device_.physical_device, surface_, &count, nullptr),
              "query window formats");
        std::vector<VkSurfaceFormatKHR> formats(count);
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(device_.physical_device, surface_, &count, formats.data()),
              "read window formats");
        std::optional<VkSurfaceFormatKHR> format;
        for (const VkSurfaceFormatKHR candidate : formats) {
            if (candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                (candidate.format == VK_FORMAT_B8G8R8A8_UNORM ||
                 candidate.format == VK_FORMAT_R8G8B8A8_UNORM || candidate.format == VK_FORMAT_UNDEFINED)) {
                format = candidate;
                if (format->format == VK_FORMAT_UNDEFINED)
                    format->format = VK_FORMAT_B8G8R8A8_UNORM;
                break;
            }
        }
        if (!format.has_value())
            throw std::runtime_error("Vulkan presentation requires an RGBA8/BGRA8 UNORM surface");
        const VkCompositeAlphaFlagBitsKHR alpha = transparent_ || options_.clear_color[3] != 255U
            ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if ((capabilities.supportedCompositeAlpha & alpha) == 0U)
            throw std::runtime_error("Vulkan surface does not support the requested window alpha mode");
        check(vkDeviceWaitIdle(device_.device), "wait window resize");
        renderer_->release_target();
        release_swapchain();
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface_;
        info.minImageCount = capabilities.minImageCount + 1U;
        if (capabilities.maxImageCount != 0U)
            info.minImageCount = std::min(info.minImageCount, capabilities.maxImageCount);
        info.imageFormat = format->format;
        info.imageColorSpace = format->colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1U;
        info.imageUsage = usage;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = alpha;
        info.presentMode = present_mode_;
        info.clipped = VK_TRUE;
        check(vkCreateSwapchainKHR(device_.device, &info, nullptr, &swapchain_), "create window swap chain");
        check(vkGetSwapchainImagesKHR(device_.device, swapchain_, &count, nullptr), "query window images");
        std::vector<VkImage> images(count);
        check(vkGetSwapchainImagesKHR(device_.device, swapchain_, &count, images.data()), "read window images");
        images_.resize(count);
        for (std::size_t index = 0U; index < images.size(); ++index) {
            Image& image = images_[index];
            image.image = images[index];
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format->format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
            check(vkCreateImageView(device_.device, &view, nullptr, &image.view), "create window image view");
            VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            check(vkCreateSemaphore(device_.device, &semaphore, nullptr, &image.rendered),
                  "create window presentation semaphore");
        }
        format_ = format->format;
        extent_ = extent;
        recreate_ = false;
        return true;
    }
    void release_swapchain() noexcept {
        readback_.reset();
        for (const Image& image : images_) {
            if (image.view != VK_NULL_HANDLE)
                vkDestroyImageView(device_.device, image.view, nullptr);
            if (image.rendered != VK_NULL_HANDLE)
                vkDestroySemaphore(device_.device, image.rendered, nullptr);
        }
        images_.clear();
        if (swapchain_ != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(device_.device, std::exchange(swapchain_, VK_NULL_HANDLE), nullptr);
    }
    void destroy() noexcept {
        if (device_.device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_.device);
            renderer_.reset();
            release_swapchain();
            for (const Frame& frame : frames_) {
                if (frame.pool != VK_NULL_HANDLE)
                    vkDestroyCommandPool(device_.device, frame.pool, nullptr);
                if (frame.available != VK_NULL_HANDLE)
                    vkDestroySemaphore(device_.device, frame.available, nullptr);
                if (frame.finished != VK_NULL_HANDLE)
                    vkDestroyFence(device_.device, frame.finished, nullptr);
            }
            vkDestroyDevice(device_.device, nullptr);
        }
        if (surface_ != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (messenger_ != VK_NULL_HANDLE) {
            const auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy_messenger != nullptr)
                destroy_messenger(instance_, messenger_, nullptr);
        }
        if (instance_ != VK_NULL_HANDLE)
            vkDestroyInstance(instance_, nullptr);
    }

    WindowOptions options_;
    VkClearColorValue clear_{};
    host::PresentationInfo information_;
    host::PresentationViewport viewport_;
    VkInstance instance_{};
    VkDebugUtilsMessengerEXT messenger_{};
    VkSurfaceKHR surface_{};
    Device device_;
    std::unique_ptr<Renderer> renderer_;
    VkSwapchainKHR swapchain_{};
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent_{};
    std::array<Frame, 2U> frames_;
    std::vector<Image> images_;
    std::size_t frame_index_ = 0U;
    std::uint32_t image_index_ = 0U;
    std::optional<std::array<double, 2U>> pointer_;
    bool transparent_ = false;
    bool recreate_ = true;
    bool capture_requested_ = false;
    std::unique_ptr<detail::Buffer> readback_;
    std::optional<host::PresentationCapture> capture_;
    std::atomic_uint validation_errors_{0U};
    Clock::time_point started_ = Clock::now();
    double frame_seconds_ = 0.0;
};
} // namespace

std::unique_ptr<host::PresentationDriver> create_window_driver(WindowSurface surface,
                                                              const WindowOptions options) {
    return std::make_unique<WindowDriver>(std::move(surface), options);
}
} // namespace strata::vulkan
