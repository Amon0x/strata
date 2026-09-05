#include "renderer_internal.hpp"

namespace strata::vulkan {

VkRenderPass Renderer::Impl::render_pass(VkFormat format) {
    if (auto it = render_passes.find(format); it != render_passes.end())
        return it->second;
    VkAttachmentDescription color{};
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    VkRenderPass result{};
    check(vkCreateRenderPass(device.device, &info, nullptr, &result), "create render pass");
    render_passes.emplace(format, result);
    return result;
}

VkPipeline Renderer::Impl::pipeline(const std::string& program, const std::string& blend,
                                    VkFormat format, bool full) {
    auto key = std::tuple{program, blend, format, full};
    if (auto it = pipelines.find(key); it != pipelines.end())
        return it->second;
    const auto& vs = full ? fullscreen : vertex;
    const auto& ps = programs.at(program);
    VkShaderModule modules[2]{};
    struct Modules {
        VkDevice device;
        VkShaderModule* values;
        ~Modules() {
            for (std::size_t i = 0; i < 2; ++i)
                if (values[i])
                    vkDestroyShaderModule(device, values[i], nullptr);
        }
    } cleanup{device.device, modules};
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& words = i == 0 ? vs : ps;
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = words.size() * 4;
        mi.pCode = words.data();
        check(vkCreateShaderModule(device.device, &mi, nullptr, &modules[i]),
              "create shader module");
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (std::size_t i = 0; i < 2; ++i) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[i].module = modules[i];
        stages[i].pName = "main";
    }
    VkVertexInputBindingDescription binding{0, 88, VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription, 7> attributes{};
    const std::array<std::uint32_t, 7> offsets{0, 12, 20, 24, 40, 56, 72};
    for (std::uint32_t i = 0; i < 7; ++i)
        attributes[i] = {i, 0,
                         i == 0   ? VK_FORMAT_R32G32B32_SFLOAT
                         : i == 1 ? VK_FORMAT_R32G32_SFLOAT
                         : i == 2 ? VK_FORMAT_R8G8B8A8_UNORM
                                  : VK_FORMAT_R32G32B32A32_SFLOAT,
                         offsets[i]};
    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (!full) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 7;
        vi.pVertexAttributeDescriptions = attributes.data();
    }
    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 0xf;
    if (blend != "opaque") {
        ba.blendEnable = VK_TRUE;
        ba.colorBlendOp = ba.alphaBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        if (blend == "premultiplied_alpha")
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        else if (blend == "additive")
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        else if (blend == "multiply" || blend == "rounded_multiply") {
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
            ba.dstColorBlendFactor =
                blend == "multiply" ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        } else if (blend != "straight_alpha")
            throw std::invalid_argument("unknown Vulkan blend mode: " + blend);
    }
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    const VkDynamicState dynamics[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dynamics;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &raster;
    pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &ds;
    pi.layout = layout;
    pi.renderPass = render_pass(format);
    VkPipeline result{};
    check(vkCreateGraphicsPipelines(device.device, VK_NULL_HANDLE, 1, &pi, nullptr, &result),
          "create graphics pipeline");
    pipelines.emplace(std::move(key), result);
    return result;
}

VkDescriptorSet Renderer::Impl::descriptor_set(Slice constants,
                                               const gpu::RoundedClipConstants& clips,
                                               Image& source, Image& backdrop) {
    transition(command, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (&backdrop != &source)
        transition(command, backdrop, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorSet set{};
    for (;;) {
        if (pool_index == pools.size()) {
            const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2048},
                                               {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2048},
                                               {VK_DESCRIPTOR_TYPE_SAMPLER, 1024}};
            VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            info.maxSets = 1024;
            info.poolSizeCount = 3;
            info.pPoolSizes = sizes;
            VkDescriptorPool pool{};
            check(vkCreateDescriptorPool(device.device, &info, nullptr, &pool),
                  "create descriptor pool");
            pools.push_back(pool);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pools[pool_index];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &descriptors;
        auto result = vkAllocateDescriptorSets(device.device, &ai, &set);
        if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL) {
            ++pool_index;
            continue;
        }
        check(result, "allocate descriptor set");
        break;
    }
    const auto clip = arena.upload(&clips, sizeof(clips));
    VkDescriptorBufferInfo buffers[]{{constants.buffer, constants.offset, constants.size},
                                     {clip.buffer, clip.offset, clip.size}};
    VkDescriptorImageInfo images[]{{VK_NULL_HANDLE, source.view, source.layout},
                                   {VK_NULL_HANDLE, backdrop.view, backdrop.layout},
                                   {source.filter == VK_FILTER_NEAREST ? nearest : linear,
                                    VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}};
    VkWriteDescriptorSet writes[5]{};
    for (std::uint32_t i = 0; i < 5; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i < 2   ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                   : i < 4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                           : VK_DESCRIPTOR_TYPE_SAMPLER;
        if (i < 2)
            writes[i].pBufferInfo = &buffers[i];
        else
            writes[i].pImageInfo = &images[i - 2];
    }
    vkUpdateDescriptorSets(device.device, 5, writes, 0, nullptr);
    return set;
}

void Renderer::Impl::draw(Target& target, const std::string& program, const std::string& blend,
                          Slice constants, const gpu::RoundedClipConstants& clips, Image& source,
                          Image& backdrop, host::Scissor scissor, const host::DrawBatch* batch,
                          Slice vertices, Slice indices) {
    const std::uint32_t x = std::min(scissor.x, target.width),
                        y = std::min(scissor.y, target.height);
    const std::uint32_t width = std::min(scissor.width, target.width - x),
                        height = std::min(scissor.height, target.height - y);
    if (!width || !height || (batch && !batch->index_count))
        return;
    const auto selected = pipeline(program, blend, target.format, batch == nullptr);
    const auto set = descriptor_set(constants, clips, source, backdrop);
    barrier(target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = render_pass(target.format);
    fi.attachmentCount = 1;
    fi.pAttachments = &target.view;
    fi.width = target.width;
    fi.height = target.height;
    fi.layers = 1;
    VkFramebuffer fb{};
    check(vkCreateFramebuffer(device.device, &fi, nullptr, &fb), "create framebuffer");
    framebuffers.push_back(fb);
    VkRenderPassBeginInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    ri.renderPass = fi.renderPass;
    ri.framebuffer = fb;
    ri.renderArea.extent = {target.width, target.height};
    vkCmdBeginRenderPass(command, &ri, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0, 0, static_cast<float>(target.width), static_cast<float>(target.height),
                        0, 1};
    VkRect2D rectangle{{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)},
                       {width, height}};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &rectangle);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, selected);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0,
                            nullptr);
    if (batch) {
        vkCmdBindVertexBuffers(command, 0, 1, &vertices.buffer, &vertices.offset);
        vkCmdBindIndexBuffer(command, indices.buffer, indices.offset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(command, batch->index_count, 1, batch->first_index,
                         static_cast<std::int32_t>(batch->base_vertex), 0);
    } else
        vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
}

} // namespace strata::vulkan
