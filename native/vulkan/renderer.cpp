#include "renderer_internal.hpp"

namespace strata::vulkan {

RenderLayerTelemetry Renderer::Impl::render(const host::RenderPacket& packet,
                                            const RenderTarget& output, FrameOptions options) {
    if (!output.image || !output.view || !output.framebuffer_width || !output.framebuffer_height ||
        !std::isfinite(output.logical_width) || !std::isfinite(output.logical_height) ||
        output.logical_width <= 0 || output.logical_height <= 0 ||
        !std::isfinite(options.time_seconds))
        throw std::invalid_argument("invalid Vulkan render target");
    if (output.format != VK_FORMAT_R8G8B8A8_UNORM && output.format != VK_FORMAT_B8G8R8A8_UNORM &&
        output.format != VK_FORMAT_R16G16B16A16_SFLOAT)
        throw std::invalid_argument("Vulkan target requires RGBA8/BGRA8 UNORM or RGBA16F");
    if (output.final_layout == VK_IMAGE_LAYOUT_UNDEFINED ||
        output.final_layout == VK_IMAGE_LAYOUT_PREINITIALIZED)
        throw std::invalid_argument("invalid Vulkan final target layout");
    if (options.load_action == TargetLoadAction::preserve &&
        output.initial_layout == VK_IMAGE_LAYOUT_UNDEFINED)
        throw std::invalid_argument("cannot preserve an undefined Vulkan target");
    logical_width = output.logical_width;
    logical_height = output.logical_height;
    seconds = options.time_seconds;
    telemetry = {};
    active_epoch = packet.geometry_epoch;
    effect_index = 0;
    begin();
    if (packet.full_geometry_payload || !packet.vertex_patches.empty() ||
        !packet.index_patches.empty() || !packet.resources.empty())
        std::erase_if(cached_effects,
                      [&](const auto& entry) { return entry.first.first == active_layer; });
    try {
        resources(packet);
        Target target{output.image,
                      output.view,
                      output.format,
                      output.framebuffer_width,
                      output.framebuffer_height,
                      output.initial_layout};
        if (options.load_action == TargetLoadAction::clear) {
            barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue color{};
            std::copy(options.clear_color.begin(), options.clear_color.end(), color.float32);
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(command, target.image, target.layout, &color, 1, &range);
        }
        const auto [vertices, indices] = upload_geometry(packet);
        const FrameData frame{
            {static_cast<float>(logical_width), static_cast<float>(logical_height)},
            {static_cast<float>(target.width), static_cast<float>(target.height)},
            static_cast<float>(seconds)};
        const auto constants = arena.upload(&frame, sizeof(frame));
        Image* surface_backdrop = nullptr;
        for (const auto& batch : packet.batches) {
            if (const auto* e = std::get_if<host::EffectBatch>(&batch);
                e && e->backdrop_source == host::EffectBackdropSource::surface) {
                surface_backdrop = &capture(target);
                break;
            }
        }
        struct Content {
            Target parent;
            Image* image;
            Image* backdrop;
            host::EffectBatch batch;
        };
        std::vector<Content> stack;
        for (const auto& submission : packet.batches) {
            if (const auto* batch = std::get_if<host::DrawBatch>(&submission)) {
                if (static_cast<std::uint64_t>(batch->first_index) + batch->index_count >
                        packet.indices.size() ||
                    batch->base_vertex > INT32_MAX)
                    throw std::invalid_argument("Vulkan draw index range is invalid");
                for (std::size_t i = batch->first_index;
                     i < static_cast<std::size_t>(batch->first_index) + batch->index_count; ++i)
                    if (static_cast<std::uint64_t>(packet.indices[i]) + batch->base_vertex >=
                        packet.vertices.size() / 88)
                        throw std::invalid_argument("Vulkan draw vertex index is invalid");
                Image* texture = white.get();
                if (batch->texture) {
                    auto it = textures.find(*batch->texture);
                    if (it == textures.end())
                        throw std::invalid_argument("missing Vulkan texture: " + *batch->texture);
                    texture = it->second.get();
                }
                std::string program = "builtin";
                if (auto it = materials.find(batch->material); it != materials.end())
                    program = it->second;
                // Built-in material ids are represented by the fixed draw mode in each vertex.
                else if (!batch->material.empty() && batch->material != "strata:unified_ui" &&
                         batch->material != "strata:solid" &&
                         batch->material != "strata:textured" &&
                         batch->material != "strata:rounded_rect" &&
                         batch->material != "strata:border" &&
                         batch->material != "strata:coverage_text" &&
                         batch->material != "strata:msdf_text" &&
                         batch->material != "strata:custom_mesh" &&
                         batch->material != "strata:shadow")
                    throw std::invalid_argument("undeclared Vulkan material: " + batch->material);
                const bool clipped = !batch->rounded_clips.empty();
                const std::string blend =
                    clipped && batch->blend_mode == "opaque"     ? "straight_alpha"
                    : clipped && batch->blend_mode == "multiply" ? "rounded_multiply"
                                                                 : batch->blend_mode;
                const auto mode = batch->blend_mode == "premultiplied_alpha"
                                      ? gpu::RoundedClipMode::premultiplied_alpha
                                  : clipped && batch->blend_mode == "multiply"
                                      ? gpu::RoundedClipMode::multiply
                                  : !clipped ? gpu::RoundedClipMode::hard
                                             : gpu::RoundedClipMode::straight_alpha;
                draw(target, program, blend, constants, clip_constants(batch->rounded_clips, mode),
                     *texture, *texture, batch->scissor, batch, vertices, indices);
            } else if (const auto* blur_batch = std::get_if<host::BlurBatch>(&submission)) {
                Image& original = capture(target);
                Image& blurred = blur(original, blur_batch->radius, blur_batch->downsample);
                BlurData data;
                data.composite = 1;
                data.logical[0] = static_cast<float>(logical_width);
                data.logical[1] = static_cast<float>(logical_height);
                data.target[0] = static_cast<float>(target.width);
                data.target[1] = static_cast<float>(target.height);
                host::EffectBatch region;
                region.x = blur_batch->x;
                region.y = blur_batch->y;
                region.width = blur_batch->width;
                region.height = blur_batch->height;
                region.scissor = blur_batch->scissor;
                draw(target, "blur", "opaque", arena.upload(&data, sizeof(data)),
                     clip_constants(blur_batch->rounded_clips,
                                    gpu::RoundedClipMode::premultiplied_alpha),
                     blurred, original, bounds(region, target));
            } else if (const auto* effect_batch = std::get_if<host::EffectBatch>(&submission)) {
                Image& backdrop =
                    effect_batch->backdrop_source == host::EffectBackdropSource::surface
                        ? *surface_backdrop
                        : capture(target);
                if (effect_batch->kind == host::EffectBatchKind::content_begin) {
                    if (stack.size() >= host::maximum_content_effect_depth)
                        throw std::length_error("content effect stack overflow");
                    auto& content = temporary(target.width, target.height, target.format);
                    clear(content);
                    stack.push_back({target, &content, &backdrop, *effect_batch});
                    target = target_of(content);
                } else
                    effect(target, backdrop, backdrop, *effect_batch);
            } else {
                if (stack.empty())
                    throw std::invalid_argument("unbalanced content effect end");
                auto content = std::move(stack.back());
                stack.pop_back();
                content.image->layout = target.layout;
                target = content.parent;
                effect(target, *content.image, *content.backdrop, content.batch);
            }
        }
        if (!stack.empty())
            throw std::invalid_argument("unterminated content effect");
        barrier(target, output.final_layout);
        finish();
        return telemetry;
    } catch (...) {
        poisoned = true;
        throw;
    }
}

Renderer::Renderer(Device device) {
    if (!device.device || !device.physical_device || !device.queue)
        throw std::invalid_argument(
            "Vulkan renderer requires a physical device, device, and graphics queue");
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.physical_device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_1)
        throw std::invalid_argument("Vulkan renderer requires Vulkan 1.1 or newer");
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device.physical_device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device.physical_device, &count, families.data());
    if (device.queue_family >= count ||
        !(families[device.queue_family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        throw std::invalid_argument("Vulkan renderer requires a graphics queue family");
    impl_ = std::make_unique<Impl>(device);
    impl_->initialize();
}
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;
void Renderer::declare_material(std::string_view id, std::string_view source) {
    if (id.empty() || source.empty())
        throw std::invalid_argument("Vulkan material id and source must not be empty");
    const std::string program = "material:" + std::string(source);
    impl_->add_program(
        program, std::string(gpu::shaders::pixel_common) + std::string(gpu::rounded_clip_hlsl) +
                     std::string(gpu::shaders::material_prelude) + std::string(source) +
                     std::string(gpu::shaders::material_entry));
    impl_->materials[std::string(id)] = program;
    impl_->prune_programs();
    impl_->cached_effects.clear();
}
void Renderer::declare_effect_pass(std::string_view id, std::uint32_t index, std::uint32_t kind,
                                   double radius, std::uint32_t downsample,
                                   std::uint32_t radius_parameter,
                                   std::uint32_t downsample_parameter, std::string_view source) {
    if (id.empty() || kind > 2 || !std::isfinite(radius) || radius < 0 || !downsample ||
        (radius_parameter != UINT32_MAX && radius_parameter >= 16) ||
        (downsample_parameter != UINT32_MAX && downsample_parameter >= 16))
        throw std::invalid_argument("invalid Vulkan effect declaration");
    std::string program;
    if (kind == 1) {
        if (source.empty())
            throw std::invalid_argument("Vulkan shader effect requires source");
        program = "effect:" + std::string(source);
        impl_->add_program(program, std::string(gpu::effect_prelude) + std::string(source) +
                                        std::string(gpu::effect_entry));
    }
    impl_->cached_effects.clear();
    impl_->effects[std::string(id)].insert_or_assign(
        index,
        Impl::Pass{kind, radius, downsample, radius_parameter, downsample_parameter, program});
    impl_->prune_programs();
}
RenderLayerTelemetry Renderer::render(std::string_view layer, const host::RenderPacket& packet,
                                      const RenderTarget& target, FrameOptions options) {
    if (layer.empty())
        throw std::invalid_argument("Vulkan layer id must not be empty");
    impl_->active_layer = layer;
    return impl_->render(packet, target, options);
}
void Renderer::consume_resources(const host::RenderPacket& packet) {
    impl_->begin();
    if (!packet.resources.empty())
        impl_->cached_effects.clear();
    try {
        impl_->resources(packet);
        impl_->finish();
    } catch (...) {
        impl_->poisoned = true;
        throw;
    }
}
void Renderer::release_layer(std::string_view layer) noexcept {
    if (auto found = impl_->geometry.find(layer); found != impl_->geometry.end())
        impl_->geometry.erase(found);
    std::erase_if(impl_->cached_effects,
                  [&](const auto& entry) { return entry.first.first == layer; });
}
void Renderer::release_target() {
    impl_->clear_framebuffers();
    impl_->scratch.clear();
    impl_->cached_effects.clear();
}

} // namespace strata::vulkan
