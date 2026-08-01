#include <strata/desktop.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <windowsx.h>

#include <strata/strata.hpp>

#include "host_services.hpp"
#include "ime.hpp"
#include "host/extensions.hpp"
#include "host/module_path.hpp"
#include <strata/render_packet.hpp>
#include "renderer.hpp"

namespace strata::desktop {
namespace {

[[nodiscard]] std::string copy(const strata_string_view value) {
    return value.size == 0U ? std::string{} : std::string(value.data, value.size);
}

[[nodiscard]] double display_scale(const std::uint32_t framebuffer_width,
                                   const std::uint32_t framebuffer_height,
                                   const double dpi_scale) {
    strata_scale_policy_config policy{sizeof(strata_scale_policy_config)};
    strata::require_ok(strata_scale_policy_defaults(STRATA_SCALE_POLICY_AUTO_FIT, &policy),
                       "desktop scale-policy defaults");
    policy.preferred_logical_width = 1'600.0;
    policy.preferred_logical_height = 900.0;
    policy.min_scale = std::min(dpi_scale, policy.max_scale);
    strata_scale_context context{sizeof(strata_scale_context)};
    strata::require_ok(
        strata_resolve_scale_context(&policy, static_cast<std::int64_t>(framebuffer_width),
                                     static_cast<std::int64_t>(framebuffer_height), &context),
        "desktop scale-context resolution");
    return context.scale;
}

[[nodiscard]] std::uint32_t current_modifiers() noexcept {
    std::uint32_t result = 0U;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
        result |= STRATA_KEY_MODIFIER_SHIFT;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
        result |= STRATA_KEY_MODIFIER_CONTROL;
    if ((GetKeyState(VK_MENU) & 0x8000) != 0)
        result |= STRATA_KEY_MODIFIER_ALT;
    if ((GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0)
        result |= STRATA_KEY_MODIFIER_SUPER;
    return result;
}

[[nodiscard]] std::string key_name(const std::uint32_t key) {
    switch (key) {
    case VK_TAB: return "tab";
    case VK_RETURN: return "enter";
    case VK_SPACE: return "space";
    case VK_ESCAPE: return "escape";
    case VK_BACK: return "backspace";
    case VK_DELETE: return "delete";
    case VK_LEFT: return "left";
    case VK_RIGHT: return "right";
    case VK_UP: return "up";
    case VK_DOWN: return "down";
    case VK_HOME: return "home";
    case VK_END: return "end";
    case VK_PRIOR: return "page_up";
    case VK_NEXT: return "page_down";
    case VK_INSERT: return "insert";
    default:
        if (key >= 'A' && key <= 'Z')
            return std::string(1U, static_cast<char>('a' + key - 'A'));
        if (key >= '0' && key <= '9')
            return std::string(1U, static_cast<char>(key));
        return "win32:" + std::to_string(key);
    }
}

void require_resource_id(const std::string_view value, const std::string_view label) {
    const std::filesystem::path path{std::string(value)};
    if (value.empty() || path.is_absolute())
        throw std::invalid_argument(std::string(label) + " must be a relative resource id");
    for (const std::filesystem::path& part : path) {
        if (part == "..")
            throw std::invalid_argument(std::string(label) + " must not escape the resource root");
    }
}

} // namespace

struct ApplicationHost::Impl final {
    Impl(HWND window, std::filesystem::path resource_root, ApplicationConfig config,
         const ApplicationOptions options)
        : window(window), config(std::move(config)), services(window, std::move(resource_root)),
          renderer(window, options.vsync) {
        try {
            initialize();
        } catch (...) {
            dispose_after_failure();
            throw;
        }
    }

    ~Impl() noexcept {
        try {
            close();
        } catch (...) {
            dispose_after_failure();
        }
    }

    static std::int64_t clock(void* const user_data) noexcept {
        return static_cast<Impl*>(user_data)->frame_time;
    }

    static void diagnostic(void* const user_data, const strata_diagnostic* const value) noexcept {
        if (user_data == nullptr || value == nullptr)
            return;
        try {
            static_cast<Impl*>(user_data)->diagnostics.push_back(Diagnostic{
                value->id,
                value->severity,
                copy(value->code),
                copy(value->message),
                copy(value->source_id),
                value->range.line_start,
                value->range.column_start,
                copy(value->component_path),
                copy(value->expected),
            });
        } catch (...) {
        }
    }

