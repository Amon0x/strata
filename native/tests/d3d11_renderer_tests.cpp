#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <strata/d3d11.hpp>
#include <strata/render_packet.hpp>

#include "d3d11/effect_pass.hpp"
#include "gpu/isolation.hpp"
#include "d3d11/raster_pass.hpp"
#include <strata/raster_job.hpp>

namespace {

using Microsoft::WRL::ComPtr;
using namespace strata;

void check(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

void write_float(std::vector<std::uint8_t>& bytes, const std::size_t vertex,
                 const std::size_t offset, const float value) {
    std::memcpy(bytes.data() + vertex * 88U + offset, &value, sizeof(value));
}

/** A 4x4 opaque quad of one colour, as the packet decoder would hand it to the renderer. */
[[nodiscard]] host::RenderPacket quad(const std::uint64_t epoch, const std::uint8_t red,
                                      const std::uint8_t green, const std::uint8_t blue) {
    host::RenderPacket packet;
    packet.geometry_epoch = epoch;
    packet.vertices.resize(4U * 88U);
    constexpr float positions[4][2]{{0.0F, 0.0F}, {4.0F, 0.0F}, {4.0F, 4.0F}, {0.0F, 4.0F}};
    for (std::size_t vertex = 0U; vertex < 4U; ++vertex) {
        write_float(packet.vertices, vertex, 0U, positions[vertex][0]);
        write_float(packet.vertices, vertex, 4U, positions[vertex][1]);
        packet.vertices[vertex * 88U + 20U] = red;
        packet.vertices[vertex * 88U + 21U] = green;
        packet.vertices[vertex * 88U + 22U] = blue;
        packet.vertices[vertex * 88U + 23U] = 255U;
        write_float(packet.vertices, vertex, 84U, 1.0F);
    }
    packet.indices = {0U, 1U, 2U, 0U, 2U, 3U};
    packet.materials = {"strata:unified_ui"};
    packet.batches.emplace_back(host::DrawBatch{
        .source_order = 0U,
        .scissor = host::Scissor{0U, 0U, 4U, 4U},
        .material = 0U,
        .blend = host::DrawBlend::opaque,
        .clip_coverage = host::ClipCoverage::hard,
        .base_vertex = 0U,
        .first_index = 0U,
        .index_count = 6U,
    });
    return packet;
}

struct Fixture final {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> view;
    ComPtr<ID3D11Texture2D> staging;

    explicit Fixture(const std::uint32_t side = 4U) {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0U, nullptr, 0U,
                                     D3D11_SDK_VERSION, &device, nullptr, &context))) {
            throw std::runtime_error("could not create a WARP device");
        }
        D3D11_TEXTURE2D_DESC descriptor{};
        descriptor.Width = side;
        descriptor.Height = side;
        descriptor.MipLevels = 1U;
        descriptor.ArraySize = 1U;
        descriptor.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        descriptor.SampleDesc.Count = 1U;
        descriptor.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(SUCCEEDED(device->CreateTexture2D(&descriptor, nullptr, &texture)) &&
                  SUCCEEDED(device->CreateRenderTargetView(texture.Get(), nullptr, &view)),
              "could not create the render target");
        descriptor.Usage = D3D11_USAGE_STAGING;
        descriptor.BindFlags = 0U;
        descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(SUCCEEDED(device->CreateTexture2D(&descriptor, nullptr, &staging)),
              "could not create the readback texture");
    }

    [[nodiscard]] d3d11::RenderTarget target() const {
        return d3d11::RenderTarget{texture.Get(), view.Get(), 4U, 4U, 4.0, 4.0};
    }

    [[nodiscard]] std::array<int, 4> center() const {
        context->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(SUCCEEDED(context->Map(staging.Get(), 0U, D3D11_MAP_READ, 0U, &mapped)),
              "could not read the render target back");
        const auto* pixel =
            static_cast<const std::uint8_t*>(mapped.pData) + 2U * mapped.RowPitch + 2U * 4U;
        const std::array<int, 4> result{pixel[0], pixel[1], pixel[2], pixel[3]};
        context->Unmap(staging.Get(), 0U);
        return result;
    }
};

