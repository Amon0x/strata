#include "host.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <strata/contracts/debug_overlay.hpp>
#include <strata/contracts/demo_surface.hpp>
#include <strata/contracts/performance_hud.hpp>
#include <strata/contracts/settings_app.hpp>
#include <strata/contracts/strata_hub.hpp>
#include <strata/host.hpp>
#include <strata/strata.hpp>

#include <strata/render_packet.hpp>

#include "durable_writer.hpp"
#include "host/session.hpp"
#include "host_services.hpp"
#include "platform.hpp"
#include "renderer.hpp"
#include "showcase.hpp"
#include "win32/key_names.hpp"

namespace strata::desktop {

using host::RenderPacket;

namespace {

[[nodiscard]] std::string copy(const strata_string_view value) {
    return value.size == 0U ? std::string{} : std::string(value.data, value.size);
}

[[nodiscard]] double desktop_display_scale(
    const std::uint32_t framebuffer_width,
    const std::uint32_t framebuffer_height,
    const double dpi_scale
) {
    strata_scale_policy_config policy{sizeof(strata_scale_policy_config)};
    strata::require_ok(
        strata_scale_policy_defaults(STRATA_SCALE_POLICY_AUTO_FIT, &policy),
        "desktop scale-policy defaults"
    );
    // Desktop windows need more working room than a full-screen game surface. Auto-fit around a
    // 1600x900 logical viewport, never shrinking below the scale requested by the monitor DPI.
    policy.preferred_logical_width = 1'600.0;
    policy.preferred_logical_height = 900.0;
    policy.min_scale = std::min(dpi_scale, policy.max_scale);
    strata_scale_context context{sizeof(strata_scale_context)};
    strata::require_ok(
        strata_resolve_scale_context(
            &policy,
            static_cast<std::int64_t>(framebuffer_width),
            static_cast<std::int64_t>(framebuffer_height),
            &context
        ),
        "desktop scale-context resolution"
    );
    return context.scale;
}

[[nodiscard]] std::uint32_t current_modifiers() noexcept {
    std::uint32_t result = 0U;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) result |= STRATA_KEY_MODIFIER_SHIFT;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) result |= STRATA_KEY_MODIFIER_CONTROL;
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) result |= STRATA_KEY_MODIFIER_ALT;
    if ((GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0) {
        result |= STRATA_KEY_MODIFIER_SUPER;
    }
    return result;
}

template <typename Byte>
[[nodiscard]] std::uint64_t fingerprint(const std::span<const Byte> bytes) noexcept {
    std::uint64_t result = 14695981039346656037ULL;
    for (const Byte byte : bytes) {
        result ^= static_cast<std::uint8_t>(byte);
        result *= 1099511628211ULL;
    }
    return result;
}

} // namespace

struct Host::Impl final {
    enum HostTiming : std::size_t {
        host_total,
        showcase_core,
        showcase_submit,
        tuning_core,
        tuning_submit,
        tooling_total,
        present,
        tooling_update,
        debug_core,
        debug_submit,
        performance_core,
        performance_submit,
        overlay_setup,
        host_timing_count,
    };

    static constexpr std::array<std::string_view, host_timing_count> host_timing_paths{
        "demo.desktop/host/total",
        "demo.desktop/host/showcase-core",
        "demo.desktop/host/showcase-submit",
        "demo.desktop/host/tuning-core",
        "demo.desktop/host/tuning-submit",
        "demo.desktop/host/tooling",
        "demo.desktop/host/present",
        "demo.desktop/host/tooling-update",
        "demo.desktop/host/debug-core",
        "demo.desktop/host/debug-submit",
        "demo.desktop/host/performance-core",
        "demo.desktop/host/performance-submit",
        "demo.desktop/host/overlay-setup",
    };

    struct ProfileSection final {
        std::string path;
        std::uint64_t last_sample_frame_index = 0U;
        std::int64_t last_nanos = 0;
        std::int64_t p95_nanos = 0;
        std::int64_t p99_nanos = 0;
        std::int64_t maximum_nanos = 0;
    };

    struct ProfileCounter final {
        std::string name;
        std::uint64_t value = 0U;
    };

    struct ProfileSnapshot final {
        std::string scope_id;
        std::uint64_t frame_index = 0U;
        std::vector<ProfileSection> sections;
        std::vector<ProfileCounter> counters;
    };

    struct FrameBucket final {
        std::int64_t index = 0;
        std::int64_t maximum_total_nanos = 0;
        std::int64_t maximum_native_nanos = 0;
        std::uint64_t total_nanos = 0U;
        std::uint64_t frame_count = 0U;
    };

    struct TimingWindow final {
        void add(const std::int64_t nanos) {
            samples.push_back(std::max<std::int64_t>(0, nanos));
            if (samples.size() > 120U) samples.pop_front();
        }

        [[nodiscard]] std::int64_t last() const noexcept {
            return samples.empty() ? 0 : samples.back();
        }

        [[nodiscard]] std::int64_t p95() const {
            if (samples.empty()) return 0;
            std::vector<std::int64_t> ordered(samples.begin(), samples.end());
            const std::size_t index = (ordered.size() * 95U + 99U) / 100U - 1U;
            std::ranges::nth_element(ordered, ordered.begin() + index);
            return ordered[index];
        }

        [[nodiscard]] std::int64_t maximum() const noexcept {
            return samples.empty() ? 0 : *std::ranges::max_element(samples);
        }

        std::deque<std::int64_t> samples;
    };

    struct TimingSummary final {
        std::int64_t last_nanos = 0;
        std::int64_t p95_nanos = 0;
        std::int64_t maximum_nanos = 0;
    };

    struct ProfileCapture final {
        std::int64_t captured_at = 0;
        ProfileSnapshot runtime;
        ProfileSnapshot surface;
        std::array<TimingSummary, host_timing_count> host;
    };

    struct AsyncBridge final {
        struct Pending final {
            std::uint64_t id = 0U;
            std::string binding;
            std::string payload;
            std::int64_t started_nanos = 0;
            bool progress_published = false;
        };

        Impl* owner = nullptr;
        std::map<std::uint64_t, Pending> pending;
    };

    /** One of the window's composited applications: its Session and the async host it calls. */
    struct Layer final {
        std::string id;
        /** Declared first so it outlives the Session, whose runtime holds it as its async host. */
        std::unique_ptr<AsyncBridge> async;
        std::unique_ptr<host::Session> session;

        [[nodiscard]] bool live() const noexcept {
            return session != nullptr && session->has_surface();
        }
        [[nodiscard]] strata::Surface& surface() { return session->surface(); }
        [[nodiscard]] strata::Runtime& runtime() { return session->runtime(); }
        [[nodiscard]] strata::host::Bindings& bindings() { return session->bindings(); }
    };

