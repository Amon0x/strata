#include <strata/desktop.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
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

#include "frame_clock.hpp"
#include "host/session.hpp"
#include "host_services.hpp"
#include "ime.hpp"
#include "platform.hpp"
#include "renderer.hpp"
#include "win32/key_names.hpp"
#include "window_input.hpp"
#include <strata/render_packet.hpp>

namespace strata::desktop {
namespace {

[[nodiscard]] std::string copy(const strata_string_view value) {
    return value.size == 0U ? std::string{} : std::string(value.data, value.size);
}

[[nodiscard]] double display_scale(const std::uint32_t framebuffer_width,
                                   const std::uint32_t framebuffer_height, const double dpi_scale) {
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

void require_resource_id(const std::string_view value, const std::string_view label) {
    const std::filesystem::path path{std::string(value)};
    // On Windows "/x" (root directory, no drive) and "D:x" (drive-relative) are not absolute, yet
    // either one joined to the resource root leaves it.
    if (value.empty() || path.has_root_name() || path.has_root_directory())
        throw std::invalid_argument(std::string(label) + " must be a relative resource id");
    for (const std::filesystem::path& part : path) {
        if (part == "..")
            throw std::invalid_argument(std::string(label) + " must not escape the resource root");
    }
}

void append_unique(std::vector<std::string>& values, const std::string_view value) {
    if (std::ranges::find(values, value) == values.end())
        values.emplace_back(value);
}

} // namespace

struct ApplicationHost::Impl final {
    Impl(HWND window, std::filesystem::path resource_root, ApplicationConfig config,
         const ApplicationOptions options)
        : window(window), config(std::move(config)), resource_root(resource_root),
          services(window, std::move(resource_root)),
          platform(
              window, [this](const std::string_view id) { return services.bytes(id); },
              [this](const std::string_view id) { return services.path(id).wstring(); }),
          renderer(create_presentation(window, options.vsync,
                                       options.asynchronous_shader_compilation,
                                       options.presentation)) {
        try {
            initialize();
        } catch (...) {
            dispose_after_failure();
            throw;
        }
    }

    ~Impl() noexcept {
        try {
            while (!close())
                dispatch_presentation_shutdown_messages();
        } catch (...) {
            dispose_after_failure();
        }
    }

    [[nodiscard]] std::int64_t now() const noexcept { return clock.now(); }

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
        result.input_capabilities = STRATA_SURFACE_INPUT_POINTER | STRATA_SURFACE_INPUT_KEYBOARD |
                                    STRATA_SURFACE_INPUT_IME | STRATA_SURFACE_INPUT_CLIPBOARD;
        result.reduced_motion = config.reduced_motion ? 1U : 0U;
        platform.appearance().apply(result);
        if (config.text_antialiasing == TextAntialiasing::grayscale) {
            result.text_antialiasing = STRATA_TEXT_ANTIALIASING_GRAYSCALE;
        } else if (config.text_antialiasing == TextAntialiasing::subpixel &&
                   result.text_antialiasing == STRATA_TEXT_ANTIALIASING_GRAYSCALE) {
            result.text_antialiasing = STRATA_TEXT_ANTIALIASING_SUBPIXEL_RGB;
        }
        return result;
    }

