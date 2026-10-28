#include "application_host.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
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
#include "host/resource_root.hpp"
#include "host/session.hpp"
#include <strata/render_packet.hpp>

namespace strata::headless {
namespace {

/** The largest side a headless framebuffer may have: D3D11's texture limit, for every backend. */
constexpr double maximum_framebuffer_side = 16384.0;

/**
 * Rejects a surface whose framebuffer would be empty or larger than any backend can render, before
 * anything allocates it. Both the scenario's surface and a later resize step come through here.
 */
void require_renderable_surface(const double width, const double height, const double scale) {
    const double pixel_width = std::round(width * scale);
    const double pixel_height = std::round(height * scale);
    if (!(pixel_width >= 1.0 && pixel_height >= 1.0 && pixel_width <= maximum_framebuffer_side &&
          pixel_height <= maximum_framebuffer_side)) {
        throw std::invalid_argument(std::format(
            "headless surface must be 1 to 16384 device pixels on each side, not {:g} x {:g}",
            pixel_width, pixel_height));
    }
}

[[nodiscard]] std::string copy(const strata_string_view value) {
    return value.size == 0U ? std::string{} : std::string(value.data, value.size);
}

[[nodiscard]] std::string
initialization_failure(const std::string_view reason,
                       const std::vector<CapturedDiagnostic>& diagnostics) {
    std::string result(reason);
    if (diagnostics.empty())
        return result;
    result += "\nCaptured diagnostics:";
    for (const CapturedDiagnostic& diagnostic : diagnostics) {
        result += "\n  ";
        result += diagnostic.code.empty() ? "diagnostic " + std::to_string(diagnostic.id)
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

    Impl(const Scenario& scenario, std::filesystem::path resource_root,
         ApplicationHostOptions options)
        : scenario(scenario), resource_files(std::move(resource_root)), options(std::move(options)),
          renderer(create_capture_renderer(scenario.render_backend)) {
        try {
            initialize();
        } catch (const std::exception& error) {
            abandon();
            throw std::runtime_error(initialization_failure(error.what(), diagnostics));
        } catch (...) {
            abandon();
            throw;
        }
    }

    ~Impl() noexcept {
        try {
            close();
        } catch (...) {
            abandon();
        }
    }

    void abandon() noexcept {
        if (session.has_value())
            session->abandon();
    }

    [[nodiscard]] std::string resource_text(const std::filesystem::path& relative) const {
        return resource_files.read_text(relative);
    }

    [[nodiscard]] std::string resource_text(const std::string& id) const {
        return resource_files.read_text(host::utf8_path(id));
    }

    static strata_status load_resource(void* const user_data, const strata_string_view id,
                                       strata_bytes_view* const output) noexcept {
        if (user_data == nullptr || output == nullptr)
            return STRATA_STATUS_INVALID_ARGUMENT;
        return static_cast<Impl*>(user_data)->resource_files.load(
            std::string_view(id.data != nullptr ? id.data : "", id.size), *output);
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
        if (!std::filesystem::is_directory(resource_files.root())) {
            throw std::invalid_argument("headless resource root is not a directory");
        }
        require_renderable_surface(scenario.width, scenario.height, scenario.scale);
        logical_width = scenario.width;
        logical_height = scenario.height;
        display_scale = scenario.scale;

        host::SessionOptions session_options;
        session_options.application_id = scenario.application_id;
        session_options.stable_identity_seed = 0x484541444C455353ULL;
        session_options.schemas =
            scenario.schemas.empty() ? std::string{} : resource_text(scenario.schemas);
        session_options.extension_packages = scenario.packages;
        session_options.extension_search_paths = scenario.extension_search_paths;
        session_options.read_text = [this](const std::string_view id) {
            return resource_text(std::string(id));
        };
        session_options.on_diagnostic = [this](const strata_diagnostic& value) {
            diagnostics.push_back(CapturedDiagnostic{
                value.id,
                value.severity,
                copy(value.code),
                copy(value.message),
                copy(value.source_id),
                copy(value.component_path),
                copy(value.expected),
            });
        };
        host::SessionAdapters adapters;
        adapters.resources = strata_resource_adapter{
            sizeof(strata_resource_adapter),
            this,
            &Impl::load_resource,
        };
        adapters.clipboard = options.clipboard.value_or(strata_clipboard_adapter{
            sizeof(strata_clipboard_adapter),
            this,
            &Impl::clipboard_read,
            &Impl::clipboard_write,
        });
        adapters.ime = options.ime.value_or(strata_ime_adapter{
            sizeof(strata_ime_adapter),
            this,
            &Impl::ime_active,
            &Impl::ime_rect,
        });
        adapters.effects = strata_effect_adapter{
            sizeof(strata_effect_adapter),
            this,
            &Impl::effect,
        };
        adapters.durable_store = strata_durable_store_adapter{
            sizeof(strata_durable_store_adapter),
            this,
            &Impl::durable_load,
            &Impl::durable_write,
        };
        adapters.async_host = strata_async_host_adapter{
            sizeof(strata_async_host_adapter),
            this,
            &Impl::async_begin,
            &Impl::async_cancel,
        };
        session.emplace(std::move(session_options), adapters);
        strata::Runtime& runtime = session->runtime();
        for (const strata::MaterialDeclaration& material : runtime.material_declarations("hlsl")) {
            if (material.source.empty())
                continue;
            renderer->declare_material(material.id, resource_text(material.source));
        }
        const std::string_view effect_backend =
            renderer->backend() == "reference" ? "reference" : "hlsl";
        for (const strata::EffectPassDeclaration& pass :
             runtime.effect_pass_declarations(effect_backend)) {
            renderer->declare_effect_pass(
                pass.effect_id, pass.index, static_cast<std::uint32_t>(pass.kind), pass.radius,
                pass.downsample, pass.radius_parameter.value_or(STRATA_EFFECT_PARAMETER_NONE),
                pass.downsample_parameter.value_or(STRATA_EFFECT_PARAMETER_NONE),
                pass.source.empty() ? std::string{} : resource_text(pass.source));
        }

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
            strata::require_ok(strata_runtime_register_action_handler(
                                   session->runtime().native_handle(), &handler, &registration),
                               "headless action registration");
            registrations.emplace_back(registration, &strata_action_registration_release);
        }
        for (const SnapshotConfig& snapshot : scenario.snapshots)
            publish(snapshot);

        const std::string source_id = scenario.module.generic_string();
        const strata_activation_info activated = session->activate_source(source_id, 1U);
        if (activated.status != STRATA_ACTIVATION_ACTIVATED) {
            throw std::runtime_error("headless scenario module did not activate");
        }

        host::SessionSurface surface;
        surface.id = scenario.surface_id;
        surface.root_role = scenario.root_role == "screen" ? STRATA_SURFACE_ROOT_SCREEN
                                                           : STRATA_SURFACE_ROOT_OVERLAY;
        surface.root_name = scenario.root;
        surface.environment = environment();
        for (const FontConfig& font : scenario.fonts)
            surface.fonts.push_back(host::SessionFont{font.id, font.resource, font.system});
        for (const ImageConfig& image : scenario.images)
            surface.images.push_back(host::SessionImage{image.id, image.resource, image.sampling});
        session->create_surface(surface);
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
        strata::require_ok(
            strata_runtime_publish_host_snapshot(session->runtime().native_handle(), &config),
            "headless host snapshot publication");
    }

    void frame(const std::int64_t time_nanoseconds) {
        const host::SurfacePacketFrame framed = session->frame(time_nanoseconds);
        const strata_surface_frame_info& info = framed.surface;
        renderer->render(*framed.packet, time_nanoseconds);
        frame_available = true;
        const std::vector<strata_backdrop_sample> backdrop = renderer->take_backdrop_samples();
        if (!backdrop.empty())
            session->surface().report_backdrop(backdrop);
        if (!options.capture_frames)
            return;
        last_frame_json = session->surface().frame_json();
        data::JsonLimits frame_limits; // the host's own frame: no untrusted-input byte budget
        frame_limits.maximum_input_bytes =
            std::max(frame_limits.maximum_input_bytes, last_frame_json.size());
        last_frame_document = data::parse_json(last_frame_json, frame_limits);
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
        session->enqueue(events);
    }

    void resize(const double width, const double height, const double scale,
                const std::int64_t time_nanoseconds) {
        require_renderable_surface(width, height, scale);
        logical_width = width;
        logical_height = height;
        display_scale = scale;
        ++environment_generation;
        if (!session->adopt_environment(environment()))
            throw std::runtime_error("headless resize was not adopted");
        resize_renderer();
        frame(time_nanoseconds);
    }

    void close() {
        if (!session.has_value() || !session->has_surface())
            return;
        session->close_surface(
            [this](const host::RenderPacket& packet) { renderer->consume_resources(packet); });
        // Action registrations belong to the runtime, which closes after them.
        registrations.clear();
        session->close({});
        session.reset();
    }

    const Scenario& scenario;
    host::ResourceRoot resource_files;
    ApplicationHostOptions options;
    std::unique_ptr<CaptureRenderer> renderer;
    std::optional<host::Session> session;
    std::vector<Registration> registrations;
    std::vector<CapturedDiagnostic> diagnostics;
    std::vector<CapturedAction> actions;
    std::vector<CapturedEffect> effects;
    std::vector<CapturedAsyncRequest> async_requests;
    std::vector<CapturedFrame> frames;
    std::string clipboard;
    std::string durable;
    std::string last_frame_json;
    data::JsonValue last_frame_document;
    std::uint64_t next_snapshot_generation = 1U;
    std::uint64_t environment_generation = 1U;
    double logical_width = 0.0;
    double logical_height = 0.0;
    double display_scale = 1.0;
    bool frame_available = false;
};

ApplicationHost::ApplicationHost(const Scenario& scenario, std::filesystem::path resource_root,
                                 ApplicationHostOptions options)
    : impl_(std::make_unique<Impl>(scenario, std::move(resource_root), std::move(options))) {}

ApplicationHost::~ApplicationHost() = default;

void ApplicationHost::cancel_interactions() {
    strata::require_ok(
        strata_surface_cancel_interactions(impl_->session->surface().native_handle()),
        "preview focus cancellation");
}

void ApplicationHost::clear_observations() {
    impl_->diagnostics.clear();
    impl_->actions.clear();
    impl_->effects.clear();
    impl_->async_requests.clear();
    impl_->frames.clear();
}

std::string ApplicationHost::inspect() const {
    return impl_->session->surface().frame_json();
}

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
