#include "host/session.hpp"

#include <exception>
#include <new>
#include <stdexcept>
#include <utility>

#include "host/module_path.hpp"
#include "host/resource_root.hpp"
#include "host/system_fonts.hpp"

namespace strata::host {
namespace {

[[nodiscard]] std::string copy(const strata_string_view value) {
    return value.size == 0U ? std::string{} : std::string(value.data, value.size);
}

[[nodiscard]] strata_capabilities required_capabilities(const SessionOptions& options,
                                                        const SessionAdapters& adapters) noexcept {
    strata_capabilities result = session_capabilities | options.capabilities;
    if (adapters.clipboard.has_value() || adapters.ime.has_value())
        result |= STRATA_CAPABILITY_CLIPBOARD_IME_ADAPTER;
    if (adapters.effects.has_value())
        result |= STRATA_CAPABILITY_EFFECT_ADAPTER;
    if (adapters.durable_store.has_value())
        result |= STRATA_CAPABILITY_DURABLE_STATE;
    if (adapters.async_host.has_value())
        result |= STRATA_CAPABILITY_ASYNC_HOST_DATA;
    return result;
}

} // namespace

struct Session::Impl final {
    Impl(SessionOptions session_options, const SessionAdapters& adapters)
        : options(std::move(session_options)) {
        if (options.application_id.empty())
            throw std::invalid_argument("a Session requires an application id");
        strata_runtime_config config{};
        config.struct_size = sizeof(config);
        config.abi_version = STRATA_ABI_VERSION_CURRENT;
        config.required_capabilities = required_capabilities(options, adapters);
        config.stable_identity_seed = options.stable_identity_seed;
        config.clock = strata_clock{sizeof(strata_clock), this, &Impl::clock};
        config.diagnostics = strata_diagnostic_sink{sizeof(strata_diagnostic_sink), this,
                                                    &Impl::diagnostic};
        runtime = std::make_unique<strata::Runtime>(config);
        strata_runtime* const native = runtime->native_handle();

        if (adapters.resources.has_value()) {
            strata::require_ok(strata_runtime_set_resource_adapter(native, &*adapters.resources),
                               "Session resource adapter installation");
        }
        if (adapters.clipboard.has_value()) {
            strata::require_ok(strata_runtime_set_clipboard_adapter(native, &*adapters.clipboard),
                               "Session clipboard adapter installation");
        }
        if (adapters.ime.has_value()) {
            strata::require_ok(strata_runtime_set_ime_adapter(native, &*adapters.ime),
                               "Session IME adapter installation");
        }
        if (adapters.effects.has_value()) {
            strata::require_ok(strata_runtime_set_effect_adapter(native, &*adapters.effects),
                               "Session effect adapter installation");
        }

        std::vector<std::string> packages = declared_extension_packages(options.schemas);
        if (packages.empty()) {
            packages = options.extension_packages;
        } else if (!options.extension_packages.empty() && options.extension_packages != packages) {
            throw std::invalid_argument(
                "the Session's extension packages disagree with its schema declaration");
        }
        extensions =
            select_extensions(packages, options.extension_search_paths, strata::widget_api());
        const std::vector<std::string> extension_schemas = extensions.schemas();
        std::vector<strata_string_view> extension_views;
        extension_views.reserve(extension_schemas.size());
        for (const std::string& document : extension_schemas)
            extension_views.push_back(strata::view(document));
        const strata_application_config application{
            sizeof(strata_application_config),
            strata::view(options.application_id),
            strata::view(options.schemas),
            extension_views.empty() ? nullptr : extension_views.data(),
            extension_views.size(),
        };
        runtime->configure_application(application);

        if (adapters.durable_store.has_value())
            runtime->set_durable_store(*adapters.durable_store);
        if (adapters.async_host.has_value())
            runtime->set_async_host(*adapters.async_host);
        bindings = std::make_unique<Bindings>(*runtime, options.bindings_owner.empty()
                                                            ? options.application_id
                                                            : options.bindings_owner);
    }

    ~Impl() {
        // Bindings unregister through the runtime, so they go first.
        bindings.reset();
        stream.reset();
        if (surface.has_value()) {
            try {
                surface->abandon();
            } catch (...) {
                std::terminate();
            }
        }
        surface.reset();
        runtime.reset();
    }

    static std::int64_t clock(void* const user_data) noexcept {
        return static_cast<Impl*>(user_data)->time;
    }