    /**
     * The window's host. Another window of the same application (window.open) shares its model
     * and runs only `root`, an overlay of the showcase module, as the session `window.<id>`.
     */
    Impl(HWND window, std::filesystem::path resource_root, std::string instance_label,
         const HostOptions options, std::shared_ptr<ShowcaseModel> model = nullptr,
         std::string window_id = {}, const std::string_view root = {})
        : window(window), resource_root(resource_root), services(window, std::move(resource_root)),
          platform(
              window, [this](const std::string_view id) { return services.bytes(id); },
              [this](const std::string_view id) { return services.path(id).wstring(); }),
          renderer(window, options.vsync), instance_label(std::move(instance_label)),
          shared_model(model != nullptr ? std::move(model)
                                        : std::make_shared<ShowcaseModel>(this->instance_label)),
          showcase_model(*shared_model), window_id(std::move(window_id)), vsync(options.vsync) {
        if (this->instance_label.empty()) {
            throw std::invalid_argument("desktop instance label must not be empty");
        }
        RECT client{};
        if (!GetClientRect(window, &client)) throw std::runtime_error("could not read desktop client area");
        framebuffer_width = static_cast<std::uint32_t>(std::max<LONG>(1L, client.right - client.left));
        framebuffer_height = static_cast<std::uint32_t>(std::max<LONG>(1L, client.bottom - client.top));
        const UINT dpi = GetDpiForWindow(window);
        const double dpi_scale = dpi == 0U ? 1.0 : static_cast<double>(dpi) / 96.0;
        display_scale = desktop_display_scale(
            framebuffer_width, framebuffer_height, dpi_scale
        );
        services.set_surface_scale(display_scale);
        renderer.resize(
            framebuffer_width,
            framebuffer_height,
            logical_width(),
            logical_height()
        );
        // Files dragged in from the shell go to the layer that takes input, in logical units.
        platform.accept_files([this](const std::uint32_t kind, const double x, const double y,
                                     const std::string& paths) { files(kind, x, y, paths); });
        performance_hud_enabled = options.performance_hud;
        profile_sampling_enabled = options.profile_sampling;
        if (options.performance_hud) {
            const auto started = std::chrono::steady_clock::now();
            create_performance_hud();
            overlay_setup_timings.add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started
                ).count()
            );
        }
        if (!root.empty()) {
            create_demo_surface(showcase, "window." + this->window_id, root);
            showcase_enabled = true;
        } else {
            write_desktop_note();
        }
    }

    /**
     * The file the DESKTOP page's card drags out to other applications: a real file, written to
     * <temp>/Strata at every start (the page finds the folder with system.paths).
     */
    static void write_desktop_note() noexcept {
        try {
            const std::filesystem::path folder = std::filesystem::temp_directory_path() / "Strata";
            std::filesystem::create_directories(folder);
            std::ofstream note(folder / "Strata note.txt", std::ios::binary | std::ios::trunc);
            note << "This file came out of a Strata window.\r\n\r\n"
                    "The DESKTOP page's card carries it with strata.dragOut: drop it on Explorer, "
                    "a chat or a mail to copy it there.\r\n";
        } catch (...) {
            // Without the file the card has nothing to carry; the page says so when dropped.
        }
    }

    ~Impl() noexcept {
        close_session(hub);
        close_session(performance_hud);
        close_session(debug);
        close_session(settings);
        close_session(control_deck);
        close_session(tuning_panel);
        close_session(effect_gallery);
        close_session(showcase);
        try {
            // Paths that still fail were already reported by the writer.
            static_cast<void>(durable_writer.flush());
        } catch (...) {
        }
    }

    [[nodiscard]] double logical_width() const noexcept {
        return static_cast<double>(framebuffer_width) / display_scale;
    }

    [[nodiscard]] double logical_height() const noexcept {
        return static_cast<double>(framebuffer_height) / display_scale;
    }

    [[nodiscard]] std::int64_t now() const noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - epoch
        ).count();
    }

    [[nodiscard]] std::filesystem::path durable_path(
        const std::string_view application_id
    ) const {
        wchar_t* local = nullptr;
        std::size_t local_size = 0U;
        const errno_t environment_status = _wdupenv_s(
            &local, &local_size, L"LOCALAPPDATA"
        );
        const std::filesystem::path root = environment_status == 0 && local != nullptr
            ? std::filesystem::path(local)
            : std::filesystem::temp_directory_path();
        std::free(local);
        const std::string identity = instance_label + ":" + std::string(application_id);
        std::ostringstream name;
        name << std::hex << fingerprint(std::span(identity));
        return root / "Strata" / "durable" / (name.str() + ".json");
    }

    static void report_durable_failure(
        const std::filesystem::path& path,
        const std::string_view message
    ) noexcept {
        try {
            std::cerr << "strata desktop: STRATA.DESKTOP.DURABLE_WRITE_FAILED: "
                      << path.string() << ": " << message << '\n';
        } catch (...) {
        }
    }

    static strata_status durable_load(
        void* const user_data,
        const strata_string_view application_id,
        strata_bytes_view* const out_bytes
    ) noexcept {
        if (user_data == nullptr || out_bytes == nullptr) return STRATA_STATUS_INVALID_ARGUMENT;
        try {
            auto& self = *static_cast<Impl*>(user_data);
            const std::filesystem::path path = self.durable_path(copy(application_id));
            if (std::optional<std::string> pending = self.durable_writer.pending(path);
                pending.has_value()) {
                self.durable_read_buffer = std::move(*pending);
                *out_bytes = strata_bytes_view{
                    reinterpret_cast<const std::uint8_t*>(self.durable_read_buffer.data()),
                    self.durable_read_buffer.size(),
                };
                return STRATA_STATUS_OK;
            }
            if (!std::filesystem::is_regular_file(path)) return STRATA_STATUS_NOT_FOUND;
            std::ifstream input(path, std::ios::binary);
            if (!input.is_open()) return STRATA_STATUS_INTERNAL_ERROR;
            self.durable_read_buffer.assign(
                std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()
            );
            if (input.bad()) return STRATA_STATUS_INTERNAL_ERROR;
            *out_bytes = strata_bytes_view{
                reinterpret_cast<const std::uint8_t*>(self.durable_read_buffer.data()),
                self.durable_read_buffer.size(),
            };
            return STRATA_STATUS_OK;
        } catch (...) {
            return STRATA_STATUS_INTERNAL_ERROR;
        }
    }

    static strata_status durable_write(
        void* const user_data,
        const strata_string_view application_id,
        const strata_bytes_view bytes
    ) noexcept {
        if (user_data == nullptr || (bytes.data == nullptr && bytes.size != 0U)) {
            return STRATA_STATUS_INVALID_ARGUMENT;
        }
        try {
            auto& self = *static_cast<Impl*>(user_data);
            self.durable_writer.enqueue(
                self.durable_path(copy(application_id)),
                bytes.size == 0U
                    ? std::string{}
                    : std::string(
                          reinterpret_cast<const char*>(bytes.data),
                          reinterpret_cast<const char*>(bytes.data) + bytes.size
                      )
            );
            return STRATA_STATUS_OK;
        } catch (...) {
            return STRATA_STATUS_INTERNAL_ERROR;
        }
    }

    static strata_status async_begin(
        void* const user_data,
        const std::uint64_t request_id,
        const strata_string_view binding,
        const strata_string_view,
        const strata_string_view payload_json
    ) noexcept {
        if (user_data == nullptr) return STRATA_STATUS_INVALID_ARGUMENT;
        try {
            auto& bridge = *static_cast<AsyncBridge*>(user_data);
            const std::string binding_value = copy(binding);
            if (binding_value != "asyncResults" && binding_value != "asyncTreeChildren") {
                return STRATA_STATUS_NOT_FOUND;
            }
            const std::string payload = copy(payload_json);
            bridge.pending.insert_or_assign(request_id, AsyncBridge::Pending{
                request_id,
                binding_value,
                payload,
                bridge.owner->frame_time,
                false,
            });
            if (binding_value == "asyncResults") {
                bridge.owner->showcase_model.data_activity("query " + query_text(payload));
            }
            return STRATA_STATUS_OK;
        } catch (...) {
            return STRATA_STATUS_INTERNAL_ERROR;
        }
    }

    static void async_cancel(void* const user_data, const std::uint64_t request_id) noexcept {
        if (user_data != nullptr) {
            static_cast<AsyncBridge*>(user_data)->pending.erase(request_id);
        }
    }

    static std::string query_text(const std::string_view payload_json) {
        std::istringstream input{std::string(payload_json)};
        std::string value;
        if (!(input >> std::quoted(value))) return {};
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        std::ranges::transform(value, value.begin(), [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    }

    void advance_async(Layer& layer) {
        if (layer.async == nullptr || layer.session == nullptr) return;
        strata_runtime* const runtime = layer.runtime().native_handle();
        std::vector<std::uint64_t> completed;
        for (auto& [id, request] : layer.async->pending) {
            const std::int64_t elapsed = frame_time - request.started_nanos;
            const bool tree_children = request.binding == "asyncTreeChildren";
            if (!tree_children && !request.progress_published && elapsed >= 350'000'000) {
                request.progress_published = true;
                const std::string message = "Simulated desktop host query";
                const strata_async_progress progress{
                    sizeof(strata_async_progress), 1.0, 2.0, 1U, 0U,
                    strata::view(message),
                };
                const strata_result result = strata_runtime_async_progress(
                    runtime, id, &progress
                );
                if (result.status != STRATA_STATUS_NOT_FOUND) {
                    strata::require_ok(result, "desktop async progress");
                }
            }
            const std::int64_t completion_delay = tree_children
                ? 100'000'000 : 850'000'000;
            if (elapsed < completion_delay) continue;
            strata_result result;
            const std::string query = query_text(request.payload);
            if (tree_children) {
                const strata::host::Value event = strata::host::Value::parse(request.payload);
                if (event.string() != nullptr) {
                    static_cast<void>(showcase_model.load_tree_children(*event.string()));
                }
                std::vector<contracts::demo_surface::AsyncTreeChildrenValueItem> children;
                for (const contracts::demo_surface::DataTreeItemsItem& item :
                     showcase_model.tree_items()) {
                    children.push_back(contracts::demo_surface::AsyncTreeChildrenValueItem{
                        .key = item.key,
                        .label = item.label,
                        .parent_key = item.parent_key,
                        .may_have_children = item.may_have_children,
                        .children_loaded = item.children_loaded,
                    });
                }
                const std::string items = contracts::demo_surface::to_value(children).json();
                result = strata_runtime_async_succeed_json(
                    runtime, id, strata::view(items)
                );
            } else if (query == "fail") {
                result = strata_runtime_async_fail(
                    runtime, id,
                    strata::view("The simulated source rejected the query."),
                    strata::view("SIMULATED_FAILURE")
                );
            } else {
                std::vector<contracts::demo_surface::AsyncResultsValueItem> values;
                if (query != "empty") {
                    values.reserve(12U);
                    for (std::size_t index = 0U; index < 12U; ++index) {
                        values.push_back(contracts::demo_surface::AsyncResultsValueItem{
                            .key = "async." + std::to_string(index),
                            .label = "Simulated result " + std::to_string(index),
                        });
                    }
                }
                const std::string value_json =
                    contracts::demo_surface::to_value(values).json();
                result = strata_runtime_async_succeed_json(
                    runtime, id, strata::view(value_json)
                );
            }
            if (result.status != STRATA_STATUS_NOT_FOUND) {
                strata::require_ok(result, "desktop async completion");
            }
            completed.push_back(id);
        }
        for (const std::uint64_t id : completed) layer.async->pending.erase(id);
    }

    static void diagnostic(const strata_diagnostic& value) {
        std::cerr << "strata desktop: " << copy(value.code) << ": " << copy(value.message) << '\n';
    }

    [[nodiscard]] strata_surface_environment environment() const noexcept {
        strata_surface_environment result{
            sizeof(strata_surface_environment),
            environment_generation,
            static_cast<std::int64_t>(framebuffer_width),
            static_cast<std::int64_t>(framebuffer_height),
            logical_width(),
            logical_height(),
            display_scale,
            0.0,
            0.0,
            0.0,
            0.0,
            STRATA_POINT_SNAP_NEAREST,
            STRATA_RECTANGLE_SNAP_OUTWARD,
            STRATA_SURFACE_DENSITY_COMFORTABLE,
            STRATA_POINTER_PRECISION_FINE,
            STRATA_SURFACE_INPUT_POINTER | STRATA_SURFACE_INPUT_KEYBOARD |
                STRATA_SURFACE_INPUT_IME | STRATA_SURFACE_INPUT_CLIPBOARD,
            0U,
            0U,
        };
        platform.appearance().apply(result);
        return result;
    }

    static void profile(void* const user_data, const strata_profiler_snapshot* const snapshot) {
        if (user_data == nullptr || snapshot == nullptr) return;
        auto& result = *static_cast<ProfileSnapshot*>(user_data);
        result.scope_id = copy(snapshot->scope_id);
        result.frame_index = snapshot->frame_index;
        result.sections.clear();
        result.counters.clear();
        result.sections.reserve(snapshot->section_count);
        result.counters.reserve(snapshot->counter_count);
        for (std::size_t index = 0U; index < snapshot->section_count; ++index) {
            const strata_profiler_section& section = snapshot->sections[index];
            result.sections.push_back(ProfileSection{
                copy(section.path), section.last_sample_frame_index,
                section.last_nanos, section.p95_nanos,
                section.p99_nanos, section.maximum_nanos,
            });
        }
        for (std::size_t index = 0U; index < snapshot->counter_count; ++index) {
            const strata_profiler_counter& counter = snapshot->counters[index];
            result.counters.push_back(ProfileCounter{copy(counter.name), counter.value});
        }
    }

    void publish(Layer& layer, const std::string_view id, const std::string& json) {
        static_cast<void>(layer.runtime().publish_host_snapshot(id, json));
    }

    /**
     * Compiles every material the application declares an HLSL stage for. The declaration is the
     * same backend-neutral one every host reads, so adding a backend adds a source key, not a
     * second contract.
     */
    void declare_materials(const strata::Runtime& runtime) {
        for (const strata::MaterialDeclaration& declaration :
             runtime.material_declarations("hlsl")) {
            if (declaration.source.empty()) continue;
            renderer.declare_material(declaration.id, services.text(declaration.source));
        }
        for (const strata::EffectPassDeclaration& pass :
             runtime.effect_pass_declarations("hlsl")) {
            renderer.declare_effect_pass(
                pass.effect_id,
                pass.index,
                static_cast<std::uint32_t>(pass.kind),
                pass.radius,
                pass.downsample,
                pass.radius_parameter.value_or(STRATA_EFFECT_PARAMETER_NONE),
                pass.downsample_parameter.value_or(STRATA_EFFECT_PARAMETER_NONE),
                pass.source.empty() ? std::string{} : services.text(pass.source)
            );
        }
    }

    [[nodiscard]] Layer create_session(
        std::string id,
        const std::string_view schemas_resource
    ) {
        frame_time = now();
        Layer result;
        result.id = std::move(id);
        result.async = std::make_unique<AsyncBridge>(AsyncBridge{this, {}});

        host::SessionOptions options;
        options.application_id = result.id;
        options.stable_identity_seed = 0x5354524154414445ULL;
        // The bundled applications activate their checked-in compiled artifacts.
        options.capabilities = STRATA_CAPABILITY_COMPILED_MODULE_ACTIVATION;
        options.schemas = services.text(schemas_resource);
        options.read_text = [this](const std::string_view resource) {
            return services.text(resource);
        };
        options.on_diagnostic = &Impl::diagnostic;
        options.bindings_owner = "strata.desktop." + result.id;
        host::SessionAdapters adapters;
        adapters.resources = services.resource_adapter();
        adapters.clipboard = services.clipboard_adapter();
        adapters.ime = services.ime_adapter(result.id);
        adapters.effects = platform.effect_adapter(result.id);
        adapters.durable_store = strata_durable_store_adapter{
            sizeof(strata_durable_store_adapter), this, &Impl::durable_load, &Impl::durable_write,
        };
        adapters.async_host = strata_async_host_adapter{
            sizeof(strata_async_host_adapter),
            result.async.get(),
            &Impl::async_begin,
            &Impl::async_cancel,
        };
        result.session = std::make_unique<host::Session>(std::move(options), adapters);
        result.session->set_time(frame_time);
        declare_materials(result.runtime());
        return result;
    }

    void activate(
        Layer& layer,
        const std::string_view source_resource,
        const std::string_view root_name,
        std::vector<host::SessionImage> images = {}
    ) {
        if (!source_resource.ends_with(".strata")) {
            throw std::invalid_argument("desktop bundled module must use a .strata resource id");
        }
        std::string artifact_resource(source_resource);
        artifact_resource.resize(artifact_resource.size() - std::string_view(".strata").size());
        artifact_resource += ".compiled.bin";
        const std::vector<std::uint8_t> artifact = services.bytes(artifact_resource);
        if (layer.session->activate_compiled(artifact, 1U).status != STRATA_ACTIVATION_ACTIVATED) {
            throw std::runtime_error("desktop compiled Strata module did not activate");
        }

        host::SessionSurface surface;
        surface.id = layer.id;
        surface.root_role = STRATA_SURFACE_ROOT_OVERLAY;
        surface.root_name = std::string(root_name);
        surface.environment = environment();
        surface.fonts = {
            {"strata:fonts/default-medium", "assets/strata/fonts/medium.ttf"},
            {"strata:fonts/default", "assets/strata/fonts/default.ttf"},
            {"strata:fonts/mono", "assets/strata/fonts/mono.ttf"},
        };
        surface.images = std::move(images);
        layer.session->create_surface(surface);
    }

    void create_settings() {
        settings = create_session(
            "settings.desktop",
            "assets/strata/ui/settings_app.schemas.json"
        );
        settings.bindings().on(
            contracts::settings_app::SettingsSaveAction::id,
            [this](const strata::host::ActionEvent& event) {
                static_cast<void>(contracts::settings_app::SettingsSaveAction::decode(event));
                ++settings_save_count;
                settings_revision.changed();
                return strata::host::ActionResult::handled;
            }
        );
        settings.bindings().snapshot(
            "settings.desktop",
            [this] { return settings_revision.value(); },
            [this] {
                const std::string saved = settings_save_count == 0U
                    ? "No changes saved yet"
                    : "Saved " + std::to_string(settings_save_count) + " time" +
                        (settings_save_count == 1U ? std::string{} : std::string("s"));
                const auto profile = [](const std::string_view id, const std::string_view label) {
                    return contracts::settings_app::SettingsProfileTreeItem{
                        .key = std::string("settings.profile.") + std::string(id),
                        .label = std::string(label),
                        .may_have_children = false,
                        .children_loaded = true,
                    };
                };
                return contracts::settings_app::encode_settings(
                    contracts::settings_app::Settings{
                        .saved_message = saved,
                        .profile_tree = {
                            profile("balanced", "Balanced — Recommended visual and input defaults"),
                            profile("performance", "Performance — Reduced effects and tighter layout"),
                            profile("cinematic", "Cinematic — High-fidelity effects and comfortable spacing"),
                        },
                    }
                );
            }
        );
        settings.bindings().synchronize();
        activate(
            settings,
            "assets/strata/ui/settings_app.strata",
            "SettingsApp"
        );
    }

    void bind_showcase_model(Layer& layer) {
        static constexpr std::array extension_action_ids{
            std::string_view("demo.custom.pulse"),
            std::string_view("demo.inspect.pointer"),
        };
        for (const std::string_view action_id : contracts::demo_surface::action_ids) {
            layer.bindings().on(action_id, [this](
                                                        const strata::host::ActionEvent& event
                                                    ) {
                return showcase_model.handle(event);
            });
        }
        for (const std::string_view action_id : extension_action_ids) {
            layer.bindings().on(action_id, [this](const strata::host::ActionEvent& event) {
                return showcase_model.handle(event);
            });
        }
        layer.bindings().snapshot(
            "demo.desktop.data.tree",
            [this] { return showcase_model.tree_revision().value(); },
            [this] { return showcase_model.tree_snapshot(); }
        );
        layer.bindings().snapshot(
            "demo.desktop.data.table",
            [this] { return showcase_model.table_revision().value(); },
            [this] { return showcase_model.table_snapshot(); }
        );
        layer.bindings().snapshot(
            "demo.desktop.data.grid",
            [this] { return showcase_model.grid_revision().value(); },
            [this] { return showcase_model.grid_snapshot(); }
        );
        layer.bindings().snapshot(
            "demo.desktop.state",
            [this] { return showcase_model.demo_revision().value(); },
            [this] { return showcase_model.demo_snapshot(); }
        );
        layer.bindings().synchronize();
    }

    void create_demo_surface(
        Layer& layer,
        std::string id,
        const std::string_view root_name
    ) {
        layer = create_session(
            std::move(id),
            "assets/strata/ui/demo_surface.schemas.json"
        );
        bind_showcase_model(layer);
        activate(
            layer, "assets/strata/ui/demo_surface.strata", root_name,
            {
                {"strata:ui/icons/chevron-down", "assets/strata/images/ui/icons/chevron-down.svg"},
                {"strata:ui/icons/chevron-up", "assets/strata/images/ui/icons/chevron-up.svg"},
                // The MEDIA page: animated GIF, APNG and WebP, and a JPEG photo.
                {"demo:media/orbit", "assets/strata/media/orbit.gif"},
                {"demo:media/tunnel", "assets/strata/media/tunnel.gif"},
                {"demo:media/rings", "assets/strata/media/rings.png"},
                {"demo:media/plasma", "assets/strata/media/plasma.webp"},
                {"demo:media/sunset", "assets/strata/media/sunset.jpg"},
            });
    }

    void create_showcase() {
        create_demo_surface(showcase, "demo.desktop", "MainShowcase");
        strata::require_ok(
            strata_surface_set_profiler_capture(showcase.surface().native_handle(), 1U),
            "desktop showcase profiler capture"
        );
    }

    void create_effect_gallery() {
        create_demo_surface(
            effect_gallery,
            "effects.desktop",
            "GlassEffectShowcase"
        );
    }

    void create_control_deck() {
        if (control_deck.live()) return;
        control_deck = create_session(
            "control-deck.desktop",
            "assets/strata/ui/control_deck.schemas.json"
        );
        activate(
            control_deck,
            "assets/strata/ui/control_deck.strata",
            "ControlDeckMenu"
        );
    }

    void create_tuning_panel() {
        if (tuning_panel.live()) return;
        tuning_panel = create_session(
            "tuning-panel.desktop",
            "assets/strata/ui/tuning_panel.schemas.json"
        );
        activate(
            tuning_panel,
            "assets/strata/ui/tuning_panel.strata",
            "TuningPanelMenu"
        );
    }

    [[nodiscard]] static TimingSummary summarize(const TimingWindow& timings) {
        return TimingSummary{timings.last(), timings.p95(), timings.maximum()};
    }

    void capture_profile_snapshot() {
        Layer* const profiled = interactive_layer();
        if (profiled == nullptr || !profiled->live()) return;
        ProfileCapture capture;
        capture.captured_at = frame_time;
        const strata_profiler_snapshot_sink runtime_sink{
            sizeof(strata_profiler_snapshot_sink), &capture.runtime, &Impl::profile,
        };
        strata::require_ok(
            strata_runtime_read_profiler(profiled->runtime().native_handle(), &runtime_sink),
            "desktop runtime profiler sampling"
        );
        const strata_profiler_snapshot_sink surface_sink{
            sizeof(strata_profiler_snapshot_sink), &capture.surface, &Impl::profile,
        };
        strata::require_ok(
            strata_surface_read_profiler(profiled->surface().native_handle(), &surface_sink),
            "desktop surface profiler sampling"
        );
        capture.host = {
            summarize(host_total_timings),
            summarize(showcase_core_timings),
            summarize(showcase_submit_timings),
            summarize(tuning_core_timings),
            summarize(tuning_submit_timings),
            summarize(tooling_timings),
            summarize(present_timings),
            summarize(tooling_update_timings),
            summarize(debug_core_timings),
            summarize(debug_submit_timings),
            summarize(performance_core_timings),
            summarize(performance_submit_timings),
            summarize(overlay_setup_timings),
        };
        profile_history.push_back(std::move(capture));
        constexpr std::size_t maximum_profile_captures = 64U;
        if (profile_history.size() > maximum_profile_captures) profile_history.pop_front();
    }

    [[nodiscard]] ProfileSnapshot merged_profile_snapshot() const {
        ProfileSnapshot result;
        std::map<std::string, std::size_t, std::less<>> sections;
        const auto merge_sections = [&result, &sections](
                                        const ProfileSnapshot& snapshot,
                                        const bool runtime
                                    ) {
            for (const ProfileSection& source : snapshot.sections) {
                const std::string path =
                    (runtime ? "runtime:" : std::string{}) +
                    snapshot.scope_id + "/" + source.path;
                const auto [found, inserted] = sections.try_emplace(
                    path,
                    result.sections.size()
                );
                if (inserted) {
                    result.sections.push_back(source);
                    result.sections.back().path = path;
                    continue;
                }
                ProfileSection& destination = result.sections[found->second];
                destination.last_sample_frame_index = source.last_sample_frame_index;
                destination.last_nanos = source.last_nanos;
                destination.p95_nanos = std::max(
                    destination.p95_nanos, source.p95_nanos
                );
                destination.p99_nanos = std::max(
                    destination.p99_nanos, source.p99_nanos
                );
                destination.maximum_nanos = std::max(
                    destination.maximum_nanos, source.maximum_nanos
                );
            }
        };
        for (const ProfileCapture& capture : profile_history) {
            result.scope_id = capture.surface.scope_id;
            result.frame_index = capture.surface.frame_index;
            result.counters = capture.surface.counters;
            merge_sections(capture.runtime, true);
            merge_sections(capture.surface, false);
        }
        return result;
    }

    [[nodiscard]] static std::string full_profile_path(
        const std::string_view,
        const std::string_view path
    ) {
        return std::string(path);
    }

    [[nodiscard]] std::array<TimingSummary, host_timing_count>
    merged_host_timings() const {
        std::array<TimingSummary, host_timing_count> result{};
        for (const ProfileCapture& capture : profile_history) {
            for (std::size_t index = 0U; index < result.size(); ++index) {
                result[index].last_nanos = capture.host[index].last_nanos;
                result[index].p95_nanos = std::max(
                    result[index].p95_nanos, capture.host[index].p95_nanos
                );
                result[index].maximum_nanos = std::max(
                    result[index].maximum_nanos, capture.host[index].maximum_nanos
                );
            }
        }
        return result;
    }

    [[nodiscard]] std::string debug_json() const {
        const ProfileSnapshot profile_snapshot = merged_profile_snapshot();
        const std::array<TimingSummary, host_timing_count> host_timings =
            merged_host_timings();
        std::vector<ProfileSection> hot_paths = profile_snapshot.sections;
        std::ranges::stable_sort(hot_paths, [](const ProfileSection& left, const ProfileSection& right) {
            return left.p99_nanos > right.p99_nanos;
        });
        if (hot_paths.size() > 24U) hot_paths.resize(24U);
        std::int64_t last_frame = 0;
        for (const ProfileSection& section : profile_snapshot.sections) {
            if (section.path == profile_snapshot.scope_id + "/frame") {
                last_frame = std::max(last_frame, section.last_nanos);
            }
        }
        const auto metric_text = [](const double value, const std::string_view suffix = {}) {
            std::ostringstream output;
            output << value << suffix;
            return output.str();
        };
        std::vector<contracts::debug_overlay::DebugSummaryMetricsItem> summary_metrics{
            {"Active native surfaces", "1"},
            {"Latest native frame", std::to_string(profile_snapshot.frame_index)},
            {
                "Last native frame",
                metric_text(static_cast<double>(last_frame) / 1'000'000.0, " ms"),
            },
            {
                "Last complete host frame",
                metric_text(static_cast<double>(last_total_nanos) / 1'000'000.0, " ms"),
            },
            {
                "Last showcase core frame",
                metric_text(static_cast<double>(last_native_nanos) / 1'000'000.0, " ms"),
            },
        };
        std::vector<contracts::debug_overlay::DebugCountersItem> counters;
        counters.reserve(profile_snapshot.counters.size());
        for (const ProfileCounter& counter : profile_snapshot.counters) {
            counters.push_back({counter.name, std::to_string(counter.value)});
        }

        std::vector<contracts::debug_overlay::DebugHotPathsItem> typed_hot_paths;
        typed_hot_paths.reserve(hot_paths.size() + host_timings.size());
        const auto append_host_path = [&typed_hot_paths](
                                          const std::string_view path,
                                          const TimingSummary& timings
                                      ) {
            typed_hot_paths.push_back(contracts::debug_overlay::DebugHotPathsItem{
                .path = std::string(path),
                .last_millis = static_cast<double>(timings.last_nanos) / 1'000'000.0,
                .p95_millis = static_cast<double>(timings.p95_nanos) / 1'000'000.0,
                .p99_millis = static_cast<double>(timings.maximum_nanos) / 1'000'000.0,
                .max_millis = static_cast<double>(timings.maximum_nanos) / 1'000'000.0,
            });
        };
        for (std::size_t index = 0U; index < host_timing_paths.size(); ++index) {
            append_host_path(host_timing_paths[index], host_timings[index]);
        }
        for (const ProfileSection& section : hot_paths) {
            typed_hot_paths.push_back(contracts::debug_overlay::DebugHotPathsItem{
                .path = full_profile_path(profile_snapshot.scope_id, section.path),
                .last_millis = static_cast<double>(section.last_nanos) / 1'000'000.0,
                .p95_millis = static_cast<double>(section.p95_nanos) / 1'000'000.0,
                .p99_millis = static_cast<double>(section.p99_nanos) / 1'000'000.0,
                .max_millis = static_cast<double>(section.maximum_nanos) / 1'000'000.0,
            });
        }
        return contracts::debug_overlay::encode_debug(contracts::debug_overlay::Debug{
            .mode = std::string(contracts::debug_overlay::wire_name(debug_mode)),
            .frame = static_cast<double>(profile_snapshot.frame_index),
            .errors = 0.0,
            .warnings = 0.0,
            .infos = 0.0,
            .diagnostic_count = 0.0,
            .dropped_diagnostics = 0.0,
            .hot_path_count = static_cast<double>(typed_hot_paths.size()),
            .spike_count = 0.0,
            .motion_count = 0.0,
            .running_motion_count = 0.0,
            .semantic_count = 0.0,
            .collection_count = 0.0,
            .summary_metrics = std::move(summary_metrics),
            .counters = std::move(counters),
            .hot_paths = std::move(typed_hot_paths),
            .spikes = {},
            .diagnostics = {},
            .motions = {},
            .semantics = {},
            .collections = {},
        }).json();
    }

    [[nodiscard]] std::string debug_export_text() const {
        const ProfileSnapshot profile_snapshot = merged_profile_snapshot();
        const std::array<TimingSummary, host_timing_count> host_timings =
            merged_host_timings();
        std::vector<ProfileSection> hot_paths = profile_snapshot.sections;
        std::ranges::stable_sort(
            hot_paths,
            [](const ProfileSection& left, const ProfileSection& right) {
                return left.p95_nanos > right.p95_nanos;
            }
        );
        const auto millis = [](const std::int64_t nanos) {
            return static_cast<double>(nanos) / 1'000'000.0;
        };
        std::ostringstream output;
        output << std::fixed << std::setprecision(4)
               << "STRATA DEBUG PROFILE\n"
               << "frame\t" << profile_snapshot.frame_index << '\n'
               << "columns\tpath\tlast_ms\trecent_p95_peak_ms\tmax_ms\n\n"
               << "HOST PATHS\n";
        for (std::size_t index = 0U; index < host_timing_paths.size(); ++index) {
            output << host_timing_paths[index] << '\t'
                   << millis(host_timings[index].last_nanos) << '\t'
                   << millis(host_timings[index].p95_nanos) << '\t'
                   << millis(host_timings[index].maximum_nanos) << '\n';
        }
        output << "\nNATIVE HOT PATHS (all sections)\n";
        for (const ProfileSection& section : hot_paths) {
            output << full_profile_path(profile_snapshot.scope_id, section.path) << '\t'
                   << millis(section.last_nanos) << '\t'
                   << millis(section.p95_nanos) << '\t'
                   << millis(section.maximum_nanos) << '\n';
        }
        output << "\nCOUNTERS (latest snapshot)\n";
        for (const ProfileCounter& counter : profile_snapshot.counters) {
            output << counter.name << '\t' << counter.value << '\n';
        }
        return output.str();
    }

    void create_debug() {
        if (debug.live()) return;
        debug = create_session(
            "profiler.desktop", "assets/strata/ui/debug_overlay.schemas.json"
        );
        debug.bindings().on(
            contracts::debug_overlay::StrataDebugCloseAction::id,
            [this](const strata::host::ActionEvent&) {
                debug_enabled = false;
                return strata::host::ActionResult::handled;
            }
        );
        debug.bindings().on(
            contracts::debug_overlay::StrataDebugClearDiagnosticsAction::id,
            [this](const strata::host::ActionEvent&) {
                clear_diagnostics_pending = true;
                return strata::host::ActionResult::handled;
            }
        );
        debug.bindings().on(
            contracts::debug_overlay::StrataDebugCopyProfileAction::id,
            [this](const strata::host::ActionEvent&) {
                static_cast<void>(services.write_clipboard(debug_export_text()));
                return strata::host::ActionResult::handled;
            }
        );
        debug.bindings().on(
            contracts::debug_overlay::StrataDebugSelectModeAction::id,
            [this](const strata::host::ActionEvent& event) {
                const contracts::debug_overlay::StrataDebugSelectModeAction action =
                    contracts::debug_overlay::StrataDebugSelectModeAction::decode(event);
                debug_mode = action.mode;
                debug_snapshot_dirty = true;
                return strata::host::ActionResult::handled;
            }
        );
        publish(debug, "profiler.desktop", debug_json());
        activate(
            debug,
            "assets/strata/ui/debug_overlay.strata",
            "ProfilerOverlay"
        );
    }

    [[nodiscard]] std::string performance_json() const {
        std::uint64_t recent_frames = 0U;
        std::uint64_t recent_total_nanos = 0U;
        if (!frame_buckets.empty()) {
            const std::int64_t first_recent = frame_buckets.back().index - 3;
            for (const FrameBucket& bucket : frame_buckets) {
                if (bucket.index < first_recent) continue;
                recent_frames += bucket.frame_count;
                recent_total_nanos += bucket.total_nanos;
            }
        }
        const double fps = recent_total_nanos > 0U
            ? static_cast<double>(recent_frames) * 1'000'000'000.0 /
                static_cast<double>(recent_total_nanos)
            : 0.0;
        const double total_millis = static_cast<double>(last_total_nanos) / 1'000'000.0;
        const double native_millis = static_cast<double>(last_native_nanos) / 1'000'000.0;
        const auto decimal = [](const double value) {
            std::ostringstream text;
            text << std::fixed << std::setprecision(1) << value;
            return text.str();
        };

        std::vector<contracts::performance_hud::PerformanceSamplesItem> samples;
        samples.reserve(frame_buckets.size());
        for (const FrameBucket& bucket : frame_buckets) {
            const auto graph_height = [](const std::int64_t nanos) {
                if (nanos <= 0) return 0.0;
                const double millis = static_cast<double>(nanos) / 1'000'000.0;
                return std::clamp(
                    std::log1p(millis) / std::log1p(500.0) * 58.0, 1.0, 58.0
                );
            };
            samples.push_back(contracts::performance_hud::PerformanceSamplesItem{
                .key = "bucket." + std::to_string(bucket.index),
                .total_height = graph_height(bucket.maximum_total_nanos),
                .native_height = graph_height(bucket.maximum_native_nanos),
                .severe = bucket.maximum_total_nanos > 100'000'000,
                .slow = bucket.maximum_total_nanos > 16'666'667,
            });
        }
        return contracts::performance_hud::encode_performance(
            contracts::performance_hud::Performance{
                .fps = decimal(fps),
                .total_millis = decimal(total_millis),
                .native_millis = decimal(native_millis),
                .samples = std::move(samples),
            }
        ).json();
    }

    /** Hub view over the surfaces this host owns, in the shape the hub module declares. */
    [[nodiscard]] std::string hub_json() const {
        const double millis = static_cast<double>(host_total_timings.last()) / 1'000'000.0;
        std::ostringstream summary;
        summary << std::fixed << std::setprecision(1) << millis << " ms  ·  "
                << std::setprecision(0) << (millis > 0.0 ? 1'000.0 / millis : 0.0)
                << " fps";

        // The chart reads against the window's own peak rather than a fixed budget: a host holding
        // 1 ms frames would otherwise draw a flat line along the floor and say nothing at all.
        double scale = 8.0;
        for (const double sample : frame_history) scale = std::max(scale, sample);
        const std::size_t sample_count = frame_history.size();
        std::vector<contracts::strata_hub::HubFrameHistoryItem> history;
        std::vector<contracts::strata_hub::HubFrameAreaItem> area;
        history.reserve(sample_count);
        area.reserve(sample_count + (sample_count == 0U ? 0U : 2U));
        for (std::size_t index = 0U; index < sample_count; ++index) {
            const double x = sample_count < 2U
                ? 0.0
                : static_cast<double>(index) / static_cast<double>(sample_count - 1U);
            const double y = 1.0 - std::clamp(frame_history[index] / scale, 0.0, 1.0);
            history.push_back(contracts::strata_hub::HubFrameHistoryItem{x, y});
            area.push_back(contracts::strata_hub::HubFrameAreaItem{x, y});
        }
        if (sample_count != 0U) {
            area.push_back(contracts::strata_hub::HubFrameAreaItem{1.0, 1.0});
            area.push_back(contracts::strata_hub::HubFrameAreaItem{0.0, 1.0});
        }

        const int open_count = (control_deck_enabled ? 1 : 0) +
            (tuning_panel_enabled ? 1 : 0) +
            (effect_gallery_enabled ? 1 : 0) +
            (settings_enabled ? 1 : 0) + (showcase_enabled ? 1 : 0) +
            (debug_enabled ? 1 : 0) + (performance_hud_enabled ? 1 : 0);
        std::string status;
        if (open_count == 0) status = "No surface open";
        else if (open_count == 1) status = "1 surface open · shortcuts stay live";
        else status = std::to_string(open_count) + " surfaces open · shortcuts stay live";

        const std::array<std::array<std::string_view, 4U>, 7U> entries{{
            {"hub.control-deck", "Control Deck", "Dark authored configuration menu", "F2"},
            {"hub.effects", "Effect gallery", "Authored backdrop and content programs", "F3"},
            {"hub.tuning", "Tuning panel", "Dense live material tuning overlay", "F4"},
            {"hub.settings", "Settings", "Durable application state", "F6"},
            {"hub.showcase", "Component showcase", "Widgets and extensions", "F7"},
            {"hub.profiler", "Profiler overlay", "Layout and render timings", "F8"},
            {"hub.performance", "Performance HUD", "Passive frame telemetry", "F9"},
        }};
        const std::array<bool, 7U> active{
            control_deck_enabled,
            effect_gallery_enabled,
            tuning_panel_enabled,
            settings_enabled,
            showcase_enabled,
            debug_enabled,
            performance_hud_enabled,
        };
        std::vector<contracts::strata_hub::HubSurfacesItem> surfaces;
        surfaces.reserve(entries.size());
        for (std::size_t index = 0U; index < entries.size(); ++index) {
            surfaces.push_back(contracts::strata_hub::HubSurfacesItem{
                .key = std::string(entries[index][0]),
                .name = std::string(entries[index][1]),
                .detail = std::string(entries[index][2]),
                .shortcut = std::string(entries[index][3]),
                .active = active[index],
            });
        }
        std::ostringstream frame_scale;
        frame_scale << std::fixed << std::setprecision(1) << "full scale " << scale << " ms";
        return contracts::strata_hub::encode_hub(contracts::strata_hub::Hub{
            .frame_summary = summary.str(),
            .frame_millis = millis,
            .frame_history = std::move(history),
            .frame_area = std::move(area),
            .frame_scale = frame_scale.str(),
            .status = std::move(status),
            .surfaces = std::move(surfaces),
        }).json();
    }

    void launch_from_hub(const std::string_view target) {
        if (target == "hub.self") {
            hub_enabled = false;
            if (hub.live()) {
                static_cast<void>(strata_surface_cancel_interactions(hub.surface().native_handle()));
            }
        } else if (target == "hub.control-deck") {
            toggle_control_deck();
        } else if (target == "hub.tuning") {
            toggle_tuning_panel();
        } else if (target == "hub.effects") {
            toggle_effect_gallery();
        } else if (target == "hub.settings") {
            toggle_settings();
        } else if (target == "hub.showcase") {
            toggle_showcase();
        } else if (target == "hub.profiler") {
            cycle_debug(false);
        } else if (target == "hub.performance") {
            toggle_performance_hud();
        }
    }

    void create_hub() {
        if (hub.live()) return;
        hub = create_session("hub.desktop", "assets/strata/ui/strata_hub.schemas.json");
        hub.bindings().on(
            contracts::strata_hub::HubLaunchAction::id,
            [this](const strata::host::ActionEvent& event) {
                const contracts::strata_hub::HubLaunchAction action =
                    contracts::strata_hub::HubLaunchAction::decode(event);
                launch_from_hub(action.target);
                return strata::host::ActionResult::handled;
            }
        );
        publish(hub, "hub.desktop", hub_json());
        activate(
            hub,
            "assets/strata/ui/strata_hub.strata",
            "StrataHub",
            {
                {"strata:brand/mark", "assets/strata/images/brand/strata-mark.svg"},
                {"strata:brand/northstar-hero", "assets/strata/images/brand/northstar-hero.svg"},
            }
        );
    }

    void toggle_hub() {
        if (!hub_enabled && !hub.live()) {
            initialize_overlay([this] { create_hub(); });
        }
        hub_enabled = !hub_enabled;
        if (!hub_enabled && hub.live()) {
            static_cast<void>(strata_surface_cancel_interactions(hub.surface().native_handle()));
        }
    }

    void create_performance_hud() {
        if (performance_hud.live()) return;
        performance_hud = create_session(
            "performance.desktop", "assets/strata/ui/performance_hud.schemas.json"
        );
        publish(performance_hud, "performance.desktop", performance_json());
        activate(
            performance_hud,
            "assets/strata/ui/performance_hud.strata",
            "PerformanceHud"
        );
    }

    void record_frame_sample(
        const std::int64_t timestamp,
        const std::int64_t total_nanos,
        const std::int64_t native_nanos
    ) {
        const std::int64_t bucket_index = timestamp / 500'000'000;
        if (frame_buckets.empty() || bucket_index > frame_buckets.back().index) {
            std::int64_t first_index = frame_buckets.empty()
                ? bucket_index - 59
                : frame_buckets.back().index + 1;
            if (bucket_index - first_index >= 60) {
                frame_buckets.clear();
                first_index = bucket_index - 59;
            }
            for (std::int64_t index = first_index; index <= bucket_index; ++index) {
                frame_buckets.push_back(FrameBucket{index});
            }
            while (frame_buckets.size() > 60U) frame_buckets.pop_front();
        }
        if (frame_buckets.empty() || frame_buckets.back().index != bucket_index) return;

        FrameBucket& bucket = frame_buckets.back();
        bucket.maximum_total_nanos = std::max(bucket.maximum_total_nanos, total_nanos);
        bucket.maximum_native_nanos = std::max(bucket.maximum_native_nanos, native_nanos);
        bucket.total_nanos += static_cast<std::uint64_t>(std::max<std::int64_t>(0, total_nanos));
        ++bucket.frame_count;
        last_total_nanos = total_nanos;
        last_native_nanos = native_nanos;
    }

    void close_session(Layer& layer) noexcept {
        if (layer.session == nullptr) return;
        // Its requests in flight, sockets, watches and sounds go with it.
        platform.close_client(layer.id);
        try {
            layer.session->close([this](const RenderPacket& terminal) {
                renderer.consume_resources(terminal);
            });
        } catch (...) {
            layer.session->abandon();
        }
        layer.session.reset();
        layer.async.reset();
        renderer.release_layer(layer.id);
    }

    template <typename Create>
    void initialize_overlay(Create&& create) {
        const auto started = std::chrono::steady_clock::now();
        std::forward<Create>(create)();
        overlay_setup_timings.add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started
            ).count()
        );
    }

    void toggle_settings() {
        if (!settings_enabled && !settings.live()) {
            initialize_overlay([this] { create_settings(); });
        }
        settings_enabled = !settings_enabled;
        if (!settings_enabled && settings.live()) {
            static_cast<void>(strata_surface_cancel_interactions(
                settings.surface().native_handle()
            ));
        }
    }

    void toggle_effect_gallery() {
        if (!effect_gallery_enabled && !effect_gallery.live()) {
            initialize_overlay([this] { create_effect_gallery(); });
        }
        effect_gallery_enabled = !effect_gallery_enabled;
        if (!effect_gallery_enabled && effect_gallery.live()) {
            static_cast<void>(strata_surface_cancel_interactions(
                effect_gallery.surface().native_handle()
            ));
        }
    }

    void toggle_control_deck() {
        if (!control_deck_enabled && !control_deck.live()) {
            initialize_overlay([this] { create_control_deck(); });
        }
        control_deck_enabled = !control_deck_enabled;
        if (!control_deck_enabled && control_deck.live()) {
            static_cast<void>(strata_surface_cancel_interactions(
                control_deck.surface().native_handle()
            ));
        }
    }

    void toggle_tuning_panel() {
        if (!tuning_panel_enabled && !tuning_panel.live()) {
            initialize_overlay([this] { create_tuning_panel(); });
        }
        tuning_panel_enabled = !tuning_panel_enabled;
        if (!tuning_panel_enabled && tuning_panel.live()) {
            static_cast<void>(strata_surface_cancel_interactions(
                tuning_panel.surface().native_handle()
            ));
        }
    }

    void toggle_showcase() {
        if (!showcase_enabled && !showcase.live()) {
            initialize_overlay([this] { create_showcase(); });
        }
        if (showcase_enabled) {
            showcase_enabled = false;
            showcase_closing = true;
            showcase_hidden_at = now();
            showcase_model.surface_visible(false);
            showcase.bindings().synchronize();
            static_cast<void>(strata_surface_cancel_interactions(showcase.surface().native_handle()));
            return;
        }
        showcase_enabled = true;
        showcase_closing = false;
        showcase_model.surface_visible(true);
        showcase.bindings().synchronize();
    }

    void cycle_debug(const bool reverse) {
        if (!debug_enabled) {
            debug_mode = reverse ? debug_modes.back() : debug_modes.front();
            if (!debug.live()) {
                initialize_overlay([this] { create_debug(); });
            }
            debug_enabled = true;
        } else {
            const auto current = std::ranges::find(debug_modes, debug_mode);
            const std::ptrdiff_t index = current != debug_modes.end()
                ? current - debug_modes.begin()
                : 0;
            const std::ptrdiff_t next = index + (reverse ? -1 : 1);
            if (next < 0 || next >= static_cast<std::ptrdiff_t>(debug_modes.size())) {
                debug_enabled = false;
                if (debug.live()) {
                    static_cast<void>(strata_surface_cancel_interactions(
                        debug.surface().native_handle()
                    ));
                }
                return;
            }
            debug_mode = debug_modes[static_cast<std::size_t>(next)];
        }
        debug_snapshot_dirty = true;
        next_debug_snapshot = 0;
    }

    void toggle_performance_hud() {
        performance_hud_enabled = !performance_hud_enabled;
        if (performance_hud_enabled) {
            if (!performance_hud.live()) {
                initialize_overlay([this] { create_performance_hud(); });
            }
            performance_snapshot_dirty = true;
            next_performance_snapshot = 0;
        }
    }

    /** Every layer, in the order the window composites them. */
    [[nodiscard]] std::array<Layer*, 8U> layers() noexcept {
        return {
            &showcase,
            &effect_gallery,
            &control_deck,
            &tuning_panel,
            &settings,
            &debug,
            &hub,
            &performance_hud,
        };
    }

    /** Whether the window draws `layer` this frame; a closing showcase draws its transition. */
    [[nodiscard]] bool shown(const Layer& layer) const noexcept {
        if (&layer == &showcase) return showcase_enabled || showcase_closing;
        if (&layer == &effect_gallery) return effect_gallery_enabled;
        if (&layer == &control_deck) return control_deck_enabled;
        if (&layer == &tuning_panel) return tuning_panel_enabled;
        if (&layer == &settings) return settings_enabled;
        if (&layer == &debug) return debug_enabled;
        if (&layer == &hub) return hub_enabled;
        return performance_hud_enabled;
    }

    void resize(
        const std::uint32_t next_width,
        const std::uint32_t next_height,
        const double next_scale
    ) {
        if (next_width == 0U || next_height == 0U || next_scale <= 0.0) return;
        framebuffer_width = next_width;
        framebuffer_height = next_height;
        display_scale = desktop_display_scale(next_width, next_height, next_scale);
        services.set_surface_scale(display_scale);
        renderer.resize(next_width, next_height, logical_width(), logical_height());
        ++environment_generation;
        const strata_surface_environment next = environment();
        for (Layer* const layer : layers()) {
            if (layer->live() && !layer->session->adopt_environment(next)) {
                throw std::runtime_error("desktop environment generation was rejected");
            }
        }
    }

    void enqueue(strata_input_event event, const std::string_view input_text = {}) {
        Layer* const layer = interactive_layer();
        if (layer == nullptr || !layer->live()) return;
        event.struct_size = sizeof(event);
        event.version = STRATA_INPUT_EVENT_VERSION_2;
        event.modifiers = current_modifiers();
        event.text = strata::view(input_text);
        event.timestamp_nanoseconds = now();
        if (event.kind != STRATA_INPUT_KEY) event.key_action = STRATA_KEY_PRESS;
        layer->session->enqueue(std::span<const strata_input_event>(&event, 1U));
    }

    void flush_pointer_move() {
        if (!pending_pointer_move.has_value()) return;
        const strata_input_event event = *pending_pointer_move;
        pending_pointer_move.reset();
        enqueue(event);
    }

    struct LayerFrame final {
        /** Framing the Session: the core frame, packet read and decode. */
        std::int64_t core_nanos = 0;
        /** Drawing the packet into the layer and collecting its backdrop probes. */
        std::int64_t submit_nanos = 0;
        bool drew = false;
    };

    /** Frames one layer's Session, draws its packet into the layer and publishes the work. */
    [[nodiscard]] LayerFrame frame_layer(Layer& layer, DesktopFrameSample* const sample) {
        const auto started = std::chrono::steady_clock::now();
        const host::SurfacePacketFrame framed = layer.session->frame(frame_time);
        const RenderPacket& packet = *framed.packet;
        const auto submit_started = std::chrono::steady_clock::now();
        const RenderLayerTelemetry rendered = renderer.render_layer(layer.id, packet);
        const std::vector<strata_backdrop_sample> backdrop =
            renderer.take_backdrop_samples(layer.id);
        if (!backdrop.empty())
            layer.surface().report_backdrop(backdrop);
        const auto finished = std::chrono::steady_clock::now();
        const strata_surface_frame_info& info = framed.surface;
        const LayerFrame result{
            std::chrono::duration_cast<std::chrono::nanoseconds>(submit_started - started).count(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - submit_started).count(),
            info.render_command_count > 0U && packet.planned_draw_count > 0U,
        };
        if (sample != nullptr) {
            sample->frame_index = info.frame_index;
            sample->frame_time_nanos = info.frame_time_nanoseconds;
            sample->input_events = info.processed_input_event_count;
            sample->emitted_events = info.emitted_event_count;
            sample->render_commands = info.render_command_count;
            sample->packet_bytes = info.render_packet_size;
            sample->draw_calls = packet.planned_draw_count;
            sample->batches = packet.batches.size();
            sample->vertices = packet.vertices.size() / host::packet_vertex_bytes;
            sample->blur_passes = rendered.blur_passes;
        }
        ProfilerHostFrame backend;
        backend.blur_passes = rendered.blur_passes;
        backend.blur_target_width = rendered.blur_target_width;
        backend.blur_target_height = rendered.blur_target_height;
        backend.blur_nanoseconds = rendered.blur_nanos;
        backend.submit_nanoseconds = result.submit_nanos;
        layer.session->record_host_frame(packet, backend);
        return result;
    }

    /** The topmost open layer, which receives input and is the one performance samples describe. */
    [[nodiscard]] Layer* interactive_layer() noexcept {
        return hub_enabled ? &hub
            : debug_enabled ? &debug
            : settings_enabled ? &settings
            : tuning_panel_enabled ? &tuning_panel
            : control_deck_enabled ? &control_deck
            : effect_gallery_enabled ? &effect_gallery
            : showcase_enabled ? &showcase
            : nullptr;
    }

    void files(const std::uint32_t kind, const double x, const double y, const std::string& paths) {
        flush_pointer_move();
        strata_input_event event{};
        event.kind = kind;
        event.x = x / display_scale;
        event.y = y / display_scale;
        enqueue(event, paths);
    }

    /**
     * Runs the platform requests the last frame made (a dialog blocks here, between frames) and
     * hands each answer to the layer that asked, publishing any image it brings first.
     */
    void answer_platform_requests() {
        for (PlatformAnswer& answer : platform.pump()) {
            const std::string said =
                answer.channel.has_value() ? PlatformServices::event_json(answer) : std::string{};
            for (Layer* const layer : layers()) {
                if (!layer->live())
                    continue;
                // Events go to the session they name, or to all of them; answers to the asker.
                if (answer.channel.has_value()) {
                    if (!answer.client.empty() && layer->id != answer.client)
                        continue;
                    strata_input_event event{};
                    event.struct_size = sizeof(event);
                    event.version = STRATA_INPUT_EVENT_VERSION_2;
                    event.kind = STRATA_INPUT_PLATFORM_EVENT;
                    event.text = strata::view(said);
                    event.timestamp_nanoseconds = now();
                    layer->session->enqueue(std::span<const strata_input_event>(&event, 1U));
                    continue;
                }
                if (layer->id != answer.client)
                    continue;
                if (answer.image.has_value()) {
                    layer->runtime().publish_image(answer.image->id, answer.image->width,
                                                   answer.image->height, answer.image->rgba);
                }
                strata_input_event event{};
                event.struct_size = sizeof(event);
                event.version = STRATA_INPUT_EVENT_VERSION_2;
                event.kind = STRATA_INPUT_PLATFORM_RESULT;
                event.selection_start = answer.request;
                event.text = strata::view(answer.json);
                event.timestamp_nanoseconds = now();
                layer->session->enqueue(std::span<const strata_input_event>(&event, 1U));
            }
        }
        // Videos show their newest frame through the runtime image they play into.
        for (auto& [client, frame] : platform.video_frames()) {
            for (Layer* const layer : layers()) {
                if (layer->live() && layer->id == client)
                    layer->runtime().publish_image(frame.id, frame.width, frame.height, frame.rgba);
            }
        }
        // A video nothing shows any more (its page switched away) pauses until something does.
        platform.follow_video_use([this](const std::string_view client,
                                         const std::string_view image) {
            for (Layer* const layer : layers()) {
                if (layer->live() && layer->id == client)
                    return layer->surface().shows_image(image);
            }
            return false;
        });
        // The system's move loop swallowed the release of the press that began it.
        if (platform.take_window_moved()) {
            pending_pointer_move.reset();
            for (Layer* const layer : layers()) {
                if (layer->live()) {
                    static_cast<void>(
                        strata_surface_cancel_interactions(layer->surface().native_handle()));
                }
            }
        }
        renderer.set_transparent(platform.transparent());
        // The user changed the system's theme or accent: every layer's env follows.
        if (platform.take_appearance_changed()) {
            ++environment_generation;
            const strata_surface_environment next = environment();
            for (Layer* const layer : layers()) {
                if (layer->live())
                    static_cast<void>(layer->session->adopt_environment(next));
            }
        }
        // A node capture reads this frame's pixels once it is presented.
        if (platform.wants_frame()) {
            platform_capture = true;
            renderer.capture_next_frame();
        }
    }

    /** Hands the frame just presented to the node captures waiting for it. */
    void deliver_platform_capture() {
        if (!platform_capture)
            return;
        platform_capture = false;
        std::optional<FrameCapture> captured = renderer.take_capture();
        if (!captured.has_value())
            return;
        platform.frame_presented(captured->width, captured->height, captured->rgba);
        // A capture the host asked for itself (a performance scenario) is the same frame.
        if (host_capture)
            held_capture = std::move(captured);
    }

    void frame() {
        const auto host_frame_started = std::chrono::steady_clock::now();
        answer_platform_requests();
        Layer* const interactive = interactive_layer();
        std::int64_t tooling_nanos = 0;
        flush_pointer_move();
        if (clear_diagnostics_pending && showcase.live()) {
            clear_diagnostics_pending = false;
            strata::require_ok(
                strata_surface_clear_diagnostics(showcase.surface().native_handle()),
                "desktop diagnostic clear"
            );
            debug_snapshot_dirty = true;
        }
        for (Layer* const layer : layers()) {
            if (layer->session != nullptr) layer->bindings().synchronize();
        }
        frame_time = now();
        // Every layer reads one window clock, framed or not.
        for (Layer* const layer : layers()) {
            if (layer->session != nullptr) layer->session->set_time(frame_time);
        }
        for (Layer* const layer : layers()) advance_async(*layer);
        const auto tooling_update_started = std::chrono::steady_clock::now();
        if (debug_enabled && (debug_snapshot_dirty || frame_time >= next_debug_snapshot)) {
            debug_snapshot_dirty = false;
            publish(debug, "profiler.desktop", debug_json());
            next_debug_snapshot = frame_time + 100'000'000;
        }
        if (performance_hud_enabled &&
            (performance_snapshot_dirty || frame_time >= next_performance_snapshot)) {
            performance_snapshot_dirty = false;
            publish(performance_hud, "performance.desktop", performance_json());
            next_performance_snapshot = frame_time + 100'000'000;
        }
        const std::int64_t tooling_update_nanos =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - tooling_update_started
            ).count();
        tooling_nanos += tooling_update_nanos;
        renderer.begin_frame();
        bool had_draws = false;
        LayerFrame active;
        LayerFrame showcase_frame;
        LayerFrame tuning_frame;
        LayerFrame debug_frame;
        LayerFrame performance_frame;
        DesktopFrameSample sample;
        for (Layer* const layer : layers()) {
            if (!shown(*layer) || !layer->live()) continue;
            const LayerFrame result =
                frame_layer(*layer, layer == interactive ? &sample : nullptr);
            had_draws = result.drew || had_draws;
            if (layer == interactive) active = result;
            if (layer == &showcase) {
                showcase_frame = result;
                if (rendered_frames == 0U) {
                    const std::string canonical_frame = showcase.surface().frame_json();
                    first_render_fingerprint = fingerprint(std::span(canonical_frame));
                    first_frame_contains_instance =
                        canonical_frame.find(instance_label) != std::string::npos;
                }
                if (!showcase_enabled &&
                    frame_time - showcase_hidden_at >= showcase_transition_nanos) {
                    showcase_closing = false;
                }
            } else if (layer == &tuning_panel) {
                tuning_frame = result;
            } else if (layer == &debug) {
                debug_frame = result;
                tooling_nanos += result.core_nanos + result.submit_nanos;
            } else if (layer == &performance_hud) {
                performance_frame = result;
                tooling_nanos += result.core_nanos + result.submit_nanos;
            }
        }
        const auto present_started = std::chrono::steady_clock::now();
        const bool presented = renderer.end_frame();
        deliver_platform_capture();
        const std::int64_t present_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - present_started
        ).count();
        const std::int64_t total_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - host_frame_started
        ).count();
        host_total_timings.add(total_nanos);
        frame_history.push_back(static_cast<double>(total_nanos) / 1'000'000.0);
        while (frame_history.size() > 48U) frame_history.pop_front();
        showcase_core_timings.add(showcase_frame.core_nanos);
        showcase_submit_timings.add(showcase_frame.submit_nanos);
        tuning_core_timings.add(tuning_frame.core_nanos);
        tuning_submit_timings.add(tuning_frame.submit_nanos);
        tooling_timings.add(tooling_nanos);
        present_timings.add(present_nanos);
        tooling_update_timings.add(tooling_update_nanos);
        debug_core_timings.add(debug_frame.core_nanos);
        debug_submit_timings.add(debug_frame.submit_nanos);
        performance_core_timings.add(performance_frame.core_nanos);
        performance_submit_timings.add(performance_frame.submit_nanos);
        if (profile_sampling_enabled && frame_time >= next_profile_capture) {
            capture_profile_snapshot();
            next_profile_capture = frame_time + 100'000'000;
        }
        record_frame_sample(
            now(),
            total_nanos,
            active.core_nanos
        );
        if (rendered_frames == 0U && !showcase_enabled && debug.live()) {
            const std::string canonical_frame = debug.surface().frame_json();
            first_render_fingerprint = fingerprint(std::span(canonical_frame));
            first_frame_contains_instance = true;
        }
        sample.total_nanos = total_nanos;
        sample.core_nanos = active.core_nanos;
        sample.submit_nanos = active.submit_nanos;
        sample.tooling_nanos = tooling_nanos;
        sample.present_nanos = present_nanos;
        sample.had_draws = had_draws;
        sample.presented = presented;
        last_performance_sample = sample;
        ++rendered_frames;
        last_frame_had_draws = had_draws;
    }

    HWND window = nullptr;
    std::filesystem::path resource_root;
    HostServices services;
    PlatformServices platform;
    Renderer renderer;
    std::string instance_label;
    /** Shared with the other windows this one opened, or that opened it. */
    std::shared_ptr<ShowcaseModel> shared_model;
    ShowcaseModel& showcase_model;
    /** Empty for the first window; else the id window.open gave this one. */
    std::string window_id;
    bool vsync = true;
    /** A node capture waits for this frame; a capture the host asked for may share it. */
    bool platform_capture = false;
    bool host_capture = false;
    std::optional<FrameCapture> held_capture;
    desktop::DurableWriter durable_writer{&report_durable_failure};
    std::string durable_read_buffer;
    Layer showcase;
    Layer control_deck;
    Layer tuning_panel;
    Layer effect_gallery;
    Layer settings;
    Layer debug;
    Layer performance_hud;
    Layer hub;
    std::deque<FrameBucket> frame_buckets;
    std::deque<ProfileCapture> profile_history;
    TimingWindow host_total_timings;
    TimingWindow showcase_core_timings;
    TimingWindow showcase_submit_timings;
    TimingWindow tuning_core_timings;
    TimingWindow tuning_submit_timings;
    TimingWindow tooling_timings;
    TimingWindow present_timings;
    TimingWindow tooling_update_timings;
    TimingWindow debug_core_timings;
    TimingWindow debug_submit_timings;
    TimingWindow performance_core_timings;
    TimingWindow performance_submit_timings;
    TimingWindow overlay_setup_timings;
    DesktopFrameSample last_performance_sample;
    std::optional<strata_input_event> pending_pointer_move;
    contracts::debug_overlay::StrataDebugSelectModeActionMode debug_mode =
        contracts::debug_overlay::StrataDebugSelectModeActionMode::diagnostics;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
    std::int64_t frame_time = 0;
    std::int64_t last_total_nanos = 0;
    std::int64_t last_native_nanos = 0;
    std::int64_t next_debug_snapshot = 0;
    std::int64_t next_performance_snapshot = 0;
    std::int64_t next_profile_capture = 0;
    std::int64_t showcase_hidden_at = 0;
    std::uint64_t environment_generation = 1U;
    std::uint64_t rendered_frames = 0U;
    std::uint64_t settings_save_count = 0U;
    strata::host::Revision settings_revision;
    std::uint64_t first_render_fingerprint = 0U;
    std::uint32_t framebuffer_width = 1U;
    std::uint32_t framebuffer_height = 1U;
    double display_scale = 1.0;
    bool showcase_enabled = false;
    bool showcase_closing = false;
    bool control_deck_enabled = false;
    bool tuning_panel_enabled = false;
    bool effect_gallery_enabled = false;
    bool settings_enabled = false;
    bool debug_enabled = false;
    bool performance_hud_enabled = true;
    bool profile_sampling_enabled = true;
    bool hub_enabled = false;
    std::deque<double> frame_history;
    bool debug_snapshot_dirty = true;
    bool performance_snapshot_dirty = true;
    bool clear_diagnostics_pending = false;
    bool last_frame_had_draws = false;
    bool first_frame_contains_instance = false;
    static constexpr std::int64_t showcase_transition_nanos = 180'000'000;
    static constexpr std::array debug_modes{
        contracts::debug_overlay::StrataDebugSelectModeActionMode::diagnostics,
        contracts::debug_overlay::StrataDebugSelectModeActionMode::summary,
        contracts::debug_overlay::StrataDebugSelectModeActionMode::motion,
        contracts::debug_overlay::StrataDebugSelectModeActionMode::semantics,
        contracts::debug_overlay::StrataDebugSelectModeActionMode::hot_tree,
        contracts::debug_overlay::StrataDebugSelectModeActionMode::spikes,
    };
};