    static strata_status load_module(void* const user_data,
                                     const strata_string_view importer_source_id,
                                     const strata_string_view import_path,
                                     strata_module_source* const output) noexcept {
        if (user_data == nullptr || output == nullptr)
            return STRATA_STATUS_INVALID_ARGUMENT;
        try {
            auto& self = *static_cast<Impl*>(user_data);
            self.module_id_scratch =
                host::resolve_module_id(copy(importer_source_id), copy(import_path));
            self.module_text_scratch = self.services.text(self.module_id_scratch);
            *output = strata_module_source{
                sizeof(strata_module_source),
                strata::view(self.module_id_scratch),
                strata::view(self.module_text_scratch),
            };
            return STRATA_STATUS_OK;
        } catch (const std::bad_alloc&) {
            return STRATA_STATUS_OUT_OF_MEMORY;
        } catch (...) {
            return STRATA_STATUS_NOT_FOUND;
        }
    }

    [[nodiscard]] std::int64_t now() const noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - epoch)
            .count();
    }

    [[nodiscard]] double logical_width() const noexcept {
        return static_cast<double>(framebuffer_width) / scale;
    }

    [[nodiscard]] double logical_height() const noexcept {
        return static_cast<double>(framebuffer_height) / scale;
    }

    [[nodiscard]] strata_surface_environment environment() const noexcept {
        strata_surface_environment result{};
        result.struct_size = sizeof(result);
        result.generation = environment_generation;
        result.framebuffer_width = framebuffer_width;
        result.framebuffer_height = framebuffer_height;
        result.logical_width = logical_width();
        result.logical_height = logical_height();
        result.scale = scale;
        result.point_snapping = STRATA_POINT_SNAP_NEAREST;
        result.rectangle_snapping = STRATA_RECTANGLE_SNAP_OUTWARD;
        result.density = STRATA_SURFACE_DENSITY_COMFORTABLE;
        result.pointer_precision = STRATA_POINTER_PRECISION_FINE;
        result.input_capabilities = STRATA_SURFACE_INPUT_POINTER |
                                    STRATA_SURFACE_INPUT_KEYBOARD |
                                    STRATA_SURFACE_INPUT_IME |
                                    STRATA_SURFACE_INPUT_CLIPBOARD;
        result.reduced_motion = config.reduced_motion ? 1U : 0U;
        return result;
    }

