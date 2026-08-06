#include <strata/render_packet.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "image_codec.hpp"
#ifdef _WIN32
#include "d3d11_renderer.hpp"
#endif
#include "software_renderer.hpp"

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

[[nodiscard]] std::array<std::uint8_t, 4U>
center(const strata::headless::CaptureRenderer& renderer) {
    const std::span<const std::uint8_t> pixels = renderer.pixels();
    const std::size_t offset =
        (static_cast<std::size_t>(renderer.height() / 2U) * renderer.width() +
         renderer.width() / 2U) * 4U;
    return {
        pixels[offset],
        pixels[offset + 1U],
        pixels[offset + 2U],
        pixels[offset + 3U],
    };
}

void write_float(
    std::vector<std::uint8_t>& bytes,
    const std::size_t vertex,
    const std::size_t offset,
    const float value
) {
    constexpr std::size_t vertex_size = 88U;
    std::memcpy(
        bytes.data() + vertex * vertex_size + offset,
        &value,
        sizeof(value)
    );
}

[[nodiscard]] strata::host::RenderPacket surface_backdrop_packet(
    const strata::host::EffectBackdropSource source,
    const std::uint64_t geometry_epoch
) {
    using namespace strata;
    constexpr std::size_t vertex_size = 88U;
    host::RenderPacket packet;
    packet.geometry_epoch = geometry_epoch;
    packet.vertices.resize(4U * vertex_size);
    constexpr std::array<std::array<float, 2U>, 4U> positions{{
        {0.0F, 0.0F},
        {4.0F, 0.0F},
        {4.0F, 4.0F},
        {0.0F, 4.0F},
    }};
    for (std::size_t vertex = 0U; vertex < positions.size(); ++vertex) {
        write_float(packet.vertices, vertex, 0U, positions[vertex][0U]);
        write_float(packet.vertices, vertex, 4U, positions[vertex][1U]);
        packet.vertices[vertex * vertex_size + 22U] = 255U;
        packet.vertices[vertex * vertex_size + 23U] = 255U;
        write_float(packet.vertices, vertex, 84U, 1.0F);
    }
    packet.indices = {0U, 1U, 2U, 0U, 2U, 3U};
    packet.batches.emplace_back(host::DrawBatch{
        .source_order = 0U,
        .scissor = host::Scissor{0U, 0U, 4U, 4U},
        .material = "strata:unified_ui",
        .blend_mode = "opaque",
        .base_vertex = 0U,
        .first_index = 0U,
        .index_count = 6U,
    });
    host::EffectBatch effect;
    effect.backdrop_source = source;
    effect.source_order = 1U;
    effect.scissor = host::Scissor{0U, 0U, 4U, 4U};
    effect.width = 4.0;
    effect.height = 4.0;
    effect.effect = "fixture.identity";
    effect.refresh_rate = 100.0;
    packet.batches.emplace_back(std::move(effect));
    return packet;
}

