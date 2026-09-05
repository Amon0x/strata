#include "renderer_internal.hpp"

namespace strata::vulkan {

Image& Renderer::Impl::blur(Image& source, double radius, std::uint32_t downsample) {
    if (!std::isfinite(radius) || radius < 0)
        throw std::invalid_argument("invalid blur radius");
    downsample = std::clamp(downsample, 1U, 16U);
    const auto w = std::max(1U, source.width / downsample),
               h = std::max(1U, source.height / downsample);
    Image& ping = temporary(w, h, source.format);
    Image& pong = temporary(w, h, source.format);
    clear(ping);
    clear(pong);
    BlurData data;
    data.radius = static_cast<float>(radius) / static_cast<float>(downsample);
    data.texel[0] = 1.0F / static_cast<float>(w);
    data.texel[1] = 1.0F / static_cast<float>(h);
    data.direction[0] = 1;
    auto target = target_of(ping);
    draw(target, "blur", "opaque", arena.upload(&data, sizeof(data)), {}, source, source,
         {0, 0, w, h});
    ping.layout = target.layout;
    data.direction[0] = 0;
    data.direction[1] = 1;
    target = target_of(pong);
    draw(target, "blur", "opaque", arena.upload(&data, sizeof(data)), {}, ping, ping, {0, 0, w, h});
    pong.layout = target.layout;
    telemetry.blur_passes += 2;
    return pong;
}

gpu::EffectConstants Renderer::Impl::effect_constants(const host::EffectBatch& effect,
                                                      const Target& target) {
    gpu::EffectConstants c;
    const double sx = target.width / logical_width, sy = target.height / logical_height;
    c.logical_size[0] = static_cast<float>(logical_width);
    c.logical_size[1] = static_cast<float>(logical_height);
    c.target_size[0] = static_cast<float>(target.width);
    c.target_size[1] = static_cast<float>(target.height);
    c.bounds[0] = static_cast<float>(effect.x * sx);
    c.bounds[1] = static_cast<float>(effect.y * sy);
    c.bounds[2] = static_cast<float>(effect.width * sx);
    c.bounds[3] = static_cast<float>(effect.height * sy);
    for (std::size_t i = 0; i < 4; ++i)
        c.radii[i] = static_cast<float>(effect.radii[i] * std::min(sx, sy));
    for (std::size_t i = 0; i < 16; ++i)
        c.parameters[i] = static_cast<float>(effect.parameters[i]);
    c.opacity = static_cast<float>(effect.opacity);
    c.time = static_cast<float>(seconds);
    return c;
}

host::Scissor Renderer::Impl::bounds(const host::EffectBatch& effect, const Target& target) {
    const double sx = target.width / logical_width, sy = target.height / logical_height;
    const auto x = static_cast<std::uint32_t>(
        std::clamp(std::floor(effect.x * sx), 0.0, static_cast<double>(target.width)));
    const auto y = static_cast<std::uint32_t>(
        std::clamp(std::floor(effect.y * sy), 0.0, static_cast<double>(target.height)));
    const auto right = static_cast<std::uint32_t>(std::clamp(
        std::ceil((effect.x + effect.width) * sx), 0.0, static_cast<double>(target.width)));
    const auto bottom = static_cast<std::uint32_t>(std::clamp(
        std::ceil((effect.y + effect.height) * sy), 0.0, static_cast<double>(target.height)));
    const auto left = std::max(x, std::min(effect.scissor.x, target.width)),
               top = std::max(y, std::min(effect.scissor.y, target.height));
    const auto r = std::min<std::uint64_t>(right, static_cast<std::uint64_t>(effect.scissor.x) +
                                                      effect.scissor.width);
    const auto b = std::min<std::uint64_t>(bottom, static_cast<std::uint64_t>(effect.scissor.y) +
                                                       effect.scissor.height);
    return {left, top, static_cast<std::uint32_t>(r > left ? r - left : 0),
            static_cast<std::uint32_t>(b > top ? b - top : 0)};
}

void Renderer::Impl::effect(Target& destination, Image& source, Image& backdrop,
                            const host::EffectBatch& batch) {
    auto& cached = cached_effects[{active_layer, effect_index++}];
    const auto constants = effect_constants(batch, destination);
    const bool reusable =
        cached.output && cached.batch == batch && cached.epoch == active_epoch &&
        cached.width == destination.width && cached.height == destination.height &&
        cached.output->format == destination.format && cached.logical_width == logical_width &&
        cached.logical_height == logical_height && batch.refresh_rate > 0 &&
        seconds >= cached.time && seconds - cached.time < 1.0 / batch.refresh_rate;
    if (reusable) {
        draw(destination, "composite", "premultiplied_alpha",
             arena.upload(&constants, sizeof(constants)),
             clip_constants(batch.rounded_clips, gpu::RoundedClipMode::premultiplied_alpha),
             *cached.output, *cached.output, bounds(batch, destination));
        return;
    }
    const auto found = effects.find(batch.effect);
    if (found == effects.end())
        throw std::invalid_argument("undeclared Vulkan effect: " + batch.effect);
    Image* current = &source;
    for (const auto& [index, pass] : found->second) {
        (void)index;
        if (pass.kind == 0) {
            double radius = pass.radius;
            auto downsample = pass.downsample;
            if (pass.radius_parameter != UINT32_MAX) {
                if (pass.radius_parameter >= batch.parameter_count)
                    throw std::invalid_argument("effect radius parameter out of range");
                radius = batch.parameters[pass.radius_parameter];
            }
            if (pass.downsample_parameter != UINT32_MAX) {
                if (pass.downsample_parameter >= batch.parameter_count)
                    throw std::invalid_argument("effect downsample parameter out of range");
                downsample = static_cast<std::uint32_t>(
                    std::clamp(batch.parameters[pass.downsample_parameter], 1.0, 16.0));
            }
            current = &blur(*current, radius, downsample);
        } else if (pass.kind == 1) {
            auto& next = temporary(destination.width, destination.height, destination.format);
            clear(next);
            auto target = target_of(next);
            draw(target, pass.program, "opaque", arena.upload(&constants, sizeof(constants)), {},
                 *current, backdrop, {0, 0, target.width, target.height});
            next.layout = target.layout;
            current = &next;
            ++telemetry.effect_passes;
        }
    }
    draw(destination, "composite", "premultiplied_alpha",
         arena.upload(&constants, sizeof(constants)),
         clip_constants(batch.rounded_clips, gpu::RoundedClipMode::premultiplied_alpha), *current,
         *current, bounds(batch, destination));
    ++telemetry.effect_passes;
    if (batch.refresh_rate > 0) {
        // Cache only completed effect output; the host target itself is never retained.
        if (!cached.output || cached.output->width != current->width ||
            cached.output->height != current->height || cached.output->format != current->format) {
            if (cached.output)
                retired.push_back(std::move(cached.output));
            cached.output =
                std::make_unique<Image>(device, current->width, current->height, current->format);
        }
        transition(command, *current, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(command, *cached.output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.extent = {current->width, current->height, 1};
        vkCmdCopyImage(command, current->image, current->layout, cached.output->image,
                       cached.output->layout, 1, &region);
        cached.batch = batch;
        cached.epoch = active_epoch;
        cached.time = seconds;
        cached.logical_width = logical_width;
        cached.logical_height = logical_height;
        cached.width = destination.width;
        cached.height = destination.height;
    }
}

} // namespace strata::vulkan