    void initialize() {
        if (config.application_id.empty())
            throw std::invalid_argument("desktop application id must not be empty");
        if (config.surface_id.empty())
            config.surface_id = config.application_id;
        if (config.root_name.empty())
            throw std::invalid_argument("desktop application root name must not be empty");
        require_resource_id(config.module_resource, "desktop application module");
        if (!config.schemas_resource.empty())
            require_resource_id(config.schemas_resource, "desktop application schemas");
        for (const FontResource& font : config.fonts) {
            if (font.id.empty())
                throw std::invalid_argument("desktop font id must not be empty");
            require_resource_id(font.resource, "desktop font resource");
        }
        for (const TextureResource& texture : config.textures) {
            if (texture.id.empty())
                throw std::invalid_argument("desktop texture id must not be empty");
            require_resource_id(texture.resource, "desktop texture resource");
        }

        RECT client{};
        if (!GetClientRect(window, &client))
            throw std::runtime_error("could not read desktop application window bounds");
        const std::uint32_t width =
            static_cast<std::uint32_t>(std::max<LONG>(client.right - client.left, 1L));
        const std::uint32_t height =
            static_cast<std::uint32_t>(std::max<LONG>(client.bottom - client.top, 1L));
        const UINT dpi = GetDpiForWindow(window);
        resize_viewport(width, height, dpi == 0U ? 1.0 : static_cast<double>(dpi) / 96.0, false);

        strata_runtime_config runtime_config{};
        runtime_config.struct_size = sizeof(runtime_config);
        runtime_config.abi_version = STRATA_ABI_VERSION_CURRENT;
        runtime_config.required_capabilities =
            STRATA_CAPABILITY_CORE_LIFECYCLE | STRATA_CAPABILITY_CALLER_CLOCK |
            STRATA_CAPABILITY_HOST_SNAPSHOTS | STRATA_CAPABILITY_APPLICATION_LIFECYCLE |
            STRATA_CAPABILITY_COMPILER_ACTIVATION | STRATA_CAPABILITY_ACTION_DISPATCH |
            STRATA_CAPABILITY_RESOURCE_ADAPTER | STRATA_CAPABILITY_CLIPBOARD_IME_ADAPTER |
            STRATA_CAPABILITY_SURFACE_RUNTIME | STRATA_CAPABILITY_SURFACE_RENDER_PACKET;
        runtime_config.stable_identity_seed = 0x5354524154414150ULL;
        runtime_config.clock = strata_clock{sizeof(strata_clock), this, &Impl::clock};
        runtime_config.diagnostics = strata_diagnostic_sink{
            sizeof(strata_diagnostic_sink), this, &Impl::diagnostic,
        };
        runtime = std::make_unique<strata::Runtime>(runtime_config);

        const strata_resource_adapter resources = services.resource_adapter();
        strata::require_ok(strata_runtime_set_resource_adapter(runtime->native_handle(), &resources),
                           "desktop resource adapter installation");
        const strata_clipboard_adapter clipboard = services.clipboard_adapter();
        strata::require_ok(
            strata_runtime_set_clipboard_adapter(runtime->native_handle(), &clipboard),
            "desktop clipboard adapter installation");
        const strata_ime_adapter ime = services.ime_adapter(config.application_id);
        strata::require_ok(
            strata_runtime_set_ime_adapter(runtime->native_handle(), &ime),
            "desktop IME adapter installation"
        );

        registry = services.text("strata/registry-v1.json");
        schemas = config.schemas_resource.empty() ? std::string{}
                                                  : services.text(config.schemas_resource);
        extensions = host::select_extensions(
            config.extension_packages,
            config.extension_search_paths
        );
        extension_schemas = extensions.schemas();
        std::vector<strata_string_view> extension_views;
        extension_views.reserve(extension_schemas.size());
        for (const std::string& document : extension_schemas)
            extension_views.push_back(strata::view(document));
        const strata_application_config application{
            sizeof(strata_application_config),
            strata::view(config.application_id),
            strata::view(registry),
            strata::view(schemas),
            extension_views.empty() ? nullptr : extension_views.data(),
            extension_views.size(),
        };
        runtime->configure_application(application);
        bindings = std::make_unique<strata::host::Bindings>(*runtime, config.application_id);

        for (const strata::MaterialDeclaration& declaration :
             runtime->material_declarations("hlsl")) {
            if (declaration.source.empty()) continue;
            renderer.declare_material(declaration.id, services.text(declaration.source));
        }
    }

    void activate() {
        if (surface.has_value())
            throw std::logic_error("desktop application is already active");
        bindings->synchronize();
        source = services.text(config.module_resource);
        const strata_activation_config activation{
            sizeof(strata_activation_config),
            1U,
            strata::view(config.module_resource),
            strata::view(source),
            this,
            &Impl::load_module,
        };
        const strata_activation_info activated = runtime->activate(activation);
        if (activated.status != STRATA_ACTIVATION_ACTIVATED) {
            const std::string detail = diagnostics.empty()
                ? std::string("no compiler diagnostic was published")
                : diagnostics.back().code + ": " + diagnostics.back().message;
            throw std::runtime_error("desktop application activation was rejected: " + detail);
        }

        std::vector<strata_surface_font_resource> fonts;
        fonts.reserve(config.fonts.size());
        for (const FontResource& font : config.fonts) {
            fonts.push_back(strata_surface_font_resource{
                strata::view(font.id),
                strata::view(font.resource),
            });
        }
        std::vector<strata_surface_texture_resource> textures;
        textures.reserve(config.textures.size());
        for (const TextureResource& texture : config.textures) {
            textures.push_back(strata_surface_texture_resource{
                strata::view(texture.id),
                strata::view(texture.resource),
                static_cast<std::uint32_t>(texture.sampling),
                0U,
            });
        }
        const strata_surface_config surface_config{
            sizeof(strata_surface_config),
            strata::view(config.surface_id),
            config.root_role == SurfaceRole::screen ? STRATA_SURFACE_ROOT_SCREEN
                                                    : STRATA_SURFACE_ROOT_OVERLAY,
            0U,
            strata::view(config.root_name),
            environment(),
            fonts.empty() ? nullptr : fonts.data(),
            fonts.size(),
            extensions.pointer(),
            textures.empty() ? nullptr : textures.data(),
            textures.size(),
        };
        surface.emplace(runtime->create_surface(surface_config));
    }