// Geometry patches step from the previous epoch only. When a frame fails after the decoder has
// applied its epoch, the next patch packet must not be laid over the older buffer.
void test_geometry_after_a_failed_frame() {
    Fixture fixture;
    d3d11::Renderer renderer(fixture.device.Get(), fixture.context.Get());
    d3d11::FrameOptions options;
    options.load_action = d3d11::TargetLoadAction::clear;

    host::RenderPacket blue = quad(1U, 0U, 0U, 255U);
    blue.full_geometry_payload = true;
    blue.geometry_dirty_all = true;
    static_cast<void>(renderer.render("layer", blue, fixture.target(), options));
    check(fixture.center() == std::array<int, 4>{0, 0, 255, 255}, "the first frame was not drawn");

    host::RenderPacket green = quad(2U, 0U, 255U, 0U);
    green.vertex_patches.push_back(host::GeometryPatch{0U, green.vertices});
    host::ResourceOperation missing_atlas;
    missing_atlas.kind = host::resource_upload;
    missing_atlas.texture = "missing";
    missing_atlas.width = 1U;
    missing_atlas.height = 1U;
    missing_atlas.bytes = {0U};
    green.resources.push_back(missing_atlas);
    bool failed = false;
    try {
        static_cast<void>(renderer.render("layer", green, fixture.target(), options));
    } catch (const std::exception&) {
        failed = true;
    }
    check(failed, "the frame with an impossible upload did not fail");

    host::RenderPacket next = quad(3U, 0U, 255U, 0U);
    next.index_patches.push_back(host::GeometryPatch{0U, {0U, 0U, 0U, 0U}});
    static_cast<void>(renderer.render("layer", next, fixture.target(), options));
    check(fixture.center() == std::array<int, 4>{0, 255, 0, 255},
          "a patch after a failed frame was applied over stale geometry");
}

// A target that cannot be rendered to is reported before a presenter takes the next packet.
void test_target_is_validated_on_its_own() {
    Fixture fixture;
    const d3d11::Renderer renderer(fixture.device.Get(), fixture.context.Get());
    renderer.validate_target(fixture.target());
    d3d11::RenderTarget resized = fixture.target();
    resized.framebuffer_width = 8U;
    bool rejected = false;
    try {
        renderer.validate_target(resized);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "a target whose size disagrees with its texture was accepted");
}

/** Drives one layer frame of rate-limited backdrop effects through the effect renderer. */
void effect_frame(const Fixture& fixture, d3d11::EffectPassRenderer& effects,
                  const std::uint32_t effect_count, const double seconds) {
    std::vector<host::SubmissionBatch> batches;
    for (std::uint32_t order = 0U; order < effect_count; ++order) {
        host::EffectBatch effect;
        effect.source_order = order;
        effect.scissor = host::Scissor{0U, 0U, 4U, 4U};
        effect.width = 4.0;
        effect.height = 4.0;
        effect.effect = 0U;
        effect.refresh_rate = 60.0;
        effect.sample_revision = 1U + order;
        batches.emplace_back(std::move(effect));
    }
    const std::array<std::string, 1U> ids{"fixture.unprogrammed"};
    effects.begin_layer("layer", ids);
    effects.begin_surface(fixture.texture.Get(),
                          effects.surface_backdrop_required("layer", batches, 4.0, 4.0, seconds));
    for (const host::SubmissionBatch& batch : batches) {
        static_cast<void>(effects.apply_backdrop("layer", std::get<host::EffectBatch>(batch),
                                                 fixture.texture.Get(), fixture.view.Get(), 4.0,
                                                 4.0, seconds));
    }
    effects.end_layer("layer");
}