    static void diagnostic(void* const user_data, const strata_diagnostic* const value) noexcept {
        if (user_data == nullptr || value == nullptr)
            return;
        const auto& self = *static_cast<Impl*>(user_data);
        if (!self.options.on_diagnostic)
            return;
        try {
            self.options.on_diagnostic(*value);
        } catch (...) {
            // A diagnostic observer must not unwind into the runtime.
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
            self.module_id_scratch = resolve_module_id(copy(importer_source_id), copy(import_path));
            self.remember_module(self.module_id_scratch);
            self.module_text_scratch = self.read_text(self.module_id_scratch);
            *output = strata_module_source{
                sizeof(strata_module_source),
                strata::view(self.module_id_scratch),
                strata::view(self.module_text_scratch),
            };
            return STRATA_STATUS_OK;
        } catch (const std::bad_alloc&) {
            return STRATA_STATUS_OUT_OF_MEMORY;
        } catch (const ResourceNotFound&) {
            return STRATA_STATUS_NOT_FOUND;
        } catch (const std::invalid_argument&) {
            return STRATA_STATUS_INVALID_ARGUMENT;
        } catch (...) {
            return STRATA_STATUS_SERVICE_UNAVAILABLE;
        }
    }

    [[nodiscard]] std::string read_text(const std::string_view id) const {
        if (!options.read_text)
            throw std::logic_error("the Session has no source reader for module '" +
                                   std::string(id) + "'");
        return options.read_text(id);
    }

    void remember_module(const std::string_view id) {
        for (const std::string& known : module_resources) {
            if (known == id)
                return;
        }
        module_resources.emplace_back(id);
    }

    [[nodiscard]] strata::Surface& live_surface() {
        if (!surface.has_value())
            throw std::logic_error("the Session has no Surface");
        return *surface;
    }

    void close_surface(const std::function<void(const RenderPacket&)>& consume) {
        if (!surface.has_value())
            return;
        // The packet stream owns the decoder state for the terminal release.
        if (stream == nullptr)
            stream = std::make_unique<SurfacePacketStream>(*surface);
        const RenderPacket& terminal = stream->prepare_release();
        if (consume)
            consume(terminal);
        stream->acknowledge_release();
        stream.reset();
        surface->close();
        surface.reset();
    }