    void resize_viewport(const std::uint32_t width, const std::uint32_t height,
                         const double dpi_scale, const bool adopt) {
        if (width == 0U || height == 0U || !std::isfinite(dpi_scale) || dpi_scale <= 0.0)
            return;
        framebuffer_width = width;
        framebuffer_height = height;
        scale = display_scale(width, height, dpi_scale);
        services.set_surface_scale(scale);
        renderer.resize(width, height, logical_width(), logical_height());
        if (!adopt || !surface.has_value())
            return;
        ++environment_generation;
        const strata_surface_environment next = environment();
        std::uint32_t adopted = 0U;
        strata::require_ok(
            strata_surface_adopt_environment(surface->native_handle(), &next, &adopted),
            "desktop environment adoption");
        if (adopted == 0U)
            throw std::runtime_error("desktop environment generation was rejected");
    }

    void enqueue(strata_input_event event, const std::string_view event_text = {}) {
        if (!surface.has_value())
            return;
        frame_time = now();
        event.struct_size = sizeof(event);
        event.version = STRATA_INPUT_EVENT_VERSION_2;
        event.modifiers = current_modifiers();
        event.text = strata::view(event_text);
        event.timestamp_nanoseconds = frame_time;
        if (event.kind != STRATA_INPUT_KEY)
            event.key_action = STRATA_KEY_PRESS;
        strata_surface_input_batch_info info{sizeof(strata_surface_input_batch_info)};
        strata::require_ok(
            strata_surface_enqueue_input(surface->native_handle(), &event, 1U, &info),
            "desktop input enqueue");
        if (info.accepted_event_count != 1U)
            throw std::runtime_error("desktop input event was not accepted atomically");
    }

    void flush_pointer_move() {
        if (!pending_pointer_move.has_value())
            return;
        const strata_input_event event = *pending_pointer_move;
        pending_pointer_move.reset();
        enqueue(event);
    }

    void reload_resources() {
        if (!surface.has_value()) {
            throw std::logic_error("desktop application must be activated before reloading resources");
        }
        const strata_resource_adapter resources = services.reload_resource_adapter();
        strata::require_ok(
            strata_runtime_set_resource_adapter(runtime->native_handle(), &resources),
            "desktop resource adapter refresh"
        );
        surface->reload_resources();
    }

    void frame() {
        if (!surface.has_value())
            throw std::logic_error("desktop application must be activated before framing");
        flush_pointer_move();
        bindings->synchronize();
        frame_time = now();
        const strata_surface_frame_info info = surface->frame(frame_time);
        const host::RenderPacket& packet = decoder.decode(surface->render_packet());
        renderer.render(packet);
        frame_available = true;

        strata_profiler_host_frame telemetry{};
        telemetry.struct_size = sizeof(telemetry);
        telemetry.version = STRATA_PROFILER_HOST_FRAME_VERSION_CURRENT;
        telemetry.draw_calls = packet.planned_draw_count;
        telemetry.batches = packet.batches.size();
        telemetry.vertices = packet.vertices.size() / 88U;
        telemetry.submit_nanos = 0;
        static_cast<void>(info);
        strata::require_ok(
            strata_surface_record_host_frame(surface->native_handle(), &telemetry),
            "desktop host profiler publication");
    }

    void close() {
        pending_pointer_move.reset();
        if (surface.has_value()) {
            const host::RenderPacket& terminal = decoder.decode(surface->prepare_release_packet());
            renderer.consume_resources(terminal);
            surface->acknowledge_release_packet();
            surface->close();
            surface.reset();
        }
        bindings.reset();
        if (runtime != nullptr) {
            runtime->close();
            runtime.reset();
        }
    }

