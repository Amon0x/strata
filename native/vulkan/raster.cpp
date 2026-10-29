#include "gpu/raster_shaders.hpp"
#include "renderer_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <strata/raster_job.hpp>

namespace strata::vulkan {
namespace {

namespace shaders = gpu::raster_shaders;

constexpr std::uint32_t raster_bindings = 10U;
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
    VkPushConstantRange constants{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(shaders::Dispatch)};
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

// Art compiles without the SPIR-V optimizer: it is compiled the first time a page draws it, and
// the driver optimizes the kernel itself, so it appears sooner and runs as fast.
VkPipeline Renderer::Impl::art_kernel(const std::string& source) {
    ArtProgram& kernel = art_programs[std::hash<std::string>{}(source)];
    if (kernel.failed)
        return VK_NULL_HANDLE;
    if (kernel.pipeline != VK_NULL_HANDLE)
        return kernel.pipeline;
    if (!kernel.code.valid()) {
        kernel.code = std::async(std::launch::async, [text = shaders::art_source(source),
                                                      entry = shaders::art_entry] {
                          return compile_hlsl_compute(text, entry, "strata.art", false);
                      }).share();
    }
    if (kernel.code.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return VK_NULL_HANDLE;
    try {
        const std::vector<std::uint32_t>& code = kernel.code.get();
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = code.size() * sizeof(std::uint32_t);
        mi.pCode = code.data();
        VkShaderModule module{};
        check(vkCreateShaderModule(device.device, &mi, nullptr, &module),
              "create art shader module");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = shaders::art_entry;
        ci.layout = raster_layout;
        const VkResult result = vkCreateComputePipelines(device.device, pipeline_cache, 1, &ci,
                                                         nullptr, &kernel.pipeline);
        vkDestroyShaderModule(device.device, module, nullptr);
        check(result, "create art pipeline");
    } catch (const std::exception& error) {
        // A program that does not compile draws nothing, rather than failing frames; the author
        // sees why once.
        std::fprintf(stderr, "strata: an art program failed to compile: %s\n", error.what());
        kernel.failed = true;
        return VK_NULL_HANDLE;
    }
    return kernel.pipeline;
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
    // Every live job of the frame in one batch.
    std::vector<RasterWork> batch;
    for (LiveArt& live : live_art) {
        if (live.seconds == seconds)
            continue;
        const auto found = textures.find(live.operation.texture);
        if (found == textures.end())
            continue;
        batch.push_back(RasterWork{&live.operation, found->second.get()});
        live.seconds = seconds;
    }
    rasterize(batch, seconds);
}

void Renderer::Impl::rasterize(const host::ResourceOperation& operation, Image& image,
                               const double time) {
    const RasterWork one{&operation, &image};
    rasterize(std::span<const RasterWork>(&one, 1U), time);
}

void Renderer::Impl::rasterize(const std::span<const RasterWork> batch, const double time) {
    if (batch.empty())
        return;
    for (const RasterWork& work : batch) {
        const host::ResourceOperation& operation = *work.operation;
        const bool rgba = operation.format == host::texture_format_rgba8;
        if ((work.image->format == VK_FORMAT_R8G8B8A8_UNORM) != rgba ||
            operation.x > work.image->width || operation.y > work.image->height ||
            operation.width > work.image->width - operation.x ||
            operation.height > work.image->height - operation.y) {
            throw std::invalid_argument("invalid Vulkan raster job region");
        }
    }
    if (!raster_layout)
        create_raster_kernels();

    // Every job's segments, pieces, colour tables, art values and output end to end, and a
    // table entry for each layer saying where its own begin.
    std::vector<host::RasterSegment> segment_data;
    std::vector<host::RasterStrokePiece> piece_data;
    std::vector<std::uint32_t> luts;
    std::vector<float> art_values;
    std::vector<shaders::Constants> entries;
    std::vector<std::size_t> entry_jobs;
    std::vector<std::size_t> entry_layers;
    std::vector<bool> entry_shares;
    std::vector<std::pair<const host::RasterArt*, std::uint32_t>> art_sources;
    std::vector<std::uint32_t> strides(batch.size(), 0U);
    std::vector<std::uint32_t> output_bases(batch.size(), 0U);
    std::size_t cell_total = 0U;
    std::size_t sum_total = 0U;
    std::size_t point_total = 0U;
    std::size_t output_total = 0U;
    std::size_t most_layers = 0U;
    for (std::size_t index = 0U; index < batch.size(); ++index) {
        const host::ResourceOperation& operation = *batch[index].operation;
        const host::RasterJob& job = operation.raster;
        const bool rgba = operation.format == host::texture_format_rgba8;
        const auto segment_base = static_cast<std::uint32_t>(segment_data.size());
        segment_data.insert(segment_data.end(), job.segments.begin(), job.segments.end());
        const auto piece_base = static_cast<std::uint32_t>(piece_data.size());
        piece_data.insert(piece_data.end(), job.pieces.begin(), job.pieces.end());
        const std::size_t pixels = static_cast<std::size_t>(job.width) * job.height;
        const auto sum_base = static_cast<std::uint32_t>(sum_total);
        if (rgba)
            sum_total += pixels * 5U;
        // R8 rows are packed four texels a uint; RGBA rows one texel a uint.
        strides[index] = rgba ? operation.width : (operation.width + 3U) / 4U;
        output_bases[index] = static_cast<std::uint32_t>(output_total);
        output_total += static_cast<std::size_t>(strides[index]) * operation.height;
        most_layers = std::max(most_layers, job.layers.size());
        for (std::size_t layer_index = 0U; layer_index < job.layers.size(); ++layer_index) {
            const host::RasterLayer& layer = job.layers[layer_index];
            shaders::Constants values;
            bool sharing = false;
            values.content_x = job.x;
            values.content_y = job.y;
            values.content_width = job.width;
            values.content_height = job.height;
            values.region_width = operation.width;
            values.region_height = operation.height;
            values.first_segment = segment_base + layer.first_segment;
            values.segment_count = layer.segment_count;
            values.fill = static_cast<std::uint32_t>(layer.fill);
            values.paint = static_cast<std::uint32_t>(layer.paint);
            values.color = layer.color;
            values.extend = static_cast<std::uint32_t>(layer.extend);
            values.lut_offset = static_cast<std::uint32_t>(luts.size());
            luts.insert(luts.end(), layer.lut.begin(), layer.lut.end());
            values.output_stride = strides[index];
            std::copy(layer.parameters.begin(), layer.parameters.end(), values.parameters);
            values.first_piece = piece_base + layer.first_piece;
            values.piece_count = layer.piece_count;
            values.join = static_cast<std::uint32_t>(layer.stroke.join);
            values.cap = static_cast<std::uint32_t>(layer.stroke.cap);
            values.half_width = layer.stroke.half_width;
            values.miter_limit = layer.stroke.miter_limit;
            values.tolerance = layer.stroke.tolerance;
            if (const std::optional<host::RasterArt>& art = layer.art; art.has_value()) {
                values.art_base = static_cast<std::uint32_t>(art_values.size());
                const std::array<float, 8U> head{static_cast<float>(time),
                                                 static_cast<float>(art->count),
                                                 art->closed ? 1.0F : 0.0F,
                                                 art->x,
                                                 art->y,
                                                 art->width,
                                                 art->height,
                                                 0.0F};
                art_values.insert(art_values.end(), head.begin(), head.end());
                art_values.insert(art_values.end(), art->parameters.begin(), art->parameters.end());
                // A glow and its line run one program on one canvas: their points are the same,
                // so the second shares the first's rather than running it again.
                const auto same =
                    std::find_if(art_sources.begin(), art_sources.end(), [&](const auto& source) {
                        const host::RasterArt& other = *source.first;
                        return other.count == art->count && other.closed == art->closed &&
                               other.x == art->x && other.y == art->y &&
                               other.width == art->width && other.height == art->height &&
                               other.parameters == art->parameters && other.source == art->source;
                    });
                if (same != art_sources.end()) {
                    values.points_base = same->second;
                    sharing = true;
                } else {
                    values.points_base = static_cast<std::uint32_t>(point_total);
                    point_total += art->count;
                    art_sources.emplace_back(&*art, values.points_base);
                }
            }
            values.cell_base = static_cast<std::uint32_t>(cell_total);
            cell_total += pixels * 2U;
            values.sum_base = sum_base;
            values.output_base = output_bases[index];
            entries.push_back(values);
            entry_jobs.push_back(index);
            entry_layers.push_back(layer_index);
            entry_shares.push_back(sharing);
        }
    }
    if (entries.empty())
        return;

    // The dispatches, in the order their work depends on: every layer's edges into its own
    // cells, then each round of layers resolved (a colour job composites its layers in order),
    // then colour jobs finished. Each is a pipeline, its items in the list, and its size.
    struct Planned final {
        VkPipeline pipeline = VK_NULL_HANDLE;
        shaders::Dispatch values;
        std::uint32_t groups = 0U;
    };
    std::vector<Planned> planned;
    std::vector<shaders::BatchItem> items;
    const auto plan = [&](const VkPipeline pipeline, const bool by_group,
                          const std::vector<std::pair<std::uint32_t, std::uint32_t>>& work) {
        const std::uint32_t capacity = by_group ? 65535U : 65535U * shaders::group_size;
        Planned current;
        const auto close = [&] {
            if (current.values.total == 0U)
                return;
            current.groups = by_group ? current.values.total : group_count(current.values.total);
            planned.push_back(current);
        };
        const auto open = [&] {
            current = Planned{pipeline, {}, 0U};
            current.values.first_item = static_cast<std::uint32_t>(items.size());
        };
        open();
        for (const auto& [entry, count] : work) {
            std::uint32_t done = 0U;
            while (done < count) {
                if (current.values.total == capacity) {
                    close();
                    open();
                }
                const std::uint32_t take = std::min(count - done, capacity - current.values.total);
                items.push_back(shaders::BatchItem{entry, current.values.total, done, 0U});
                ++current.values.item_count;
                current.values.total += take;
                done += take;
            }
        }
        close();
    };
    const auto entry_count = static_cast<std::uint32_t>(entries.size());
    const auto art_of = [&](const std::uint32_t entry) -> const std::optional<host::RasterArt>& {
        return batch[entry_jobs[entry]].operation->raster.layers[entry_layers[entry]].art;
    };
    const auto rgba_job = [&](const std::size_t index) {
        return batch[index].operation->format == host::texture_format_rgba8;
    };
    std::vector<std::pair<std::uint32_t, std::uint32_t>> work;
    const auto gather = [&](const auto& threads) {
        work.clear();
        for (std::uint32_t entry = 0U; entry < entry_count; ++entry) {
            if (const std::uint32_t count = threads(entry); count > 0U)
                work.emplace_back(entry, count);
        }
    };
    gather([&](const std::uint32_t entry) { return entries[entry].segment_count; });
    plan(raster_pipelines[0], false, work);
    gather([&](const std::uint32_t entry) {
        return entries[entry].piece_count * shaders::stroke_parts;
    });
    plan(raster_pipelines[1], false, work);
    // An art program writes its layer's points; shared kernels outline every layer's.
    std::vector<bool> drawn(entries.size(), false);
    for (std::uint32_t entry = 0U; entry < entry_count; ++entry) {
        const std::optional<host::RasterArt>& art = art_of(entry);
        if (!art.has_value() || art->count == 0U)
            continue;
        if (const VkPipeline kernel = art_kernel(art->source); kernel != VK_NULL_HANDLE) {
            if (!entry_shares[entry])
                plan(kernel, false, {{entry, art->count}});
            drawn[entry] = true;
        }
    }
    for (const bool stroked : {true, false}) {
        gather([&](const std::uint32_t entry) -> std::uint32_t {
            const std::optional<host::RasterArt>& art = art_of(entry);
            if (!drawn[entry] || art->stroked != stroked)
                return 0U;
            if (!stroked)
                return art->count;
            return (art->closed ? art->count : art->count - 1U) * shaders::stroke_parts;
        });
        plan(raster_pipelines[stroked ? 5U : 6U], false, work);
    }
    for (std::size_t round = 0U; round < most_layers; ++round) {
        for (const bool rgba : {false, true}) {
            gather([&](const std::uint32_t entry) -> std::uint32_t {
                const std::size_t index = entry_jobs[entry];
                if (rgba_job(index) != rgba || entry_layers[entry] != round)
                    return 0U;
                return rgba ? batch[index].operation->raster.height
                            : batch[index].operation->height;
            });
            plan(raster_pipelines[rgba ? 3U : 2U], true, work);
        }
    }
    // A job's last layer stands for it: they share its region and sums.
    gather([&](const std::uint32_t entry) -> std::uint32_t {
        const std::size_t index = entry_jobs[entry];
        const host::ResourceOperation& operation = *batch[index].operation;
        return rgba_job(index) && entry_layers[entry] + 1U == operation.raster.layers.size()
                   ? operation.width * operation.height
                   : 0U;
    });
    plan(raster_pipelines[4], false, work);

    if (luts.empty())
        luts.push_back(0U);
    if (art_values.empty())
        art_values.resize(shaders::art_value_count, 0.0F);
    const Slice segments =
        arena->upload(segment_data.data(), segment_data.size() * sizeof(host::RasterSegment));
    const Slice tables = arena->upload(luts.data(), luts.size() * sizeof(std::uint32_t));
    const Slice strokes =
        arena->upload(piece_data.data(), piece_data.size() * sizeof(host::RasterStrokePiece));
    const Slice arts = arena->upload(art_values.data(), art_values.size() * sizeof(float));
    const Slice layers = arena->upload(entries.data(), entries.size() * sizeof(shaders::Constants));
    const Slice list = arena->upload(items.data(), items.size() * sizeof(shaders::BatchItem));
    reserve_raster_buffer(raster_cells,
                          std::max<std::size_t>(cell_total, 1U) * sizeof(std::int32_t), true);
    reserve_raster_buffer(raster_sums, std::max<std::size_t>(sum_total, 1U) * sizeof(std::uint32_t),
                          true);
    reserve_raster_buffer(raster_output,
                          std::max<std::size_t>(output_total, 1U) * sizeof(std::uint32_t), false);
    reserve_raster_buffer(raster_points,
                          std::max<std::size_t>(point_total, 1U) * 2U * sizeof(float), false);

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
        {raster_points->buffer, 0, VK_WHOLE_SIZE},
        {layers.buffer, layers.offset, layers.size},
        {list.buffer, list.offset, list.size},
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
    for (const Planned& dispatch : planned) {
        compute_barrier(command);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, dispatch.pipeline);
        vkCmdPushConstants(command, raster_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(dispatch.values), &dispatch.values);
        vkCmdDispatch(command, dispatch.groups, 1, 1);
    }

    // Each region's texels into its atlas.
    VkMemoryBarrier written{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    written.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    written.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &written, 0, nullptr, 0, nullptr);
    for (std::size_t index = 0U; index < batch.size(); ++index) {
        const host::ResourceOperation& operation = *batch[index].operation;
        if (operation.raster.layers.empty())
            continue;
        Image& image = *batch[index].image;
        transition(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.bufferOffset = VkDeviceSize{output_bases[index]} * sizeof(std::uint32_t);
        copy.bufferRowLength = rgba_job(index) ? strides[index] : strides[index] * 4U;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset = {static_cast<std::int32_t>(operation.x),
                            static_cast<std::int32_t>(operation.y), 0};
        copy.imageExtent = {operation.width, operation.height, 1};
        vkCmdCopyBufferToImage(command, raster_output->buffer, image.image, image.layout, 1, &copy);
    }
}

} // namespace strata::vulkan
