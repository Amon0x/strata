#include "application_host.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <strata/strata.hpp>

#include "capture_renderer.hpp"
#include "host/extensions.hpp"
#include "host/module_path.hpp"
#include <strata/render_packet.hpp>

namespace strata::headless {
namespace {

[[nodiscard]] std::string copy(const strata_string_view value) {
    return value.size == 0U ? std::string{} : std::string(value.data, value.size);
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("could not open headless resource: " + path.string());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("could not open headless resource: " + path.string());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>());
}

[[nodiscard]] std::string initialization_failure(
    const std::string_view reason,
    const std::vector<CapturedDiagnostic>& diagnostics
) {
    std::string result(reason);
    if (diagnostics.empty()) return result;
    result += "\nCaptured diagnostics:";
    for (const CapturedDiagnostic& diagnostic : diagnostics) {
        result += "\n  ";
        result += diagnostic.code.empty()
            ? "diagnostic " + std::to_string(diagnostic.id)
            : diagnostic.code;
        if (!diagnostic.source.empty()) {
            result += " [";
            result += diagnostic.source;
            result += ']';
        }
        if (!diagnostic.component_path.empty()) {
            result += " at ";
            result += diagnostic.component_path;
        }
        if (!diagnostic.message.empty()) {
            result += ": ";
            result += diagnostic.message;
        }
        if (!diagnostic.expected.empty()) {
            result += " Expected: ";
            result += diagnostic.expected;
        }
    }
    return result;
}

} // namespace

struct ApplicationHost::Impl final {
    using Registration =
        std::unique_ptr<strata_action_registration, decltype(&strata_action_registration_release)>;

    Impl(const Scenario& scenario, std::filesystem::path resource_root)
        : scenario(scenario), resource_root(std::move(resource_root)),
          renderer(create_capture_renderer(scenario.render_backend)) {
        try {
            initialize();
        } catch (const std::exception& error) {
            if (surface.has_value()) surface->abandon();
            throw std::runtime_error(initialization_failure(error.what(), diagnostics));
        } catch (...) {
            if (surface.has_value()) surface->abandon();
            throw;
        }
    }

    ~Impl() noexcept {
        try {
            close();
        } catch (...) {
            if (surface.has_value())
                surface->abandon();
        }
    }

    static std::int64_t clock_now(void* const user_data) noexcept {
        return static_cast<Impl*>(user_data)->clock_time;
    }

    static void diagnostic(void* const user_data, const strata_diagnostic* const value) noexcept {
        if (user_data == nullptr || value == nullptr)
            return;
        try {
            static_cast<Impl*>(user_data)->diagnostics.push_back(CapturedDiagnostic{
                value->id,
                value->severity,
                copy(value->code),
                copy(value->message),
                copy(value->source_id),
                copy(value->component_path),
                copy(value->expected),
            });
        } catch (...) {
        }
    }

    [[nodiscard]] std::filesystem::path resource_path(const std::string_view id) const {
        const std::filesystem::path relative{std::string(id)};
        if (id.empty() || relative.is_absolute()) {
            throw std::invalid_argument("headless resource id must be relative");
        }
        for (const std::filesystem::path& part : relative) {
            if (part == "..")
                throw std::invalid_argument("headless resource id escapes its root");
        }
        return resource_root / relative;
    }

