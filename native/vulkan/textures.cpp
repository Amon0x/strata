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

void Renderer::Impl::recycle(Image& image) {
    for (auto& entry : scratch) {
        if (entry.image.get() == &image && entry.used == scratch_frame)
            entry.used = scratch_frame - 1U;
    }
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

bool Renderer::Impl::append_vector_glyphs(const host::ResourceOperation& operation) {
    VectorGlyphs& glyphs = vector_glyphs[operation.texture];
    const host::VectorGlyphBuffers& append = operation.vector_glyphs;
    const std::array<std::pair<std::uint32_t, const std::vector<std::uint8_t>*>, 3> parts{{
        {append.first_record, &append.records},
        {append.first_segment, &append.segments},
        {append.first_band, &append.bands},
    }};
    constexpr std::array<std::size_t, 3> strides{host::vector_glyph_record_bytes,
                                                 host::vector_glyph_segment_bytes,
                                                 host::vector_glyph_band_bytes};
    bool copied = false;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto [first, bytes] = parts[index];
        std::vector<std::uint8_t>& held = glyphs.bytes[index];
        std::unique_ptr<Buffer>& buffer = glyphs.buffers[index];
        const std::size_t offset = static_cast<std::size_t>(first) * strides[index];
        if (offset > held.size())
            throw std::invalid_argument("vector glyph append leaves a gap in its buffer");
        // Rewriting what a frame in flight may read takes a new buffer; appending does not.
        const bool rewrites = offset < held.size();
        held.resize(offset + bytes->size());
        std::copy(bytes->begin(), bytes->end(),
                  held.begin() + static_cast<std::ptrdiff_t>(offset));
        if (buffer != nullptr && !rewrites && buffer->size >= held.size()) {
            if (!bytes->empty()) {
                const Slice slice = arena->upload(bytes->data(), bytes->size());
                const VkBufferCopy region{slice.offset, offset, bytes->size()};
                vkCmdCopyBuffer(command, slice.buffer, buffer->buffer, 1, &region);
                copied = true;
            }
            continue;
        }
        VkDeviceSize size = std::max<VkDeviceSize>(buffer != nullptr ? buffer->size : 0U, 4096U);
        while (size < held.size())
            size *= 2U;
        retire(std::move(buffer));
        buffer = std::make_unique<Buffer>(device, size,
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          true);
        if (!held.empty()) {
            const Slice slice = arena->upload(held.data(), held.size());
            const VkBufferCopy region{slice.offset, 0, held.size()};
            vkCmdCopyBuffer(command, slice.buffer, buffer->buffer, 1, &region);
            copied = true;
        }
    }
    return copied;
}

void Renderer::Impl::resources(const host::RenderPacket& packet) {
    end_pass();
    bool vectors_copied = false;
    for (const auto& operation : packet.resources) {
        const auto& id = operation.texture;
        if (operation.kind == host::resource_vector_glyphs) {
            vectors_copied = append_vector_glyphs(operation) || vectors_copied;
            continue;
        }
        forget_live_art(operation, operation.kind != host::resource_upload &&
                                       operation.kind != host::resource_raster);
        if (operation.kind == host::resource_release) {
            if (auto it = textures.find(id); it != textures.end()) {
                retire(std::move(it->second));
                textures.erase(it);
            }
            if (auto it = vector_glyphs.find(id); it != vector_glyphs.end()) {
                for (std::unique_ptr<Buffer>& buffer : it->second.buffers)
                    retire(std::move(buffer));
                vector_glyphs.erase(it);
            }
            continue;
        }
        if (operation.kind == host::resource_create || operation.kind == host::resource_image) {
            // An image's texels are always RGBA8, decoded by the core.
            const bool r8 = operation.kind == host::resource_create &&
                            !host::texture_format_rgba(operation.format);
            auto image = std::make_unique<Image>(device, operation.width, operation.height,
                                                 r8 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM);
            // Only encoded images carry a sampling mode. A created atlas is sampled as the other
            // renderers sample it: glyph coverage (R8, and subpixel RGBA) texel-exact, shape masks
            // (filtered R8) and MSDF (RGBA) bilinear, which its distance reconstruction depends on.
            image->filter = operation.kind == host::resource_create
                                ? (host::texture_format_exact(operation.format) ? VK_FILTER_NEAREST
                                                                                : VK_FILTER_LINEAR)
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
        if (operation.kind == host::resource_raster) {
            auto it = textures.find(id);
            if (it == textures.end())
                throw std::invalid_argument("raster job references missing texture: " + id);
            // Art draws every frame, with the frame's time: it runs when the frame renders.
            if (host::raster_job_has_art(operation.raster)) {
                live_art.push_back(LiveArt{operation, std::numeric_limits<double>::quiet_NaN()});
                continue;
            }
            rasterize(operation, *it->second);
            continue;
        }
        if (operation.kind != host::resource_upload && operation.kind != host::resource_image)
            throw std::invalid_argument("unknown Vulkan resource operation");
        auto it = textures.find(id);
        if (it == textures.end())
            throw std::invalid_argument("upload references missing texture: " + id);
        auto& image = *it->second;
        const std::span<const std::uint8_t> bytes = operation.bytes;
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
    if (vectors_copied) {
        // The glyphs copied in are read by the frame's pixel stages.
        VkMemoryBarrier written{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        written.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        written.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &written, 0, nullptr, 0,
                             nullptr);
    }
}

std::pair<Slice, Slice> Renderer::Impl::upload_geometry(const host::RenderPacket& packet) {
    auto& layer = geometry[active_layer];
    // The decoder's arrays are the complete current geometry, patches already applied, so a frame
    // slot that is behind copies straight from them; no host mirror is kept or patched. A retained
    // packet repeats the epoch and changes nothing; a complete payload always replaces.
    if (!layer.seen || layer.epoch != packet.geometry_epoch || packet.full_geometry_payload) {
        ++layer.generation;
        layer.seen = true;
    }
    layer.epoch = packet.geometry_epoch;
    const std::span<const std::uint8_t> vertices = packet.vertices;
    const std::span<const std::uint8_t> indices{
        reinterpret_cast<const std::uint8_t*>(packet.indices.data()), packet.indices.size() * 4};
    const auto vertex_size = std::max<std::size_t>(16, vertices.size());
    const auto index_size = std::max<std::size_t>(16, indices.size());
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
        if (!vertices.empty())
            std::memcpy(slot.vertices->mapped, vertices.data(), vertices.size());
        if (!indices.empty())
            std::memcpy(slot.indices->mapped, indices.data(), indices.size());
        slot.generation = layer.generation;
    }
    return {{slot.vertices->buffer, 0, vertex_size}, {slot.indices->buffer, 0, index_size}};
}

} // namespace strata::vulkan