// Retained effect samples follow the effects of the latest frame. An effect that is no longer
// drawn must not keep its sample texture alive for as long as the layer lives.
void test_unused_effect_samples_are_evicted() {
    Fixture fixture;
    d3d11::EffectPassRenderer effects(fixture.device.Get(), fixture.context.Get());
    effects.resize(4U, 4U, DXGI_FORMAT_R8G8B8A8_UNORM);
    effect_frame(fixture, effects, 3U, 0.0);
    check(effects.cached_sample_count("layer") == 3U, "rate-limited effects were not retained");
    effect_frame(fixture, effects, 3U, 0.001);
    check(effects.cached_sample_count("layer") == 3U, "reused effect samples were dropped");
    effect_frame(fixture, effects, 1U, 0.002);
    check(effects.cached_sample_count("layer") == 1U,
          "samples of effects the frame no longer draws were retained");
}

// Images arrive decoded by the core as straight-alpha RGBA8, and are sampled exactly as uploaded.
// Isolated layers nest past any fixed depth without a framebuffer-sized target per level: the
// innermost few draw into whole targets and the rest park at their read bounds. Later frames
// reuse those targets rather than add more, and layer renders that open no layers release them.
void test_deep_isolation_is_bounded() {
    constexpr std::uint32_t side = 512U;
    constexpr std::size_t depth = 50U;
    Fixture fixture(side);
    d3d11::EffectPassRenderer effects(fixture.device.Get(), fixture.context.Get());
    effects.resize(side, side, DXGI_FORMAT_R8G8B8A8_UNORM);
    std::vector<host::SubmissionBatch> batches;
    for (std::size_t level = 0U; level < depth; ++level) {
        host::EffectBatch layer;
        layer.kind = host::EffectBatchKind::content_begin;
        layer.source_order = static_cast<std::uint32_t>(level);
        layer.scissor = host::Scissor{0U, 0U, side, side};
        layer.x = layer.y = static_cast<double>(level) * 5.0;
        layer.width = layer.height = side - static_cast<double>(level) * 10.0;
        batches.emplace_back(std::move(layer));
    }
    for (std::size_t level = 0U; level < depth; ++level)
        batches.emplace_back(host::ContentEffectEndBatch{});
    const std::array<std::string, 1U> ids{"fixture.layer"};
    const auto frame = [&](const std::span<const host::SubmissionBatch> drawn) {
        effects.begin_layer("layer", ids);
        effects.begin_surface(fixture.texture.Get(), false);
        effects.begin_content_layers(drawn, {fixture.texture.Get(), fixture.view.Get()}, side,
                                     side);
        std::vector<const host::EffectBatch*> open;
        for (const host::SubmissionBatch& batch : drawn) {
            if (const auto* layer = std::get_if<host::EffectBatch>(&batch)) {
                open.push_back(layer);
                check(effects.begin_content().target != nullptr, "a nested layer had no target");
                // Something is drawn into every level, so each one parked keeps its pixels.
                effects.touch_content();
            } else {
                static_cast<void>(
                    effects.finish_content("layer", *open.back(), side, side, 0.0));
                open.pop_back();
            }
        }
        check(effects.active_content().target == fixture.view.Get(),
              "the outermost layer did not composite into the surface");
        effects.end_layer("layer");
    };
    frame(batches);
    const d3d11::IsolationTargetTelemetry first = effects.isolation_targets();
    check(first.framebuffer_targets == gpu::resident_content_layers,
          "nested layers held more framebuffer-sized targets than the resident count");
    check(first.parked_targets == depth - gpu::resident_content_layers,
          "every level past the resident count was not parked once");
    check(first.parked_pixels < first.parked_targets * std::uint64_t{side} * side * 3U / 4U,
          "parked layers were not sized to their read bounds");
    frame(batches);
    const d3d11::IsolationTargetTelemetry second = effects.isolation_targets();
    check(second.framebuffer_targets == first.framebuffer_targets &&
              second.parked_targets == first.parked_targets &&
              second.parked_pixels == first.parked_pixels,
          "a repeated frame did not reuse its isolation targets");
    for (std::uint64_t render = 0U; render <= gpu::isolation_target_lifetime; ++render)
        frame({});
    const d3d11::IsolationTargetTelemetry idle = effects.isolation_targets();
    check(idle.framebuffer_targets == 0U && idle.parked_targets == 0U,
          "isolation targets unused for their lifetime were kept");
}

