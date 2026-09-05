// Renderer wall time includes command submission and fence completion, excludes readback.
#include "vulkan/offscreen.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <numeric>
#include <strata/render_packet.hpp>
#include <strata/vulkan.hpp>
using namespace strata;
namespace {
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
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
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 3)
            throw std::invalid_argument(
                "usage: strata_vulkan_preparation_benchmark lazy|prepare|cache CACHE_PATH");
        const std::string mode = argv[1];
        if (mode != "lazy" && mode != "prepare" && mode != "cache")
            throw std::invalid_argument("unknown mode");
        vulkan::detail::Offscreen context(false);
        context.resize(1280, 800);
        vulkan::Renderer renderer(context.device);
        renderer.declare_material(
            "bench", "float4 material(PixelInput input) { return float4(input.uv, 0.5, 1); }");
        renderer.declare_effect_pass("tint", 0, STRATA_EFFECT_PASS_SHADER, 0, 1, UINT32_MAX,
                                     UINT32_MAX,
                                     "float4 effect(EffectInput input) { return "
                                     "sampleEffectSource(input.uv) * float4(0.8,1,1,1); }");
        auto packet = quad();
        std::get<host::DrawBatch>(packet.batches[0]).material = "bench";
        std::get<host::DrawBatch>(packet.batches[0]).scissor = {0, 0, 1280, 800};
        host::BlurBatch blur;
        blur.width = blur.height = 32;
        blur.radius = 8;
        blur.downsample = 2;
        blur.scissor = {0, 0, 1280, 800};
        packet.batches.emplace_back(blur);
        host::EffectBatch effect;
        effect.effect = "tint";
        effect.width = effect.height = 32;
        effect.scissor = blur.scissor;
        packet.batches.emplace_back(effect);
        auto start = Clock::now();
        const bool loaded = mode == "cache" && renderer.load_pipeline_cache(argv[2]);
        if (mode != "lazy")
            renderer.prepare(VK_FORMAT_R8G8B8A8_UNORM);
        const double prepare_ms = milliseconds(start);
        const auto before = renderer.pipeline_count();
        auto render = [&](int frame) {
            auto begin = Clock::now();
            (void)renderer.render("benchmark", packet, context.target(32, 32),
                                  {vulkan::TargetLoadAction::clear, {}, frame / 240.0});
            context.image->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            return milliseconds(begin);
        };
        const auto first = render(0);
        const auto created = renderer.pipeline_count() - before;
        packet.full_geometry_payload = false;
        for (int i = 1; i <= 120; ++i)
            (void)render(i);
        std::vector<double> frames;
        for (int i = 121; i <= 720; ++i)
            frames.push_back(render(i));
        std::sort(frames.begin(), frames.end());
        const bool saved = renderer.save_pipeline_cache(argv[2]);
        std::cout << "{\"device\":\"" << context.device_name << "\",\"mode\":\"" << mode
                  << "\",\"cache_loaded\":" << loaded << ",\"cache_saved\":" << saved
                  << ",\"prepare_ms\":" << prepare_ms << ",\"first_render_ms\":" << first
                  << ",\"first_render_new_pipelines\":" << created << ",\"steady_mean_ms\":"
                  << std::accumulate(frames.begin(), frames.end(), 0.0) /
                         static_cast<double>(frames.size())
                  << ",\"steady_p95_ms\":" << frames[569] << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