Host::Host(
    HWND window,
    std::filesystem::path resource_root,
    std::string instance_label,
    const HostOptions options
)
    : impl_(std::make_unique<Impl>(
          window, std::move(resource_root), std::move(instance_label), options
      )) {}
Host::Host(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Host::~Host() = default;

std::vector<WindowRequest> Host::take_window_requests() {
    return impl_->platform.take_window_requests();
}

void Host::answer_window_request(const WindowRequest& request, std::string json) {
    impl_->platform.answer(request.client, request.request, std::move(json));
}

std::unique_ptr<Host> Host::open_window(HWND window, const WindowRequest& request) {
    return std::unique_ptr<Host>(new Host(std::make_unique<Impl>(
        window, impl_->resource_root, impl_->instance_label,
        HostOptions{.vsync = impl_->vsync, .performance_hud = false, .profile_sampling = false},
        impl_->shared_model, request.id, request.root)));
}

void Host::resize(
    const std::uint32_t framebuffer_width,
    const std::uint32_t framebuffer_height,
    const double scale
) {
    impl_->resize(framebuffer_width, framebuffer_height, scale);
}

std::optional<std::uint32_t> Host::pointer_cursor() noexcept {
    Impl::Layer* const layer = impl_->interactive_layer();
    if (layer == nullptr || !layer->live())
        return std::nullopt;
    std::uint32_t cursor = STRATA_POINTER_CURSOR_DEFAULT;
    if (strata_surface_pointer_cursor(layer->surface().native_handle(), &cursor).status !=
        STRATA_STATUS_OK) {
        return std::nullopt;
    }
    return cursor;
}

void Host::pointer(
    const std::uint32_t kind,
    const std::int32_t button,
    const double x,
    const double y
) {
    strata_input_event event{};
    event.kind = kind;
    event.pointer_id = 0;
    event.button = button;
    event.x = x / impl_->display_scale;
    event.y = y / impl_->display_scale;
    // Materials follow the pointer on the GPU (materialPointer), with no frame of the interface.
    impl_->renderer.set_pointer(kind == STRATA_INPUT_POINTER_CANCEL
                                    ? std::nullopt
                                    : std::optional(std::array<double, 2U>{x, y}));
    if (kind == STRATA_INPUT_POINTER_MOVE) {
        impl_->pending_pointer_move = event;
        return;
    }
    impl_->flush_pointer_move();
    impl_->enqueue(event);
}

void Host::scroll(
    const double x,
    const double y,
    const double delta_x,
    const double delta_y
) {
    impl_->flush_pointer_move();
    strata_input_event event{};
    event.kind = STRATA_INPUT_SCROLL;
    event.x = x / impl_->display_scale;
    event.y = y / impl_->display_scale;
    event.delta_x = delta_x;
    event.delta_y = delta_y;
    impl_->enqueue(event);
}

void Host::key(const std::uint32_t virtual_key, const std::uint32_t action) {
    impl_->flush_pointer_move();
    if (virtual_key == VK_F2 || virtual_key == VK_F3 || virtual_key == VK_F4 ||
        (virtual_key >= VK_F6 && virtual_key <= VK_F10)) {
        if (action == STRATA_KEY_PRESS) {
            switch (virtual_key) {
            case VK_F2: impl_->toggle_control_deck(); break;
            case VK_F3: impl_->toggle_effect_gallery(); break;
            case VK_F4: impl_->toggle_tuning_panel(); break;
            case VK_F6: impl_->toggle_settings(); break;
            case VK_F7: impl_->toggle_showcase(); break;
            case VK_F8: impl_->cycle_debug((GetKeyState(VK_SHIFT) & 0x8000) != 0); break;
            case VK_F9: impl_->toggle_performance_hud(); break;
            case VK_F10: impl_->toggle_hub(); break;
            default: break;
            }
        }
        return;
    }
    const std::string name = strata::win32::key_name(virtual_key);
    strata_input_event event{};
    event.kind = STRATA_INPUT_KEY;
    event.key_action = action;
    impl_->enqueue(event, name);
}

void Host::text(std::string utf8) {
    impl_->flush_pointer_move();
    strata_input_event event{};
    event.kind = STRATA_INPUT_TEXT;
    impl_->enqueue(event, utf8);
}

void Host::ime_preedit(
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

void Host::cancel_interactions() noexcept {
    impl_->pending_pointer_move.reset();
    for (Impl::Layer* const layer : impl_->layers()) {
        if (layer->live()) {
            static_cast<void>(strata_surface_cancel_interactions(
                layer->surface().native_handle()
            ));
        }
    }
}

void Host::frame() { impl_->frame(); }

void Host::files(const std::uint32_t kind, const double x, const double y,
                 const std::string& paths) {
    impl_->files(kind, x, y, paths);
}

std::optional<std::intptr_t> Host::window_message(const std::uint32_t message,
                                                  const std::uintptr_t word,
                                                  const std::intptr_t long_value) {
    return impl_->platform.window_message(message, word, long_value);
}
double Host::scale() const noexcept { return impl_->display_scale; }

const DesktopFrameSample& Host::last_frame_sample() const noexcept {
    return impl_->last_performance_sample;
}

void Host::capture_next_frame() noexcept {
    impl_->host_capture = true;
    impl_->renderer.capture_next_frame();
}

std::optional<FrameCapture> Host::take_capture() {
    impl_->host_capture = false;
    if (impl_->held_capture.has_value())
        return std::exchange(impl_->held_capture, std::nullopt);
    return impl_->renderer.take_capture();
}

DesktopHostInfo Host::performance_info() const {
    const RendererInfo renderer = impl_->renderer.info();
    return DesktopHostInfo{
        renderer.adapter,
        renderer.driver_version,
        renderer.vendor_id,
        renderer.device_id,
        renderer.dedicated_video_memory,
        impl_->framebuffer_width,
        impl_->framebuffer_height,
        impl_->logical_width(),
        impl_->logical_height(),
        impl_->display_scale,
        renderer.vsync,
    };
}

std::string Host::performance_frame_json() {
    Impl::Layer* const layer = impl_->interactive_layer();
    if (layer == nullptr || !layer->live()) {
        throw std::logic_error(
            "desktop performance selectors require an open interactive surface"
        );
    }
    return layer->surface().frame_json();
}

bool Host::smoke_ready() const noexcept {
    return impl_->rendered_frames >= 2U && impl_->last_frame_had_draws;
}

std::uint64_t Host::smoke_fingerprint() const noexcept {
    return impl_->first_render_fingerprint;
}

bool Host::smoke_identity_bound() const noexcept {
    return impl_->first_frame_contains_instance;
}

void Host::reset_profile() {
    impl_->profile_history.clear();
    impl_->host_total_timings.samples.clear();
    impl_->showcase_core_timings.samples.clear();
    impl_->showcase_submit_timings.samples.clear();
    impl_->tuning_core_timings.samples.clear();
    impl_->tuning_submit_timings.samples.clear();
    impl_->tooling_timings.samples.clear();
    impl_->present_timings.samples.clear();
    impl_->tooling_update_timings.samples.clear();
    impl_->debug_core_timings.samples.clear();
    impl_->debug_submit_timings.samples.clear();
    impl_->performance_core_timings.samples.clear();
    impl_->performance_submit_timings.samples.clear();
    impl_->overlay_setup_timings.samples.clear();
    impl_->next_profile_capture = 0;
}

std::string Host::profile_report() {
    impl_->capture_profile_snapshot();
    return impl_->debug_export_text();
}

} // namespace strata::desktop
