#include "gpu/raster_shaders.hpp"
#include "renderer_internal.hpp"

#include <cstdio>
#include <functional>
#include <future>
#include <mutex>

#include <strata/raster_job.hpp>

namespace strata::vulkan {
namespace {

namespace shaders = gpu::raster_shaders;

constexpr std::uint32_t raster_bindings = 7U;
constexpr const auto& raster_entries = shaders::entries;

/** Orders everything the queue did before (kernels, copies) ahead of the next kernel. */
void compute_barrier(const VkCommandBuffer command) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0,
                         nullptr);
}

[[nodiscard]] std::uint32_t group_count(const std::uint32_t threads) noexcept {
    return (threads + shaders::group_size - 1U) / shaders::group_size;
}

using Kernels = std::array<std::vector<std::uint32_t>, raster_entries.size()>;

/** The kernels' SPIR-V, compiled once per process on a background thread; it needs no device. */
[[nodiscard]] std::shared_future<Kernels>& kernels() {
    static std::once_flag started;
    static std::shared_future<Kernels> result;
    std::call_once(started, [] {
        result = std::async(std::launch::async, [] {
                     Kernels code;
                     for (std::size_t index = 0U; index < raster_entries.size(); ++index) {
                         code[index] = compile_hlsl_compute(shaders::source, raster_entries[index],
                                                            "strata.raster");
                     }
                     return code;
                 }).share();
    });
    return result;
}

} // namespace

void Renderer::Impl::prepare_raster_kernels() {
    static_cast<void>(kernels());
}