void test_image_texels_are_sampled_as_uploaded() {
    Fixture fixture;
    d3d11::Renderer renderer(fixture.device.Get(), fixture.context.Get());
    d3d11::FrameOptions options;
    options.load_action = d3d11::TargetLoadAction::clear;
    host::RenderPacket packet = quad(1U, 255U, 255U, 255U);
    packet.full_geometry_payload = true;
    packet.geometry_dirty_all = true;
    for (std::size_t vertex = 0U; vertex < 4U; ++vertex) {
        float position[2]{};
        std::memcpy(position, packet.vertices.data() + vertex * 88U, sizeof(position));
        write_float(packet.vertices, vertex, 12U, position[0] / 4.0F);
        write_float(packet.vertices, vertex, 16U, position[1] / 4.0F);
        write_float(packet.vertices, vertex, 80U, 1.0F);
    }
    std::get<host::DrawBatch>(packet.batches[0]).texture = "image";
    host::ResourceOperation image;
    image.kind = host::resource_image;
    image.texture = "image";
    image.format = host::texture_format_rgba8;
    image.sampling = host::texture_sampling_nearest;
    image.width = 1U;
    image.height = 1U;
    image.bytes = {0U, 255U, 128U, 255U};
    packet.resources.push_back(std::move(image));
    static_cast<void>(renderer.render("image", packet, fixture.target(), options));
    check(fixture.center() == std::array<int, 4>{0, 255, 128, 255},
          "an RGBA8 image texture was not sampled as uploaded");
}

/** A polygon of `count` points on an ellipse, its radius modulated by `wobble` lobes. */
[[nodiscard]] std::vector<host::RasterPoint> blob(const double x, const double y, const double radius,
                                                  const std::size_t count, const double wobble) {
    std::vector<host::RasterPoint> points;
    for (std::size_t index = 0U; index < count; ++index) {
        const double angle = 6.283185307179586 * static_cast<double>(index) / count;
        const double r = radius * (1.0 + 0.25 * std::sin(angle * wobble));
        points.push_back(host::RasterPoint{x + std::cos(angle) * r, y + std::sin(angle) * r * 0.8});
    }
    return points;
}

/**
 * Raster jobs rasterized by compute shaders match the reference rasterizer texel for texel:
 * coverage exactly, and colour composites exactly but for gradients, whose parameter is float.
 */
