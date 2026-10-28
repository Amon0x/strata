#include <strata/render_packet.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

/**
 * A retained frame that only moves a presentation group keeps its geometry epoch and reports the
 * moved bounds as dirty regions. A rate-limited effect over that region has to resample then
 * rather than show the content where it used to be until its next refresh.
 */
void test_group_move_refreshes_effect(strata::headless::CaptureRenderer& renderer) {
    using namespace strata;
    renderer.resize(4U, 4U, 4.0, 4.0);
    host::RenderPacket packet = surface_backdrop_packet(host::EffectBackdropSource::current, 7U);
    packet.batches.erase(packet.batches.begin());
    renderer.set_clear_color({255U, 0U, 0U, 255U});
    renderer.render(packet, 0);
    renderer.set_clear_color({0U, 0U, 255U, 255U});
    renderer.render(packet, 1'000'000);
    check(center(renderer) == std::array<std::uint8_t, 4U>{255U, 0U, 0U, 255U},
          "rate-limited effect refreshed before its deadline");

    packet.geometry_dirty_regions.push_back(host::GeometryDirtyRegion{1.0, 1.0, 2.0, 2.0});
    renderer.set_clear_color({0U, 255U, 0U, 255U});
    renderer.render(packet, 2'000'000);
    check(center(renderer) == std::array<std::uint8_t, 4U>{0U, 255U, 0U, 255U},
          "a group move under a rate-limited effect reused the sample from before it");

    packet.geometry_dirty_regions.clear();
    renderer.set_clear_color({255U, 255U, 0U, 255U});
    renderer.render(packet, 3'000'000);
    check(center(renderer) == std::array<std::uint8_t, 4U>{0U, 255U, 0U, 255U},
          "an unchanged retained frame resampled a rate-limited effect");
}

/**
 * A blurred effect reads its radius beyond its own bounds. Content that changes just outside a
 * glass panel shows in the blurred edge, so the retained sample must be resampled even though the
 * change does not overlap the panel itself; a change beyond everything the effect read must not
 * cost a resample. A packet that skips an epoch carries no record of what the skipped one changed.
 */
