#include <strata/strata.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

static_assert(STRATA_THEME_MODEL_VERSION_CURRENT == STRATA_THEME_MODEL_VERSION_3);

std::int64_t smoke_clock(void* const user_data) {
    return *static_cast<const std::int64_t*>(user_data);
}

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

        strata_runtime_config runtime_config{};
        runtime_config.struct_size = sizeof(runtime_config);
        runtime_config.abi_version = STRATA_ABI_VERSION_CURRENT;
        runtime_config.required_capabilities = STRATA_CAPABILITY_CORE_LIFECYCLE |
            STRATA_CAPABILITY_APPLICATION_LIFECYCLE |
            STRATA_CAPABILITY_COMPILER_ACTIVATION |
            STRATA_CAPABILITY_SURFACE_RUNTIME |
            STRATA_CAPABILITY_SURFACE_RENDER_PACKET |
            STRATA_CAPABILITY_ALLOCATOR_TELEMETRY |
            STRATA_CAPABILITY_SURFACE_RESOURCE_RELOAD;
        runtime_config.clock = strata_clock{sizeof(strata_clock), &now, &smoke_clock};

        strata::Surface surface = [&] {
            strata::Runtime runtime(runtime_config);
            const strata_application_config application{
                sizeof(strata_application_config),
                strata::view("installed.cpp.application"),
                strata::view(registry),
                {},
            };
            runtime.configure_application(application);
            const strata_activation_config activation{
                sizeof(strata_activation_config),
                1U,
                strata::view("installed/cpp/main.strata"),
                strata::view(source),
                nullptr,
                nullptr,
            };
            if (runtime.activate(activation).status != STRATA_ACTIVATION_ACTIVATED) {
                throw std::runtime_error("installed C++ source did not activate");
            }
            if (runtime.memory_info().routed_current_bytes == 0U) {
                throw std::runtime_error("installed C++ allocator telemetry is empty");
            }
            strata_surface_config config{};
            config.struct_size = sizeof(config);
            config.id = strata::view("installed.cpp.surface");
            config.root_role = STRATA_SURFACE_ROOT_OVERLAY;
            config.root_name = strata::view("Main");
            config.environment = strata_surface_environment{
                sizeof(strata_surface_environment),
                1U,
                800,
                600,
                800.0,
                600.0,
                1.0,
                0.0,
                0.0,
                0.0,
                0.0,
                STRATA_POINT_SNAP_NEAREST,
                STRATA_RECTANGLE_SNAP_OUTWARD,
                STRATA_SURFACE_DENSITY_COMFORTABLE,
                STRATA_POINTER_PRECISION_FINE,
                STRATA_SURFACE_INPUT_POINTER | STRATA_SURFACE_INPUT_KEYBOARD,
                0U,
                0U,
            };
            return runtime.create_surface(config);
        }();

        static_cast<void>(surface.frame(now));
        surface.reload_resources();
        ++now;
        const strata_surface_frame_info frame = surface.frame(now);
        const std::vector<std::uint8_t> packet = surface.render_packet();
        const std::string json = surface.frame_json();
        if (frame.frame_index != 2U || frame.render_command_count == 0U || packet.size() < 12U ||
            std::string_view(reinterpret_cast<const char*>(packet.data()), 8U) != "STRATARP" ||
            json.find("embedded.panel") == std::string::npos) {
            throw std::runtime_error("installed C++ frame or packet was invalid");
        }
        const std::vector<std::uint8_t> release_packet = surface.prepare_release_packet();
        if (release_packet.size() < 12U ||
            std::string_view(reinterpret_cast<const char*>(release_packet.data()), 8U) != "STRATARP") {
            throw std::runtime_error("installed C++ release packet was invalid");
        }
        surface.acknowledge_release_packet();
        surface.close();
        std::cout << "strata_cpp_smoke: RAII runtime, compile, Surface, packet, and telemetry OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_cpp_smoke: " << error.what() << '\n';
        return 1;
    }
}