    void record_diagnostic(const strata_diagnostic& value) {
        diagnostics.push_back(Diagnostic{
            value.id,
            value.severity,
            copy(value.code),
            copy(value.message),
            copy(value.source_id),
            value.range.line_start,
            value.range.column_start,
            copy(value.component_path),
            copy(value.expected),
        });
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
        for (const ImageResource& image : config.images) {
            if (image.id.empty())
                throw std::invalid_argument("desktop image id must not be empty");
            require_resource_id(image.resource, "desktop image resource");
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

        setup.emplace();
        setup->resources = services.resources();
        setup->schemas_resource = config.schemas_resource;
        setup->program_language = "hlsl";
        host::SessionOptions& options = setup->session;
        options.application_id = config.application_id;
        options.stable_identity_seed = 0x5354524154414150ULL;
        options.frame_wake = [target = window] {
            if (!PostMessageW(target, WM_NULL, 0U, 0))
                throw std::runtime_error("could not wake the desktop window");
        };
        options.extension_packages = config.extension_packages;
        options.extension_search_paths = config.extension_search_paths;
        options.on_diagnostic = [this](const strata_diagnostic& value) { record_diagnostic(value); };
        pending_bindings = std::make_unique<host::Bindings>(config.application_id);
        pending_modules.push_back(config.module_resource);
        // Files dragged in from the shell, in logical units.
        platform.accept_files([this](const std::uint32_t kind, const double x, const double y,
                                     const std::string& paths) {
            flush_pointer_move();
            strata_input_event event{};
            event.kind = kind;
            event.x = x / scale;
            event.y = y / scale;
            enqueue(event, paths);
        });
    }

    void adopt_prepared() {
        if (!preparation.has_value() || !preparation->ready())
            return;
        host::SessionAdapters adapters;
        adapters.clipboard = services.clipboard_adapter();
        adapters.ime = services.ime_adapter(config.application_id);
        adapters.effects = platform.effect_adapter(config.application_id);
        host::PreparedSession prepared = preparation->take(adapters, std::move(pending_bindings));
        session = std::move(prepared.session);
        preparation.reset();
        source = std::move(prepared.source);
        if (prepared_environment != environment_generation &&
            !session->adopt_environment(environment())) {
            throw std::runtime_error("prepared desktop environment generation was rejected");
        }
        for (const host::MaterialSource& material : prepared.materials) {
            const strata::MaterialDeclaration& declaration = material.declaration;
            append_unique(program_resources, declaration.source);
        }
        for (const host::EffectSource& effect : prepared.effects) {
            const strata::EffectPassDeclaration& pass = effect.declaration;
            if (!pass.source.empty())
                append_unique(program_resources, pass.source);
        }
        render_stream = renderer->attach(config.surface_id);
        programs = renderer->declare_programs(
            render_stream, {std::move(prepared.materials), std::move(prepared.effects)});
        for (host::CapturedSnapshot& snapshot : pending_publications) {
            static_cast<void>(
                session->runtime().publish_host_snapshot(snapshot.source.id, snapshot.value.json()));
        }
        pending_publications.clear();
    }

    [[nodiscard]] bool activate_source() {
        if (activation_generation == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("desktop application activation generation is exhausted");
        }
        session->bindings().synchronize();
        std::string candidate = services.text(config.module_resource);
        const strata_activation_info activated =
            session->activate_source(config.module_resource, candidate, ++activation_generation);
        if (activated.status != STRATA_ACTIVATION_ACTIVATED)
            return false;
        source = std::move(candidate);
        return true;
    }

    void activate() {
        if (session != nullptr || preparation.has_value() || !setup.has_value())
            throw std::logic_error("desktop application has already started or closed");
        setup->module_resource = config.module_resource;
        setup->generation = ++activation_generation;
        setup->time_nanoseconds = now();
        auto snapshots = pending_bindings->capture_snapshots();
        if (setup->snapshots.empty()) {
            setup->snapshots = std::move(snapshots);
        } else {
            // Preserve explicit startup order without copying large values during vector growth.
            setup->snapshots.reserve(setup->snapshots.size() + snapshots.size());
            for (host::CapturedSnapshot& snapshot : snapshots)
                setup->snapshots.push_back(std::move(snapshot));
        }
        setup->actions = pending_bindings->capture_actions();
        host::SessionSurface& surface = setup->surface;
        surface.id = config.surface_id;
        surface.root_role = config.root_role == SurfaceRole::screen ? STRATA_SURFACE_ROOT_SCREEN
                                                                    : STRATA_SURFACE_ROOT_OVERLAY;
        surface.root_name = config.root_name;
        surface.environment = environment();
        for (const FontResource& font : config.fonts)
            surface.fonts.push_back(host::SessionFont{font.id, font.resource, font.system});
        for (const ImageResource& image : config.images) {
            surface.images.push_back(host::SessionImage{
                image.id, image.resource, static_cast<std::uint32_t>(image.sampling)});
        }
        prepared_environment = surface.environment.generation;
        preparation.emplace(host::Session::prepare(std::move(*setup)));
        setup.reset();
    }

    [[nodiscard]] std::optional<host::PresentationReceipt> reload_program_source(
        const std::string_view resource_id) {
        if (session == nullptr)
            throw std::logic_error("desktop application must be active before program reloading");
        if (std::ranges::find(program_resources, resource_id) == program_resources.end()) {
            return std::nullopt;
        }
        const std::string hlsl_source = services.text(resource_id);
        host::PresentationPrograms replacements;
        for (const strata::MaterialDeclaration& declaration :
             session->runtime().material_declarations("hlsl")) {
            if (declaration.source == resource_id) {
                replacements.materials.push_back({declaration, hlsl_source});
            }
        }
        for (const strata::EffectPassDeclaration& pass :
             session->runtime().effect_pass_declarations("hlsl")) {
            if (pass.source != resource_id)
                continue;
            replacements.effects.push_back({pass, hlsl_source});
        }
        return renderer->declare_programs(render_stream, std::move(replacements));
    }

    void resize_viewport(const std::uint32_t width, const std::uint32_t height,
                         const double dpi_scale, const bool adopt) {
        if (closing || width == 0U || height == 0U || !std::isfinite(dpi_scale) || dpi_scale <= 0.0)
            return;
        framebuffer_width = width;
        framebuffer_height = height;
        scale = display_scale(width, height, dpi_scale);
        services.set_surface_scale(scale);
        renderer->resize({width, height, logical_width(), logical_height()});
        if (session == nullptr)
            background_presented = false;
        if (!adopt)
            return;
        ++environment_generation;
        if (closing || session == nullptr || !session->has_surface())
            return;
        if (!session->adopt_environment(environment()))
            throw std::runtime_error("desktop environment generation was rejected");
    }

    void enqueue(strata_input_event event, const std::string_view event_text = {}) {
        if (closing || session == nullptr || !session->has_surface())
            return;
        frame_time = now();
        session->set_time(frame_time);
        event.struct_size = sizeof(event);
        event.version = STRATA_INPUT_EVENT_VERSION_2;
        event.modifiers = current_modifiers();
        event.text = strata::view(event_text);
        event.timestamp_nanoseconds = frame_time;
        if (event.kind != STRATA_INPUT_KEY)
            event.key_action = STRATA_KEY_PRESS;
        session->enqueue(std::span<const strata_input_event>(&event, 1U));
    }

    void flush_pointer_move() {
        if (!pending_pointer_move.has_value())
            return;
        const strata_input_event event = *pending_pointer_move;
        pending_pointer_move.reset();
        enqueue(event);
    }

    /** Runs the platform requests the last frame made and hands their answers back. */
    void answer_platform_requests() {
        for (PlatformAnswer& answer : platform.pump()) {
            if (answer.channel.has_value()) {
                const std::string said = PlatformServices::event_json(answer);
                strata_input_event event{};
                event.kind = STRATA_INPUT_PLATFORM_EVENT;
                enqueue(event, said);
                continue;
            }
            if (answer.image.has_value()) {
                session->runtime().publish_image(answer.image->id, answer.image->width,
                                                 answer.image->height, answer.image->rgba);
            }
            strata_input_event event{};
            event.kind = STRATA_INPUT_PLATFORM_RESULT;
            event.selection_start = answer.request;
            enqueue(event, answer.json);
        }
        // Videos show their newest frame through the runtime image they play into.
        for (auto& [client, frame] : platform.video_frames())
            session->runtime().publish_image(frame.id, frame.width, frame.height, frame.rgba);
        // A video nothing shows any more (its page closed) pauses until something does again.
        platform.follow_video_use([this](std::string_view, const std::string_view image) {
            return session != nullptr && session->has_surface() &&
                   session->surface().shows_image(image);
        });
        // The system's move loop swallowed the release of the press that began it.
        if (platform.take_window_moved()) {
            pending_pointer_move.reset();
            static_cast<void>(
                strata_surface_cancel_interactions(session->surface().native_handle()));
        }
        renderer->set_transparent(platform.transparent());
        // The user changed the system's theme or accent: env follows.
        if (platform.take_appearance_changed() && session->has_surface()) {
            ++environment_generation;
            static_cast<void>(session->adopt_environment(environment()));
        }
        // A node capture reads this frame's pixels once it is presented.
        if (platform.wants_frame() && !platform_capture.has_value()) {
            platform_capture = renderer->capture_next_frame();
            platform.frame_requested(*platform_capture);
        }
    }

    void adopt_rendering() {
        renderer->rethrow_failure();
        if (!closing && programs.has_value() && programs->ready()) {
            programs->get();
            programs.reset();
        }
        renderer->take_frames(rendered);
        for (host::PresentationFrame& frame : rendered) {
            last_frame_presented = last_frame_presented || frame.presented;
            for (const host::PresentationLayerFeedback& layer : frame.layers) {
                if (closing || session == nullptr || layer.stream != render_stream)
                    continue;
                frame_available = frame_available || frame.drawn;
                if (!layer.backdrop.empty())
                    session->surface().report_backdrop(layer.backdrop);
                session->surface().record_host_frame(layer.profiler);
            }
            if (!closing && frame.capture.has_value() && platform_capture.has_value() &&
                std::ranges::find(frame.capture_requests, *platform_capture) !=
                    frame.capture_requests.end()) {
                platform.frame_presented(*platform_capture, frame.capture->width, frame.capture->height,
                                          frame.capture->rgba);
                platform_capture.reset();
            }
        }
    }

    void frame() {
        if (closing) {
            static_cast<void>(close());
            return;
        }
        if (session == nullptr && !preparation.has_value())
            throw std::logic_error("desktop application must be activated before framing");
        clock.begin_frame();
        last_frame_presented = false;
        adopt_rendering();
        adopt_prepared();
        if (session == nullptr) {
            if (!background_presented) {
                static_cast<void>(renderer->commit({}));
                background_presented = true;
            }
            adopt_rendering();
            last_frame_idle = true;
            clock.end_frame();
            return;
        }
        answer_platform_requests();
        flush_pointer_move();
        frame_time = now();
        host::SurfacePacketCommit framed = session->frame(frame_time);
        last_frame_idle = framed.surface.idle != 0U;
        host::PresentationCommit commit;
        commit.animate = !last_frame_idle;
        commit.layers.push_back(
            {render_stream, std::move(framed.encoded), framed.surface.unchanged != 0U});
        commit.order.push_back(render_stream);
        static_cast<void>(renderer->commit(std::move(commit)));
        adopt_rendering();
        clock.end_frame();
    }

    [[nodiscard]] bool close() {
        if (!closing) {
            closing = true;
            pending_pointer_move.reset();
            preparation.reset();
            setup.reset();
            pending_bindings.reset();
            pending_publications.clear();
            programs.reset();
            platform_capture.reset();
            platform.close_client(config.application_id);
            if (session != nullptr)
                release = renderer->retire(render_stream, session->prepare_release());
        }
        renderer->rethrow_failure();
        if (release.has_value()) {
            if (!release->ready())
                return false;
            release->get();
            session->acknowledge_release();
            session->close({});
            session.reset();
            release.reset();
        }
        renderer->stop();
        return renderer->stopped();
    }

    void dispose_after_failure() noexcept {
        pending_pointer_move.reset();
        preparation.reset();
        setup.reset();
        pending_bindings.reset();
        if (session != nullptr)
            session->abandon();
        session.reset();
        renderer->stop();
        while (!renderer->stopped())
            dispatch_presentation_shutdown_messages();
    }

    HWND window = nullptr;
    ApplicationConfig config;
    std::filesystem::path resource_root;
    HostServices services;
    PlatformServices platform;
    std::unique_ptr<host::Presentation, PresentationDeleter> renderer;
    host::PresentationStream render_stream = 0U;
    std::optional<host::PresentationReceipt> programs;
    std::optional<host::PresentationReceipt> release;
    std::vector<host::PresentationFrame> rendered;
    std::unique_ptr<host::Bindings> pending_bindings;
    std::optional<host::SessionPrepareOptions> setup;
    std::optional<host::SessionPreparation> preparation;
    std::unique_ptr<host::Session> session;
    std::vector<host::CapturedSnapshot> pending_publications;
    std::vector<std::string> pending_modules;
    std::uint64_t prepared_environment = 0U;
    std::vector<Diagnostic> diagnostics;
    std::vector<std::string> program_resources;
    std::string source;
    std::optional<strata_input_event> pending_pointer_move;
    FrameClock clock;
    std::int64_t frame_time = 0;
    std::uint64_t environment_generation = 1U;
    std::uint64_t activation_generation = 0U;
    std::uint32_t framebuffer_width = 1U;
    std::uint32_t framebuffer_height = 1U;
    double scale = 1.0;
    bool frame_available = false;
    bool closing = false;
    bool background_presented = false;
    /** The last frame showed something new; and it left nothing to do until input. */
    bool last_frame_presented = false;
    bool last_frame_idle = false;
    /** A node capture waits for the frame being drawn. */
    std::optional<std::uint64_t> platform_capture;
    win32::WindowInputState input;
};

ApplicationHost::ApplicationHost(HWND window, std::filesystem::path resource_root,
                                 ApplicationConfig config, const ApplicationOptions options)
    : impl_(std::make_unique<Impl>(window, std::move(resource_root), std::move(config), options)) {}

ApplicationHost::~ApplicationHost() = default;

strata::Runtime& ApplicationHost::runtime() {
    if (impl_->session == nullptr)
        throw std::logic_error("desktop runtime is not yet adopted or has closed");
    return impl_->session->runtime();
}
strata::host::Bindings& ApplicationHost::bindings() {
    if (impl_->session != nullptr)
        return impl_->session->bindings();
    if (impl_->pending_bindings == nullptr)
        throw std::logic_error("desktop bindings have closed");
    return *impl_->pending_bindings;
}

void ApplicationHost::publish(const std::string_view snapshot_id,
                              strata::host::Value value) {
    if (impl_->session != nullptr) {
        static_cast<void>(impl_->session->runtime().publish_host_snapshot(snapshot_id, value.json()));
    } else if (impl_->setup.has_value()) {
        impl_->setup->snapshots.push_back({{std::string(snapshot_id), 0U}, std::move(value)});
    } else if (impl_->preparation.has_value()) {
        impl_->pending_publications.push_back({{std::string(snapshot_id), 0U}, std::move(value)});
    } else {
        throw std::logic_error("desktop application has closed");
    }
}

void ApplicationHost::activate() {
    impl_->activate();
}

bool ApplicationHost::reactivate() {
    if (impl_->session == nullptr || !impl_->session->has_surface()) {
        throw std::logic_error("desktop application must be active before source reactivation");
    }
    return impl_->activate_source();
}

std::optional<host::PresentationReceipt> ApplicationHost::reload_program_source(
    const std::string_view resource_id) {
    return impl_->reload_program_source(resource_id);
}

const std::vector<std::string>& ApplicationHost::module_resources() const noexcept {
    return impl_->session != nullptr ? impl_->session->module_resources() : impl_->pending_modules;
}

const std::vector<std::string>& ApplicationHost::program_resources() const noexcept {
    return impl_->program_resources;
}

void ApplicationHost::resize(const std::uint32_t framebuffer_width,
                             const std::uint32_t framebuffer_height, const double dpi_scale) {
    impl_->resize_viewport(framebuffer_width, framebuffer_height, dpi_scale, true);
}

void ApplicationHost::pointer(const std::uint32_t kind, const std::int32_t button,
                              const double framebuffer_x, const double framebuffer_y) {
    if (impl_->closing)
        return;
    strata_input_event event{};
    event.kind = kind;
    event.pointer_id = 0;
    event.button = button;
    event.x = framebuffer_x / impl_->scale;
    event.y = framebuffer_y / impl_->scale;
    // Materials follow the pointer on the GPU (materialPointer), with no frame of the interface.
    impl_->renderer->set_pointer(
        kind == STRATA_INPUT_POINTER_CANCEL
            ? std::nullopt
            : std::optional(std::array<double, 2U>{framebuffer_x, framebuffer_y}));
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
    impl_->enqueue(event, strata::win32::key_name(virtual_key));
}

void ApplicationHost::text(std::string utf8) {
    impl_->flush_pointer_move();
    strata_input_event event{};
    event.kind = STRATA_INPUT_TEXT;
    impl_->enqueue(event, utf8);
}

void ApplicationHost::ime_preedit(std::string utf8, const std::size_t selection_start,
                                  const std::size_t selection_end) {
    impl_->flush_pointer_move();
    strata_input_event event{};
    event.kind = STRATA_INPUT_IME_PREEDIT;
    event.selection_start = selection_start;
    event.selection_end = selection_end;
    impl_->enqueue(event, utf8);
}

std::optional<std::intptr_t>
ApplicationHost::handle_window_message(const std::uint32_t message, const std::uintptr_t word,
                                       const std::intptr_t long_value) {
    const WPARAM word_parameter = static_cast<WPARAM>(word);
    const LPARAM long_parameter = static_cast<LPARAM>(long_value);
    if (const std::optional<std::intptr_t> platform =
            impl_->platform.window_message(message, word, long_value);
        platform.has_value()) {
        return platform;
    }
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
            SetWindowPos(impl_->window, nullptr, bounds->left, bounds->top,
                         bounds->right - bounds->left, bounds->bottom - bounds->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
        }
        return 0;
    }
    default:
        break;
    }
    const std::optional<LRESULT> handled = win32::translate_window_input(
        *this, impl_->input, impl_->window, message, word_parameter, long_parameter);
    return handled.has_value() ? std::optional<std::intptr_t>(*handled) : std::nullopt;
}

