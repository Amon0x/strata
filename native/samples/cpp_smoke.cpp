#include <strata/host.hpp>
#include <strata/render_packet.hpp>
#include <strata/svg.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

static_assert(STRATA_THEME_MODEL_VERSION_CURRENT == STRATA_THEME_MODEL_VERSION_3);

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open installed Strata registry");
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    );
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count != 2) return 64;
        const strata_theme_visual_style theme_visual = strata::theme_visual_style_defaults();
        const strata_theme_layout_style theme_layout = strata::theme_layout_style_defaults();
        static_cast<void>(theme_visual);
        static_cast<void>(theme_layout);
        std::int64_t now = 654321;
        const std::string registry = read_file(arguments[1]);
        constexpr std::string_view source =
            "style Root { width: { weight: 1 }; height: { weight: 1 }; background: #334155FF; } "
            "overlay Main { root Panel(key: \"embedded.panel\", style: Root) }";
        const strata::svg::Document svg = strata::svg::parse(
            "<svg width='16' height='16'><path d='M1 1h14v14H1z'/></svg>"
        );
        if (svg.commands.size() != 1U) {
            throw std::runtime_error("installed Strata::svg parser lost static geometry");
        }

        strata::Surface surface = [&] {
            strata::RuntimeOptions runtime_options;
            runtime_options.clock = [&now] { return now; };
            strata::Runtime runtime(std::move(runtime_options));
            runtime.configure_application(strata::ApplicationOptions{
                .id = "installed.cpp.application",
                .registry_json = registry,
            });
            strata::host::Revision model_revision;
            strata::host::Bindings bindings(runtime, "installed.cpp.host");
            bindings.snapshot(
                "installed.cpp.model",
                [&model_revision] { return model_revision.value(); },
                [] {
                    return strata::host::Value::object({
                        {"model", strata::host::Value::object({{"title", "Typed host model"}})},
                    });
                }
            );
            bindings.synchronize();
            if (!runtime.activate(strata::SourceActivation{
                    .generation = 1U,
                    .entry_source_id = "installed/cpp/main.strata",
                    .entry_text = std::string(source),
                }).activated()) {
                throw std::runtime_error("installed C++ source did not activate");
            }
            if (runtime.memory_info().routed_current_bytes == 0U) {
                throw std::runtime_error("installed C++ allocator telemetry is empty");
            }
            strata::SurfaceOptions surface_options;
            surface_options.id = "installed.cpp.surface";
            surface_options.root_role = strata::SurfaceRootRole::overlay;
            surface_options.root_name = "Main";
            surface_options.environment.framebuffer_width = 800;
            surface_options.environment.framebuffer_height = 600;
            surface_options.environment.logical_width = 800.0;
            surface_options.environment.logical_height = 600.0;
            surface_options.environment.rectangle_snapping = strata::RectangleSnap::outward;
            return runtime.create_surface(surface_options);
        }();

        const strata::InputEvent pointer = strata::InputEvent::pointer(
            strata::InputKind::pointer_move,
            strata::Point{400.0, 300.0}
        );
        if (surface.enqueue(pointer).accepted_event_count != 1U) {
            throw std::runtime_error("installed C++ input facade rejected pointer input");
        }
        static_cast<void>(surface.frame(now));
        surface.reload_resources();
        ++now;
        const strata_surface_frame_info frame = surface.frame(now);
        const std::vector<std::uint8_t> packet = surface.render_packet();
        strata::host::RenderPacketDecoder decoder;
        const strata::host::RenderPacket& decoded = decoder.decode(packet);
        const std::string json = surface.frame_json();
        if (frame.frame_index != 2U || frame.render_command_count == 0U || packet.size() < 12U ||
            decoded.frame_index != frame.frame_index || decoded.batches.empty() ||
            json.find("embedded.panel") == std::string::npos) {
            throw std::runtime_error("installed C++ frame or packet was invalid");
        }
        const std::vector<std::uint8_t> release_packet = surface.prepare_release_packet();
        static_cast<void>(decoder.decode(release_packet));
        if (release_packet.size() < 12U) {
            throw std::runtime_error("installed C++ release packet was invalid");
        }
        surface.acknowledge_release_packet();
        surface.close();
        std::cout << "strata_cpp_smoke: typed runtime, input, Surface, packet, and telemetry OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_cpp_smoke: " << error.what() << '\n';
        return 1;
    }
}
