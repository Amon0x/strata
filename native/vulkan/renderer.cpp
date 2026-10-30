#include "renderer_internal.hpp"

namespace strata::vulkan {

void Renderer::Impl::validate_target(const RenderTarget& output, const FrameOptions& options) {
    if (!output.image || !output.view || !output.framebuffer_width || !output.framebuffer_height ||
        !std::isfinite(output.logical_width) || !std::isfinite(output.logical_height) ||
        output.logical_width <= 0 || output.logical_height <= 0)
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
}

RenderLayerTelemetry Renderer::Impl::render(const host::RenderPacket& packet,
                                            const RenderTarget& output, FrameOptions options) {
    if (!std::isfinite(options.time_seconds))
        throw std::invalid_argument("invalid Vulkan render target");
    validate_target(output, options);
    logical_width = output.logical_width;
    logical_height = output.logical_height;
    seconds = options.time_seconds;
    pointer = options.pointer;
    telemetry = {};
    ++render_count;
    begin();
    try {
        resources(packet);
        run_live_art();
        // The packet names materials and effects by index; resolve each table once.
        layer_materials.clear();
        layer_materials.reserve(packet.materials.size());
        for (const std::string& material : packet.materials) {
            const auto found = materials.find(material);
            layer_materials.push_back(found != materials.end() ? std::string_view(found->second)
                                                               : std::string_view("builtin"));
        }
        layer_effects.clear();
        layer_effects.reserve(packet.effects.size());
        for (const std::string& effect : packet.effects) {
            const auto found = effects.find(effect);
            layer_effects.push_back(found != effects.end() ? &found->second : nullptr);
        }
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
            static_cast<float>(seconds),
            {pointer.has_value() ? static_cast<float>((*pointer)[0]) : 0.0F,
             pointer.has_value() ? static_cast<float>((*pointer)[1]) : 0.0F,
             pointer.has_value() ? 1.0F : 0.0F}};
        const auto constants = arena->upload(&frame, sizeof(frame));
        if (packet.groups.empty()) {
            groups = Slice{identity_groups->buffer, 0, sizeof(packed_groups.values)};
        } else {
            packed_groups.assign(packet.groups);
            groups = arena->upload(packed_groups.values.data(), sizeof(packed_groups.values));
        }
        // SURFACE effects sample the incoming target. Only those due a refresh need it, and only
        // within their sample regions.
        Image* surface_backdrop = nullptr;
        {
            std::uint32_t left = UINT32_MAX, top = UINT32_MAX, right = 0, bottom = 0;
            for (const auto& batch : packet.batches) {
                const auto* e = std::get_if<host::EffectBatch>(&batch);
                if (!e || e->backdrop_source != host::EffectBackdropSource::surface ||
                    reusable(*e, false, target))
                    continue;
                const host::Scissor region = sample_region(*e, target);
                if (!region.width || !region.height)
                    continue;
                left = std::min(left, region.x);
                top = std::min(top, region.y);
                right = std::max(right, region.x + region.width);
                bottom = std::max(bottom, region.y + region.height);
            }
            if (left < right && top < bottom)
                surface_backdrop = &capture(target, {left, top, right - left, bottom - top});
        }
        // Isolated layers nest without limit. The innermost few draw into framebuffer-sized
        // images; a layer nested past them parks the outermost of those, keeping only the pixels
        // later passes read back from it in an image sized to them (gpu/isolation.hpp).
        const std::vector<gpu::PixelBounds> reads = gpu::content_read_bounds(
            packet.batches, target.width, target.height, logical_width, logical_height,
            [&](const host::EffectBatch& effect) {
                const host::Scissor region = sample_region(effect, target);
                return gpu::PixelBounds{region.x, region.y, region.x + region.width,
                                        region.y + region.height};
            });
        std::size_t next_read = 0;
        struct Content {
            /** The framebuffer-sized image it draws into, or null while it is parked. */
            Image* image;
            /** Its read bounds while parked, when anything had been drawn into it. */
            Image* parked;
            gpu::PixelBounds read;
            bool drawn;
            host::EffectBatch batch;
        };
        std::vector<Content> stack;
        // The host target, while layers are open over it.
        Target surface = target;
        const auto park = [&](Content& level) {
            if (level.drawn && !level.read.empty()) {
                level.parked =
                    &temporary(gpu::parked_extent(level.read.width(), surface.width),
                               gpu::parked_extent(level.read.height(), surface.height),
                               surface.format);
                auto held = target_of(*level.image);
                copy(held, *level.parked,
                     {level.read.left, level.read.top, level.read.width(), level.read.height()}, 0,
                     0);
                level.image->layout = held.layout;
            }
            recycle(*level.image);
            level.image = nullptr;
        };
        const auto restore = [&](Content& level) {
            level.image = &temporary(surface.width, surface.height, surface.format);
            clear(*level.image);
            if (!level.parked)
                return;
            auto kept = target_of(*level.parked);
            copy(kept, *level.image, {0, 0, level.read.width(), level.read.height()},
                 static_cast<std::int32_t>(level.read.left),
                 static_cast<std::int32_t>(level.read.top));
            level.parked->layout = kept.layout;
            recycle(*level.parked);
            level.parked = nullptr;
        };
        const auto touch = [&stack] {
            if (!stack.empty())
                stack.back().drawn = true;
        };
        for (const auto& submission : packet.batches) {
            if (const auto* batch = std::get_if<host::DrawBatch>(&submission)) {
                // The decoder has checked every index against the vertex payload.
                if (batch->base_vertex > INT32_MAX)
                    throw std::invalid_argument("Vulkan draw base vertex is invalid");
                // As on D3D11, a texture that has not arrived samples white and a material without
                // a program draws with the built-in shader (whose mode comes from each vertex), so
                // one missing resource costs a draw its look rather than the renderer.
                Image* texture = white.get();
                if (batch->texture) {
                    if (auto it = textures.find(*batch->texture); it != textures.end())
                        texture = it->second.get();
                }
                const std::string_view program = batch->material < layer_materials.size()
                                                     ? layer_materials[batch->material]
                                                     : std::string_view("builtin");
                // The core already adapted the blend and clip coverage to the rounded clips.
                draw(target, program, batch->blend, constants,
                     clip_constants(batch->rounded_clips, batch->clip_coverage), *texture, *texture,
                     batch->scissor, batch, vertices, indices);
                touch();
            } else if (const auto* blur_batch = std::get_if<host::BlurBatch>(&submission)) {
                host::EffectBatch region;
                region.x = blur_batch->x;
                region.y = blur_batch->y;
                region.width = blur_batch->width;
                region.height = blur_batch->height;
                region.scissor = blur_batch->scissor;
                if (!(blur_batch->radius > 0))
                    continue;
                blur(target, bounds(region, target), blur_batch->radius * pixel_scale(target),
                     blur_batch->downsample, blur_batch->rounded_clips);
                touch();
            } else if (const auto* effect_batch = std::get_if<host::EffectBatch>(&submission)) {
                if (effect_batch->kind == host::EffectBatchKind::content_begin) {
                    if (stack.size() >= host::maximum_content_effect_depth)
                        throw std::length_error("content effect stack overflow");
                    if (stack.empty())
                        surface = target;
                    else
                        stack.back().image->layout = target.layout;
                    // The layers holding images are always the innermost ones open; past the
                    // resident count the outermost of them, the last needed again, is parked.
                    const auto holds = [](const Content& open) { return open.image != nullptr; };
                    if (static_cast<std::size_t>(std::ranges::count_if(stack, holds)) >=
                        gpu::resident_content_layers)
                        park(*std::ranges::find_if(stack, holds));
                    auto& content = temporary(surface.width, surface.height, surface.format);
                    clear(content);
                    const gpu::PixelBounds read =
                        next_read < reads.size()
                            ? reads[next_read]
                            : gpu::PixelBounds{0, 0, surface.width, surface.height};
                    ++next_read;
                    stack.push_back({&content, nullptr, read, false, *effect_batch});
                    target = target_of(content);
                } else {
                    if (effect_batch->backdrop_source == host::EffectBackdropSource::surface) {
                        if (!surface_backdrop && !reusable(*effect_batch, false, target) &&
                            bounds(*effect_batch, target).width &&
                            bounds(*effect_batch, target).height)
                            throw std::logic_error(
                                "Vulkan SURFACE effect refresh has no captured input");
                        effect(target, *effect_batch, false, surface_backdrop, nullptr);
                    } else
                        effect(target, *effect_batch, false, nullptr, nullptr);
                    touch();
                }
            } else {
                if (stack.empty())
                    throw std::invalid_argument("unbalanced content effect end");
                auto content = std::move(stack.back());
                stack.pop_back();
                content.image->layout = target.layout;
                if (!stack.empty() && !stack.back().image)
                    restore(stack.back());
                target = stack.empty() ? surface : target_of(*stack.back().image);
                // Authored passes read what is behind the content as the backdrop, and a layer
                // composited by an operator reads it too. Nothing draws into the parent while
                // the layer is open, so it is captured now, when the layer composites, and only
                // when the effect refreshes this frame.
                Image* backdrop = nullptr;
                if (!reusable(content.batch, true, target) &&
                    (content.batch.composite != host::LayerComposite::source_over ||
                     std::ranges::any_of(passes(content.batch), [](const auto& entry) {
                         return entry.second.kind == 1;
                     })))
                    backdrop = &capture(target, sample_region(content.batch, target));
                effect(target, content.batch, true, content.image, backdrop);
                // Its content has been composited: later layers this frame may reuse the images.
                recycle(*content.image);
                if (backdrop != nullptr)
                    recycle(*backdrop);
                touch();
            }
        }
        if (!stack.empty())
            throw std::invalid_argument("unterminated content effect");
        // A sample this frame did not composite belongs to an effect that is gone.
        for (auto entry = cached_effects.begin(); entry != cached_effects.end();) {
            if (std::get<0>(entry->first) == active_layer && entry->second.used != render_count) {
                retire(std::move(entry->second.output));
                entry = cached_effects.erase(entry);
            } else
                ++entry;
        }
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
    impl_->drop_effects();
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
    impl_->drop_effects();
    impl_->effects[std::string(id)].insert_or_assign(
        index,
        Impl::Pass{kind, radius, downsample, radius_parameter, downsample_parameter, program});
    impl_->prune_programs();
}
void Renderer::validate_target(const RenderTarget& target, const FrameOptions& options) const {
    Impl::validate_target(target, options);
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
        impl_->drop_effects();
    try {
        impl_->resources(packet);
        impl_->finish();
    } catch (...) {
        impl_->poisoned = true;
        throw;
    }
}
void Renderer::release_layer(std::string_view layer) noexcept {
    if (auto found = impl_->geometry.find(layer); found != impl_->geometry.end()) {
        for (auto& slot : found->second.slots) {
            impl_->retire(std::move(slot.vertices));
            impl_->retire(std::move(slot.indices));
        }
        impl_->geometry.erase(found);
    }
    impl_->drop_effects(layer);
}
void Renderer::release_target() {
    // Target-sized scratch and cached output may still be read by frames in flight.
    impl_->wait_idle();
    impl_->scratch.clear();
    impl_->cached_effects.clear();
}

} // namespace strata::vulkan
