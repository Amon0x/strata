#include "host/session.hpp"

#include <exception>
#include <mutex>
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

/** A prepared runtime and its ticket share only notification lifetime, never UI services. */
class PreparationWake final {
  public:
    explicit PreparationWake(std::function<void()> callback) : callback_(std::move(callback)) {}

    void notify() {
        const std::scoped_lock lock(mutex_);
        if (callback_)
            callback_();
    }

    void close() noexcept {
        const std::scoped_lock lock(mutex_);
        callback_ = {};
    }

    static strata_status signal(void* const context) noexcept {
        try {
            static_cast<PreparationWake*>(context)->notify();
            return STRATA_STATUS_OK;
        } catch (...) {
            return STRATA_STATUS_INTERNAL_ERROR;
        }
    }

  private:
    std::mutex mutex_;
    std::function<void()> callback_;
};

/** Startup loading owns its reader; the live write callback is installed only after transfer. */
struct PreparedDurability final {
    std::function<std::optional<std::string>()> read;
    std::optional<strata_durable_store_adapter> live;
    std::string bytes;

    static strata_status load(void* const context, strata_string_view,
                               strata_bytes_view* const output) noexcept {
        try {
            auto& self = *static_cast<PreparedDurability*>(context);
            std::optional<std::string> value = self.read();
            if (!value.has_value())
                return STRATA_STATUS_NOT_FOUND;
            self.bytes = std::move(*value);
            *output = {reinterpret_cast<const std::uint8_t*>(self.bytes.data()), self.bytes.size()};
            return STRATA_STATUS_OK;
        } catch (const std::bad_alloc&) {
            return STRATA_STATUS_OUT_OF_MEMORY;
        } catch (...) {
            return STRATA_STATUS_SERVICE_UNAVAILABLE;
        }
    }

    static strata_status write(void* const context, const strata_string_view application,
                                const strata_bytes_view value) noexcept {
        try {
            const auto& self = *static_cast<PreparedDurability*>(context);
            if (!self.live.has_value())
                return STRATA_STATUS_SERVICE_UNAVAILABLE;
            return self.live->write(self.live->user_data, application, value);
        } catch (...) {
            return STRATA_STATUS_INTERNAL_ERROR;
        }
    }
};

/** Preparation mutates this exclusively; after transfer only the Session owner may access it. */
struct PreparedServices final {
    std::shared_ptr<ResourceRoot> resources;
    std::shared_ptr<PreparationWake> wake;
    std::optional<PreparedDurability> durability;
    std::vector<Diagnostic> diagnostics;
    std::vector<SnapshotRevision> published;
};

