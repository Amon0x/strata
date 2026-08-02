#include <strata/render_packet.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "image_codec.hpp"
#include "software_renderer.hpp"

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

[[nodiscard]] std::array<std::uint8_t, 4U>
center(const strata::headless::SoftwareRenderer& renderer) {
    const std::span<const std::uint8_t> pixels = renderer.pixels();
    const std::size_t offset = (1U * renderer.width() + 1U) * 4U;
    return {
        pixels[offset],
        pixels[offset + 1U],
        pixels[offset + 2U],
        pixels[offset + 3U],
    };
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
        test_temporal_refresh_contract();
        std::cout << "strata_effect_refresh_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_effect_refresh_tests: " << error.what() << '\n';
        return 1;
    }
}
