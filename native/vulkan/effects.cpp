#include "renderer_internal.hpp"

namespace strata::vulkan {
namespace {

/**
 * Blur draws cover the whole target with uv in [0, 1] and sample sourceUv.xy + uv * sourceUv.zw.
 * This returns that rectangle for a draw that should map `area` (device pixels of a target_width x
 * target_height target) onto (u, v, width, height) of the sampled image.
 */
void place(float (&output)[4], const double u, const double v, const double width,
           const double height, const host::Scissor area, const std::uint32_t target_width,
           const std::uint32_t target_height) {
    const double scale_x = width / area.width;
    const double scale_y = height / area.height;
    output[0] = static_cast<float>(u - area.x * scale_x);
    output[1] = static_cast<float>(v - area.y * scale_y);
    output[2] = static_cast<float>(scale_x * target_width);
    output[3] = static_cast<float>(scale_y * target_height);
}

} // namespace

void Renderer::Impl::blur(Target& image, const host::Scissor area, const double physical_radius,
                          std::uint32_t downsample,
                          const std::span<const host::RoundedClip> clips) {
    if (!std::isfinite(physical_radius) || physical_radius < 0)
        throw std::invalid_argument("invalid blur radius");
    if (physical_radius <= 0 || !area.width || !area.height)
        return;
    // The same plan as D3D11's BlurPass, so a packet blurs alike everywhere: read the area plus
    // the radius, reduce it by the downsample, blur both ways there, and write the area back.
    downsample = std::clamp(downsample, 1U, 8U);
    const auto padding =
        static_cast<std::uint32_t>(std::clamp(std::ceil(physical_radius), 1.0, 256.0));
    const auto source_left = area.x > padding ? area.x - padding : 0U;
    const auto source_top = area.y > padding ? area.y - padding : 0U;
    const auto source_right = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(area.x) + area.width + padding, image.width));
    const auto source_bottom = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(area.y) + area.height + padding, image.height));
    const host::Scissor source_area{source_left, source_top, source_right - source_left,
                                    source_bottom - source_top};
    const auto reduced = [downsample](std::uint32_t extent) {
        return std::max(1U, extent / downsample + (extent % downsample == 0U ? 0U : 1U));
    };
    const auto width = reduced(source_area.width), height = reduced(source_area.height);
    const float radius = static_cast<float>(std::clamp(physical_radius / downsample, 0.5, 32.0));

    Image& source = temporary(source_area.width, source_area.height, image.format);
    copy(image, source, source_area, 0, 0);
    Image& read = temporary(width, height, image.format);
    Image& write = temporary(width, height, image.format);
    const auto pass = [&](Image& output, Image& input, float direction_x, float direction_y,
                          float pass_radius) {
        BlurData data;
        data.texel[0] = 1.0F / static_cast<float>(input.width);
        data.texel[1] = 1.0F / static_cast<float>(input.height);
        data.direction[0] = direction_x;
        data.direction[1] = direction_y;
        data.radius = pass_radius;
        auto target = target_of(output);
        draw(target, "blur", Blend::opaque, arena->upload(&data, sizeof(data)), {}, input, input,
             {0, 0, output.width, output.height});
        output.layout = target.layout;
    };
    pass(read, source, 0, 0, 0);
    pass(write, read, 1, 0, radius);
    pass(read, write, 0, 1, radius);

    BlurData data;
    data.texel[0] = 1.0F / static_cast<float>(width);
    data.texel[1] = 1.0F / static_cast<float>(height);
    data.composite = clips.empty() ? 0.0F : 1.0F;
    place(data.source_uv, static_cast<double>((area.x - source_area.x) / downsample) / width,
          static_cast<double>((area.y - source_area.y) / downsample) / height,
          static_cast<double>(reduced(area.width)) / width,
          static_cast<double>(reduced(area.height)) / height, area, image.width, image.height);
    place(data.original_uv, static_cast<double>(area.x - source_area.x) / source_area.width,
          static_cast<double>(area.y - source_area.y) / source_area.height,
          static_cast<double>(area.width) / source_area.width,
          static_cast<double>(area.height) / source_area.height, area, image.width, image.height);
    data.logical[0] = static_cast<float>(logical_width);
    data.logical[1] = static_cast<float>(logical_height);
    data.target[0] = static_cast<float>(image.width);
    data.target[1] = static_cast<float>(image.height);
    draw(image, "blur", Blend::opaque, arena->upload(&data, sizeof(data)),
         clip_constants(clips, gpu::RoundedClipMode::premultiplied_alpha), read, source, area);
    telemetry.blur_passes += 4;
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
    c.padding[0] = static_cast<float>(effect.tone);
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