void ApplicationHost::cancel_interactions() noexcept {
    impl_->pending_pointer_move.reset();
    if (impl_->session != nullptr && impl_->session->has_surface()) {
        static_cast<void>(
            strata_surface_cancel_interactions(impl_->session->surface().native_handle()));
    }
}

std::optional<std::uint32_t> ApplicationHost::pointer_cursor() const noexcept {
    if (impl_->session == nullptr || !impl_->session->has_surface())
        return std::nullopt;
    std::uint32_t cursor = STRATA_POINTER_CURSOR_DEFAULT;
    if (strata_surface_pointer_cursor(impl_->session->surface().native_handle(), &cursor).status !=
        STRATA_STATUS_OK) {
        return std::nullopt;
    }
    return cursor;
}

void ApplicationHost::frame() {
    impl_->frame();
}

std::vector<WindowRequest> ApplicationHost::take_window_requests() {
    return impl_->platform.take_window_requests();
}

void ApplicationHost::answer_window_request(const WindowRequest& request, std::string json) {
    impl_->platform.answer(request, std::move(json));
}

std::unique_ptr<ApplicationHost> ApplicationHost::open_window(HWND window,
                                                              const WindowRequest& request,
                                                              const ApplicationOptions options) {
    ApplicationConfig config = impl_->config;
    config.root_name = request.root;
    config.root_role = SurfaceRole::overlay;
    config.surface_id = impl_->config.surface_id + "/" + request.id;
    return std::make_unique<ApplicationHost>(window, impl_->resource_root, std::move(config),
                                             options);
}
bool ApplicationHost::close() {
    return impl_->close();
}
bool ApplicationHost::active() const noexcept {
    return !impl_->closing && impl_->session != nullptr && impl_->session->has_surface();
}
bool ApplicationHost::has_frame() const noexcept {
    return impl_->frame_available;
}
bool ApplicationHost::presented() const noexcept { return impl_->last_frame_presented; }
std::uint64_t ApplicationHost::present_count() const noexcept {
    return impl_->renderer->present_count();
}
bool ApplicationHost::idle() const noexcept { return impl_->last_frame_idle; }
double ApplicationHost::scale() const noexcept {
    return impl_->scale;
}
const std::vector<Diagnostic>& ApplicationHost::diagnostics() const noexcept {
    return impl_->diagnostics;
}

} // namespace strata::desktop
