#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <strata/vulkan.hpp>
#include <string>
#include <utility>
#include <vector>

namespace strata::vulkan::detail {
inline void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed (VkResult " +
                                 std::to_string(result) + ")");
}
inline std::uint32_t memory_type(Device device, std::uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(device.physical_device, &properties);
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1U << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    throw std::runtime_error("Vulkan memory type unavailable");
}
struct Buffer final {
    VkDevice device{};
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped = nullptr;
    VkDeviceSize size{};
    Buffer(Device d, VkDeviceSize bytes, VkBufferUsageFlags usage) : device(d.device), size(bytes) {
        try {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = bytes;
            info.usage = usage;
            check(vkCreateBuffer(device, &info, nullptr, &buffer), "create buffer");
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(device, buffer, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memory_type(d, requirements.memoryTypeBits,
                                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            check(vkAllocateMemory(device, &allocation, nullptr, &memory),
                  "allocate buffer memory");
            check(vkBindBufferMemory(device, buffer, memory, 0), "bind buffer memory");
            check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map buffer");
        } catch (...) {
            destroy();
            throw;
        }
    }
    ~Buffer() { destroy(); }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    void destroy() noexcept {
        if (mapped)
            vkUnmapMemory(device, memory);
        if (buffer)
            vkDestroyBuffer(device, buffer, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
    }
};
struct Image final {
    VkDevice device{};
    VkImage image{};
    VkImageView view{};
    VkDeviceMemory memory{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format{};
    std::uint32_t width{}, height{};
    VkFilter filter = VK_FILTER_LINEAR;
    Image(Device d, std::uint32_t w, std::uint32_t h, VkFormat f)
        : device(d.device), format(f), width(w), height(h) {
        if (!w || !h)
            throw std::invalid_argument("Vulkan image dimensions must be positive");
        try {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = f;
            info.extent = {w, h, 1};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (f != VK_FORMAT_R8_UNORM)
                info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            check(vkCreateImage(device, &info, nullptr, &image), "create image");
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device, image, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex =
                memory_type(d, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(vkAllocateMemory(device, &allocation, nullptr, &memory), "allocate image memory");
            check(vkBindImageMemory(device, image, memory, 0), "bind image memory");
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = f;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            check(vkCreateImageView(device, &vi, nullptr, &view), "create image view");
        } catch (...) {
            destroy();
            throw;
        }
    }
    ~Image() { destroy(); }
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    void destroy() noexcept {
        if (view)
            vkDestroyImageView(device, view, nullptr);
        if (image)
            vkDestroyImage(device, image, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
    }
};
inline void transition(VkCommandBuffer command, VkImage image, VkImageLayout before,
                       VkImageLayout after) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = before == VK_IMAGE_LAYOUT_UNDEFINED
                                ? 0U
                                : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = before;
    barrier.newLayout = after;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}
inline void transition(VkCommandBuffer command, Image& image, VkImageLayout after) {
    transition(command, image.image, image.layout, after);
    image.layout = after;
}
struct Slice {
    VkBuffer buffer;
    VkDeviceSize offset;
    VkDeviceSize size;
};
class UploadArena final {
    Device device_;
    std::vector<std::unique_ptr<Buffer>> buffers_;
    std::size_t active_ = 0;
    VkDeviceSize offset_ = 0;
    VkDeviceSize alignment_ = 256;

  public:
    explicit UploadArena(Device d) : device_(d) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d.physical_device, &p);
        alignment_ = std::max<VkDeviceSize>(256, p.limits.minUniformBufferOffsetAlignment);
    }
    void reset() {
        active_ = 0;
        offset_ = 0;
    }
    Slice upload(const void* data, std::size_t size) {
        const VkDeviceSize bytes = std::max<std::size_t>(size, 16);
        offset_ = (offset_ + alignment_ - 1) / alignment_ * alignment_;
        while (active_ < buffers_.size() && offset_ + bytes > buffers_[active_]->size) {
            ++active_;
            offset_ = 0;
        }
        if (active_ == buffers_.size())
            buffers_.push_back(std::make_unique<Buffer>(
                device_, std::max<VkDeviceSize>(4 * 1024 * 1024, bytes),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
        auto& buffer = *buffers_[active_];
        if (size)
            std::memcpy(static_cast<char*>(buffer.mapped) + offset_, data, size);
        Slice result{buffer.buffer, offset_, bytes};
        offset_ += bytes;
        return result;
    }
};
} // namespace strata::vulkan::detail
