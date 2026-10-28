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
    for (auto& entry : scratch) {
        if (entry.used != scratch_frame && entry.image->width == width &&
            entry.image->height == height && entry.image->format == format) {
            entry.used = scratch_frame;
            return *entry.image;
        }
    }
    scratch.push_back({std::make_unique<Image>(device, width, height, format), scratch_frame});
    return *scratch.back().image;
}

void Renderer::Impl::barrier(Target& target, VkImageLayout next) {
    end_pass();
    transition(command, target.image, target.layout, next);
    target.layout = next;
}

void Renderer::Impl::copy(Target& source, Image& destination, host::Scissor region,
                          std::int32_t destination_x, std::int32_t destination_y) {
    if (!region.width || !region.height)
        return;
    barrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(command, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copied{};
    copied.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copied.dstSubresource = copied.srcSubresource;
    copied.srcOffset = {static_cast<std::int32_t>(region.x), static_cast<std::int32_t>(region.y),
                        0};
    copied.dstOffset = {destination_x, destination_y, 0};
    copied.extent = {region.width, region.height, 1};
    vkCmdCopyImage(command, source.image, source.layout, destination.image, destination.layout, 1,
                   &copied);
    telemetry.copied_pixels += static_cast<std::uint64_t>(region.width) * region.height;
}

Image& Renderer::Impl::capture(Target& source, host::Scissor region) {
    Image& captured = temporary(source.width, source.height, source.format);
    copy(source, captured, region, static_cast<std::int32_t>(region.x),
         static_cast<std::int32_t>(region.y));
    transition(command, captured, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return captured;
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
            // An encoded image's format field is its encoding; its texels are always RGBA8.
            const bool r8 = operation.kind == host::resource_create &&
                            operation.format == host::texture_format_r8;
            auto image = std::make_unique<Image>(device, operation.width, operation.height,
                                                 r8 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM);
            // Only encoded images carry a sampling mode. A created atlas is sampled as the other
            // renderers sample it: coverage (R8) texel-exact, MSDF (RGBA) bilinear, which its
            // distance reconstruction depends on.
            image->filter = operation.kind == host::resource_create
                                ? (r8 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR)
                            : operation.sampling == host::texture_sampling_nearest
                                ? VK_FILTER_NEAREST
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
        if (operation.kind == host::resource_encoded_image &&
            operation.format == host::texture_encoding_png) {
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
    // A retained packet repeats the epoch and changes nothing. A patch packet is the decoder's step
    // from the previous epoch (it rejects any other), so it applies only on top of exactly that
    // epoch; after a skipped epoch (a frame that failed after the decoder applied it) the copy is
    // older than the patches assume and is replaced from the decoder's complete geometry, as D3D11
    // does.
    const bool same_size = layer.seen && !packet.full_geometry_payload &&
                           packet.vertices.size() == layer.vertices.size() &&
                           index_byte_count == layer.indices.size();
    const bool retained = same_size && layer.epoch == packet.geometry_epoch;
    const bool incremental = same_size && layer.epoch + 1U == packet.geometry_epoch;
    if (!retained && !incremental) {
        layer.vertices.assign(packet.vertices.begin(), packet.vertices.end());
        layer.indices.assign(index_bytes, index_bytes + index_byte_count);
        ++layer.generation;
        layer.seen = true;
    } else if (incremental && (!packet.vertex_patches.empty() || !packet.index_patches.empty())) {
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