void Renderer::Impl::create_raster_kernels() {
    std::array<VkDescriptorSetLayoutBinding, raster_bindings> bindings{};
    for (std::uint32_t index = 0U; index < raster_bindings; ++index) {
        bindings[index].binding = index;
        bindings[index].descriptorCount = 1;
        bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    di.bindingCount = raster_bindings;
    di.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(device.device, &di, nullptr, &raster_descriptors),
          "create raster descriptor layout");
    VkPushConstantRange constants{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(shaders::Constants)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &raster_descriptors;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &constants;
    check(vkCreatePipelineLayout(device.device, &li, nullptr, &raster_layout),
          "create raster pipeline layout");
    for (std::size_t index = 0U; index < raster_entries.size(); ++index) {
        const std::vector<std::uint32_t>& code = kernels().get()[index];
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = code.size() * sizeof(std::uint32_t);
        mi.pCode = code.data();
        VkShaderModule module{};
        check(vkCreateShaderModule(device.device, &mi, nullptr, &module),
              "create raster shader module");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = raster_entries[index];
        ci.layout = raster_layout;
        const VkResult result = vkCreateComputePipelines(device.device, pipeline_cache, 1, &ci,
                                                         nullptr, &raster_pipelines[index]);
        vkDestroyShaderModule(device.device, module, nullptr);
        check(result, "create raster pipeline");
    }
}

void Renderer::Impl::reserve_raster_buffer(std::unique_ptr<Buffer>& buffer,
                                           const VkDeviceSize bytes, const bool zeroed) {
    if (buffer && buffer->size >= bytes)
        return;
    const VkDeviceSize size =
        std::max<VkDeviceSize>({bytes, buffer ? buffer->size * 2U : 0U, 64U * 1024U});
    retire(std::move(buffer));
    buffer = std::make_unique<Buffer>(device, size,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      true);
    // The kernels leave every cell and sum they use at zero; a new buffer starts there.
    if (zeroed)
        vkCmdFillBuffer(command, buffer->buffer, 0, VK_WHOLE_SIZE, 0U);
}

Renderer::Impl::ArtProgram* Renderer::Impl::art_program(const std::string& source) {
    const std::size_t key = std::hash<std::string>{}(source);
    auto found = art_programs.find(key);
    if (found == art_programs.end()) {
        ArtProgram program;
        program.code = std::async(std::launch::async, [text = shaders::art_source(source)] {
                           std::array<std::vector<std::uint32_t>, 2U> code;
                           for (std::size_t index = 0U; index < code.size(); ++index) {
                               code[index] = compile_hlsl_compute(
                                   text, shaders::art_entries[index], "strata.art");
                           }
                           return code;
                       }).share();
        found = art_programs.emplace(key, std::move(program)).first;
    }
    ArtProgram& program = found->second;
    if (program.failed)
        return nullptr;
    if (program.stroke == VK_NULL_HANDLE) {
        if (program.code.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return nullptr;
        try {
            const auto& code = program.code.get();
            std::array<VkPipeline*, 2U> targets{&program.stroke, &program.fill};
            for (std::size_t index = 0U; index < code.size(); ++index) {
                VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                mi.codeSize = code[index].size() * sizeof(std::uint32_t);
                mi.pCode = code[index].data();
                VkShaderModule module{};
                check(vkCreateShaderModule(device.device, &mi, nullptr, &module),
                      "create art shader module");
                VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                ci.stage.module = module;
                ci.stage.pName = shaders::art_entries[index];
                ci.layout = raster_layout;
                const VkResult result = vkCreateComputePipelines(device.device, pipeline_cache, 1,
                                                                 &ci, nullptr, targets[index]);
                vkDestroyShaderModule(device.device, module, nullptr);
                check(result, "create art pipeline");
            }
        } catch (const std::exception& error) {
            // A program that does not compile draws nothing, rather than failing frames; the
            // author sees why once.
            std::fprintf(stderr, "strata: an art program failed to compile: %s\n", error.what());
            program.failed = true;
            return nullptr;
        }
    }
    return &program;
}

void Renderer::Impl::forget_live_art(const host::ResourceOperation& operation, const bool whole) {
    std::erase_if(live_art, [&](const LiveArt& live) {
        const host::ResourceOperation& other = live.operation;
        if (other.texture != operation.texture)
            return false;
        return whole || (operation.x < other.x + other.width && other.x < operation.x + operation.width &&
                         operation.y < other.y + other.height && other.y < operation.y + operation.height);
    });
}

void Renderer::Impl::run_live_art() {
    for (LiveArt& live : live_art) {
        if (live.seconds == seconds)
            continue;
        const auto found = textures.find(live.operation.texture);
        if (found == textures.end())
            continue;
        rasterize(live.operation, *found->second, seconds);
        live.seconds = seconds;
    }
}

void Renderer::Impl::rasterize(const host::ResourceOperation& operation, Image& image,
                               const double time) {
    const host::RasterJob& job = operation.raster;
    const bool rgba = operation.format == host::texture_format_rgba8;
    if ((image.format == VK_FORMAT_R8G8B8A8_UNORM) != rgba || operation.x > image.width ||
        operation.y > image.height || operation.width > image.width - operation.x ||
        operation.height > image.height - operation.y) {
        throw std::invalid_argument("invalid Vulkan raster job region");
    }
    if (!raster_layout)
        create_raster_kernels();

    std::vector<std::uint32_t> luts;
    std::vector<std::uint32_t> lut_offsets;
    for (const host::RasterLayer& layer : job.layers) {
        lut_offsets.push_back(static_cast<std::uint32_t>(luts.size()));
        luts.insert(luts.end(), layer.lut.begin(), layer.lut.end());
    }
    if (luts.empty())
        luts.push_back(0U);
    const Slice segments =
        arena->upload(job.segments.data(), job.segments.size() * sizeof(host::RasterSegment));
    const Slice tables = arena->upload(luts.data(), luts.size() * sizeof(std::uint32_t));
    const Slice strokes =
        arena->upload(job.pieces.data(), job.pieces.size() * sizeof(host::RasterStrokePiece));
    // Every art layer's values end to end; each layer finds its own at its base.
    std::vector<float> art_values;
    std::vector<std::uint32_t> art_bases(job.layers.size(), 0U);
    for (std::size_t index = 0U; index < job.layers.size(); ++index) {
        const std::optional<host::RasterArt>& art = job.layers[index].art;
        if (!art.has_value())
            continue;
        art_bases[index] = static_cast<std::uint32_t>(art_values.size());
        const std::array<float, 8U> head{static_cast<float>(time), static_cast<float>(art->count),
                                         art->closed ? 1.0F : 0.0F, art->x, art->y, art->width,
                                         art->height, 0.0F};
        art_values.insert(art_values.end(), head.begin(), head.end());
        art_values.insert(art_values.end(), art->parameters.begin(), art->parameters.end());
    }
    if (art_values.empty())
        art_values.resize(shaders::art_value_count, 0.0F);
    const Slice arts = arena->upload(art_values.data(), art_values.size() * sizeof(float));
    const std::size_t pixels = static_cast<std::size_t>(job.width) * job.height;
    // R8 rows are packed four texels a uint; RGBA rows one texel a uint.
    const std::uint32_t stride = rgba ? operation.width : (operation.width + 3U) / 4U;
    reserve_raster_buffer(raster_cells, pixels * 2U * sizeof(std::int32_t), true);
    reserve_raster_buffer(raster_sums, (rgba ? pixels * 5U : 1U) * sizeof(std::uint32_t), true);
    reserve_raster_buffer(raster_output,
                          static_cast<VkDeviceSize>(stride) * operation.height * 4U, false);

    auto& pools = frame().raster_pools;
    auto& pool_index = frame().raster_pool_index;
    VkDescriptorSet set{};
    for (;;) {
        if (pool_index == pools.size()) {
            const VkDescriptorPoolSize sizes{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                             256U * raster_bindings};
            VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            info.maxSets = 256;
            info.poolSizeCount = 1;
            info.pPoolSizes = &sizes;
            VkDescriptorPool pool{};
            check(vkCreateDescriptorPool(device.device, &info, nullptr, &pool),
                  "create raster descriptor pool");
            pools.push_back(pool);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pools[pool_index];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &raster_descriptors;
        const VkResult result = vkAllocateDescriptorSets(device.device, &ai, &set);
        if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL) {
            ++pool_index;
            continue;
        }
        check(result, "allocate raster descriptor set");
        break;
    }
    const std::array<VkDescriptorBufferInfo, raster_bindings> buffers{{
        {segments.buffer, segments.offset, segments.size},
        {tables.buffer, tables.offset, tables.size},
        {raster_cells->buffer, 0, VK_WHOLE_SIZE},
        {raster_sums->buffer, 0, VK_WHOLE_SIZE},
        {raster_output->buffer, 0, VK_WHOLE_SIZE},
        {strokes.buffer, strokes.offset, strokes.size},
        {arts.buffer, arts.offset, arts.size},
    }};
    std::array<VkWriteDescriptorSet, raster_bindings> writes{};
    for (std::uint32_t index = 0U; index < raster_bindings; ++index) {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = set;
        writes[index].dstBinding = index;
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[index].pBufferInfo = &buffers[index];
    }
    vkUpdateDescriptorSets(device.device, raster_bindings, writes.data(), 0, nullptr);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, raster_layout, 0, 1, &set, 0,
                            nullptr);

    shaders::Constants values;
    const auto dispatch_with = [&](const VkPipeline pipeline, const std::uint32_t threads) {
        if (threads == 0U)
            return;
        compute_barrier(command);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdPushConstants(command, raster_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(values), &values);
        vkCmdDispatch(command, group_count(threads), 1, 1);
    };
    const auto dispatch = [&](const std::size_t kernel, const std::uint32_t threads) {
        dispatch_with(raster_pipelines[kernel], threads);
    };
    for (std::size_t index = 0U; index < job.layers.size(); ++index) {
        const host::RasterLayer& layer = job.layers[index];
        values = shaders::Constants{};
        values.content_x = job.x;
        values.content_y = job.y;
        values.content_width = job.width;
        values.content_height = job.height;
        values.region_width = operation.width;
        values.region_height = operation.height;
        values.first_segment = layer.first_segment;
        values.segment_count = layer.segment_count;
        values.fill = static_cast<std::uint32_t>(layer.fill);
        values.paint = static_cast<std::uint32_t>(layer.paint);
        values.color = layer.color;
        values.extend = static_cast<std::uint32_t>(layer.extend);
        values.lut_offset = lut_offsets[index];
        values.output_stride = stride;
        std::copy(layer.parameters.begin(), layer.parameters.end(), values.parameters);
        values.first_piece = layer.first_piece;
        values.piece_count = layer.piece_count;
        values.join = static_cast<std::uint32_t>(layer.stroke.join);
        values.cap = static_cast<std::uint32_t>(layer.stroke.cap);
        values.half_width = layer.stroke.half_width;
        values.miter_limit = layer.stroke.miter_limit;
        values.tolerance = layer.stroke.tolerance;
        dispatch(0U, layer.segment_count);
        dispatch(1U, layer.piece_count);
        if (layer.art.has_value()) {
            if (ArtProgram* program = art_program(layer.art->source); program != nullptr) {
                values.art_base = art_bases[index];
                const std::uint32_t count = layer.art->count;
                if (layer.art->stroked)
                    dispatch_with(program->stroke, layer.art->closed ? count : count - 1U);
                else
                    dispatch_with(program->fill, count);
            }
        }
        // A group of threads a row.
        dispatch(rgba ? 3U : 2U, (rgba ? job.height : operation.height) * shaders::group_size);
    }
    if (rgba)
        dispatch(4U, operation.width * operation.height);

    // The region's texels into the atlas.
    VkMemoryBarrier written{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    written.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    written.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &written, 0, nullptr, 0, nullptr);
    transition(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = rgba ? stride : stride * 4U;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = {static_cast<std::int32_t>(operation.x),
                        static_cast<std::int32_t>(operation.y), 0};
    copy.imageExtent = {operation.width, operation.height, 1};
    vkCmdCopyBufferToImage(command, raster_output->buffer, image.image, image.layout, 1, &copy);
}

} // namespace strata::vulkan