    static strata_status load_resource(void* const user_data, const strata_string_view id,
                                       strata_bytes_view* const output) noexcept {
        if (user_data == nullptr || output == nullptr)
            return STRATA_STATUS_INVALID_ARGUMENT;
        try {
            auto& self = *static_cast<Impl*>(user_data);
            const std::string resource_id = copy(id);
            auto [found, inserted] = self.resource_cache.try_emplace(resource_id);
            if (inserted)
                found->second = read_bytes(self.resource_path(resource_id));
            output->data = found->second.data();
            output->size = found->second.size();
            return output->size == 0U ? STRATA_STATUS_NOT_FOUND : STRATA_STATUS_OK;
        } catch (const std::bad_alloc&) {
            return STRATA_STATUS_OUT_OF_MEMORY;
        } catch (...) {
            return STRATA_STATUS_NOT_FOUND;
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
            self.module_text_scratch = read_text(self.resource_path(self.module_id_scratch));
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

    static strata_action_handler_result action(void* const user_data,
                                               const strata_action_call* const call) noexcept {
        if (user_data == nullptr || call == nullptr)
            return STRATA_ACTION_HANDLER_IGNORED;
        try {
            static_cast<Impl*>(user_data)->actions.push_back(CapturedAction{
                copy(call->action_id),
                copy(call->payload_json),
                copy(call->event_kind),
                copy(call->source_key),
                copy(call->event_value_json),
            });
            return STRATA_ACTION_HANDLER_HANDLED;
        } catch (...) {
            return STRATA_ACTION_HANDLER_IGNORED;
        }
    }

    static strata_status clipboard_read(void* const user_data,
                                        strata_string_view* const output) noexcept {
        if (user_data == nullptr || output == nullptr)
            return STRATA_STATUS_INVALID_ARGUMENT;
        auto& self = *static_cast<Impl*>(user_data);
        if (self.clipboard.empty())
            return STRATA_STATUS_NOT_FOUND;
        *output = strata::view(self.clipboard);
        return STRATA_STATUS_OK;
    }

    static strata_status clipboard_write(void* const user_data,
                                         const strata_string_view value) noexcept {
        if (user_data == nullptr)
            return STRATA_STATUS_INVALID_ARGUMENT;
        try {
            static_cast<Impl*>(user_data)->clipboard = copy(value);
            return STRATA_STATUS_OK;
        } catch (...) {
            return STRATA_STATUS_OUT_OF_MEMORY;
        }
    }

    static strata_status ime_active(void*, std::uint32_t) noexcept {
        return STRATA_STATUS_OK;
    }
    static strata_status ime_rect(void*, strata_rect) noexcept {
        return STRATA_STATUS_OK;
    }

    static strata_status effect(void* const user_data, const strata_string_view id,
                                const strata_string_view payload) noexcept {
        if (user_data == nullptr)
            return STRATA_STATUS_INVALID_ARGUMENT;
        try {
            static_cast<Impl*>(user_data)->effects.push_back(
                CapturedEffect{copy(id), copy(payload)});
            return STRATA_STATUS_OK;
        } catch (...) {
            return STRATA_STATUS_OUT_OF_MEMORY;
        }
    }

    static strata_status durable_load(void* const user_data, strata_string_view,
                                      strata_bytes_view* const output) noexcept {
        if (user_data == nullptr || output == nullptr)
            return STRATA_STATUS_INVALID_ARGUMENT;
        auto& self = *static_cast<Impl*>(user_data);
        if (self.durable.empty())
            return STRATA_STATUS_NOT_FOUND;
        output->data = reinterpret_cast<const std::uint8_t*>(self.durable.data());
        output->size = self.durable.size();
        return STRATA_STATUS_OK;
    }

    static strata_status durable_write(void* const user_data, strata_string_view,
                                       const strata_bytes_view bytes) noexcept {
        if (user_data == nullptr)
            return STRATA_STATUS_INVALID_ARGUMENT;
        try {
            auto& value = static_cast<Impl*>(user_data)->durable;
            if (bytes.size == 0U)
                value.clear();
            else
                value.assign(reinterpret_cast<const char*>(bytes.data),
                             reinterpret_cast<const char*>(bytes.data + bytes.size));
            return STRATA_STATUS_OK;
        } catch (...) {
            return STRATA_STATUS_OUT_OF_MEMORY;
        }
    }

    static strata_status async_begin(void* const user_data, const std::uint64_t request_id,
                                     const strata_string_view binding,
                                     const strata_string_view owner,
                                     const strata_string_view payload) noexcept {
        if (user_data == nullptr)
            return STRATA_STATUS_INVALID_ARGUMENT;
        try {
            static_cast<Impl*>(user_data)->async_requests.push_back(CapturedAsyncRequest{
                request_id,
                copy(binding),
                copy(owner),
                copy(payload),
                false,
            });
            return STRATA_STATUS_OK;
        } catch (...) {
            return STRATA_STATUS_OUT_OF_MEMORY;
        }
    }

    static void async_cancel(void* const user_data, const std::uint64_t request_id) noexcept {
        if (user_data == nullptr)
            return;
        auto& requests = static_cast<Impl*>(user_data)->async_requests;
        const auto found = std::ranges::find(requests, request_id, &CapturedAsyncRequest::id);
        if (found != requests.end())
            found->cancelled = true;
    }

    [[nodiscard]] strata_surface_environment environment() const {
        strata_surface_environment result{};
        result.struct_size = sizeof(result);
        result.generation = environment_generation;
        result.framebuffer_width =
            static_cast<std::int64_t>(std::llround(logical_width * display_scale));
        result.framebuffer_height =
            static_cast<std::int64_t>(std::llround(logical_height * display_scale));
        result.logical_width = logical_width;
        result.logical_height = logical_height;
        result.scale = display_scale;
        result.point_snapping = STRATA_POINT_SNAP_NEAREST;
        result.rectangle_snapping = STRATA_RECTANGLE_SNAP_OUTWARD;
        result.density = STRATA_SURFACE_DENSITY_COMFORTABLE;
        result.pointer_precision = STRATA_POINTER_PRECISION_FINE;
        result.input_capabilities = STRATA_SURFACE_INPUT_POINTER | STRATA_SURFACE_INPUT_KEYBOARD |
                                    STRATA_SURFACE_INPUT_TOUCH | STRATA_SURFACE_INPUT_IME |
                                    STRATA_SURFACE_INPUT_CLIPBOARD |
                                    STRATA_SURFACE_INPUT_CONTROLLER;
        result.reduced_motion = scenario.reduced_motion ? 1U : 0U;
        return result;
    }

    void initialize() {
        if (!std::filesystem::is_directory(resource_root)) {
            throw std::invalid_argument("headless resource root is not a directory");
        }
        logical_width = scenario.width;
        logical_height = scenario.height;
        display_scale = scenario.scale;

        strata_runtime_config runtime_config{};
        runtime_config.struct_size = sizeof(runtime_config);
        runtime_config.abi_version = STRATA_ABI_VERSION_CURRENT;
        runtime_config.required_capabilities =
            STRATA_CAPABILITY_CORE_LIFECYCLE | STRATA_CAPABILITY_CALLER_CLOCK |
            STRATA_CAPABILITY_HOST_SNAPSHOTS | STRATA_CAPABILITY_APPLICATION_LIFECYCLE |
            STRATA_CAPABILITY_COMPILER_ACTIVATION | STRATA_CAPABILITY_ACTION_DISPATCH |
            STRATA_CAPABILITY_RESOURCE_ADAPTER | STRATA_CAPABILITY_CLIPBOARD_IME_ADAPTER |
            STRATA_CAPABILITY_DURABLE_STATE | STRATA_CAPABILITY_ASYNC_HOST_DATA |
            STRATA_CAPABILITY_SURFACE_RUNTIME | STRATA_CAPABILITY_SURFACE_RENDER_PACKET;
        runtime_config.stable_identity_seed = 0x484541444C455353ULL;
        runtime_config.clock = strata_clock{sizeof(strata_clock), this, &Impl::clock_now};
        runtime_config.diagnostics = strata_diagnostic_sink{
            sizeof(strata_diagnostic_sink),
            this,
            &Impl::diagnostic,
        };
        runtime = std::make_unique<strata::Runtime>(runtime_config);

        const strata_resource_adapter resources{
            sizeof(strata_resource_adapter),
            this,
            1U,
            &Impl::load_resource,
        };
        strata::require_ok(
            strata_runtime_set_resource_adapter(runtime->native_handle(), &resources),
            "headless resource adapter installation");
        const strata_clipboard_adapter clipboard_adapter{
            sizeof(strata_clipboard_adapter),
            this,
            &Impl::clipboard_read,
            &Impl::clipboard_write,
        };
        strata::require_ok(
            strata_runtime_set_clipboard_adapter(runtime->native_handle(), &clipboard_adapter),
            "headless clipboard adapter installation");
        const strata_ime_adapter ime{
            sizeof(strata_ime_adapter),
            this,
            &Impl::ime_active,
            &Impl::ime_rect,
        };
        strata::require_ok(strata_runtime_set_ime_adapter(runtime->native_handle(), &ime),
                           "headless IME adapter installation");
        const strata_effect_adapter effect_adapter{
            sizeof(strata_effect_adapter),
            this,
            &Impl::effect,
        };
        strata::require_ok(
            strata_runtime_set_effect_adapter(runtime->native_handle(), &effect_adapter),
            "headless effect adapter installation");

        const std::string schemas =
            scenario.schemas.empty() ? std::string{}
                                     : read_text(resource_path(scenario.schemas.generic_string()));
        std::vector<std::string> packages = host::declared_extension_packages(schemas);
        if (packages.empty()) {
            packages = scenario.packages;
        } else if (!scenario.packages.empty() && scenario.packages != packages) {
            throw std::invalid_argument(
                "headless extension packages disagree with the application schema declaration"
            );
        }
        extensions = host::select_extensions(packages, scenario.extension_search_paths);
        std::vector<std::string> extension_schemas = extensions.schemas();
        std::vector<strata_string_view> extension_schema_views;
        extension_schema_views.reserve(extension_schemas.size());
        for (const std::string& schema : extension_schemas) {
            extension_schema_views.push_back(strata::view(schema));
        }
        const strata_application_config application{
            sizeof(strata_application_config),
            strata::view(scenario.application_id),
            strata::view(schemas),
            extension_schema_views.empty() ? nullptr : extension_schema_views.data(),
            extension_schema_views.size(),
        };
        runtime->configure_application(application);
        for (const strata::MaterialDeclaration& material :
             runtime->material_declarations("hlsl")) {
            if (material.source.empty()) continue;
            renderer->declare_material(
                material.id,
                read_text(resource_path(material.source))
            );
        }
        const std::string_view effect_backend =
            renderer->backend() == "d3d11" ? "hlsl" : "reference";
        for (const strata::EffectPassDeclaration& pass :
             runtime->effect_pass_declarations(effect_backend)) {
            renderer->declare_effect_pass(
                pass.effect_id,
                pass.index,
                static_cast<std::uint32_t>(pass.kind),
                pass.radius,
                pass.downsample,
                pass.radius_parameter.value_or(STRATA_EFFECT_PARAMETER_NONE),
                pass.downsample_parameter.value_or(STRATA_EFFECT_PARAMETER_NONE),
                pass.source.empty()
                    ? std::string{}
                    : read_text(resource_path(pass.source))
            );
        }

        const strata_durable_store_adapter durable_adapter{
            sizeof(strata_durable_store_adapter),
            this,
            &Impl::durable_load,
            &Impl::durable_write,
        };
        runtime->set_durable_store(durable_adapter);
        const strata_async_host_adapter async_adapter{
            sizeof(strata_async_host_adapter),
            this,
            &Impl::async_begin,
            &Impl::async_cancel,
        };
        runtime->set_async_host(async_adapter);

        registrations.reserve(scenario.actions.size());
        for (const std::string& id : scenario.actions) {
            strata_action_registration* registration = nullptr;
            const strata_action_handler_config handler{
                sizeof(strata_action_handler_config),
                strata::view(id),
                strata::view("strata.headless"),
                this,
                &Impl::action,
            };
            strata::require_ok(strata_runtime_register_action_handler(runtime->native_handle(),
                                                                      &handler, &registration),
                               "headless action registration");
            registrations.emplace_back(registration, &strata_action_registration_release);
        }
        for (const SnapshotConfig& snapshot : scenario.snapshots)
            publish(snapshot);

        const std::string source_id = scenario.module.generic_string();
        const std::string source = read_text(resource_path(source_id));
        const strata_activation_config activation{
            sizeof(strata_activation_config),
            1U,
            strata::view(source_id),
            strata::view(source),
            this,
            &Impl::load_module,
        };
        const strata_activation_info activated = runtime->activate(activation);
        if (activated.status != STRATA_ACTIVATION_ACTIVATED) {
            throw std::runtime_error("headless scenario module did not activate");
        }

        std::vector<strata_surface_font_resource> fonts;
        fonts.reserve(scenario.fonts.size());
        for (const FontConfig& font : scenario.fonts) {
            fonts.push_back(strata_surface_font_resource{
                strata::view(font.id),
                strata::view(font.resource),
            });
        }
        std::vector<strata_surface_image_resource> images;
        images.reserve(scenario.images.size());
        for (const ImageConfig& image : scenario.images) {
            images.push_back(strata_surface_image_resource{
                strata::view(image.id),
                strata::view(image.resource),
                image.sampling,
                0U,
            });
        }
        const strata_surface_config surface_config{
            sizeof(strata_surface_config),
            strata::view(scenario.surface_id),
            scenario.root_role == "screen" ? STRATA_SURFACE_ROOT_SCREEN
                                           : STRATA_SURFACE_ROOT_OVERLAY,
            0U,
            strata::view(scenario.root),
            environment(),
            fonts.empty() ? nullptr : fonts.data(),
            fonts.size(),
            extensions.pointer(),
            images.empty() ? nullptr : images.data(),
            images.size(),
        };
        surface.emplace(runtime->create_surface(surface_config));
        resize_renderer();
    }

    void resize_renderer() {
        renderer->resize(static_cast<std::uint32_t>(std::llround(logical_width * display_scale)),
                         static_cast<std::uint32_t>(std::llround(logical_height * display_scale)),
                         logical_width, logical_height);
        renderer->set_clear_color(scenario.clear_color);
    }

    void publish(const SnapshotConfig& snapshot) {
        const std::string values = data::encode_canonical_json(snapshot.values);
        const strata_host_snapshot_config config{
            sizeof(strata_host_snapshot_config),
            strata::view(snapshot.id),
            next_snapshot_generation++,
            strata::view(values),
        };
        strata::require_ok(strata_runtime_publish_host_snapshot(runtime->native_handle(), &config),
                           "headless host snapshot publication");
    }

    void frame(const std::int64_t time_nanoseconds) {
        clock_time = time_nanoseconds;
        const strata_surface_frame_info info = surface->frame(time_nanoseconds);
        const std::vector<std::uint8_t> encoded = surface->render_packet();
        const host::RenderPacket& packet = decoder.decode(encoded);
        renderer->render(packet, time_nanoseconds);
        last_frame_json = surface->frame_json();
        last_frame_document = data::parse_json(last_frame_json);
        frame_available = true;
        frames.push_back(CapturedFrame{
            info.frame_index,
            info.frame_time_nanoseconds,
            info.processed_input_event_count,
            info.emitted_event_count,
            info.action_outcome_count,
            info.render_command_count,
            info.render_packet_size,
        });
    }

    void enqueue(const std::span<const strata_input_event> events) {
        strata_surface_input_batch_info info{};
        info.struct_size = sizeof(info);
        strata::require_ok(strata_surface_enqueue_input(surface->native_handle(), events.data(),
                                                        events.size(), &info),
                           "headless input enqueue");
        if (info.accepted_event_count != events.size()) {
            throw std::runtime_error("headless input queue accepted only part of a batch");
        }
    }

    void resize(const double width, const double height, const double scale,
                const std::int64_t time_nanoseconds) {
        logical_width = width;
        logical_height = height;
        display_scale = scale;
        ++environment_generation;
        const strata_surface_environment next = environment();
        std::uint32_t adopted = 0U;
        strata::require_ok(
            strata_surface_adopt_environment(surface->native_handle(), &next, &adopted),
            "headless environment adoption");
        if (adopted == 0U)
            throw std::runtime_error("headless resize was not adopted");
        resize_renderer();
        frame(time_nanoseconds);
    }

    void close() {
        if (!surface.has_value())
            return;
        const std::vector<std::uint8_t> encoded = surface->prepare_release_packet();
        const host::RenderPacket& packet = decoder.decode(encoded);
        renderer->consume_resources(packet);
        surface->acknowledge_release_packet();
        surface->close();
        surface.reset();
        registrations.clear();
        runtime->close();
        runtime.reset();
    }

    const Scenario& scenario;
    std::filesystem::path resource_root;
    std::unique_ptr<strata::Runtime> runtime;
    std::vector<Registration> registrations;
    host::SelectedExtensions extensions;
    std::optional<strata::Surface> surface;
    host::RenderPacketDecoder decoder;
    std::unique_ptr<CaptureRenderer> renderer;
    std::map<std::string, std::vector<std::uint8_t>, std::less<>> resource_cache;
    std::vector<CapturedDiagnostic> diagnostics;
    std::vector<CapturedAction> actions;
    std::vector<CapturedEffect> effects;
    std::vector<CapturedAsyncRequest> async_requests;
    std::vector<CapturedFrame> frames;
    std::string module_id_scratch;
    std::string module_text_scratch;
    std::string clipboard;
    std::string durable;
    std::string last_frame_json;
    data::JsonValue last_frame_document;
    std::int64_t clock_time = 0;
    std::uint64_t next_snapshot_generation = 1U;
    std::uint64_t environment_generation = 1U;
    double logical_width = 0.0;
    double logical_height = 0.0;
    double display_scale = 1.0;
    bool frame_available = false;
};

ApplicationHost::ApplicationHost(const Scenario& scenario, std::filesystem::path resource_root)
    : impl_(std::make_unique<Impl>(scenario, std::move(resource_root))) {}

ApplicationHost::~ApplicationHost() = default;

void ApplicationHost::frame(const std::int64_t time_nanoseconds) {
    impl_->frame(time_nanoseconds);
}

void ApplicationHost::enqueue(const std::span<const strata_input_event> events) {
    impl_->enqueue(events);
}

void ApplicationHost::publish(const SnapshotConfig& snapshot) {
    impl_->publish(snapshot);
}

void ApplicationHost::resize(const double width, const double height, const double scale,
                             const std::int64_t time_nanoseconds) {
    impl_->resize(width, height, scale, time_nanoseconds);
}

void ApplicationHost::close() {
    impl_->close();
}

bool ApplicationHost::has_frame() const noexcept {
    return impl_->frame_available;
}
std::string_view ApplicationHost::render_backend() const noexcept {
    return impl_->renderer->backend();
}
const std::string& ApplicationHost::frame_json() const noexcept {
    return impl_->last_frame_json;
}
const data::JsonValue& ApplicationHost::frame_document() const noexcept {
    return impl_->last_frame_document;
}
std::uint32_t ApplicationHost::framebuffer_width() const noexcept {
    return impl_->renderer->width();
}
std::uint32_t ApplicationHost::framebuffer_height() const noexcept {
    return impl_->renderer->height();
}
std::span<const std::uint8_t> ApplicationHost::pixels() const noexcept {
    return impl_->renderer->pixels();
}
const std::vector<CapturedDiagnostic>& ApplicationHost::diagnostics() const noexcept {
    return impl_->diagnostics;
}
const std::vector<CapturedAction>& ApplicationHost::actions() const noexcept {
    return impl_->actions;
}
const std::vector<CapturedEffect>& ApplicationHost::effects() const noexcept {
    return impl_->effects;
}
const std::vector<CapturedAsyncRequest>& ApplicationHost::async_requests() const noexcept {
    return impl_->async_requests;
}
const std::vector<CapturedFrame>& ApplicationHost::frames() const noexcept {
    return impl_->frames;
}
const std::vector<std::string>& ApplicationHost::material_fallbacks() const noexcept {
    return impl_->renderer->material_fallbacks();
}

} // namespace strata::headless