[[nodiscard]] strata::host::RenderPacket nested_surface_backdrop_packet(
    const strata::host::EffectBackdropSource source,
    const std::uint64_t geometry_epoch
) {
    using namespace strata;
    constexpr std::size_t vertex_size = 88U;
    host::RenderPacket packet = surface_backdrop_packet(source, geometry_epoch);
    packet.batches.clear();
    for (std::size_t vertex = 0U; vertex < 4U; ++vertex) {
        packet.vertices[vertex * vertex_size + 21U] = 255U;
        packet.vertices[vertex * vertex_size + 22U] = 0U;
    }
    const std::vector<std::uint8_t> child_vertices(
        packet.vertices.begin(),
        packet.vertices.begin() + static_cast<std::ptrdiff_t>(4U * vertex_size)
    );
    packet.vertices.insert(
        packet.vertices.end(),
        child_vertices.begin(),
        child_vertices.end()
    );
    for (std::size_t vertex = 4U; vertex < 8U; ++vertex) {
        packet.vertices[vertex * vertex_size + 21U] = 0U;
        packet.vertices[vertex * vertex_size + 22U] = 255U;
    }
    packet.indices.insert(packet.indices.end(), {0U, 1U, 2U, 0U, 2U, 3U});
    packet.batches.emplace_back(host::DrawBatch{
        .source_order = 0U,
        .scissor = host::Scissor{0U, 0U, 4U, 4U},
        .material = "strata:unified_ui",
        .blend_mode = "opaque",
        .base_vertex = 0U,
        .first_index = 0U,
        .index_count = 6U,
    });
    host::EffectBatch content;
    content.kind = host::EffectBatchKind::content_begin;
    content.source_order = 1U;
    content.scissor = host::Scissor{0U, 0U, 4U, 4U};
    content.width = 4.0;
    content.height = 4.0;
    content.effect = "fixture.identity";
    content.refresh_rate = 0.0;
    packet.batches.emplace_back(content);
    packet.batches.emplace_back(host::DrawBatch{
        .source_order = 2U,
        .scissor = host::Scissor{0U, 0U, 4U, 4U},
        .material = "strata:unified_ui",
        .blend_mode = "opaque",
        .base_vertex = 4U,
        .first_index = 6U,
        .index_count = 6U,
    });
    host::EffectBatch backdrop;
    backdrop.backdrop_source = source;
    backdrop.source_order = 3U;
    backdrop.scissor = host::Scissor{0U, 0U, 4U, 4U};
    backdrop.width = 4.0;
    backdrop.height = 4.0;
    backdrop.effect = "fixture.identity";
    backdrop.refresh_rate = 0.0;
    packet.batches.emplace_back(backdrop);
    packet.batches.emplace_back(host::ContentEffectEndBatch{
        4U,
        host::Scissor{0U, 0U, 4U, 4U},
        {},
    });
    return packet;
}

void test_surface_backdrop_source(strata::headless::CaptureRenderer& renderer) {
    using namespace strata;
    renderer.resize(4U, 4U, 4.0, 4.0);
    renderer.set_clear_color({255U, 0U, 0U, 255U});

    renderer.render(
        surface_backdrop_packet(host::EffectBackdropSource::current, 1U),
        0
    );
    check(center(renderer) == std::array<std::uint8_t, 4U>{0U, 0U, 255U, 255U},
          "CURRENT backdrop source did not sample already-rendered Surface content");

    renderer.render(
        surface_backdrop_packet(host::EffectBackdropSource::surface, 1U),
        1
    );
    check(center(renderer) == std::array<std::uint8_t, 4U>{255U, 0U, 0U, 255U},
          "SURFACE backdrop source did not restore the incoming Surface framebuffer");

    renderer.set_clear_color({0U, 255U, 0U, 255U});
    renderer.render(
        surface_backdrop_packet(host::EffectBackdropSource::surface, 1U),
        2'000'000
    );
    check(center(renderer) == std::array<std::uint8_t, 4U>{255U, 0U, 0U, 255U},
          "rate-limited SURFACE backdrop did not reuse its retained sample");

    renderer.resize(8U, 8U, 8.0, 8.0);
    renderer.resize(4U, 4U, 4.0, 4.0);
    renderer.render(
        surface_backdrop_packet(host::EffectBackdropSource::surface, 1U),
        3'000'000
    );
    check(center(renderer) == std::array<std::uint8_t, 4U>{0U, 255U, 0U, 255U},
          "target replacement retained a stale SURFACE backdrop sample");
}

void test_surface_backdrop_inside_content(strata::headless::CaptureRenderer& renderer) {
    using namespace strata;
    renderer.resize(4U, 4U, 4.0, 4.0);
    renderer.set_clear_color({255U, 0U, 0U, 255U});
    renderer.render(
        nested_surface_backdrop_packet(host::EffectBackdropSource::current, 3U),
        0
    );
    check(center(renderer) == std::array<std::uint8_t, 4U>{0U, 0U, 255U, 255U},
          "CURRENT backdrop inside CONTENT did not sample the isolated subtree");
    renderer.render(
        nested_surface_backdrop_packet(host::EffectBackdropSource::surface, 4U),
        1
    );
    check(center(renderer) == std::array<std::uint8_t, 4U>{255U, 0U, 0U, 255U},
          "SURFACE backdrop inside CONTENT did not sample the incoming framebuffer");
}