    void dispose_after_failure() noexcept {
        pending_pointer_move.reset();
        if (surface.has_value()) {
            try {
                surface->abandon();
            } catch (...) {
                std::terminate();
            }
            surface.reset();
        }
        bindings.reset();
        runtime.reset();
    }

    HWND window = nullptr;
    ApplicationConfig config;
    HostServices services;
    Renderer renderer;
    std::unique_ptr<strata::Runtime> runtime;
    std::unique_ptr<strata::host::Bindings> bindings;
    host::SelectedExtensions extensions;
    std::optional<strata::Surface> surface;
    host::RenderPacketDecoder decoder;
    std::vector<Diagnostic> diagnostics;
    std::vector<std::string> extension_schemas;
    std::string registry;
    std::string schemas;
    std::string source;
    std::string module_id_scratch;
    std::string module_text_scratch;
    std::optional<strata_input_event> pending_pointer_move;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
    std::int64_t frame_time = 0;
    std::uint64_t environment_generation = 1U;
    std::uint32_t framebuffer_width = 1U;
    std::uint32_t framebuffer_height = 1U;
    std::uint32_t captured_buttons = 0U;
    double scale = 1.0;
    wchar_t high_surrogate = 0;
    bool frame_available = false;
    bool tracking_mouse_leave = false;
};

ApplicationHost::ApplicationHost(HWND window, std::filesystem::path resource_root,
                                 ApplicationConfig config, const ApplicationOptions options)
    : impl_(std::make_unique<Impl>(window, std::move(resource_root), std::move(config), options)) {}

ApplicationHost::~ApplicationHost() = default;

strata::Runtime& ApplicationHost::runtime() noexcept { return *impl_->runtime; }
strata::host::Bindings& ApplicationHost::bindings() noexcept { return *impl_->bindings; }

void ApplicationHost::publish(const std::string_view snapshot_id,
                              const strata::host::Value& value) {
    static_cast<void>(impl_->runtime->publish_host_snapshot(snapshot_id, value.json()));
}

void ApplicationHost::activate() { impl_->activate(); }

void ApplicationHost::resize(const std::uint32_t framebuffer_width,
                             const std::uint32_t framebuffer_height, const double dpi_scale) {
    impl_->resize_viewport(framebuffer_width, framebuffer_height, dpi_scale, true);
}

void ApplicationHost::pointer(const std::uint32_t kind, const std::int32_t button,
                              const double framebuffer_x, const double framebuffer_y) {
    strata_input_event event{};
    event.kind = kind;
    event.pointer_id = 0;
    event.button = button;
    event.x = framebuffer_x / impl_->scale;
    event.y = framebuffer_y / impl_->scale;
    if (kind == STRATA_INPUT_POINTER_MOVE) {
        impl_->pending_pointer_move = event;
        return;
    }
    impl_->flush_pointer_move();
    impl_->enqueue(event);
}

void ApplicationHost::scroll(const double framebuffer_x, const double framebuffer_y,
                             const double delta_x, const double delta_y) {
    impl_->flush_pointer_move();
    strata_input_event event{};
    event.kind = STRATA_INPUT_SCROLL;
    event.x = framebuffer_x / impl_->scale;
    event.y = framebuffer_y / impl_->scale;
    event.delta_x = delta_x;
    event.delta_y = delta_y;
    impl_->enqueue(event);
}

void ApplicationHost::key(const std::uint32_t virtual_key, const std::uint32_t action) {
    impl_->flush_pointer_move();
    strata_input_event event{};
    event.kind = STRATA_INPUT_KEY;
    event.key_action = action;
    impl_->enqueue(event, key_name(virtual_key));
}

void ApplicationHost::text(std::string utf8) {
    impl_->flush_pointer_move();
    strata_input_event event{};
    event.kind = STRATA_INPUT_TEXT;
    impl_->enqueue(event, utf8);
}

void ApplicationHost::ime_preedit(
    std::string utf8,
    const std::size_t selection_start,
    const std::size_t selection_end
) {
    impl_->flush_pointer_move();
    strata_input_event event{};
    event.kind = STRATA_INPUT_IME_PREEDIT;
    event.selection_start = selection_start;
    event.selection_end = selection_end;
    impl_->enqueue(event, utf8);
}