void test_raster_jobs_match_the_reference(const D3D_DRIVER_TYPE driver, const std::string_view name) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, driver, nullptr, 0U, nullptr, 0U, D3D11_SDK_VERSION,
                                 &device, nullptr, &context))) {
        std::cout << "  " << name << " device unavailable; raster parity skipped\n";
        return;
    }
    using host::RasterPoint;
    constexpr std::uint32_t width = 150U;
    constexpr std::uint32_t height = 110U;
    constexpr std::uint32_t region_width = width + 2U;
    constexpr std::uint32_t region_height = height + 2U;
    const std::vector<std::vector<RasterPoint>> star{{{75.0, 4.0}, {112.0, 104.0},
                                                      {16.0, 42.0}, {134.0, 42.0},
                                                      {38.0, 104.0}}};
    // Reaching past every side of the content, with edges far longer than a piece.
    const std::vector<std::vector<RasterPoint>> outside{{{-40.0, -30.0}, {190.0, 20.0},
                                                         {160.0, 150.0}, {-20.0, 90.0}}};
    const std::vector<std::vector<RasterPoint>> thin{{{10.2, 100.1}, {140.7, 8.3},
                                                      {141.1, 8.9}, {10.6, 100.7}}};
    const std::vector<std::vector<RasterPoint>> wobbly{blob(70.3, 55.7, 41.0, 700U, 7.0),
                                                       blob(82.1, 50.2, 18.0, 90U, 3.0)};
    const auto read = [&](ID3D11Texture2D* const texture, const std::uint32_t channels) {
        D3D11_TEXTURE2D_DESC descriptor{};
        texture->GetDesc(&descriptor);
        descriptor.Usage = D3D11_USAGE_STAGING;
        descriptor.BindFlags = 0U;
        descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(SUCCEEDED(device->CreateTexture2D(&descriptor, nullptr, &staging)),
              "could not create the raster readback texture");
        context->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(SUCCEEDED(context->Map(staging.Get(), 0U, D3D11_MAP_READ, 0U, &mapped)),
              "could not read the raster atlas back");
        std::vector<std::uint8_t> result(static_cast<std::size_t>(region_width) * region_height *
                                         channels);
        for (std::uint32_t row = 0U; row < region_height; ++row) {
            std::memcpy(result.data() + static_cast<std::size_t>(row) * region_width * channels,
                        static_cast<const std::uint8_t*>(mapped.pData) + row * mapped.RowPitch,
                        static_cast<std::size_t>(region_width) * channels);
        }
        context->Unmap(staging.Get(), 0U);
        return result;
    };
    const auto rasterize = [&](const host::RasterJobBuilder& builder, const bool rgba) {
        const std::uint32_t channels = rgba ? 4U : 1U;
        const std::uint32_t format = rgba ? host::texture_format_rgba8
                                          : host::texture_format_r8_filtered;
        const host::RasterJob job =
            host::decode_raster_job(builder.encode(1U, 1U), region_width, region_height, format);
        std::vector<std::uint8_t> reference(static_cast<std::size_t>(region_width) * region_height *
                                            channels);
        host::rasterize_job(job, format, region_width, region_height, reference,
                            static_cast<std::size_t>(region_width) * channels);
        D3D11_TEXTURE2D_DESC descriptor{};
        descriptor.Width = region_width;
        descriptor.Height = region_height;
        descriptor.MipLevels = 1U;
        descriptor.ArraySize = 1U;
        descriptor.Format = rgba ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8_UNORM;
        descriptor.SampleDesc.Count = 1U;
        descriptor.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11UnorderedAccessView> writer;
        check(SUCCEEDED(device->CreateTexture2D(&descriptor, nullptr, &texture)) &&
                  SUCCEEDED(device->CreateUnorderedAccessView(texture.Get(), nullptr, &writer)),
              "could not create the raster atlas");
        d3d11::RasterPass pass(device.Get(), context.Get());
        // Twice: the second run must find the work buffers cleared by the first.
        pass.run(job, rgba, writer.Get(), 0U, 0U, region_width, region_height);
        pass.run(job, rgba, writer.Get(), 0U, 0U, region_width, region_height);
        return std::pair{read(texture.Get(), channels), reference};
    };

    for (const auto& [label, polygons, fill] :
         {std::tuple{"star", star, host::RasterFill::nonzero},
          std::tuple{"even-odd star", star, host::RasterFill::even_odd},
          std::tuple{"clipped quad", outside, host::RasterFill::nonzero},
          std::tuple{"thin line", thin, host::RasterFill::nonzero},
          std::tuple{"wobbly blobs", wobbly, host::RasterFill::even_odd}}) {
        host::RasterJobBuilder builder(width, height);
        builder.add_layer(fill, host::RasterLayer{}, polygons, RasterPoint{0.25, -0.375});
        const auto [gpu, reference] = rasterize(builder, false);
        std::size_t differing = 0U;
        std::size_t covered = 0U;
        for (std::size_t index = 0U; index < gpu.size(); ++index) {
            differing += gpu[index] != reference[index] ? 1U : 0U;
            covered += reference[index] != 0U ? 1U : 0U;
        }
        std::cout << "  " << name << " " << label << ": " << differing << " of " << covered
                  << " covered texels differ\n";
        check(covered > 0U && differing == 0U,
              std::string(name) + " compute coverage differs from the reference: " + label);
    }

    // Strokes the kernel expands in float: within a level or so of the reference along edges.
    std::vector<host::RasterStrokeRun> runs{{blob(70.3, 55.7, 41.0, 300U, 5.0), true},
                                            {{{8.0, 100.0}, {40.0, 20.0}, {60.0, 95.0},
                                              {90.0, 15.0}, {146.0, 60.0}},
                                             false}};
    for (const auto& [label, stroke] :
         {std::pair{"round stroke", host::RasterStroke{3.5F, host::RasterJoin::round,
                                                       host::RasterCap::round, 4.0F, 0.03F}},
          std::pair{"mitred stroke", host::RasterStroke{2.0F, host::RasterJoin::miter,
                                                        host::RasterCap::square, 4.0F, 0.03F}},
          std::pair{"bevelled hairline", host::RasterStroke{0.35F, host::RasterJoin::bevel,
                                                            host::RasterCap::butt, 4.0F, 0.03F}}}) {
        host::RasterJobBuilder builder(width, height);
        builder.add_stroke_layer(host::RasterLayer{}, stroke, runs, RasterPoint{0.25, -0.375});
        const auto [gpu, reference] = rasterize(builder, false);
        int largest = 0;
        std::size_t off = 0U;
        std::size_t covered = 0U;
        for (std::size_t index = 0U; index < gpu.size(); ++index) {
            const int difference = std::abs(int{gpu[index]} - int{reference[index]});
            largest = std::max(largest, difference);
            off += difference > 2 ? 1U : 0U;
            covered += reference[index] != 0U ? 1U : 0U;
        }
        std::cout << "  " << name << " " << label << ": " << off << " of " << covered
                  << " covered texels off by more than 2, largest " << largest << "\n";
        check(covered > 0U && off == 0U && largest <= 4,
              std::string(name) + " compute stroke differs from the reference: " + label);
    }

    // Colour: an opaque star in front of a translucent blob, over gradients.
    host::RasterLayer opaque;
    opaque.paint = host::RasterPaint::solid;
    opaque.color = 0xFF2080F0U;
    host::RasterLayer translucent;
    translucent.paint = host::RasterPaint::solid;
    translucent.color = 0x8040C020U;
    host::RasterLayer linear;
    linear.paint = host::RasterPaint::linear;
    linear.parameters = {0.0071F, 0.0023F, -0.1F, 0.0F, 0.0F, 0.0F};
    linear.extend = host::RasterExtend::repeat;
    host::RasterLayer radial;
    radial.paint = host::RasterPaint::radial;
    radial.parameters = {0.013F, 0.0F, -0.9F, 0.0F, 0.017F, -0.8F};
    radial.extend = host::RasterExtend::mirror;
    for (host::RasterLayer* layer : {&linear, &radial}) {
        layer->lut.resize(host::raster_lut_size);
        for (std::uint32_t index = 0U; index < host::raster_lut_size; ++index)
            layer->lut[index] = (255U << 24U) | (index << 16U) | ((255U - index) << 8U) | 40U;
    }
    for (const bool gradients : {false, true}) {
        host::RasterJobBuilder builder(width, height);
        builder.add_layer(host::RasterFill::nonzero, opaque, star, RasterPoint{});
        builder.add_layer(host::RasterFill::even_odd, translucent, wobbly, RasterPoint{});
        if (gradients) {
            builder.add_layer(host::RasterFill::nonzero, linear, outside, RasterPoint{});
            builder.add_layer(host::RasterFill::nonzero, radial, thin, RasterPoint{});
        } else {
            builder.add_layer(host::RasterFill::nonzero, translucent, outside, RasterPoint{});
        }
        const auto [gpu, reference] = rasterize(builder, true);
        int largest = 0;
        std::size_t differing = 0U;
        for (std::size_t index = 0U; index < gpu.size(); ++index) {
            const int difference = std::abs(int{gpu[index]} - int{reference[index]});
            largest = std::max(largest, difference);
            differing += difference != 0 ? 1U : 0U;
        }
        std::cout << "  " << name << (gradients ? " gradient" : " solid") << " composite: "
                  << differing << " channels differ, largest by " << largest << "\n";
        check(gradients ? largest <= 2 : differing == 0U,
              std::string(name) + " compute composite differs from the reference");
    }
}

