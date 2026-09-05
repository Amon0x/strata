#include "renderer_internal.hpp"
namespace strata::vulkan {
void Renderer::Impl::clear(Image& image, std::array<float, 4> color) {
    transition(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue value{};
    std::copy(color.begin(), color.end(), value.float32);
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(command, image.image, image.layout, &value, 1, &range);
}
Image& Renderer::Impl::temporary(std::uint32_t width, std::uint32_t height, VkFormat format) {
    if (scratch_index == scratch.size())
        scratch.push_back(nullptr);
    auto& image = scratch[scratch_index++];
    if (!image || image->width != width || image->height != height || image->format != format) {
        if (image)
            retired.push_back(std::move(image));
        image = std::make_unique<Image>(device, width, height, format);
    }
    return *image;
}

void Renderer::Impl::barrier(Target& target, VkImageLayout next) {
    transition(command, target.image, target.layout, next);
    target.layout = next;
}

Image& Renderer::Impl::capture(Target& source) {
    Image& copy = temporary(source.width, source.height, source.format);
    barrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(command, copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {source.width, source.height, 1};
    vkCmdCopyImage(command, source.image, source.layout, copy.image, copy.layout, 1, &region);
    transition(command, copy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return copy;
}

void Renderer::Impl::resources(const host::RenderPacket& packet) {
    for (const auto& operation : packet.resources) {
        const auto& id = operation.texture;
        if (operation.kind == host::resource_release) {
            if (auto it = textures.find(id); it != textures.end()) {
                retired.push_back(std::move(it->second));
                textures.erase(it);
            }
            continue;
        }
        if (operation.kind == host::resource_create ||
            operation.kind == host::resource_encoded_image) {
            auto image = std::make_unique<Image>(device, operation.width, operation.height,
                                                 operation.format == host::texture_format_r8
                                                     ? VK_FORMAT_R8_UNORM
                                                     : VK_FORMAT_R8G8B8A8_UNORM);
            image->filter = operation.sampling == host::texture_sampling_nearest ? VK_FILTER_NEAREST
                                                                                 : VK_FILTER_LINEAR;
            if (auto it = textures.find(id); it != textures.end())
                retired.push_back(std::move(it->second));
            textures[id] = std::move(image);
            clear(*textures.at(id));
        }
        if (operation.kind == host::resource_create)
            continue;
        if (operation.kind != host::resource_upload &&
            operation.kind != host::resource_encoded_image)
            throw std::invalid_argument("unknown Vulkan resource operation");
        auto it = textures.find(id);
        if (it == textures.end())
            throw std::invalid_argument("upload references missing texture: " + id);
        auto& image = *it->second;
        std::vector<std::uint8_t> decoded;
        std::span<const std::uint8_t> bytes = operation.bytes;
        if (operation.kind == host::resource_encoded_image) {
            decoded = gpu::decode_png(bytes, operation.width, operation.height);
            bytes = decoded;
        }
        const std::size_t channels = image.format == VK_FORMAT_R8_UNORM ? 1U : 4U;
        if (operation.x > image.width || operation.y > image.height ||
            operation.width > image.width - operation.x ||
            operation.height > image.height - operation.y ||
            bytes.size() != static_cast<std::size_t>(operation.width) * operation.height * channels)
            throw std::invalid_argument("invalid Vulkan texture upload range");
        const auto slice = arena.upload(bytes.data(), bytes.size());
        transition(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.bufferOffset = slice.offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset = {static_cast<std::int32_t>(operation.x),
                            static_cast<std::int32_t>(operation.y), 0};
        copy.imageExtent = {operation.width, operation.height, 1};
        vkCmdCopyBufferToImage(command, slice.buffer, image.image, image.layout, 1, &copy);
    }
}

std::pair<Slice, Slice> Renderer::Impl::upload_geometry(const host::RenderPacket& packet) {
    auto& layer = geometry[active_layer];
    const auto vertex_size = std::max<std::size_t>(16, packet.vertices.size());
    const auto index_size = std::max<std::size_t>(16, packet.indices.size() * 4);
    bool full = packet.full_geometry_payload || layer.epoch != packet.geometry_epoch;
    if (!layer.vertices || layer.vertices->size < vertex_size) {
        layer.vertices =
            std::make_unique<Buffer>(device, vertex_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        full = true;
    }
    if (!layer.indices || layer.indices->size < index_size) {
        layer.indices =
            std::make_unique<Buffer>(device, index_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        full = true;
    }
    if (full) {
        if (!packet.vertices.empty())
            std::memcpy(layer.vertices->mapped, packet.vertices.data(), packet.vertices.size());
        if (!packet.indices.empty())
            std::memcpy(layer.indices->mapped, packet.indices.data(), packet.indices.size() * 4);
    } else {
        const auto patch = [](Buffer& buffer, const auto& patches) {
            for (const auto& update : patches) {
                if (update.offset > buffer.size ||
                    update.bytes.size() > buffer.size - update.offset)
                    throw std::invalid_argument("Vulkan geometry patch is out of range");
                if (!update.bytes.empty())
                    std::memcpy(static_cast<char*>(buffer.mapped) + update.offset,
                                update.bytes.data(), update.bytes.size());
            }
        };
        patch(*layer.vertices, packet.vertex_patches);
        patch(*layer.indices, packet.index_patches);
    }
    layer.epoch = packet.geometry_epoch;
    return {{layer.vertices->buffer, 0, vertex_size}, {layer.indices->buffer, 0, index_size}};
}

} // namespace strata::vulkan
