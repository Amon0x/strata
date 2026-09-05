#include "renderer_internal.hpp"
namespace strata::vulkan {
Renderer::Impl::~Impl() {
    if (fence)
        vkWaitForFences(device.device, 1, &fence, VK_TRUE, UINT64_MAX);
    clear_framebuffers();
    for (auto& [key, pipeline] : pipelines) {
        (void)key;
        vkDestroyPipeline(device.device, pipeline, nullptr);
    }
    if (pipeline_cache)
        vkDestroyPipelineCache(device.device, pipeline_cache, nullptr);
    for (auto& [key, pass] : render_passes) {
        (void)key;
        vkDestroyRenderPass(device.device, pass, nullptr);
    }
    for (auto pool : pools)
        vkDestroyDescriptorPool(device.device, pool, nullptr);
    if (linear)
        vkDestroySampler(device.device, linear, nullptr);
    if (nearest)
        vkDestroySampler(device.device, nearest, nullptr);
    if (layout)
        vkDestroyPipelineLayout(device.device, layout, nullptr);
    if (descriptors)
        vkDestroyDescriptorSetLayout(device.device, descriptors, nullptr);
    if (commands)
        vkDestroyCommandPool(device.device, commands, nullptr);
    if (fence)
        vkDestroyFence(device.device, fence, nullptr);
}
void Renderer::Impl::clear_framebuffers() {
    for (auto fb : framebuffers)
        vkDestroyFramebuffer(device.device, fb, nullptr);
    framebuffers.clear();
}
void Renderer::Impl::initialize() {
    VkPipelineCacheCreateInfo cache_info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    check(vkCreatePipelineCache(device.device, &cache_info, nullptr, &pipeline_cache),
          "create pipeline cache");
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.queueFamilyIndex = device.queue_family;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    check(vkCreateCommandPool(device.device, &ci, nullptr, &commands), "create command pool");
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = commands;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(device.device, &ai, &command), "allocate command buffer");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    check(vkCreateFence(device.device, &fi, nullptr, &fence), "create fence");
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    for (std::uint32_t i = 0; i < 5; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[i].descriptorType = i < 2   ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                     : i < 4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                             : VK_DESCRIPTOR_TYPE_SAMPLER;
    }
    VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    di.bindingCount = static_cast<std::uint32_t>(bindings.size());
    di.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(device.device, &di, nullptr, &descriptors),
          "create descriptor layout");
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &descriptors;
    check(vkCreatePipelineLayout(device.device, &li, nullptr, &layout), "create pipeline layout");
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    check(vkCreateSampler(device.device, &si, nullptr, &linear), "create linear sampler");
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    check(vkCreateSampler(device.device, &si, nullptr, &nearest), "create nearest sampler");
    vertex = compile_hlsl(gpu::shaders::vertex, true, "strata.vertex");
    fullscreen = compile_hlsl(gpu::blur_shaders::vertex, true, "strata.fullscreen");
    add_program("builtin", std::string(gpu::shaders::pixel_common) +
                               std::string(gpu::rounded_clip_hlsl) +
                               std::string(gpu::shaders::builtin_entry));
    add_program("blur",
                std::string(gpu::rounded_clip_hlsl) + std::string(gpu::blur_shaders::pixel));
    add_program("composite",
                std::string(gpu::rounded_clip_hlsl) + std::string(gpu::composite_pixel));
    white = std::make_unique<Image>(device, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
    begin();
    clear(*white, {1, 1, 1, 1});
    transition(command, *white, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    finish();
}

void Renderer::Impl::add_program(const std::string& name, const std::string& source) {
    if (!programs.contains(name))
        programs.emplace(name, compile_hlsl(source, false, name));
}
void Renderer::Impl::prune_programs() {
    const auto used = [&](const std::string& name) {
        if (name == "builtin" || name == "blur" || name == "composite")
            return true;
        for (const auto& [id, program] : materials) {
            (void)id;
            if (program == name)
                return true;
        }
        for (const auto& [id, passes] : effects) {
            (void)id;
            for (const auto& [index, pass] : passes) {
                (void)index;
                if (pass.program == name)
                    return true;
            }
        }
        return false;
    };
    std::erase_if(pipelines, [&](const auto& entry) {
        if (used(std::get<0>(entry.first)))
            return false;
        vkDestroyPipeline(device.device, entry.second, nullptr);
        return true;
    });
    std::erase_if(programs, [&](const auto& entry) { return !used(entry.first); });
}

void Renderer::Impl::begin() {
    if (poisoned)
        throw std::logic_error("Vulkan renderer must be recreated after a failed submission");
    check(vkWaitForFences(device.device, 1, &fence, VK_TRUE, UINT64_MAX), "wait render fence");
    clear_framebuffers();
    retired.clear();
    for (auto pool : pools)
        check(vkResetDescriptorPool(device.device, pool, 0), "reset descriptor pool");
    check(vkResetCommandPool(device.device, commands, 0), "reset command pool");
    arena.reset();
    pool_index = 0;
    scratch_index = 0;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(command, &bi), "begin command buffer");
}
void Renderer::Impl::finish() {
    check(vkEndCommandBuffer(command), "end command buffer");
    check(vkResetFences(device.device, 1, &fence), "reset fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    const VkResult result = vkQueueSubmit(device.queue, 1, &submit, fence);
    if (result != VK_SUCCESS) {
        // An unsubmitted fence cannot be waited on during destruction.
        vkDestroyFence(device.device, fence, nullptr);
        fence = VK_NULL_HANDLE;
        check(result, "submit renderer");
    }
    check(vkWaitForFences(device.device, 1, &fence, VK_TRUE, UINT64_MAX), "complete render");
    clear_framebuffers();
    retired.clear();
}
} // namespace strata::vulkan
