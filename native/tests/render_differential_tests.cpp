// Cross-backend differential tests. Recorded headless packet streams and scripted effect-cache
// sequences are replayed through every backend this build has (the reference renderer, D3D11/WARP
// and Vulkan), and their frames are compared within a tolerance. Each backend's retained effect
// samples are also checked against a twin of the same backend that never reuses one, so a stale
// sample shows as a difference however the backends compare with each other.

#include <strata/render_packet.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "capture_renderer.hpp"
#include "packet_stream.hpp"
#include "runner.hpp"
#include "scenario.hpp"

namespace {

using namespace strata;

void check(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

/** How far two captures of one frame differ. */
struct Difference final {
    double mean = 0.0;          // mean absolute channel difference, 0..255
    double outlier_share = 0.0; // share of pixels with a channel differing by more than 32
    int largest = 0;
};

[[nodiscard]] Difference difference(const std::span<const std::uint8_t> left,
                                    const std::span<const std::uint8_t> right) {
    check(left.size() == right.size() && !left.empty(), "captures differ in size");
    Difference result;
    std::uint64_t total = 0U;
    std::size_t outliers = 0U;
    for (std::size_t pixel = 0U; pixel < left.size(); pixel += 4U) {
        int widest = 0;
        for (std::size_t channel = 0U; channel < 4U; ++channel) {
            const int delta = std::abs(int{left[pixel + channel]} - int{right[pixel + channel]});
            total += static_cast<std::uint64_t>(delta);
            widest = std::max(widest, delta);
        }
        result.largest = std::max(result.largest, widest);
        outliers += widest > 32 ? 1U : 0U;
    }
    result.mean = static_cast<double>(total) / static_cast<double>(left.size());
    result.outlier_share = static_cast<double>(outliers) / static_cast<double>(left.size() / 4U);
    return result;
}

/** The backends this build can run here; Vulkan is skipped where no device is available. */
[[nodiscard]] std::vector<std::string> available_backends() {
    std::vector<std::string> result{"reference"};
#if defined(_WIN32)
    result.emplace_back("d3d11");
#endif
    try {
        const std::unique_ptr<headless::CaptureRenderer> probe =
            headless::create_capture_renderer("vulkan");
        probe->resize(4U, 4U, 4.0, 4.0);
        result.emplace_back("vulkan");
    } catch (const std::exception& error) {
        std::cout << "render differential: Vulkan skipped (" << error.what() << ")\n";
    }
    return result;
}

/** A frame is compared across backends only when it uses nothing backend-specific. */
[[nodiscard]] bool portable(const host::RenderPacket& packet) {
    return std::ranges::all_of(packet.materials,
                               [](const std::string& id) { return id == "strata:unified_ui"; }) &&
           std::ranges::none_of(packet.batches, [](const host::SubmissionBatch& batch) {
               return std::holds_alternative<host::EffectBatch>(batch);
           });
}

/** The widest difference any compared frame showed, reported so tolerances can be judged. */
Difference worst;

/** Within what the reference renderer documents as its oracle scope (docs/headless-testing.md). */
void require_close(const Difference& measured, const std::string& what) {
    worst.mean = std::max(worst.mean, measured.mean);
    worst.outlier_share = std::max(worst.outlier_share, measured.outlier_share);
    worst.largest = std::max(worst.largest, measured.largest);
    if (measured.mean > 0.5 || measured.outlier_share > 0.002) {
        throw std::runtime_error(what + ": mean " + std::to_string(measured.mean) +
                                 ", outlier share " + std::to_string(measured.outlier_share) +
                                 ", largest " + std::to_string(measured.largest));
    }
}

/** Records a headless scenario's packet stream through the public ABI. */
[[nodiscard]] std::vector<headless::RecordedPacket>
record(const std::filesystem::path& scenario_path, const std::filesystem::path& resources,
       const std::filesystem::path& output) {
    headless::Scenario scenario = headless::load_scenario(scenario_path);
    scenario.render_backend = "reference";
    scenario.record_packets = true;
    headless::run_scenario(scenario, resources, output);
    return headless::read_packet_stream(output / "packets.strp");
}

/** Replays a recorded stream through every backend and compares each portable frame. */
void replay_recorded_stream(const std::vector<headless::RecordedPacket>& stream,
                            const std::vector<std::string>& backends, const std::string& name) {
    struct Lane final {
        std::string backend;
        std::unique_ptr<headless::CaptureRenderer> renderer;
        host::RenderPacketDecoder decoder;
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
    };
    std::vector<std::unique_ptr<Lane>> lanes;
    for (const std::string& backend : backends) {
        auto lane = std::make_unique<Lane>();
        lane->backend = backend;
        lane->renderer = headless::create_capture_renderer(backend);
        lane->renderer->set_clear_color({9U, 11U, 15U, 255U});
        lanes.push_back(std::move(lane));
    }
    std::size_t compared = 0U;
    for (std::size_t index = 0U; index < stream.size(); ++index) {
        const headless::RecordedPacket& frame = stream[index];
        bool comparable = true;
        for (const std::unique_ptr<Lane>& lane : lanes) {
            if (lane->width != frame.framebuffer_width || lane->height != frame.framebuffer_height) {
                lane->renderer->resize(frame.framebuffer_width, frame.framebuffer_height,
                                       frame.logical_width, frame.logical_height);
                lane->renderer->set_clear_color({9U, 11U, 15U, 255U});
                lane->width = frame.framebuffer_width;
                lane->height = frame.framebuffer_height;
            }
            const host::RenderPacket& packet = lane->decoder.decode(frame.bytes);
            comparable = comparable && portable(packet);
            lane->renderer->render(packet, frame.time_nanoseconds);
        }
        if (!comparable)
            continue;
        ++compared;
        for (std::size_t other = 1U; other < lanes.size(); ++other) {
            require_close(difference(lanes[0]->renderer->pixels(), lanes[other]->renderer->pixels()),
                          name + " frame " + std::to_string(index) + ": " + lanes[0]->backend +
                              " vs " + lanes[other]->backend);
        }
    }
    check(compared > 0U, name + " recorded no portable frame to compare");
    std::cout << "render differential: " << name << " compared " << compared << " of "
              << stream.size() << " frames across " << lanes.size() << " backends\n";
}

// ---- Scripted effect-cache sequences ------------------------------------------------------------

constexpr std::uint32_t side = 64U;

/** A v14 packet writer for the scripted sequences (layout in strata.h). */
class Writer final {
  public:
    template <typename Integer>
    void integer(const Integer value) {
        for (std::size_t byte = 0U; byte < sizeof(Integer); ++byte)
            bytes.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(value) >> (byte * 8U)));
    }
    void number(const double value) { integer(std::bit_cast<std::uint64_t>(value)); }
    void text(const std::string_view value) {
        integer(static_cast<std::uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void record(const std::uint32_t kind, const Writer& body) {
        integer(kind);
        integer(static_cast<std::uint32_t>(body.bytes.size()));
        bytes.insert(bytes.end(), body.bytes.begin(), body.bytes.end());
    }
    std::vector<std::uint8_t> bytes;
};

struct Quad final {
    float x, y, width, height;
    std::array<std::uint8_t, 4U> color;
    std::uint32_t group = 0U;
    bool textured = false;
};

/** The scene: a backdrop, a quad beside the glass, a grouped quad far from it, and an image. */
struct Scene final {
    std::vector<Quad> quads{
        {0.0F, 0.0F, 64.0F, 64.0F, {40U, 44U, 52U, 255U}},
        {8.0F, 20.0F, 12.0F, 24.0F, {220U, 40U, 40U, 255U}},
        {52.0F, 52.0F, 8.0F, 8.0F, {40U, 80U, 220U, 255U}, 1U},
        {44.0F, 4.0F, 16.0F, 16.0F, {255U, 255U, 255U, 255U}, 0U, true},
    };
    double group_translate = 0.0;
    std::array<std::uint8_t, 4U> image{250U, 200U, 20U, 255U};
};

[[nodiscard]] std::vector<std::uint8_t> vertices(const Scene& scene) {
    std::vector<std::uint8_t> result;
    for (const Quad& quad : scene.quads) {
        const std::array<std::array<float, 4U>, 4U> corners{{
            {quad.x, quad.y, 0.0F, 0.0F},
            {quad.x + quad.width, quad.y, 1.0F, 0.0F},
            {quad.x + quad.width, quad.y + quad.height, 1.0F, 1.0F},
            {quad.x, quad.y + quad.height, 0.0F, 1.0F},
        }};
        for (const auto& corner : corners) {
            std::array<std::uint8_t, host::packet_vertex_bytes> vertex{};
            const std::array<float, 5U> head{corner[0U], corner[1U],
                                             static_cast<float>(quad.group), corner[2U], corner[3U]};
            std::memcpy(vertex.data(), head.data(), sizeof(head));
            std::copy(quad.color.begin(), quad.color.end(), vertex.begin() + 20);
            const float mode = quad.textured ? 1.0F : 0.0F;
            const float opacity = 1.0F;
            std::memcpy(vertex.data() + 80U, &mode, sizeof(mode));
            std::memcpy(vertex.data() + 84U, &opacity, sizeof(opacity));
            result.insert(result.end(), vertex.begin(), vertex.end());
        }
    }
    return result;
}

enum class Geometry { complete, patch, retained };

/** One frame of the scripted scene. A patch packet carries the vertices of changed quads only. */
[[nodiscard]] std::vector<std::uint8_t> scene_packet(const Scene& scene, const Scene& previous,
                                                     const std::uint64_t frame,
                                                     const std::uint64_t epoch,
                                                     const Geometry geometry,
                                                     const bool upload_image) {
    const std::vector<std::uint8_t> vertex_bytes = vertices(scene);
    const auto quad_count = static_cast<std::uint32_t>(scene.quads.size());
    Writer packet;
    for (const char letter : std::string_view("STRATARP"))
        packet.bytes.push_back(static_cast<std::uint8_t>(letter));
    packet.integer(STRATA_RENDER_PACKET_VERSION_CURRENT);
    packet.integer(std::uint32_t{upload_image ? 1U : 0U});
    packet.integer(geometry == Geometry::retained ? 0U : quad_count + 1U);
    packet.integer(frame);
    packet.integer(epoch);
    packet.integer(geometry == Geometry::complete ? host::packet_flag_geometry_payload
                   : geometry == Geometry::patch  ? host::packet_flag_geometry_patches
                                                  : 0U);
    packet.integer(geometry == Geometry::retained ? 0U
                                                  : static_cast<std::uint32_t>(vertex_bytes.size()));
    packet.integer(geometry == Geometry::retained ? 0U : quad_count * 6U);
    packet.integer(0U);
    packet.integer(0U);
    packet.integer(1U);
    packet.integer(1U);
    // The group moves diagonally, so its quad crosses into the glass.
    for (const double value :
         {1.0, 1.0, scene.group_translate, scene.group_translate, 1.0, 0.0, 0.0, 0.0})
        packet.number(value);
    if (upload_image) {
        Writer image;
        image.text("image");
        image.integer(host::texture_sampling_nearest);
        image.integer(1U);
        image.integer(1U);
        image.integer(4U);
        image.bytes.insert(image.bytes.end(), scene.image.begin(), scene.image.end());
        packet.record(host::resource_image, image);
    }
    if (geometry == Geometry::retained)
        return packet.bytes;
    if (geometry == Geometry::complete) {
        packet.bytes.insert(packet.bytes.end(), vertex_bytes.begin(), vertex_bytes.end());
        for (std::uint32_t quad = 0U; quad < quad_count; ++quad) {
            for (const std::uint32_t index : {0U, 1U, 2U, 0U, 2U, 3U})
                packet.integer(quad * 4U + index);
        }
    } else {
        const std::vector<std::uint8_t> before = vertices(previous);
        constexpr std::size_t quad_bytes = 4U * host::packet_vertex_bytes;
        std::vector<std::uint32_t> changed;
        for (std::uint32_t quad = 0U; quad < quad_count; ++quad) {
            if (!std::equal(vertex_bytes.begin() + quad * quad_bytes,
                            vertex_bytes.begin() + (quad + 1U) * quad_bytes,
                            before.begin() + quad * quad_bytes)) {
                changed.push_back(quad);
            }
        }
        packet.integer(static_cast<std::uint32_t>(changed.size()));
        for (const std::uint32_t quad : changed) {
            packet.integer(static_cast<std::uint32_t>(quad * quad_bytes));
            packet.integer(static_cast<std::uint32_t>(quad_bytes));
            packet.bytes.insert(packet.bytes.end(), vertex_bytes.begin() + quad * quad_bytes,
                                vertex_bytes.begin() + (quad + 1U) * quad_bytes);
        }
        packet.integer(0U);
    }
    packet.integer(1U);
    packet.text("strata:unified_ui");
    packet.integer(1U);
    packet.text("test:glass");
    for (std::uint32_t quad = 0U; quad < quad_count; ++quad) {
        Writer draw;
        draw.integer(quad);
        for (const std::uint32_t value : {0U, 0U, side, side})
            draw.integer(value);
        draw.integer(0U);
        draw.integer(std::uint16_t{0U});
        draw.integer(static_cast<std::uint8_t>(STRATA_RENDER_BLEND_STRAIGHT_ALPHA));
        draw.integer(static_cast<std::uint8_t>(STRATA_RENDER_CLIP_COVERAGE_HARD));
        draw.integer(scene.quads[quad].textured ? 1U : 0U);
        if (scene.quads[quad].textured)
            draw.text("image");
        draw.integer(0U);
        draw.integer(quad * 6U);
        draw.integer(6U);
        packet.record(0U, draw);
    }
    Writer glass;
    glass.integer(quad_count);
    for (const std::uint32_t value : {0U, 0U, side, side})
        glass.integer(value);
    glass.integer(0U);
    glass.integer(0U);
    for (const double value : {22.0, 20.0, 20.0, 24.0, 0.0, 0.0, 0.0, 0.0})
        glass.number(value);
    glass.integer(std::uint16_t{0U});
    glass.integer(std::uint16_t{0U});
    glass.number(1.0);
    glass.number(1.0); // one refresh per second: within this sequence only a revision resamples
    glass.integer(0U);
    glass.integer(std::uint64_t{0U});
    glass.number(6.0); // a 4px blur reads 4 + 2 logical pixels beyond its bounds
    glass.integer(0U);
    packet.record(2U, glass);
    return packet.bytes;
}

/**
 * Replays the scene's scripted changes through every backend. Each backend renders the stream
 * twice: once retaining effect samples by the decoder's revisions, and once as a twin whose
 * revisions are always new, so it never reuses a sample. They must agree exactly; the backends
 * must agree within tolerance.
 */
void replay_effect_cache_sequences(const std::vector<std::string>& backends) {
    struct Step final {
        std::string what;
        Scene scene;
        Geometry geometry;
        bool upload_image;
        bool expect_new_revision;
    };
    std::vector<Step> steps;
    Scene scene;
    steps.push_back({"first frame", scene, Geometry::complete, true, true});
    steps.push_back({"unchanged frame", scene, Geometry::retained, false, false});
    scene.quads[1].color = {40U, 220U, 60U, 255U};
    steps.push_back({"content beside the blur edge", scene, Geometry::patch, false, true});
    scene.quads[2].color = {200U, 60U, 220U, 255U};
    steps.push_back({"content beyond the read reach", scene, Geometry::patch, false, false});
    scene.group_translate = -24.0;
    steps.push_back({"group moved under the glass", scene, Geometry::retained, false, true});
    scene.image = {20U, 200U, 240U, 255U};
    steps.push_back({"image swapped under the glass edge", scene, Geometry::retained, true, true});
    steps.push_back({"settled again", scene, Geometry::retained, false, false});

    struct Lane final {
        std::string backend;
        std::unique_ptr<headless::CaptureRenderer> cached;
        std::unique_ptr<headless::CaptureRenderer> twin;
        host::RenderPacketDecoder decoder;
    };
    std::vector<std::unique_ptr<Lane>> lanes;
    for (const std::string& backend : backends) {
        auto lane = std::make_unique<Lane>();
        lane->backend = backend;
        for (auto* renderer : {&lane->cached, &lane->twin}) {
            *renderer = headless::create_capture_renderer(backend);
            (*renderer)->resize(side, side, side, side);
            (*renderer)->set_clear_color({0U, 0U, 0U, 255U});
            (*renderer)->declare_effect_pass("test:glass", 0U, STRATA_EFFECT_PASS_BLUR, 4.0, 1U,
                                             UINT32_MAX, UINT32_MAX, {});
        }
        lanes.push_back(std::move(lane));
    }
    std::uint64_t previous_revision = 0U;
    std::uint64_t twin_revision = 1'000'000U;
    std::uint64_t epoch = 0U;
    for (std::size_t index = 0U; index < steps.size(); ++index) {
        const Step& step = steps[index];
        if (step.geometry != Geometry::retained)
            ++epoch;
        const std::vector<std::uint8_t> bytes =
            scene_packet(step.scene, index == 0U ? step.scene : steps[index - 1U].scene,
                         index + 1U, epoch, step.geometry, step.upload_image);
        const std::int64_t time = static_cast<std::int64_t>(index) * 10'000'000;
        for (const std::unique_ptr<Lane>& lane : lanes) {
            const host::RenderPacket& packet = lane->decoder.decode(bytes);
            const auto& glass = std::get<host::EffectBatch>(packet.batches.back());
            if (lane == lanes.front()) {
                check((glass.sample_revision != previous_revision) == step.expect_new_revision,
                      "the decoder's revision after '" + step.what + "' was not as expected");
                previous_revision = glass.sample_revision;
            }
            lane->cached->render(packet, time);
            host::RenderPacket never_reused = packet;
            std::get<host::EffectBatch>(never_reused.batches.back()).sample_revision =
                ++twin_revision;
            lane->twin->render(never_reused, time);
            const Difference stale = difference(lane->cached->pixels(), lane->twin->pixels());
            check(stale.largest == 0, lane->backend + " composited a stale effect sample after '" +
                                          step.what + "' (largest difference " +
                                          std::to_string(stale.largest) + ")");
        }
        for (std::size_t other = 1U; other < lanes.size(); ++other) {
            require_close(difference(lanes[0]->cached->pixels(), lanes[other]->cached->pixels()),
                          "effect sequence '" + step.what + "': " + lanes[0]->backend + " vs " +
                              lanes[other]->backend);
        }
    }
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count < 4) {
            throw std::invalid_argument(
                "usage: strata_render_differential_tests <resources> <output> <scenario>...");
        }
        const std::filesystem::path resources = arguments[1];
        const std::filesystem::path output = arguments[2];
        const std::vector<std::string> backends = available_backends();
        replay_effect_cache_sequences(backends);
        for (int index = 3; index < argument_count; ++index) {
            const std::filesystem::path scenario = arguments[index];
            const std::string name = scenario.stem().string();
            replay_recorded_stream(record(scenario, resources, output / name), backends, name);
        }
        std::cout << "render differential: widest difference mean " << worst.mean
                  << ", outlier share " << worst.outlier_share << ", largest " << worst.largest
                  << '\n';
        std::cout << "strata_render_differential_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_render_differential_tests: " << error.what() << '\n';
        return 1;
    }
}
