#include "vulkan/offscreen.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <optional>
#include <fstream>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>
#include <strata/raster_job.hpp>
#include <strata/render_packet.hpp>
#include <strata/strata.hpp>
#include <strata/vulkan.h>
#include <strata/vulkan.hpp>

namespace {
using namespace strata;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void put(host::RenderPacket& packet, std::size_t vertex, std::size_t offset, float value) {
    std::memcpy(packet.vertices.data() + vertex * 88 + offset, &value, 4);
}
host::RenderPacket quad(std::array<std::uint8_t, 4> color = {255, 0, 0, 255}) {
    host::RenderPacket packet;
    packet.full_geometry_payload = true;
    packet.geometry_epoch = 1;
    packet.vertices.resize(4 * 88);
    const std::array<std::array<float, 2>, 4> positions{{{0, 0}, {32, 0}, {32, 32}, {0, 32}}};
    for (std::size_t i = 0; i < 4; ++i) {
        put(packet, i, 0, positions[i][0]);
        put(packet, i, 4, positions[i][1]);
        put(packet, i, 12, positions[i][0] / 32);
        put(packet, i, 16, positions[i][1] / 32);
        std::memcpy(packet.vertices.data() + i * 88 + 20, color.data(), 4);
        put(packet, i, 24, 32);
        put(packet, i, 28, 32);
        put(packet, i, 84, 1);
    }
    packet.indices = {0, 1, 2, 0, 2, 3};
    host::DrawBatch batch;
    batch.scissor = {0, 0, 32, 32};
    packet.materials = {"strata:unified_ui"};
    batch.material = 0;
    batch.blend = host::DrawBlend::straight_alpha;
    batch.clip_coverage = host::ClipCoverage::hard;
    batch.index_count = 6;
    packet.batches.emplace_back(batch);
    return packet;
}
std::array<int, 4> pixel(const std::vector<std::uint8_t>& pixels, std::uint32_t width,
                         std::uint32_t x, std::uint32_t y) {
    const auto index = (static_cast<std::size_t>(y) * width + x) * 4;
    return {pixels[index], pixels[index + 1], pixels[index + 2], pixels[index + 3]};
}
void near(std::array<int, 4> actual, std::array<int, 4> expected, const char* message,
          int tolerance = 3) {
    for (std::size_t i = 0; i < 4; ++i)
        if (std::abs(actual[i] - expected[i]) > tolerance) {
            std::cerr << message << ": actual " << actual[0] << "," << actual[1] << "," << actual[2]
                      << "," << actual[3] << "\n";
            throw std::runtime_error(message);
        }
}
/**
 * Raster jobs rasterized by the compute kernels match the reference rasterizer: an atlas filled by
 * a job is drawn 1:1 into the target and read back. Coverage must match exactly; a colour
 * composite of solid layers too (its atlas samples bilinearly, at texel centres).
 */
void test_raster_jobs_match_the_reference(vulkan::detail::Offscreen& context) {
    constexpr std::uint32_t width = 150, height = 110, region_width = width + 2,
                            region_height = height + 2;
    context.resize(region_width, region_height);
    vulkan::Renderer renderer(context.device);
    std::vector<host::RasterPoint> wobbly;
    for (int index = 0; index < 600; ++index) {
        const double angle = 6.283185307179586 * index / 600.0;
        const double r = 41.0 * (1.0 + 0.25 * std::sin(angle * 7.0));
        wobbly.push_back({70.3 + std::cos(angle) * r, 55.7 + std::sin(angle) * r * 0.8});
    }
    const std::vector<std::vector<host::RasterPoint>> star{
        {{75.0, 4.0}, {112.0, 104.0}, {16.0, 42.0}, {134.0, 42.0}, {38.0, 104.0}}};
    const std::vector<std::vector<host::RasterPoint>> outside{
        {{-40.0, -30.0}, {190.0, 20.0}, {160.0, 150.0}, {-20.0, 90.0}}};
    const std::vector<std::vector<host::RasterPoint>> blob{wobbly};
    const auto draw = [&](const host::RasterJobBuilder& builder, const bool rgba) {
        const std::uint32_t format = rgba ? host::texture_format_rgba8 : host::texture_format_r8;
        host::RenderPacket packet;
        packet.full_geometry_payload = true;
        packet.geometry_epoch = 1;
        host::ResourceOperation create;
        create.kind = host::resource_create;
        create.texture = "test.raster";
        create.format = format;
        create.width = region_width;
        create.height = region_height;
        host::ResourceOperation raster = create;
        raster.kind = host::resource_raster;
        raster.raster =
            host::decode_raster_job(builder.encode(1U, 1U), region_width, region_height, format);
        packet.resources = {create, raster};
        packet.vertices.resize(4 * 88);
        const std::array<std::array<float, 2>, 4> corners{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        for (std::size_t i = 0; i < 4; ++i) {
            put(packet, i, 0, corners[i][0] * region_width);
            put(packet, i, 4, corners[i][1] * region_height);
            put(packet, i, 12, corners[i][0]);
            put(packet, i, 16, corners[i][1]);
            const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
            std::memcpy(packet.vertices.data() + i * 88 + 20, white.data(), 4);
            put(packet, i, 80, 1); // textured
            put(packet, i, 84, 1);
        }
        packet.indices = {0, 1, 2, 0, 2, 3};
        host::DrawBatch batch;
        batch.scissor = {0, 0, region_width, region_height};
        packet.materials = {"strata:unified_ui"};
        batch.material = 0;
        batch.blend = host::DrawBlend::opaque;
        batch.clip_coverage = host::ClipCoverage::hard;
        batch.index_count = 6;
        batch.texture = "test.raster";
        packet.batches.emplace_back(batch);
        (void)renderer.render("raster", packet, context.target(region_width, region_height),
                              {vulkan::TargetLoadAction::clear, {0, 0, 0, 0}, 0});
        std::vector<std::uint8_t> reference(static_cast<std::size_t>(region_width) *
                                            region_height * (rgba ? 4U : 1U));
        host::rasterize_job(raster.raster, format, region_width, region_height, reference,
                            region_width * (rgba ? 4U : 1U));
        return std::pair{context.pixels(), reference};
    };
    for (const auto& [label, polygons, fill] :
         {std::tuple{"star", star, host::RasterFill::nonzero},
          std::tuple{"even-odd star", star, host::RasterFill::even_odd},
          std::tuple{"clipped quad", outside, host::RasterFill::nonzero},
          std::tuple{"wobbly blob", blob, host::RasterFill::nonzero}}) {
        host::RasterJobBuilder builder(width, height);
        builder.add_layer(fill, host::RasterLayer{}, polygons, host::RasterPoint{0.25, -0.375});
        const auto [pixels, reference] = draw(builder, false);
        std::size_t differing = 0, covered = 0;
        for (std::size_t index = 0; index < reference.size(); ++index) {
            differing += pixels[index * 4] != reference[index] ? 1U : 0U;
            covered += reference[index] != 0 ? 1U : 0U;
        }
        std::cout << "  Vulkan " << label << ": " << differing << " of " << covered
                  << " covered texels differ\n";
        require(covered > 0 && differing == 0, "Vulkan compute coverage differs from the reference");
    }
    host::RasterLayer opaque;
    opaque.paint = host::RasterPaint::solid;
    opaque.color = 0xFF2080F0U;
    host::RasterLayer translucent;
    translucent.paint = host::RasterPaint::solid;
    translucent.color = 0x8040C020U;
    host::RasterJobBuilder builder(width, height);
    builder.add_layer(host::RasterFill::nonzero, opaque, star, {});
    builder.add_layer(host::RasterFill::even_odd, translucent, blob, {});
    builder.add_layer(host::RasterFill::nonzero, translucent, outside, {});
    const auto [pixels, reference] = draw(builder, true);
    int largest = 0;
    for (std::size_t index = 0; index < reference.size(); ++index)
        largest = std::max(largest, std::abs(int{pixels[index]} - int{reference[index]}));
    std::cout << "  Vulkan solid composite: largest difference " << largest << "\n";
    require(largest == 0, "Vulkan compute composite differs from the reference");
}

void run() {
    vulkan::detail::Offscreen context(true);
    std::cout << "Vulkan device: " << context.device_name << "\n";
    test_raster_jobs_match_the_reference(context);
    context.resize(32, 32);
    {
        vulkan::Renderer renderer(context.device);
        auto render = [&](const host::RenderPacket& packet, bool clear = true) {
            auto target = context.target(32, 32);
            (void)renderer.render(
                "test", packet, target,
                {clear ? vulkan::TargetLoadAction::clear : vulkan::TargetLoadAction::preserve,
                 {0, 0, 0, 0},
                 0.25});
            return context.pixels();
        };
        require(renderer.prepare(VK_FORMAT_R8G8B8A8_UNORM) == 9, "prepare builtins");
        require(renderer.prepare(VK_FORMAT_R8G8B8A8_UNORM) == 0, "prepare is idempotent");
        const auto cache = std::filesystem::temp_directory_path() /
            ("strata-vulkan-test-" + std::to_string(reinterpret_cast<std::uintptr_t>(&renderer)));
        struct CacheCleanup { std::filesystem::path path; ~CacheCleanup() {
            std::error_code error; std::filesystem::remove(path, error);
        } } cleanup{cache};
        require(!renderer.load_pipeline_cache(cache), "missing cache ignored");
        require(renderer.save_pipeline_cache(cache), "save pipeline cache");
        {
            vulkan::Renderer restored(context.device);
            require(restored.load_pipeline_cache(cache), "load saved pipeline cache");
            require(restored.prepare(VK_FORMAT_R8G8B8A8_UNORM) == 9, "prepare restored cache");
            (void)restored.render("restored", quad(), context.target(32, 32),
                                  {vulkan::TargetLoadAction::clear, {}, 0});
            near(pixel(context.pixels(), 32, 16, 16), {255, 0, 0, 255}, "restored cache pixels");
        }
        { std::fstream file(cache, std::ios::binary | std::ios::in | std::ios::out);
          file.seekp(-1, std::ios::end); file.put('!'); }
        require(!renderer.load_pipeline_cache(cache), "corrupt cache ignored");
        auto packet = quad();
        near(pixel(render(packet), 32, 16, 16), {255, 0, 0, 255}, "solid quad");
        require(renderer.pipeline_count() == 9, "prepared draw creates no pipelines");
        std::get<host::DrawBatch>(packet.batches[0]).scissor = {0, 0, 16, 32};
        auto image = render(packet);
        near(pixel(image, 32, 24, 16), {0, 0, 0, 0}, "scissor");
        auto preserved = quad({0, 0, 255, 255});
        std::get<host::DrawBatch>(preserved.batches[0]).scissor = {16, 0, 16, 32};
        image = render(preserved, false);
        near(pixel(image, 32, 8, 16), {255, 0, 0, 255}, "host target preserve");
        near(pixel(image, 32, 24, 16), {0, 0, 255, 255}, "host target overlay");
        packet = quad();
        host::RoundedClip clip;
        clip.width = clip.height = 32;
        clip.radii = {12, 12, 12, 12};
        std::get<host::DrawBatch>(packet.batches[0]).rounded_clips.push_back(clip);
        // As the core resolves a clipped straight-alpha draw.
        std::get<host::DrawBatch>(packet.batches[0]).clip_coverage =
            host::ClipCoverage::straight_alpha;
        image = render(packet);
        near(pixel(image, 32, 0, 0), {0, 0, 0, 0}, "rounded clip corner");
        near(pixel(image, 32, 16, 16), {255, 0, 0, 255}, "rounded clip center");
        // Vertex z selects a presentation group, which the vertex stage places and fades.
        packet = quad();
        for (std::size_t i = 0; i < 4; ++i)
            put(packet, i, 8, 1);
        packet.groups = {host::PresentationGroup{}, host::PresentationGroup{0.5, 0.5, 16, 0, 0.5}};
        image = render(packet);
        near(pixel(image, 32, 8, 8), {0, 0, 0, 0}, "presentation group moved its draw");
        near(pixel(image, 32, 24, 24), {0, 0, 0, 0}, "presentation group scaled its draw");
        near(pixel(image, 32, 24, 8), {128, 0, 0, 128}, "presentation group placement and opacity");
        // A texture that never arrived, a material without a program and an undeclared effect
        // fall back as on D3D11 instead of leaving the renderer unable to draw another frame.
        packet = quad({255, 255, 255, 255});
        std::get<host::DrawBatch>(packet.batches[0]).texture = "test.never-uploaded";
        packet.materials = {"test:undeclared-material"};
        packet.effects = {"test:undeclared-effect"};
        host::EffectBatch undeclared;
        undeclared.source_order = 1;
        undeclared.scissor = {0, 0, 32, 32};
        undeclared.width = undeclared.height = 32;
        undeclared.effect = 0;
        packet.batches.emplace_back(undeclared);
        near(pixel(render(packet), 32, 16, 16), {255, 255, 255, 255},
             "missing texture, material and effect fall back");
        near(pixel(render(quad()), 32, 16, 16), {255, 0, 0, 255},
             "renderer draws after a frame with missing resources");
        packet = quad({255, 255, 255, 255});
        packet.resources.push_back({host::resource_create,
                                    "test.texture",
                                    host::texture_format_rgba8,
                                    host::texture_sampling_nearest,
                                    0,
                                    0,
                                    2,
                                    1,
                                    {}});
        packet.resources.push_back({host::resource_upload,
                                    "test.texture",
                                    host::texture_format_rgba8,
                                    host::texture_sampling_nearest,
                                    0,
                                    0,
                                    2,
                                    1,
                                    {0, 255, 0, 255, 0, 0, 255, 255}});
        std::get<host::DrawBatch>(packet.batches[0]).texture = "test.texture";
        for (std::size_t i = 0; i < 4; ++i)
            put(packet, i, 80, 1);
        image = render(packet);
        near(pixel(image, 32, 4, 16), {0, 255, 0, 255}, "texture upload left");
        near(pixel(image, 32, 28, 16), {0, 0, 255, 255}, "texture upload right");
        packet.resources = {{host::resource_upload,
                             "test.texture",
                             host::texture_format_rgba8,
                             host::texture_sampling_nearest,
                             1,
                             0,
                             1,
                             1,
                             {255, 0, 255, 255}}};
        image = render(packet);
        near(pixel(image, 32, 28, 16), {255, 0, 255, 255}, "partial texture update");
        packet.resources.clear();
        image = render(packet);
        near(pixel(image, 32, 4, 16), {0, 255, 0, 255}, "retained resources");
        // Images arrive decoded by the core as straight-alpha RGBA8 and are sampled as uploaded.
        {
            auto decoded = packet;
            decoded.resources = {{host::resource_image,
                                  "test.image",
                                  host::texture_format_rgba8,
                                  host::texture_sampling_nearest,
                                  0,
                                  0,
                                  1,
                                  1,
                                  {0, 255, 128, 255}}};
            std::get<host::DrawBatch>(decoded.batches[0]).texture = "test.image";
            near(pixel(render(decoded), 32, 16, 16), {0, 255, 128, 255},
                 "RGBA8 image texels are sampled as uploaded", 0);
        }
        host::RenderPacket release;
        release.resources.push_back({host::resource_release, "test.texture"});
        renderer.consume_resources(release);
        packet = quad();
        packet.materials = {"test.material"};
        renderer.declare_material("test.material",
                                  "float4 material(PixelInput input) { return float4(0,1,0,1); }");
        near(pixel(render(packet), 32, 16, 16), {0, 255, 0, 255}, "authored HLSL material");
        bool rejected = false;
        try {
            renderer.declare_material("test.material", "this is invalid shader code");
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "invalid HLSL must report an error");
        near(pixel(render(packet), 32, 16, 16), {0, 255, 0, 255},
             "failed reload preserves material");
        renderer.declare_material("test.material",
                                  "float4 material(PixelInput input) { return float4(0,0,1,1); }");
        near(pixel(render(packet), 32, 16, 16), {0, 0, 255, 255}, "shader reload");
        packet = quad();
        host::EffectBatch effect;
        effect.width = effect.height = 32;
        effect.scissor = {0, 0, 32, 32};
        effect.effect = 0;
        renderer.declare_effect_pass(
            "test.effect", 0, STRATA_EFFECT_PASS_SHADER, 0, 1, UINT32_MAX, UINT32_MAX,
            "float4 effect(EffectInput input) { float4 c=sampleEffectSource(input.uv); return "
            "float4(c.b,c.r,c.g,c.a); }");
        packet.effects = {"test.effect"};
        packet.batches.emplace_back(effect);
        near(pixel(render(packet), 32, 16, 16), {0, 255, 0, 255}, "backdrop HLSL effect");
        packet.batches.clear();
        effect.kind = host::EffectBatchKind::content_begin;
        packet.batches.emplace_back(effect);
        auto draw = std::get<host::DrawBatch>(quad().batches[0]);
        packet.batches.emplace_back(draw);
        packet.batches.emplace_back(host::ContentEffectEndBatch{});
        near(pixel(render(packet), 32, 16, 16), {0, 255, 0, 255}, "isolated content effect");
        // Nested content effects compose their shader output in source order.
        packet.batches.insert(packet.batches.begin() + 1, effect);
        packet.batches.emplace_back(host::ContentEffectEndBatch{});
        near(pixel(render(packet), 32, 16, 16), {0, 0, 255, 255}, "nested content effect");
        renderer.declare_effect_pass(
            "test.identity", 0, STRATA_EFFECT_PASS_SHADER, 0, 1, UINT32_MAX, UINT32_MAX,
            "float4 effect(EffectInput input) { return sampleEffectBackdrop(input.uv); }");
        packet = quad();
        effect.kind = host::EffectBatchKind::backdrop;
        effect.effect = 0;
        effect.backdrop_source = host::EffectBackdropSource::surface;
        packet.effects = {"test.identity"};
        packet.batches.emplace_back(effect);
        auto target = context.target(32, 32);
        (void)renderer.render("surface", packet, target,
                              {vulkan::TargetLoadAction::clear, {0, 0, 1, 1}, 0});
        near(pixel(context.pixels(), 32, 16, 16), {0, 0, 255, 255},
             "surface backdrop captured before draws");
        packet = quad();
        std::get<host::DrawBatch>(packet.batches[0]).scissor = {0, 0, 16, 32};
        host::BlurBatch blur;
        blur.width = blur.height = 32;
        blur.scissor = {0, 0, 32, 32};
        blur.radius = 6;
        blur.downsample = 2;
        packet.batches.emplace_back(blur);
        image = render(packet);
        require(pixel(image, 32, 17, 16)[3] > 0 && pixel(image, 32, 17, 16)[3] < 200,
                "blur must spread edge coverage");
        // A downsample parameter rounds to the nearest factor, as on D3D11 and the reference
        // renderer: 2.6 blurs exactly as a declared downsample of 3.
        renderer.declare_effect_pass("test.blur-parameter", 0, STRATA_EFFECT_PASS_BLUR, 6, 1,
                                     UINT32_MAX, 0, {});
        renderer.declare_effect_pass("test.blur-three", 0, STRATA_EFFECT_PASS_BLUR, 6, 3,
                                     UINT32_MAX, UINT32_MAX, {});
        const auto blurred_by = [&](const char* id, std::uint32_t count, double value) {
            auto edge = quad();
            std::get<host::DrawBatch>(edge.batches[0]).scissor = {0, 0, 16, 32};
            host::EffectBatch blurred;
            blurred.source_order = 1;
            blurred.width = blurred.height = 32;
            blurred.scissor = {0, 0, 32, 32};
            blurred.effect = 0;
            blurred.parameter_count = count;
            blurred.parameters[0] = value;
            edge.effects = {id};
            edge.batches.emplace_back(blurred);
            return render(edge);
        };
        const auto parameter_blur = blurred_by("test.blur-parameter", 1, 2.6);
        const auto declared_blur = blurred_by("test.blur-three", 0, 0);
        for (std::uint32_t x = 8; x < 24; ++x)
            near(pixel(parameter_blur, 32, x, 16), pixel(declared_blur, 32, x, 16),
                 "blur downsample parameter rounds to the nearest factor", 1);
        renderer.release_target();
        context.resize(64, 64);
        auto resized = quad();
        std::get<host::DrawBatch>(resized.batches[0]).scissor = {0, 0, 64, 64};
        (void)renderer.render("resized", resized, context.target(32, 32),
                              {vulkan::TargetLoadAction::clear, {}, 0});
        near(pixel(context.pixels(), 64, 32, 32), {255, 0, 0, 255}, "resize and logical scale");
        // A retained effect respects its refresh rate and resamples when its revision moves.
        renderer.release_target();
        context.resize(32, 32);
        auto cached = quad();
        host::EffectBatch timed;
        timed.width = timed.height = 32;
        timed.scissor = {0, 0, 32, 32};
        timed.effect = 0;
        timed.refresh_rate = 1;
        cached.effects = {"test.clock"};
        renderer.declare_effect_pass(
            "test.clock", 0, STRATA_EFFECT_PASS_SHADER, 0, 1, UINT32_MAX, UINT32_MAX,
            "float4 effect(EffectInput input) { return float4(effectTime(),0,0,1); }");
        cached.batches.emplace_back(timed);
        auto at = [&](double seconds) {
            (void)renderer.render("clock", cached, context.target(32, 32),
                                  {vulkan::TargetLoadAction::clear, {}, seconds});
            return pixel(context.pixels(), 32, 16, 16);
        };
        near(at(0.1), {26, 0, 0, 255}, "initial animated effect");
        cached.full_geometry_payload = false;
        near(at(0.5), {26, 0, 0, 255}, "effect refresh cache");
        near(at(1.2), {255, 0, 0, 255}, "effect refresh deadline");
        near(at(0.2), {51, 0, 0, 255}, "effect clock rollback");
        // A decoded patch packet is the step from the previous epoch to the next, and its complete
        // arrays already carry the patch (the decoder applies it in place); the renderer uploads
        // the complete current arrays to whichever frame slot is behind.
        cached.geometry_epoch = 2;
        cached.vertex_patches.push_back({20, {0, 255, 0, 255}});
        std::copy_n(cached.vertex_patches.back().bytes.begin(), 4, cached.vertices.begin() + 20);
        // The decoder finds the patched vertex within what the effect read: a new revision.
        std::get<host::EffectBatch>(cached.batches[1]).sample_revision = 1;
        near(at(0.6), {153, 0, 0, 255}, "a new sample revision invalidates the effect cache");
        cached.batches.resize(1);
        cached.vertex_patches.clear();
        // The retained epoch-2 frame draws the patched geometry the renderer kept.
        (void)renderer.render("clock", cached, context.target(32, 32),
                              {vulkan::TargetLoadAction::clear, {}, 0});
        const auto patched = pixel(context.pixels(), 32, 0, 0);
        require(patched[1] > 240 && patched[0] < 15, "next-epoch vertex patch reaches the GPU");
        // After a skipped epoch the renderer still draws the decoder's complete arrays, never a
        // patch applied to geometry it did not see.
        cached.geometry_epoch = 4;
        cached.vertex_patches.push_back({20, {0, 0, 255, 255}});
        (void)renderer.render("clock", cached, context.target(32, 32),
                              {vulkan::TargetLoadAction::clear, {}, 0});
        const auto replaced = pixel(context.pixels(), 32, 0, 0);
        require(replaced[1] > 240 && replaced[2] < 15,
                "patches after a skipped epoch replace geometry from the complete arrays");
        cached.vertex_patches.clear();
        renderer.release_layer("clock");
        renderer.release_layer("test");
        // Glass over a large target copies and blurs only the region it samples, and nothing on
        // a frame that reuses its retained output.
        renderer.release_target();
        context.resize(512, 512);
        renderer.declare_effect_pass("test.glass", 0, STRATA_EFFECT_PASS_BLUR, 4, 1, UINT32_MAX,
                                     UINT32_MAX, {});
        auto glass = quad();
        const std::array<std::array<float, 2>, 4> half{{{0, 0}, {208, 0}, {208, 512}, {0, 512}}};
        for (std::size_t i = 0; i < 4; ++i) {
            put(glass, i, 0, half[i][0]);
            put(glass, i, 4, half[i][1]);
        }
        std::get<host::DrawBatch>(glass.batches[0]).scissor = {0, 0, 512, 512};
        host::EffectBatch pane;
        pane.source_order = 1;
        pane.x = pane.y = 200;
        pane.width = pane.height = 16;
        pane.scissor = {0, 0, 512, 512};
        pane.effect = 0;
        pane.refresh_rate = 1;
        glass.effects = {"test.glass"};
        glass.batches.emplace_back(pane);
        const auto glass_at = [&](double seconds) {
            const auto measured =
                renderer.render("glass", glass, context.target(512, 512),
                                {vulkan::TargetLoadAction::clear, {0, 0, 0, 1}, seconds});
            return std::pair{measured, context.pixels()};
        };
        const auto [refreshed, fresh] = glass_at(0);
        require(refreshed.blur_passes == 4 && refreshed.copied_pixels > 0 &&
                    refreshed.copied_pixels < 512U * 512U / 4U,
                "a refreshed effect copied more than the region it samples");
        near(pixel(fresh, 512, 190, 208), {255, 0, 0, 255}, "glass left of the pane");
        near(pixel(fresh, 512, 220, 208), {0, 0, 0, 255}, "glass right of the pane");
        const auto edge = pixel(fresh, 512, 208, 208);
        require(edge[0] > 30 && edge[0] < 225, "the pane did not blur the edge beneath it");
        glass.full_geometry_payload = false;
        const auto [reused, retained] = glass_at(0.5);
        require(reused.blur_passes == 0 && reused.copied_pixels == 0,
                "a frame that reuses the effect sample still captured or blurred");
        for (std::uint32_t x = 196; x < 220; x += 3)
            near(pixel(retained, 512, x, 208), pixel(fresh, 512, x, 208),
                 "the retained sample composited elsewhere than it was taken", 1);
        // Only a new sample revision (the decoder's verdict on what changed) resamples it.
        require(glass_at(0.6).first.blur_passes == 0, "an unchanged revision resampled the pane");
        std::get<host::EffectBatch>(glass.batches[1]).sample_revision = 7;
        require(glass_at(0.7).first.blur_passes == 4,
                "a new sample revision reused the pane's stale sample");
        renderer.release_layer("glass");
        renderer.release_target();
        context.resize(32, 32);
    }
    {
        std::int64_t now = 123456;
        RuntimeOptions runtime_options;
        runtime_options.clock = [&now] { return now; };
        Runtime runtime(std::move(runtime_options));
        runtime.configure_application(ApplicationOptions{.id = "vulkan.presenter.test"});
        require(runtime
                    .activate(SourceActivation{
                        .generation = 1,
                        .entry_source_id = "test.strata",
                        .entry_text =
                            "style Root { width: { weight: 1 }; height: { weight: 1 }; background: "
                            "#334155FF; } overlay Main { root Panel(style: Root) }"})
                    .activated(),
                "presenter application activation");
        SurfaceOptions options;
        options.id = "vulkan.presenter.surface";
        options.root_role = SurfaceRootRole::overlay;
        options.root_name = "Main";
        options.environment.framebuffer_width = 32;
        options.environment.framebuffer_height = 32;
        options.environment.logical_width = 32;
        options.environment.logical_height = 32;
        auto surface = runtime.create_surface(options);
        vulkan::Presenter presenter(runtime, context.device);
        require(presenter.prepare(VK_FORMAT_R8G8B8A8_UNORM) == 9, "presenter prepare");
        presenter.attach("presenter", surface);
        const auto frame = presenter.present("presenter", surface, context.target(32, 32), now,
                                             {vulkan::TargetLoadAction::clear, {0, 0, 0, 0}, 0});
        require(frame.surface.frame_index == 1 && frame.packet_bytes > 0,
                "presenter must frame and decode surface");
        near(pixel(context.pixels(), 32, 16, 16), {51, 65, 85, 255},
             "C++ Surface presenter pixels");
        presenter.detach("presenter");
        require(!presenter.attached("presenter"), "presenter detach");
        surface.close();
        presenter.release_target();
        // Exercise the public C lifecycle with a live Runtime, Surface, device, and target.
        options.id = "vulkan.c.surface";
        surface = runtime.create_surface(options);
        strata_vulkan_device gpu{sizeof(strata_vulkan_device), context.device.physical_device,
                                 context.device.device,        context.device.queue,
                                 context.device.queue_family,  0};
        strata_vulkan_presenter* c_presenter = nullptr;
        auto status =
            strata_vulkan_presenter_create(runtime.native_handle(), &gpu, nullptr, &c_presenter);
        if (status.status != STRATA_STATUS_OK)
            throw std::runtime_error(std::string(status.message.data, status.message.size));
        struct PresenterGuard {
            strata_vulkan_presenter* value;
            ~PresenterGuard() { strata_vulkan_presenter_destroy(value); }
        } guard{c_presenter};
        size_t prepared = 0;
        require(strata_vulkan_presenter_prepare(c_presenter, VK_FORMAT_R8G8B8A8_UNORM,
                                                &prepared).status == STRATA_STATUS_OK && prepared == 9,
                "C presenter preparation");
        strata_vulkan_render_target target{sizeof(strata_vulkan_render_target),
                                           context.image->image,
                                           context.image->view,
                                           context.image->format,
                                           context.image->layout,
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           32,
                                           32,
                                           32,
                                           32};
        strata_vulkan_frame_options frame_options{
            sizeof(strata_vulkan_frame_options), STRATA_VULKAN_TARGET_CLEAR, 0, {0, 0, 0, 0}};
        strata_vulkan_presented_frame presented{};
        presented.struct_size = sizeof(presented);
        status = strata_vulkan_presenter_present(c_presenter, view("c"), surface.native_handle(),
                                                 &target, now, &frame_options, &presented);
        require(status.status == STRATA_STATUS_OK, "C Surface presenter frame");
        near(pixel(context.pixels(), 32, 16, 16), {51, 65, 85, 255}, "C Surface presenter pixels");
        require(strata_vulkan_presenter_detach(c_presenter, view("c")).status == STRATA_STATUS_OK,
                "C presenter terminal packet");
        surface.close();
    }
    require(context.validation_errors.load() == 0, "Vulkan validation errors");
    strata_vulkan_presenter* presenter = nullptr;
    require(strata_vulkan_presenter_create(nullptr, nullptr, nullptr, &presenter).status ==
                STRATA_STATUS_INVALID_ARGUMENT,
            "C ABI invalid arguments");
    require(presenter == nullptr, "C ABI must clear failed output handle");
    strata_vulkan_presenter_destroy(nullptr);
}
} // namespace
int main() {
    try {
        run();
        std::cout << "Vulkan pixel and lifecycle tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