std::optional<std::intptr_t> ApplicationHost::handle_window_message(
    const std::uint32_t message,
    const std::uintptr_t word,
    const std::intptr_t long_value
) {
    const WPARAM word_parameter = static_cast<WPARAM>(word);
    const LPARAM long_parameter = static_cast<LPARAM>(long_value);
    switch (message) {
    case WM_SIZE: {
        const std::uint32_t width = LOWORD(long_parameter);
        const std::uint32_t height = HIWORD(long_parameter);
        if (width != 0U && height != 0U) {
            const UINT dpi = GetDpiForWindow(impl_->window);
            resize(width, height, dpi == 0U ? 1.0 : static_cast<double>(dpi) / 96.0);
        }
        return 0;
    }
    case WM_DPICHANGED: {
        const auto* const bounds = reinterpret_cast<const RECT*>(long_parameter);
        if (bounds != nullptr) {
            SetWindowPos(
                impl_->window,
                nullptr,
                bounds->left,
                bounds->top,
                bounds->right - bounds->left,
                bounds->bottom - bounds->top,
                SWP_NOACTIVATE | SWP_NOZORDER
            );
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!impl_->tracking_mouse_leave) {
            TRACKMOUSEEVENT tracking{
                sizeof(TRACKMOUSEEVENT), TME_LEAVE, impl_->window, HOVER_DEFAULT,
            };
            impl_->tracking_mouse_leave = TrackMouseEvent(&tracking) != FALSE;
        }
        pointer(
            STRATA_INPUT_POINTER_MOVE,
            0,
            GET_X_LPARAM(long_parameter),
            GET_Y_LPARAM(long_parameter)
        );
        return 0;
    }
    case WM_MOUSELEAVE:
        impl_->tracking_mouse_leave = false;
        if (impl_->captured_buttons == 0U) {
            pointer(STRATA_INPUT_POINTER_CANCEL, 0, 0.0, 0.0);
        }
        return 0;
    case WM_XBUTTONDOWN: {
        const bool first = GET_XBUTTON_WPARAM(word_parameter) == XBUTTON1;
        impl_->captured_buttons |= first ? 8U : 16U;
        SetFocus(impl_->window);
        SetCapture(impl_->window);
        pointer(
            STRATA_INPUT_POINTER_PRESS,
            first ? 3 : 4,
            GET_X_LPARAM(long_parameter),
            GET_Y_LPARAM(long_parameter)
        );
        return 1;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        impl_->captured_buttons |= message == WM_LBUTTONDOWN ? 1U
            : message == WM_RBUTTONDOWN ? 2U : 4U;
        SetFocus(impl_->window);
        SetCapture(impl_->window);
        pointer(
            STRATA_INPUT_POINTER_PRESS,
            message == WM_LBUTTONDOWN ? 0 : message == WM_RBUTTONDOWN ? 1 : 2,
            GET_X_LPARAM(long_parameter),
            GET_Y_LPARAM(long_parameter)
        );
        return 0;
    case WM_XBUTTONUP: {
        const bool first = GET_XBUTTON_WPARAM(word_parameter) == XBUTTON1;
        pointer(
            STRATA_INPUT_POINTER_RELEASE,
            first ? 3 : 4,
            GET_X_LPARAM(long_parameter),
            GET_Y_LPARAM(long_parameter)
        );
        impl_->captured_buttons &= first ? ~8U : ~16U;
        if (impl_->captured_buttons == 0U && GetCapture() == impl_->window) ReleaseCapture();
        return 1;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        pointer(
            STRATA_INPUT_POINTER_RELEASE,
            message == WM_LBUTTONUP ? 0 : message == WM_RBUTTONUP ? 1 : 2,
            GET_X_LPARAM(long_parameter),
            GET_Y_LPARAM(long_parameter)
        );
        impl_->captured_buttons &= message == WM_LBUTTONUP ? ~1U
            : message == WM_RBUTTONUP ? ~2U : ~4U;
        if (impl_->captured_buttons == 0U && GetCapture() == impl_->window) ReleaseCapture();
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        POINT point{GET_X_LPARAM(long_parameter), GET_Y_LPARAM(long_parameter)};
        ScreenToClient(impl_->window, &point);
        const double delta = static_cast<double>(GET_WHEEL_DELTA_WPARAM(word_parameter)) /
            WHEEL_DELTA;
        scroll(
            point.x,
            point.y,
            message == WM_MOUSEHWHEEL ? delta : 0.0,
            message == WM_MOUSEWHEEL ? delta : 0.0
        );
        return 0;
    }
    case WM_KEYDOWN:
    case WM_KEYUP:
        key(
            static_cast<std::uint32_t>(word_parameter),
            message == WM_KEYUP ? STRATA_KEY_RELEASE
                : (HIWORD(long_parameter) & KF_REPEAT) != 0U
                    ? STRATA_KEY_REPEAT
                    : STRATA_KEY_PRESS
        );
        return 0;
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        key(
            static_cast<std::uint32_t>(word_parameter),
            message == WM_SYSKEYUP ? STRATA_KEY_RELEASE
                : (HIWORD(long_parameter) & KF_REPEAT) != 0U
                    ? STRATA_KEY_REPEAT
                    : STRATA_KEY_PRESS
        );
        if (word_parameter == VK_F10) return 0;
        return std::nullopt;
    case WM_CHAR: {
        const wchar_t value = static_cast<wchar_t>(word_parameter);
        if (value >= 0xD800 && value <= 0xDBFF) {
            impl_->high_surrogate = value;
            return 0;
        }
        if (value >= 0xDC00 && value <= 0xDFFF) {
            if (impl_->high_surrogate != 0) {
                text(win32::utf8_character(impl_->high_surrogate, value));
            }
            impl_->high_surrogate = 0;
            return 0;
        }
        impl_->high_surrogate = 0;
        if (value >= 0x20 && value != 0x7F) {
            text(win32::utf8_character(value));
        }
        return 0;
    }
    case WM_UNICHAR:
        if (word_parameter == UNICODE_NOCHAR) return 1;
        if (word_parameter < 0x20U || word_parameter == 0x7FU) return 0;
        if (const std::string value = win32::utf8_code_point(
                static_cast<std::uint32_t>(word_parameter)
            ); !value.empty()) {
            text(value);
        }
        return 0;
    case WM_IME_STARTCOMPOSITION:
        ime_preedit({}, 0U, 0U);
        return 0;
    case WM_IME_COMPOSITION: {
        const win32::ImeUpdate update = win32::read_ime_update(impl_->window, long_value);
        if (update.committed.has_value() && !update.committed->empty()) text(*update.committed);
        if (update.preedit.has_value()) {
            ime_preedit(*update.preedit, update.selection_start, update.selection_end);
        }
        return 0;
    }
    case WM_IME_ENDCOMPOSITION:
        ime_preedit({}, 0U, 0U);
        return 0;
    case WM_IME_CHAR:
        return 0;
    case WM_KILLFOCUS:
    case WM_CANCELMODE:
        impl_->captured_buttons = 0U;
        impl_->high_surrogate = 0;
        ime_preedit({}, 0U, 0U);
        cancel_interactions();
        if (GetCapture() == impl_->window) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        if (impl_->captured_buttons != 0U) {
            impl_->captured_buttons = 0U;
            cancel_interactions();
        }
        return 0;
    default: return std::nullopt;
    }
}

void ApplicationHost::cancel_interactions() noexcept {
    impl_->pending_pointer_move.reset();
    if (impl_->surface.has_value())
        static_cast<void>(strata_surface_cancel_interactions(impl_->surface->native_handle()));
}

void ApplicationHost::reload_resources() { impl_->reload_resources(); }
void ApplicationHost::frame() { impl_->frame(); }
void ApplicationHost::close() { impl_->close(); }
bool ApplicationHost::active() const noexcept { return impl_->surface.has_value(); }
bool ApplicationHost::has_frame() const noexcept { return impl_->frame_available; }
double ApplicationHost::scale() const noexcept { return impl_->scale; }
const std::vector<Diagnostic>& ApplicationHost::diagnostics() const noexcept {
    return impl_->diagnostics;
}

} // namespace strata::desktop