const std::map<std::uint32_t, Renderer::Impl::Pass>&
Renderer::Impl::passes(const host::EffectBatch& effect) const {
    // An effect with no declared passes composites its input unchanged, as D3D11 does.
    static const std::map<std::uint32_t, Pass> none;
    const auto* const found =
        effect.effect < layer_effects.size() ? layer_effects[effect.effect] : nullptr;
    return found != nullptr ? *found : none;
}

double Renderer::Impl::widest_blur(const host::EffectBatch& effect) const {
    double widest = 0;
    for (const auto& [index, pass] : passes(effect)) {
        (void)index;
        if (pass.kind == 0)
            widest = std::max(widest, pass.radius_parameter < effect.parameter_count
                                          ? effect.parameters[pass.radius_parameter]
                                          : pass.radius);
    }
    return widest;
}

host::Scissor Renderer::Impl::sample_region(const host::EffectBatch& effect,
                                            const Target& target) const {
    // As D3D11: the prelude's sample reach, or the widest blur's padding when that is further.
    const double sx = target.width / logical_width, sy = target.height / logical_height;
    const double scale = std::max(sx, sy);
    const double reach =
        std::ceil(std::max(gpu::effect_sample_reach * scale,
                           std::clamp(std::ceil(widest_blur(effect) * scale), 1.0, 256.0)));
    const auto edge = [](double value, std::uint32_t limit) {
        return static_cast<std::uint32_t>(std::clamp(value, 0.0, static_cast<double>(limit)));
    };
    const auto left = edge(std::floor(effect.x * sx) - reach, target.width);
    const auto top = edge(std::floor(effect.y * sy) - reach, target.height);
    const auto right = edge(std::ceil((effect.x + effect.width) * sx) + reach, target.width);
    const auto bottom = edge(std::ceil((effect.y + effect.height) * sy) + reach, target.height);
    return {left, top, right > left ? right - left : 0, bottom > top ? bottom - top : 0};
}

Renderer::Impl::CachedEffect* Renderer::Impl::reusable(const host::EffectBatch& batch,
                                                       const bool content,
                                                       const Target& destination) {
    if (!(batch.refresh_rate > 0))
        return nullptr;
    const auto found = cached_effects.find(EffectKey{active_layer, batch.source_order, content});
    if (found == cached_effects.end())
        return nullptr;
    CachedEffect& cached = found->second;
    // Whether anything the sample read changed is the decoder's revision; this host checks only
    // that it covers the same pixels of the same target and that the refresh interval holds.
    const bool due =
        !cached.output || cached.revision != batch.sample_revision ||
        !(cached.clip == bounds(batch, destination)) || cached.width != destination.width ||
        cached.height != destination.height || cached.output->format != destination.format ||
        cached.logical_width != logical_width || cached.logical_height != logical_height ||
        gpu::effect_refresh_due(batch.refresh_rate, cached.time, seconds);
    return due ? nullptr : &cached;
}

