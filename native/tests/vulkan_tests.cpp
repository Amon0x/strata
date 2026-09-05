#include "vulkan/offscreen.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
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
    batch.material = "strata:unified_ui";
    batch.blend_mode = "straight_alpha";
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
void run() {
    vulkan::detail::Offscreen context(true);
    context.resize(32, 32);
    std::cout << "Vulkan device: " << context.device_name << "\n";
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
        auto packet = quad();
        near(pixel(render(packet), 32, 16, 16), {255, 0, 0, 255}, "solid quad");
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
        image = render(packet);
        near(pixel(image, 32, 0, 0), {0, 0, 0, 0}, "rounded clip corner");
        near(pixel(image, 32, 16, 16), {255, 0, 0, 255}, "rounded clip center");
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
        host::RenderPacket release;
        release.resources.push_back({host::resource_release, "test.texture"});
        renderer.consume_resources(release);
        packet = quad();
        std::get<host::DrawBatch>(packet.batches[0]).material = "test.material";
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
        effect.effect = "test.effect";
        renderer.declare_effect_pass(
            "test.effect", 0, STRATA_EFFECT_PASS_SHADER, 0, 1, UINT32_MAX, UINT32_MAX,
            "float4 effect(EffectInput input) { float4 c=sampleEffectSource(input.uv); return "
            "float4(c.b,c.r,c.g,c.a); }");
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
        effect.effect = "test.identity";
        effect.backdrop_source = host::EffectBackdropSource::surface;
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
        renderer.release_target();
        context.resize(64, 64);
        auto resized = quad();
        std::get<host::DrawBatch>(resized.batches[0]).scissor = {0, 0, 64, 64};
        (void)renderer.render("resized", resized, context.target(32, 32),
                              {vulkan::TargetLoadAction::clear, {}, 0});
        near(pixel(context.pixels(), 64, 32, 32), {255, 0, 0, 255}, "resize and logical scale");
        // A retained effect respects refresh rate and invalidates on geometry patches.
        renderer.release_target();
        context.resize(32, 32);
        auto cached = quad();
        host::EffectBatch timed;
        timed.width = timed.height = 32;
        timed.scissor = {0, 0, 32, 32};
        timed.effect = "test.clock";
        timed.refresh_rate = 1;
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
        cached.vertex_patches.push_back({20, {0, 255, 0, 255}});
        near(at(0.6), {153, 0, 0, 255}, "geometry patch invalidates effect cache");
        cached.batches.resize(1);
        cached.vertex_patches.clear();
        // The first vertex color patch was uploaded even though the geometry epoch stayed fixed.
        (void)renderer.render("clock", cached, context.target(32, 32),
                              {vulkan::TargetLoadAction::clear, {}, 0});
        const auto patched = pixel(context.pixels(), 32, 0, 0);
        require(patched[1] > 240, "retained vertex patch reaches the GPU");
        renderer.release_layer("clock");
        renderer.release_layer("test");
        renderer.release_target();
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