    SessionOptions options;
    std::unique_ptr<strata::Runtime> runtime;
    SelectedExtensions extensions;
    std::unique_ptr<Bindings> bindings;
    std::optional<strata::Surface> surface;
    std::unique_ptr<SurfacePacketStream> stream;
    std::vector<std::string> module_resources;
    std::string module_id_scratch;
    std::string module_text_scratch;
    std::int64_t time = 0;
};

Session::Session(SessionOptions options, const SessionAdapters& adapters)
    : impl_(std::make_unique<Impl>(std::move(options), adapters)) {}

Session::~Session() = default;

strata::Runtime& Session::runtime() {
    if (impl_->runtime == nullptr)
        throw std::logic_error("the Session is closed");
    return *impl_->runtime;
}

Bindings& Session::bindings() {
    if (impl_->bindings == nullptr)
        throw std::logic_error("the Session is closed");
    return *impl_->bindings;
}

const std::string& Session::application_id() const noexcept {
    return impl_->options.application_id;
}

const SelectedExtensions& Session::extensions() const noexcept {
    return impl_->extensions;
}

std::int64_t Session::time() const noexcept {
    return impl_->time;
}

void Session::set_time(const std::int64_t time_nanoseconds) noexcept {
    impl_->time = time_nanoseconds;
}

strata_activation_info Session::activate_source(const std::string_view module_id,
                                                const std::uint64_t generation) {
    impl_->remember_module(module_id);
    const std::string text = impl_->read_text(module_id);
    return activate_source(module_id, text, generation);
}

strata_activation_info Session::activate_source(const std::string_view module_id,
                                                const std::string_view text,
                                                const std::uint64_t generation) {
    impl_->remember_module(module_id);
    const strata_activation_config activation{
        sizeof(strata_activation_config),
        generation,
        strata::view(module_id),
        strata::view(text),
        impl_.get(),
        &Impl::load_module,
    };
    return runtime().activate(activation);
}

strata_activation_info Session::activate_compiled(const std::span<const std::uint8_t> artifact,
                                                  const std::uint64_t generation) {
    const strata_compiled_activation_config activation{
        sizeof(strata_compiled_activation_config),
        generation,
        strata_bytes_view{artifact.data(), artifact.size()},
    };
    return runtime().activate(activation);
}

const std::vector<std::string>& Session::module_resources() const noexcept {
    return impl_->module_resources;
}

void Session::create_surface(const SessionSurface& surface) {
    if (impl_->surface.has_value())
        throw std::logic_error("the Session already has a Surface");
    std::vector<std::string> font_resources;
    font_resources.reserve(surface.fonts.size());
    for (const SessionFont& font : surface.fonts)
        font_resources.push_back(select_system_font(font.system).value_or(font.resource));
    std::vector<strata_surface_font_resource> fonts;
    fonts.reserve(surface.fonts.size());
    for (std::size_t index = 0U; index < surface.fonts.size(); ++index) {
        fonts.push_back(strata_surface_font_resource{
            strata::view(surface.fonts[index].id),
            strata::view(font_resources[index]),
        });
    }
    std::vector<strata_surface_image_resource> images;
    images.reserve(surface.images.size());
    for (const SessionImage& image : surface.images) {
        images.push_back(strata_surface_image_resource{
            strata::view(image.id),
            strata::view(image.resource),
            image.sampling,
            0U,
        });
    }
    strata_surface_config config{};
    config.struct_size = sizeof(config);
    config.id = strata::view(surface.id.empty() ? impl_->options.application_id : surface.id);
    config.root_role = surface.root_role;
    config.root_name = strata::view(surface.root_name);
    config.environment = surface.environment;
    config.fonts = fonts.empty() ? nullptr : fonts.data();
    config.font_count = fonts.size();
    config.extensions = impl_->extensions.pointer();
    config.images = images.empty() ? nullptr : images.data();
    config.image_count = images.size();
    impl_->surface.emplace(runtime().create_surface(config));
}

bool Session::has_surface() const noexcept {
    return impl_->surface.has_value();
}

strata::Surface& Session::surface() {
    return impl_->live_surface();
}

bool Session::adopt_environment(const strata_surface_environment& environment) {
    std::uint32_t adopted = 0U;
    strata::require_ok(
        strata_surface_adopt_environment(impl_->live_surface().native_handle(), &environment,
                                         &adopted),
        "Session environment adoption");
    return adopted != 0U;
}

void Session::enqueue(const std::span<const strata_input_event> events) {
    strata_surface_input_batch_info info{};
    info.struct_size = sizeof(info);
    strata::require_ok(strata_surface_enqueue_input(impl_->live_surface().native_handle(),
                                                    events.data(), events.size(), &info),
                       "Session input enqueue");
    if (info.accepted_event_count != events.size())
        throw std::runtime_error("the Surface input queue accepted only part of a batch");
}

SurfacePacketFrame Session::frame(const std::int64_t time_nanoseconds) {
    strata::Surface& surface = impl_->live_surface();
    if (impl_->bindings != nullptr)
        impl_->bindings->synchronize();
    impl_->time = time_nanoseconds;
    if (impl_->stream == nullptr)
        impl_->stream = std::make_unique<SurfacePacketStream>(surface);
    return impl_->stream->frame(time_nanoseconds);
}

void Session::record_host_frame(const RenderPacket& packet, const std::int64_t submit_nanoseconds) {
    ProfilerHostFrame backend;
    backend.submit_nanoseconds = submit_nanoseconds;
    record_host_frame(packet, backend);
}

void Session::record_host_frame(const RenderPacket& packet, const ProfilerHostFrame& backend) {
    ProfilerHostFrame telemetry = backend;
    telemetry.draw_calls = packet.planned_draw_count;
    telemetry.batches = packet.batches.size();
    telemetry.vertices = packet.vertices.size() / packet_vertex_bytes;
    impl_->live_surface().record_host_frame(telemetry);
}

void Session::close_surface(const std::function<void(const RenderPacket&)>& consume) {
    impl_->close_surface(consume);
}

void Session::close(const std::function<void(const RenderPacket&)>& consume) {
    impl_->close_surface(consume);
    impl_->bindings.reset();
    if (impl_->runtime != nullptr) {
        impl_->runtime->close();
        impl_->runtime.reset();
    }
}

void Session::abandon() noexcept {
    impl_->stream.reset();
    if (impl_->surface.has_value()) {
        try {
            impl_->surface->abandon();
        } catch (...) {
            std::terminate();
        }
        impl_->surface.reset();
    }
}

} // namespace strata::host