void test_changes_within_blur_reach_refresh_effect(strata::headless::CaptureRenderer& renderer) {
    using namespace strata;
    renderer.resize(32U, 32U, 32.0, 32.0);
    renderer.declare_effect_pass("fixture.reach-blur", 0U, 0U, 4.0, 1U, UINT32_MAX, UINT32_MAX,
                                 {});
    host::EffectBatch effect;
    effect.source_order = 0U;
    effect.scissor = host::Scissor{0U, 0U, 32U, 32U};
    effect.x = 12.0;
    effect.y = 12.0;
    effect.width = 8.0;
    effect.height = 8.0;
    effect.effect = "fixture.reach-blur";
    effect.refresh_rate = 100.0;
    host::RenderPacket packet;
    packet.geometry_epoch = 1U;
    packet.batches.emplace_back(effect);
    renderer.set_clear_color({255U, 0U, 0U, 255U});
    renderer.render(packet, 0);
    check(center(renderer) == std::array<std::uint8_t, 4U>{255U, 0U, 0U, 255U},
          "the blurred effect did not sample its backdrop");

    packet.geometry_dirty_regions = {host::GeometryDirtyRegion{0.0, 0.0, 2.0, 2.0}};
    renderer.set_clear_color({0U, 0U, 255U, 255U});
    renderer.render(packet, 1'000'000);
    check(center(renderer) == std::array<std::uint8_t, 4U>{255U, 0U, 0U, 255U},
          "a change beyond the blur's reach resampled a rate-limited effect");

    packet.geometry_dirty_regions = {host::GeometryDirtyRegion{22.0, 12.0, 2.0, 8.0}};
    renderer.set_clear_color({0U, 255U, 0U, 255U});
    renderer.render(packet, 2'000'000);
    check(center(renderer) == std::array<std::uint8_t, 4U>{0U, 255U, 0U, 255U},
          "a change within the blur's reach outside the bounds reused a stale sample");

    packet.geometry_dirty_regions.clear();
    packet.geometry_epoch = 3U;
    renderer.set_clear_color({255U, 255U, 0U, 255U});
    renderer.render(packet, 3'000'000);
    check(center(renderer) == std::array<std::uint8_t, 4U>{255U, 255U, 0U, 255U},
          "a packet after a skipped epoch reused a sample from before it");
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

/**
 * A red/blue edge blurred at reduced resolution on a 1.5x surface. The software renderer stands in
 * for the GPU backends in captures, so its downsampled blur has to land on the same pixels.
 */
[[nodiscard]] strata::host::RenderPacket downsampled_blur_packet() {
    using namespace strata;
    constexpr std::size_t vertex_size = 88U;
    host::RenderPacket packet;
    packet.geometry_epoch = 1U;
    packet.vertices.resize(8U * vertex_size);
    constexpr std::array<std::array<float, 2U>, 4U> corners{{
        {0.0F, 0.0F},
        {1.0F, 0.0F},
        {1.0F, 1.0F},
        {0.0F, 1.0F},
    }};
    for (std::size_t half = 0U; half < 2U; ++half) {
        for (std::size_t corner = 0U; corner < 4U; ++corner) {
            const std::size_t vertex = half * 4U + corner;
            write_float(packet.vertices, vertex, 0U,
                        static_cast<float>(half) * 16.0F + corners[corner][0U] * 16.0F);
            write_float(packet.vertices, vertex, 4U, corners[corner][1U] * 32.0F);
            packet.vertices[vertex * vertex_size + (half == 0U ? 20U : 22U)] = 255U;
            packet.vertices[vertex * vertex_size + 23U] = 255U;
            write_float(packet.vertices, vertex, 84U, 1.0F);
        }
    }
    packet.indices = {0U, 1U, 2U, 0U, 2U, 3U, 4U, 5U, 6U, 4U, 6U, 7U};
    packet.batches.emplace_back(host::DrawBatch{
        .source_order = 0U,
        .scissor = host::Scissor{0U, 0U, 48U, 48U},
        .material = "strata:unified_ui",
        .blend_mode = "opaque",
        .base_vertex = 0U,
        .first_index = 0U,
        .index_count = 12U,
    });
    host::BlurBatch blur;
    blur.source_order = 1U;
    blur.scissor = host::Scissor{6U, 9U, 33U, 30U};
    blur.x = 4.0;
    blur.y = 6.0;
    blur.width = 22.0;
    blur.height = 20.0;
    blur.radius = 5.0;
    blur.downsample = 2U;
    packet.batches.emplace_back(std::move(blur));
    return packet;
}

/**
 * A rectangle whose right and bottom edges sit a hair past pixel centres. GPUs snap vertices to a
 * 1/256 grid first, which moves those edges onto the centres, so the pixels stay uncovered.
 */
[[nodiscard]] strata::host::RenderPacket subpixel_edge_packet() {
    using namespace strata;
    constexpr std::size_t vertex_size = 88U;
    host::RenderPacket packet;
    packet.geometry_epoch = 1U;
    packet.vertices.resize(4U * vertex_size);
    constexpr float edge = 2.5F + 1.0F / 2048.0F;
    constexpr std::array<std::array<float, 2U>, 4U> positions{{
        {0.0F, 0.0F},
        {edge, 0.0F},
        {edge, edge},
        {0.0F, edge},
    }};
    for (std::size_t vertex = 0U; vertex < positions.size(); ++vertex) {
        write_float(packet.vertices, vertex, 0U, positions[vertex][0U]);
        write_float(packet.vertices, vertex, 4U, positions[vertex][1U]);
        packet.vertices[vertex * vertex_size + 20U] = 255U;
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
    return packet;
}

void test_subpixel_snapping(strata::headless::CaptureRenderer& renderer) {
    renderer.resize(4U, 4U, 4.0, 4.0);
    renderer.set_clear_color({0U, 0U, 0U, 255U});
    renderer.render(subpixel_edge_packet(), 0);
    const std::span<const std::uint8_t> pixels = renderer.pixels();
    for (std::size_t y = 0U; y < 4U; ++y) {
        for (std::size_t x = 0U; x < 4U; ++x) {
            const bool inside = x < 2U && y < 2U;
            check(pixels[(y * 4U + x) * 4U] == (inside ? 255U : 0U),
                  "a vertex a hair past a pixel centre was not snapped to the subpixel grid");
        }
    }
}

#ifdef _WIN32
void test_downsampled_blur_matches_d3d11() {
    using namespace strata;
    headless::SoftwareRenderer software(headless::platform_image_codec());
    headless::D3D11Renderer d3d11;
    for (headless::CaptureRenderer* renderer :
         std::array<headless::CaptureRenderer*, 2U>{&software, &d3d11}) {
        renderer->resize(48U, 48U, 32.0, 32.0);
        renderer->set_clear_color({0U, 0U, 0U, 255U});
        renderer->render(downsampled_blur_packet(), 0);
    }
    const std::span<const std::uint8_t> expected = d3d11.pixels();
    const std::span<const std::uint8_t> actual = software.pixels();
    check(expected.size() == actual.size(), "renderers captured different sizes");
    int largest = 0;
    for (std::size_t index = 0U; index < expected.size(); ++index)
        largest = std::max(largest, std::abs(int{expected[index]} - int{actual[index]}));
    check(largest <= 4, "software downsampled blur differs from D3D11 by " +
                            std::to_string(largest) + " in a channel");
}
#endif

} // namespace

int main() {
    try {
        strata::headless::SoftwareRenderer software(strata::headless::platform_image_codec());
        test_surface_backdrop_source(software);
        test_surface_backdrop_inside_content(software);
        test_subpixel_snapping(software);
        test_group_move_refreshes_effect(software);
        test_changes_within_blur_reach_refresh_effect(software);
#ifdef _WIN32
        strata::headless::D3D11Renderer d3d11;
        test_surface_backdrop_source(d3d11);
        test_surface_backdrop_inside_content(d3d11);
        test_subpixel_snapping(d3d11);
        test_group_move_refreshes_effect(d3d11);
        test_changes_within_blur_reach_refresh_effect(d3d11);
        test_downsampled_blur_matches_d3d11();
#endif
        test_temporal_refresh_contract();
        std::cout << "strata_effect_refresh_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_effect_refresh_tests: " << error.what() << '\n';
        return 1;
    }
}