void Renderer::Impl::effect(Target& destination, const host::EffectBatch& batch, const bool content,
                            Image* source, Image* backdrop) {
    const host::Scissor clip = bounds(batch, destination);
    if (!clip.width || !clip.height)
        return;
    const auto key = EffectKey{active_layer, batch.source_order, content};
    const auto constants = effect_constants(batch, destination);
    const auto composite_cached = [&](CachedEffect& cached) {
        cached.used = render_count;
        // The cached composite reads its origin where authored passes read the tone.
        auto placed = constants;
        placed.padding[0] = static_cast<float>(cached.clip.x);
        placed.padding[1] = static_cast<float>(cached.clip.y);
        draw(destination, "cached_composite", Blend::premultiplied_alpha,
             arena->upload(&placed, sizeof(placed)),
             clip_constants(batch.rounded_clips, gpu::RoundedClipMode::premultiplied_alpha),
             *cached.output, *cached.output, clip);
        ++telemetry.effect_passes;
    };
    if (CachedEffect* cached = reusable(batch, content, destination)) {
        composite_cached(*cached);
        return;
    }
    // Refreshing reads only the effect's sample region, as on D3D11: a pass writes inside the
    // clipped bounds and samples at most the sample reach beyond them.
    const auto& declared = passes(batch);
    const bool shader_passes =
        std::ranges::any_of(declared, [](const auto& entry) { return entry.second.kind == 1; });
    const bool blur_passes =
        std::ranges::any_of(declared, [](const auto& entry) { return entry.second.kind == 0; });
    Image* current = source;
    Image* sampled_backdrop = backdrop;
    if (!content) {
        const host::Scissor region = sample_region(batch, destination);
        current = &temporary(destination.width, destination.height, destination.format);
        if (source) {
            auto surface = target_of(*source);
            copy(surface, *current, region, static_cast<std::int32_t>(region.x),
                 static_cast<std::int32_t>(region.y));
            source->layout = surface.layout;
            sampled_backdrop = source;
        } else {
            copy(destination, *current, region, static_cast<std::int32_t>(region.x),
                 static_cast<std::int32_t>(region.y));
            // A blur rewrites the capture in place; authored passes still read the raw backdrop.
            sampled_backdrop =
                shader_passes && blur_passes ? &capture(destination, region) : current;
        }
    }
    for (const auto& [index, pass] : declared) {
        (void)index;
        if (pass.kind == 0) {
            double radius = pass.radius;
            auto downsample = pass.downsample;
            if (pass.radius_parameter < batch.parameter_count)
                radius = batch.parameters[pass.radius_parameter];
            if (pass.downsample_parameter < batch.parameter_count)
                downsample = gpu::blur_downsample(batch.parameters[pass.downsample_parameter]);
            auto target = target_of(*current);
            blur(target, clip, std::max(0.0, radius) * pixel_scale(destination), downsample);
            current->layout = target.layout;
        } else if (pass.kind == 1) {
            auto& next = temporary(destination.width, destination.height, destination.format);
            clear(next);
            auto target = target_of(next);
            draw(target, pass.program, Blend::opaque, arena->upload(&constants, sizeof(constants)),
                 {}, *current, sampled_backdrop ? *sampled_backdrop : *current, clip);
            next.layout = target.layout;
            current = &next;
            ++telemetry.effect_passes;
        }
    }
    if (!(batch.refresh_rate > 0)) {
        if (const auto found = cached_effects.find(key); found != cached_effects.end()) {
            retire(std::move(found->second.output));
            cached_effects.erase(found);
        }
        draw(destination, "composite", Blend::premultiplied_alpha,
             arena->upload(&constants, sizeof(constants)),
             clip_constants(batch.rounded_clips, gpu::RoundedClipMode::premultiplied_alpha),
             *current, *current, clip);
        ++telemetry.effect_passes;
        return;
    }
    // Retain only the clipped bounds; the host target itself is never retained.
    auto& cached = cached_effects[key];
    if (!cached.output || cached.output->width != clip.width ||
        cached.output->height != clip.height || cached.output->format != destination.format) {
        retire(std::move(cached.output));
        cached.output =
            std::make_unique<Image>(device, clip.width, clip.height, destination.format);
    }
    auto processed = target_of(*current);
    copy(processed, *cached.output, clip, 0, 0);
    current->layout = processed.layout;
    cached.revision = batch.sample_revision;
    cached.clip = clip;
    cached.time = seconds;
    cached.logical_width = logical_width;
    cached.logical_height = logical_height;
    cached.width = destination.width;
    cached.height = destination.height;
    composite_cached(cached);
}

} // namespace strata::vulkan
