#include "renderer_internal.hpp"
namespace strata::vulkan {
void Renderer::Impl::clear(Image& image, std::array<float, 4> color) {
    end_pass();
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
            retire(std::move(image));
        image = std::make_unique<Image>(device, width, height, format);
    }
    return *image;
}

void Renderer::Impl::barrier(Target& target, VkImageLayout next) {
    end_pass();
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
    end_pass();
    for (const auto& operation : packet.resources) {
        const auto& id = operation.texture;
        if (operation.kind == host::resource_release) {
            if (auto it = textures.find(id); it != textures.end()) {
                retire(std::move(it->second));
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
                retire(std::move(it->second));
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
        const auto slice = arena->upload(bytes.data(), bytes.size());
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
    const auto* index_bytes = reinterpret_cast<const std::uint8_t*>(packet.indices.data());
    const std::size_t index_byte_count = packet.indices.size() * 4;
    if (!layer.seen || packet.full_geometry_payload || layer.epoch != packet.geometry_epoch ||
        packet.vertices.size() > layer.vertices.size() || index_byte_count > layer.indices.size()) {
        layer.vertices.assign(packet.vertices.begin(), packet.vertices.end());
        layer.indices.assign(index_bytes, index_bytes + index_byte_count);
        ++layer.generation;
        layer.seen = true;
    } else if (!packet.vertex_patches.empty() || !packet.index_patches.empty()) {
        const auto patch = [](std::vector<std::uint8_t>& bytes, const auto& patches) {
            for (const auto& update : patches) {
                if (update.offset > bytes.size() ||
                    update.bytes.size() > bytes.size() - update.offset)
                    throw std::invalid_argument("Vulkan geometry patch is out of range");
                std::copy(update.bytes.begin(), update.bytes.end(),
                          bytes.begin() + static_cast<std::ptrdiff_t>(update.offset));
            }
        };
        patch(layer.vertices, packet.vertex_patches);
        patch(layer.indices, packet.index_patches);
        ++layer.generation;
    }
    layer.epoch = packet.geometry_epoch;
    const auto vertex_size = std::max<std::size_t>(16, layer.vertices.size());
    const auto index_size = std::max<std::size_t>(16, layer.indices.size());
    auto& slot = layer.slots[frame_index];
    bool stale = slot.generation != layer.generation;
    if (!slot.vertices || slot.vertices->size < vertex_size) {
        retire(std::move(slot.vertices));
        slot.vertices =
            std::make_unique<Buffer>(device, vertex_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        stale = true;
    }
    if (!slot.indices || slot.indices->size < index_size) {
        retire(std::move(slot.indices));
        slot.indices =
            std::make_unique<Buffer>(device, index_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        stale = true;
    }
    if (stale) {
        // This slot's previous frame has completed (begin waited for it), so the host may write.
        if (!layer.vertices.empty())
            std::memcpy(slot.vertices->mapped, layer.vertices.data(), layer.vertices.size());
        if (!layer.indices.empty())
            std::memcpy(slot.indices->mapped, layer.indices.data(), layer.indices.size());
        slot.generation = layer.generation;
    }
    return {{slot.vertices->buffer, 0, vertex_size}, {slot.indices->buffer, 0, index_size}};
}

} // namespace strata::vulkan