void test_temporal_refresh_contract() {
    using namespace strata;
    headless::SoftwareRenderer renderer(headless::platform_image_codec());
    renderer.resize(4U, 4U, 4.0, 4.0);
    renderer.declare_effect_pass("fixture.blur", 0U, 0U, 1.5, 1U, UINT32_MAX, UINT32_MAX, {});

    host::EffectBatch effect;
    effect.source_order = 0U;
    effect.scissor = host::Scissor{0U, 0U, 4U, 4U};
    effect.width = 4.0;
    effect.height = 4.0;
    effect.effect = "fixture.blur";
    effect.refresh_rate = 100.0;

    host::RenderPacket packet;
    packet.geometry_epoch = 1U;
    packet.batches.emplace_back(effect);

    renderer.set_clear_color({255U, 0U, 0U, 255U});
    renderer.render(packet, 0);
    check(center(renderer) == std::array<std::uint8_t, 4U>{255U, 0U, 0U, 255U},
          "initial effect sample changed the source color");

    renderer.set_clear_color({0U, 0U, 255U, 255U});
    renderer.render(packet, 1'000'000);
    check(center(renderer) == std::array<std::uint8_t, 4U>{255U, 0U, 0U, 255U},
          "rate-limited effect refreshed before its deadline");

    effect.opacity = 0.5;
    packet.batches.front() = effect;
    renderer.set_clear_color({0U, 255U, 0U, 255U});
    renderer.render(packet, 2'000'000);
    check(center(renderer) == std::array<std::uint8_t, 4U>{0U, 255U, 0U, 255U},
          "same-epoch effect signature change reused a stale sample");

    effect.opacity = 1.0;
    effect.refresh_rate = 0.0;
    packet.batches.front() = effect;
    renderer.set_clear_color({255U, 255U, 0U, 255U});
    renderer.render(packet, 3'000'000);
    renderer.set_clear_color({0U, 0U, 255U, 255U});
    renderer.render(packet, 4'000'000);
    check(center(renderer) == std::array<std::uint8_t, 4U>{0U, 0U, 255U, 255U},
          "unbounded effect did not refresh on the next frame");

    effect.refresh_rate = 100.0;
    packet.batches.front() = effect;
    renderer.set_clear_color({255U, 0U, 0U, 255U});
    renderer.render(packet, 5'000'000);
    renderer.set_clear_color({255U, 255U, 0U, 255U});
    renderer.render(packet, 16'000'000);
    check(center(renderer) == std::array<std::uint8_t, 4U>{255U, 255U, 0U, 255U},
          "rate-limited effect did not refresh after its deadline");

    renderer.set_clear_color({0U, 255U, 0U, 255U});
    renderer.declare_effect_pass("fixture.blur", 0U, 0U, 0.0, 1U, UINT32_MAX, UINT32_MAX, {});
    renderer.render(packet, 17'000'000);
    check(center(renderer) == std::array<std::uint8_t, 4U>{0U, 255U, 0U, 255U},
          "effect-pass redeclaration reused a stale sample");
}

} // namespace

int main() {
    try {
        strata::headless::SoftwareRenderer software(strata::headless::platform_image_codec());
        test_surface_backdrop_source(software);
        test_surface_backdrop_inside_content(software);
#ifdef _WIN32
        strata::headless::D3D11Renderer d3d11;
        test_surface_backdrop_source(d3d11);
        test_surface_backdrop_inside_content(d3d11);
#endif
        test_temporal_refresh_contract();
        std::cout << "strata_effect_refresh_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_effect_refresh_tests: " << error.what() << '\n';
        return 1;
    }
}