/**
 * Art: a GPU program generates an outline's points, which the kernels fill or stroke as they
 * would the same points sent from the CPU. Compiled in the background, it draws nothing until
 * ready; then its coverage matches the CPU rasterizing the same points, but for float rounding.
 */
void test_art_matches_the_same_points(const D3D_DRIVER_TYPE driver, const std::string_view name) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, driver, nullptr, 0U, nullptr, 0U, D3D11_SDK_VERSION,
                                 &device, nullptr, &context))) {
        std::cout << "  " << name << " device unavailable; art skipped\n";
        return;
    }
    constexpr std::uint32_t size = 120U;
    constexpr std::uint32_t region = size + 2U;
    constexpr std::uint32_t count = 360U;
    // A rose: r = 0.3 + 0.12 cos(5 t), about the centre, in canvas space.
    const std::string program =
        "float2 artPoint(uint index, uint count) {\n"
        "    float t = 6.28318530718 * index / count;\n"
        "    float r = artParameter(0) + artParameter(1) * cos(5.0 * t);\n"
        "    return float2(0.5 + r * cos(t), 0.5 + r * sin(t));\n"
        "}\n";
    std::vector<host::RasterPoint> points;
    for (std::uint32_t index = 0U; index < count; ++index) {
        const double angle = 6.28318530718 * index / count;
        const double r = 0.3 + 0.12 * std::cos(5.0 * angle);
        points.push_back({(0.5 + r * std::cos(angle)) * size, (0.5 + r * std::sin(angle)) * size});
    }
    D3D11_TEXTURE2D_DESC descriptor{};
    descriptor.Width = region;
    descriptor.Height = region;
    descriptor.MipLevels = 1U;
    descriptor.ArraySize = 1U;
    descriptor.Format = DXGI_FORMAT_R8_UNORM;
    descriptor.SampleDesc.Count = 1U;
    descriptor.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11UnorderedAccessView> writer;
    check(SUCCEEDED(device->CreateTexture2D(&descriptor, nullptr, &texture)) &&
              SUCCEEDED(device->CreateUnorderedAccessView(texture.Get(), nullptr, &writer)),
          "could not create the art atlas");
    descriptor.Usage = D3D11_USAGE_STAGING;
    descriptor.BindFlags = 0U;
    descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    check(SUCCEEDED(device->CreateTexture2D(&descriptor, nullptr, &staging)),
          "could not create the art readback texture");
    const auto read = [&] {
        context->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(SUCCEEDED(context->Map(staging.Get(), 0U, D3D11_MAP_READ, 0U, &mapped)),
              "could not read the art atlas back");
        std::vector<std::uint8_t> result(static_cast<std::size_t>(region) * region);
        for (std::uint32_t row = 0U; row < region; ++row)
            std::memcpy(result.data() + row * region,
                        static_cast<const std::uint8_t*>(mapped.pData) + row * mapped.RowPitch,
                        region);
        context->Unmap(staging.Get(), 0U);
        return result;
    };
    d3d11::RasterPass pass(device.Get(), context.Get());
    host::RasterStroke stroke{2.5F, host::RasterJoin::round, host::RasterCap::round, 4.0F, 0.03F};
    for (const bool stroked : {false, true}) {
        host::RasterArt art;
        art.source = program;
        art.count = count;
        art.closed = true;
        art.stroked = stroked;
        art.width = static_cast<float>(size);
        art.height = static_cast<float>(size);
        art.parameters[0] = 0.3F;
        art.parameters[1] = 0.12F;
        host::RasterJobBuilder art_builder(size, size);
        art_builder.add_art_layer(host::RasterFill::nonzero, host::RasterLayer{}, stroke, art);
        const host::RasterJob job = host::decode_raster_job(art_builder.encode(1U, 1U), region,
                                                            region, host::texture_format_r8_filtered);
        // The program compiles in the background: until then the region is left empty.
        std::vector<std::uint8_t> gpu;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (;;) {
            pass.run(job, false, writer.Get(), 0U, 0U, region, region, 1.0);
            gpu = read();
            if (std::ranges::any_of(gpu, [](const std::uint8_t value) { return value != 0U; }) ||
                std::chrono::steady_clock::now() > deadline) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        host::RasterJobBuilder builder(size, size);
        if (stroked) {
            const std::vector<host::RasterStrokeRun> runs{{points, true}};
            builder.add_stroke_layer(host::RasterLayer{}, stroke, runs, {});
        } else {
            const std::vector<std::vector<host::RasterPoint>> polygons{points};
            builder.add_layer(host::RasterFill::nonzero, host::RasterLayer{}, polygons, {});
        }
        const host::RasterJob reference_job = host::decode_raster_job(
            builder.encode(1U, 1U), region, region, host::texture_format_r8_filtered);
        std::vector<std::uint8_t> reference(static_cast<std::size_t>(region) * region);
        host::rasterize_job(reference_job, host::texture_format_r8_filtered, region, region,
                            reference, region);
        int largest = 0;
        std::size_t off = 0U;
        std::size_t covered = 0U;
        for (std::size_t index = 0U; index < gpu.size(); ++index) {
            const int difference = std::abs(int{gpu[index]} - int{reference[index]});
            largest = std::max(largest, difference);
            off += difference > 4 ? 1U : 0U;
            covered += reference[index] != 0U ? 1U : 0U;
        }
        std::cout << "  " << name << (stroked ? " stroked" : " filled") << " art: " << off
                  << " of " << covered << " covered texels off by more than 4, largest "
                  << largest << "\n";
        check(covered > 0U && off * 100U <= covered && largest <= 32,
              std::string(name) + " art differs from the same points drawn from the CPU");
    }
}

