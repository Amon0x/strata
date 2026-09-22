#include "renderer_internal.hpp"
namespace strata::vulkan {
Renderer::Impl::~Impl() {
    wait_idle();
    for (auto& slot : frames) {
        release_frame(slot);
        for (auto pool : slot.pools)
            vkDestroyDescriptorPool(device.device, pool, nullptr);
        if (slot.commands)
            vkDestroyCommandPool(device.device, slot.commands, nullptr);
        if (slot.fence)
            vkDestroyFence(device.device, slot.fence, nullptr);
    }
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
    if (linear)
        vkDestroySampler(device.device, linear, nullptr);
    if (nearest)
        vkDestroySampler(device.device, nearest, nullptr);
    if (layout)
        vkDestroyPipelineLayout(device.device, layout, nullptr);
    if (descriptors)
        vkDestroyDescriptorSetLayout(device.device, descriptors, nullptr);
}
void Renderer::Impl::release_frame(Frame& slot) {
    for (auto framebuffer : slot.framebuffers)
        vkDestroyFramebuffer(device.device, framebuffer, nullptr);
    slot.framebuffers.clear();
    slot.retired_images.clear();
    slot.retired_buffers.clear();
}
void Renderer::Impl::wait_idle() {
    for (auto& slot : frames) {
        if (slot.pending && slot.fence)
            vkWaitForFences(device.device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
        slot.pending = false;
        release_frame(slot);
    }
}
void Renderer::Impl::retire(std::unique_ptr<Image> image) {
    if (!image)
        return;
    // The frame being recorded, or else the newest submitted one, is the last possible reader;
    // its fence signals only after every earlier frame has completed.
    if (recording)
        frame().retired_images.push_back(std::move(image));
    else if (last_submitted && frames[*last_submitted].pending)
        frames[*last_submitted].retired_images.push_back(std::move(image));
}
void Renderer::Impl::retire(std::unique_ptr<Buffer> buffer) {
    if (!buffer)
        return;
    if (recording)
        frame().retired_buffers.push_back(std::move(buffer));
    else if (last_submitted && frames[*last_submitted].pending)
        frames[*last_submitted].retired_buffers.push_back(std::move(buffer));
}
void Renderer::Impl::drop_effects(const std::optional<std::string_view> layer) {
    for (auto entry = cached_effects.begin(); entry != cached_effects.end();) {
        if (layer && entry->first.first != *layer) {
            ++entry;
            continue;
        }
        retire(std::move(entry->second.output));
        entry = cached_effects.erase(entry);
    }
}
void Renderer::Impl::end_pass() {
    if (!open_pass)
        return;
    vkCmdEndRenderPass(command);
    open_pass.reset();
}
void Renderer::Impl::initialize() {
    VkPipelineCacheCreateInfo cache_info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    check(vkCreatePipelineCache(device.device, &cache_info, nullptr, &pipeline_cache),
          "create pipeline cache");
    for (auto& slot : frames) {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.queueFamilyIndex = device.queue_family;
        check(vkCreateCommandPool(device.device, &ci, nullptr, &slot.commands),
              "create command pool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = slot.commands;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device.device, &ai, &slot.command),
              "allocate command buffer");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(device.device, &fi, nullptr, &slot.fence), "create fence");
        slot.arena = std::make_unique<UploadArena>(device);
    }
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
    Frame& slot = frame();
    if (slot.pending) {
        check(vkWaitForFences(device.device, 1, &slot.fence, VK_TRUE, UINT64_MAX),
              "wait render fence");
        slot.pending = false;
    }
    release_frame(slot);
    for (auto pool : slot.pools)
        check(vkResetDescriptorPool(device.device, pool, 0), "reset descriptor pool");
    check(vkResetCommandPool(device.device, slot.commands, 0), "reset command pool");
    slot.arena->reset();
    slot.pool_index = 0;
    arena = slot.arena.get();
    command = slot.command;
    scratch_index = 0;
    open_pass.reset();
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(command, &bi), "begin command buffer");
    recording = true;
}
void Renderer::Impl::finish() {
    end_pass();
    Frame& slot = frame();
    check(vkEndCommandBuffer(command), "end command buffer");
    check(vkResetFences(device.device, 1, &slot.fence), "reset fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    const VkResult result = vkQueueSubmit(device.queue, 1, &submit, slot.fence);
    recording = false;
    if (result != VK_SUCCESS)
        check(result, "submit renderer");
    // No CPU wait: queue order and the recorded barriers order this work against the host's.
    slot.pending = true;
    last_submitted = frame_index;
    frame_index = (frame_index + 1U) % frames_in_flight;
}
} // namespace strata::vulkan
