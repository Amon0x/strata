#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <strata/host.hpp>

struct HWND__;
using HWND = HWND__*;

namespace strata::desktop {

enum class SurfaceRole { screen, overlay };
enum class TextureSampling : std::uint32_t { nearest = 0U, linear = 1U };

struct FontResource final {
    std::string id;
    std::string resource;
};

struct TextureResource final {
    std::string id;
    std::string resource;
    TextureSampling sampling = TextureSampling::linear;
};

/** Description of one ordinary .strata application hosted in a Win32 window. */
struct ApplicationConfig final {
    std::string application_id;
    std::string surface_id;
    std::string module_resource;
    std::string schemas_resource;
    std::string root_name;
    SurfaceRole root_role = SurfaceRole::overlay;
    std::vector<std::string> extension_packages;
    std::vector<std::filesystem::path> extension_search_paths;
    std::vector<FontResource> fonts{
        {"strata:fonts/default-medium", "assets/strata/fonts/medium.ttf"},
        {"strata:fonts/default", "assets/strata/fonts/default.ttf"},
        {"strata:fonts/mono", "assets/strata/fonts/mono.ttf"},
    };
    std::vector<TextureResource> textures{
        {"strata:ui/icons/chevron-down", "assets/strata/textures/ui/icons/chevron-down.png"},
        {"strata:ui/icons/chevron-up", "assets/strata/textures/ui/icons/chevron-up.png"},
    };
    bool reduced_motion = false;
};

struct ApplicationOptions final {
    bool vsync = true;
};

struct Diagnostic final {
    std::uint64_t id = 0U;
    std::uint32_t severity = 0U;
    std::string code;
    std::string message;
    std::string source;
    std::uint32_t line = 0U;
    std::uint32_t column = 0U;
    std::string component_path;
    std::string expected;
};

/**
 * Reusable Win32/D3D11 host for one application and one Surface.
 *
 * Construct the host, register actions and snapshots through bindings(), then call activate(). The
 * window procedure delegates to handle_window_message() and the message loop calls frame(). The host
 * owns resource loading, source imports, clipboard/IME integration, packet decoding, D3D11
 * submission, and the ordered Surface release barrier.
 */
class ApplicationHost final {
  public:
    ApplicationHost(HWND window, std::filesystem::path resource_root,
                    ApplicationConfig config, ApplicationOptions options = {});
    ~ApplicationHost();

    ApplicationHost(const ApplicationHost&) = delete;
    ApplicationHost& operator=(const ApplicationHost&) = delete;

    [[nodiscard]] strata::Runtime& runtime() noexcept;
    [[nodiscard]] strata::host::Bindings& bindings() noexcept;

    /** Publishes an immediate immutable snapshot. Revision-watched models belong in bindings(). */
    void publish(std::string_view snapshot_id, const strata::host::Value& value);
    /** Compiles the configured source module and creates the Surface. */
    void activate();

    void resize(std::uint32_t framebuffer_width, std::uint32_t framebuffer_height,
                double dpi_scale);
    void pointer(std::uint32_t kind, std::int32_t button, double framebuffer_x,
                 double framebuffer_y);
    void scroll(double framebuffer_x, double framebuffer_y, double delta_x, double delta_y);
    void key(std::uint32_t virtual_key,
             std::uint32_t action = STRATA_KEY_PRESS);
    void text(std::string utf8);
    void ime_preedit(std::string utf8, std::size_t selection_start, std::size_t selection_end);
    void cancel_interactions() noexcept;

    /**
     * Handles resize, DPI, pointer capture/leave, wheel, keyboard, Unicode, focus cancellation,
     * and Win32 IME messages. A value is the consumed LRESULT; nullopt means call DefWindowProcW.
     */
    [[nodiscard]] std::optional<std::intptr_t> handle_window_message(
        std::uint32_t message,
        std::uintptr_t word,
        std::intptr_t long_value
    );

    /** Drops cached files and reloads Surface-owned fonts, textures, and other resources. */
    void reload_resources();
    /** Synchronizes host bindings, frames the Surface, submits D3D11 work, and presents. */
    void frame();
    /** Performs the packet consumption barrier and releases all runtime ownership. */
    void close();

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool has_frame() const noexcept;
    [[nodiscard]] double scale() const noexcept;
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::desktop