void deliver_diagnostics(const std::function<void(const strata_diagnostic&)>& observer,
                         std::vector<Diagnostic>& diagnostics) noexcept {
    const std::vector<Diagnostic> batch = std::move(diagnostics);
    if (observer) {
        for (const Diagnostic& diagnostic : batch) {
            strata_diagnostic value{};
            value.struct_size = sizeof(value);
            value.version = STRATA_DIAGNOSTIC_VERSION_CURRENT;
            value.id = diagnostic.id;
            value.severity = static_cast<strata_diagnostic_severity>(diagnostic.severity);
            value.code = strata::view(diagnostic.code);
            value.message = strata::view(diagnostic.message);
            if (diagnostic.source_id.has_value())
                value.source_id = strata::view(*diagnostic.source_id);
            if (diagnostic.range.has_value()) {
                const SourceRange& range = *diagnostic.range;
                value.range = {range.byte_start, range.byte_end, range.line_start, range.column_start,
                               range.line_end, range.column_end};
            }
            value.occurrence_count = diagnostic.occurrence_count;
            value.sequence = diagnostic.sequence;
            value.first_frame_index = diagnostic.first_frame_index;
            value.frame_index = diagnostic.frame_index;
            value.dropped_count = diagnostic.dropped_count;
            if (diagnostic.component_path.has_value())
                value.component_path = strata::view(*diagnostic.component_path);
            if (diagnostic.expected.has_value())
                value.expected = strata::view(*diagnostic.expected);
            try {
                observer(value);
            } catch (...) {
                // Diagnostic observers have the same non-unwinding contract before and after adoption.
            }
        }
    }
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
        config.task_execution = static_cast<strata_task_execution>(options.task_execution);
        if (options.frame_wake) {
            config.frame_wake = strata_frame_wake_adapter{
                sizeof(strata_frame_wake_adapter), this, &Impl::wake,
            };
        }
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
        prepared_actions = {};
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

    static strata_status wake(void* const user_data) noexcept {
        try {
            static_cast<const Impl*>(user_data)->options.frame_wake();
            return STRATA_STATUS_OK;
        } catch (...) {
            return STRATA_STATUS_INTERNAL_ERROR;
        }
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

    void acknowledge_release() {
        if (!surface.has_value())
            return;
        surface->acknowledge_release_packet();
        surface->close();
        surface.reset();
    }

    void close_surface(const std::function<void(const EncodedRenderPacket&)>& consume) {
        if (!surface.has_value())
            return;
        const EncodedRenderPacket terminal = surface->prepare_release_packet();
        if (consume)
            consume(terminal);
        acknowledge_release();
    }

    SessionOptions options;
    std::unique_ptr<strata::Runtime> runtime;
    SelectedExtensions extensions;
    std::unique_ptr<Bindings> bindings;
    std::optional<strata::Surface> surface;
    std::vector<std::string> module_resources;
    std::string module_id_scratch;
    std::string module_text_scratch;
    std::int64_t time = 0;
    std::shared_ptr<PreparedServices> prepared;
    ActionBindings prepared_actions;
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
    impl_->surface->set_frame_slicing(impl_->options.frame_slicing);
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

SurfacePacketCommit Session::frame(const std::int64_t time_nanoseconds) {
    strata::Surface& surface = impl_->live_surface();
    if (impl_->bindings != nullptr)
        impl_->bindings->synchronize();
    impl_->time = time_nanoseconds;
    return frame_surface(surface.native_handle(), time_nanoseconds);
}

EncodedRenderPacket Session::prepare_release() {
    return impl_->surface.has_value() ? impl_->surface->prepare_release_packet()
                                     : EncodedRenderPacket{};
}

void Session::acknowledge_release() {
    impl_->acknowledge_release();
}

void Session::close_surface(const std::function<void(const EncodedRenderPacket&)>& consume) {
    impl_->close_surface(consume);
}

void Session::close(const std::function<void(const EncodedRenderPacket&)>& consume) {
    impl_->close_surface(consume);
    impl_->bindings.reset();
    impl_->prepared_actions = {};
    if (impl_->runtime != nullptr) {
        impl_->runtime->close();
        impl_->runtime.reset();
    }
}

void Session::abandon() noexcept {
    if (impl_->surface.has_value()) {
        try {
            impl_->surface->abandon();
        } catch (...) {
            std::terminate();
        }
        impl_->surface.reset();
    }
}

struct SessionPreparation::Impl final {
    struct Input final {
        SessionPrepareOptions options;
        std::shared_ptr<PreparedServices> services;
        std::string error;
    };

    explicit Impl(SessionPrepareOptions options)
        : services(std::make_shared<PreparedServices>()),
          on_diagnostic(std::move(options.session.on_diagnostic)) {
        if (options.resources == nullptr || options.module_resource.empty())
            throw std::invalid_argument("Session preparation requires owned resources and a module");
        services->resources = options.resources;
        if (options.session.frame_wake) {
            services->wake =
                std::make_shared<PreparationWake>(std::move(options.session.frame_wake));
            options.session.frame_wake = [wake = services->wake] { wake->notify(); };
        }
        if (!options.session.read_text) {
            options.session.read_text = [resources = options.resources](const std::string_view id) {
                return resources->read_text(utf8_path(id));
            };
        }
        options.session.on_diagnostic = [retained = services](const strata_diagnostic& diagnostic) {
            retained->diagnostics.push_back(strata::detail::copy_diagnostic(diagnostic));
        };
        if (options.load_durable)
            services->durability.emplace(PreparedDurability{std::move(options.load_durable), {}, {}});
        auto input = std::make_unique<Input>(Input{std::move(options), services, {}});
        strata_preparation_config config{};
        config.struct_size = sizeof(config);
        config.execution = static_cast<strata_task_execution>(input->options.session.task_execution);
        if (services->wake != nullptr) {
            config.frame_wake = {sizeof(strata_frame_wake_adapter), services->wake.get(),
                                 &PreparationWake::signal};
        }
        config.context = input.get();
        config.work = &Impl::run;
        config.release_context = &Impl::release_input;
        config.release_result = &Impl::release_result;
        const strata_result started = strata_preparation_start(&config, &handle);
        if (started.status == STRATA_STATUS_OK)
            static_cast<void>(input.release());
        strata::require_ok(started, "Session preparation submission");
    }

    ~Impl() {
        if (handle != nullptr && strata_preparation_release(handle).status != STRATA_STATUS_OK)
            std::terminate();
        if (!taken && services->wake != nullptr)
            services->wake->close();
    }

    static void release_input(void* const input) noexcept {
        delete static_cast<Input*>(input);
    }

    static void release_result(void* const value) noexcept {
        delete static_cast<PreparedSession*>(value);
    }

    static strata_status fail(Input& input, const strata_status status, const char* const message,
                               strata_string_view& error) noexcept {
        try {
            input.error = message;
            error = strata::view(input.error);
            return status;
        } catch (...) {
            error = strata::view("Session preparation exhausted memory while retaining its error.");
            return STRATA_STATUS_OUT_OF_MEMORY;
        }
    }

    static strata_status run(void* const context, const strata_cancellation* const cancellation,
                              void** const output, strata_string_view* const error) noexcept {
        auto& input = *static_cast<Input*>(context);
        std::vector<CapturedSnapshot> snapshots = std::move(input.options.snapshots);
        try {
            SessionPrepareOptions& options = input.options;
            if (!options.schemas_resource.empty()) {
                options.session.schemas =
                    options.resources->read_text(utf8_path(options.schemas_resource));
            }
            if (strata_cancellation_requested(cancellation) != 0U)
                return STRATA_STATUS_SERVICE_UNAVAILABLE;
            SessionAdapters adapters;
            adapters.resources = options.resources->adapter();
            if (input.services->durability.has_value()) {
                adapters.durable_store = strata_durable_store_adapter{
                    sizeof(strata_durable_store_adapter), &*input.services->durability,
                    &PreparedDurability::load, &PreparedDurability::write,
                };
            }
            auto prepared = std::make_unique<PreparedSession>();
            prepared->session = std::make_unique<Session>(std::move(options.session), adapters);
            Session& session = *prepared->session;
            session.impl_->prepared = input.services;
            options.actions.install(session.runtime());
            session.impl_->prepared_actions = std::move(options.actions);
            session.set_time(options.time_nanoseconds);
            input.services->published.reserve(snapshots.size());
            for (CapturedSnapshot& snapshot : snapshots) {
                if (strata_cancellation_requested(cancellation) != 0U)
                    return STRATA_STATUS_SERVICE_UNAVAILABLE;
                static_cast<void>(
                    session.runtime().publish_host_snapshot(
                        snapshot.source.id, snapshot.value.json(), HostSnapshotExecution::inline_execution));
                if (snapshot.source.revision != 0U)
                    input.services->published.push_back(std::move(snapshot.source));
            }
            snapshots.clear();
            if (strata_cancellation_requested(cancellation) != 0U)
                return STRATA_STATUS_SERVICE_UNAVAILABLE;
            strata_activation_info activated{};
            if (options.module_format == SessionModuleFormat::compiled) {
                const std::vector<std::uint8_t> artifact =
                    options.resources->read(utf8_path(options.module_resource));
                if (strata_cancellation_requested(cancellation) != 0U)
                    return STRATA_STATUS_SERVICE_UNAVAILABLE;
                activated = session.activate_compiled(artifact, options.generation);
            } else if (options.module_format == SessionModuleFormat::source) {
                prepared->source = session.impl_->read_text(options.module_resource);
                if (strata_cancellation_requested(cancellation) != 0U)
                    return STRATA_STATUS_SERVICE_UNAVAILABLE;
                activated = session.activate_source(options.module_resource, prepared->source,
                                                    options.generation);
            } else {
                throw std::invalid_argument("Session preparation module format is invalid");
            }
            if (activated.status != STRATA_ACTIVATION_ACTIVATED) {
                throw std::runtime_error(
                    input.services->diagnostics.empty()
                        ? "Session module activation was rejected."
                        : input.services->diagnostics.back().code + ": " +
                              input.services->diagnostics.back().message);
            }
            if (strata_cancellation_requested(cancellation) != 0U)
                return STRATA_STATUS_SERVICE_UNAVAILABLE;
            if (!options.program_language.empty()) {
                for (MaterialDeclaration& declaration :
                     session.runtime().material_declarations(options.program_language)) {
                    if (declaration.source.empty())
                        continue;
                    std::string source = session.impl_->read_text(declaration.source);
                    prepared->materials.push_back({std::move(declaration), std::move(source)});
                }
                for (EffectPassDeclaration& declaration :
                     session.runtime().effect_pass_declarations(options.program_language)) {
                    std::string source = declaration.source.empty()
                                             ? std::string{} : session.impl_->read_text(declaration.source);
                    prepared->effects.push_back({std::move(declaration), std::move(source)});
                }
            }
            if (strata_cancellation_requested(cancellation) != 0U)
                return STRATA_STATUS_SERVICE_UNAVAILABLE;
            session.create_surface(options.surface);
            session.impl_->prepared_actions.rethrow_failure();
            *output = prepared.release();
            return STRATA_STATUS_OK;
        } catch (const strata::AbiError& exception) {
            return fail(input, exception.status(), exception.what(), *error);
        } catch (const std::bad_alloc&) {
            *error = strata::view("Session preparation exhausted memory.");
            return STRATA_STATUS_OUT_OF_MEMORY;
        } catch (const std::exception& exception) {
            return fail(input, STRATA_STATUS_INTERNAL_ERROR, exception.what(), *error);
        } catch (...) {
            *error = strata::view("Session preparation failed with a non-standard exception.");
            return STRATA_STATUS_INTERNAL_ERROR;
        }
    }

    std::shared_ptr<PreparedServices> services;
    std::function<void(const strata_diagnostic&)> on_diagnostic;
    strata_preparation* handle = nullptr;
    bool taken = false;
};

SessionPreparation Session::prepare(SessionPrepareOptions options) {
    return SessionPreparation(std::make_unique<SessionPreparation::Impl>(std::move(options)));
}

SessionPreparation::SessionPreparation(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
SessionPreparation::~SessionPreparation() = default;
SessionPreparation::SessionPreparation(SessionPreparation&&) noexcept = default;
SessionPreparation& SessionPreparation::operator=(SessionPreparation&&) noexcept = default;

bool SessionPreparation::ready() {
    if (impl_ == nullptr || impl_->taken)
        throw std::logic_error("Session preparation has already transferred or moved");
    strata_preparation_info info{};
    info.struct_size = sizeof(info);
    strata::require_ok(strata_preparation_get_info(impl_->handle, &info), "Session preparation poll");
    if (info.status == STRATA_PREPARATION_FAILED) {
        deliver_diagnostics(impl_->on_diagnostic, impl_->services->diagnostics);
        throw std::runtime_error("Session preparation failed: " + copy(info.failure_message));
    }
    return info.status == STRATA_PREPARATION_READY;
}

PreparedSession SessionPreparation::take(const SessionAdapters& adapters,
                                          std::unique_ptr<Bindings> bindings) {
    if (!ready())
        throw std::logic_error("Session preparation is not ready");
    if (adapters.resources.has_value())
        throw std::invalid_argument("prepared Session resources cannot be replaced during adoption");
    if (adapters.durable_store.has_value() != impl_->services->durability.has_value())
        throw std::invalid_argument("prepared durable state requires its matching live adapter");
    if (adapters.durable_store.has_value() &&
        (adapters.durable_store->struct_size < sizeof(strata_durable_store_adapter) ||
         adapters.durable_store->load == nullptr || adapters.durable_store->write == nullptr)) {
        throw std::invalid_argument("Session adoption requires a complete durable adapter");
    }
    void* result = nullptr;
    strata::require_ok(strata_preparation_take_result(impl_->handle, &result),
                       "Session preparation transfer");
    std::unique_ptr<PreparedSession> prepared(static_cast<PreparedSession*>(result));
    Session& session = *prepared->session;
    if (bindings == nullptr && !session.impl_->prepared_actions.empty())
        throw std::invalid_argument("prepared actions require their original owner bindings");
    strata_runtime* const native = session.runtime().native_handle();
    if (adapters.clipboard.has_value()) {
        strata::require_ok(strata_runtime_set_clipboard_adapter(native, &*adapters.clipboard),
                           "Session clipboard adapter adoption");
    }
    if (adapters.ime.has_value()) {
        strata::require_ok(strata_runtime_set_ime_adapter(native, &*adapters.ime),
                           "Session IME adapter adoption");
    }
    if (adapters.effects.has_value()) {
        strata::require_ok(strata_runtime_set_effect_adapter(native, &*adapters.effects),
                           "Session effect adapter adoption");
    }
    if (adapters.async_host.has_value())
        session.runtime().set_async_host(*adapters.async_host);
    if (adapters.durable_store.has_value())
        impl_->services->durability->live = *adapters.durable_store;
    if (bindings != nullptr) {
        bindings->attach(session.runtime(), impl_->services->published,
                         std::move(session.impl_->prepared_actions));
        session.impl_->bindings = std::move(bindings);
    }
    session.impl_->options.on_diagnostic = std::move(impl_->on_diagnostic);
    deliver_diagnostics(session.impl_->options.on_diagnostic, impl_->services->diagnostics);
    impl_->services->published.clear();
    impl_->taken = true;
    return std::move(*prepared);
}

} // namespace strata::host