// Compiled shaders persist: a renderer made with a shader cache keeps its bytecode there, and one
// made after it reads that back, compiling nothing, and draws the same.
void test_shaders_are_kept_between_renderers() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("strata-shader-cache-test-" + std::to_string(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    d3d11::set_shader_cache_directory(directory);
    const auto entries = [&directory] {
        std::size_t count = 0U;
        std::error_code listing;
        for (const auto& entry : std::filesystem::directory_iterator(directory, listing))
            count += entry.path().extension() == ".dxbc" ? 1U : 0U;
        return count;
    };
    const auto draw = [] {
        Fixture fixture;
        d3d11::Renderer renderer(fixture.device.Get(), fixture.context.Get());
        d3d11::FrameOptions options;
        options.load_action = d3d11::TargetLoadAction::clear;
        host::RenderPacket blue = quad(1U, 0U, 0U, 255U);
        blue.full_geometry_payload = true;
        blue.geometry_dirty_all = true;
        static_cast<void>(renderer.render("layer", blue, fixture.target(), options));
        return fixture.center();
    };
    check(draw() == std::array<int, 4>{0, 0, 255, 255}, "a renderer with a shader cache drew wrong");
    const std::size_t kept = entries();
    check(kept >= 2U, "a renderer with a shader cache kept no compiled shaders");
    check(draw() == std::array<int, 4>{0, 0, 255, 255},
          "a renderer made from cached shaders drew wrong");
    check(entries() == kept, "a second renderer compiled shaders the cache already held");
    d3d11::set_shader_cache_directory({});
    std::filesystem::remove_all(directory, error);
}

} // namespace

int main() {
    try {
        test_art_matches_the_same_points(D3D_DRIVER_TYPE_HARDWARE, "hardware");
        test_art_matches_the_same_points(D3D_DRIVER_TYPE_WARP, "WARP");
        test_raster_jobs_match_the_reference(D3D_DRIVER_TYPE_WARP, "WARP");
        test_raster_jobs_match_the_reference(D3D_DRIVER_TYPE_HARDWARE, "hardware");
        test_geometry_after_a_failed_frame();
        test_target_is_validated_on_its_own();
        test_unused_effect_samples_are_evicted();
        test_deep_isolation_is_bounded();
        test_image_texels_are_sampled_as_uploaded();
        test_shaders_are_kept_between_renderers();
        std::cout << "strata_d3d11_renderer_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_d3d11_renderer_tests: " << error.what() << '\n';
        return 1;
    }
}
