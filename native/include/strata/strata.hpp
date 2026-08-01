#pragma once

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <strata/strata.h>

namespace strata {

namespace host {
class Bindings;
}

class AbiError final : public std::runtime_error {
public:
    AbiError(const strata_result result, const std::string_view operation)
        : std::runtime_error(
              std::string(operation) + " failed with Strata status " +
              std::to_string(result.status)
          ),
          status_(result.status),
          diagnostic_id_(result.diagnostic_id) {}

    [[nodiscard]] strata_status status() const noexcept { return status_; }
    [[nodiscard]] std::uint64_t diagnostic_id() const noexcept { return diagnostic_id_; }

private:
    strata_status status_;
    std::uint64_t diagnostic_id_;
};

inline void require_ok(const strata_result result, const std::string_view operation) {
    if (result.status != STRATA_STATUS_OK) throw AbiError(result, operation);
}

[[nodiscard]] inline strata_scale_policy_config scale_policy_defaults(
    const strata_scale_policy_kind kind
) {
    strata_scale_policy_config policy{sizeof(strata_scale_policy_config)};
    require_ok(strata_scale_policy_defaults(kind, &policy), "scale policy defaults");
    return policy;
}

[[nodiscard]] inline strata_scale_context resolve_scale_context(
    const strata_scale_policy_config& policy,
    const std::int64_t framebuffer_width,
    const std::int64_t framebuffer_height
) {
    strata_scale_context context{sizeof(strata_scale_context)};
    require_ok(
        strata_resolve_scale_context(
            &policy,
            framebuffer_width,
            framebuffer_height,
            &context
        ),
        "scale context resolution"
    );
    return context;
}

[[nodiscard]] inline strata_theme_tokens theme_tokens_defaults() {
    strata_theme_tokens tokens{sizeof(strata_theme_tokens)};
    require_ok(strata_theme_tokens_defaults(&tokens), "theme token defaults");
    return tokens;
}

[[nodiscard]] inline strata_theme_visual_style theme_visual_style_defaults() {
    strata_theme_visual_style style{};
    style.struct_size = sizeof(style);
    require_ok(strata_theme_visual_style_defaults(&style), "theme visual-style defaults");
    return style;
}

[[nodiscard]] inline strata_theme_text_visual_style theme_text_visual_style_defaults() {
    strata_theme_text_visual_style style{};
    style.struct_size = sizeof(style);
    require_ok(strata_theme_text_visual_style_defaults(&style), "theme text-visual defaults");
    return style;
}

[[nodiscard]] inline strata_theme_text_layout_style theme_text_layout_style_defaults() {
    strata_theme_text_layout_style style{};
    style.struct_size = sizeof(style);
    require_ok(strata_theme_text_layout_style_defaults(&style), "theme text-layout defaults");
    return style;
}

[[nodiscard]] inline strata_theme_layout_style theme_layout_style_defaults() {
    strata_theme_layout_style style{};
    style.struct_size = sizeof(style);
    require_ok(strata_theme_layout_style_defaults(&style), "theme layout defaults");
    return style;
}

[[nodiscard]] inline strata_theme_layout_size theme_layout_size(
    const strata_theme_layout_size_kind kind = STRATA_THEME_SIZE_AUTO,
    const double value = 0.0
) noexcept {
    strata_theme_layout_size result{};
    result.struct_size = sizeof(result);
    result.kind = kind;
    result.value = value;
    return result;
}

[[nodiscard]] inline strata_theme_layout_size theme_layout_clamp(
    const strata_theme_layout_size* minimum,
    const strata_theme_layout_size* preferred,
    const strata_theme_layout_size* maximum
) noexcept {
    strata_theme_layout_size result = theme_layout_size(STRATA_THEME_SIZE_CLAMP);
    result.minimum = minimum;
    result.preferred = preferred;
    result.maximum = maximum;
    return result;
}

[[nodiscard]] inline strata_theme_animation_set theme_animation_set_defaults() {
    strata_theme_animation_set set{};
    set.struct_size = sizeof(set);
    require_ok(strata_theme_animation_set_defaults(&set), "theme animation-set defaults");
    return set;
}

[[nodiscard]] inline strata_theme_motion_easing theme_motion_easing(
    const strata_theme_motion_easing_kind kind = STRATA_THEME_MOTION_EASING_LINEAR
) noexcept {
    strata_theme_motion_easing result{};
    result.struct_size = sizeof(result);
    result.kind = kind;
    result.x2 = 1.0;
    result.y2 = 1.0;
    return result;
}

[[nodiscard]] inline strata_theme_motion_easing theme_cubic_bezier(
    const double x1,
    const double y1,
    const double x2,
    const double y2
) noexcept {
    strata_theme_motion_easing result = theme_motion_easing(
        STRATA_THEME_MOTION_EASING_CUBIC_BEZIER
    );
    result.x1 = x1;
    result.y1 = y1;
    result.x2 = x2;
    result.y2 = y2;
    return result;
}

[[nodiscard]] inline strata_theme_motion_timing theme_motion_timing(
    const strata_string_view name,
    const std::int64_t duration_nanoseconds
) noexcept {
    strata_theme_motion_timing result{};
    result.struct_size = sizeof(result);
    result.name = name;
    result.duration_nanoseconds = duration_nanoseconds;
    result.easing = theme_motion_easing();
    result.repeat_kind = STRATA_THEME_MOTION_REPEAT_NONE;
    result.fill_mode = STRATA_THEME_MOTION_FILL_BOTH;
    return result;
}

[[nodiscard]] inline strata_theme_animation_spec theme_named_animation(
    const strata_string_view name
) noexcept {
    return strata_theme_animation_spec{STRATA_THEME_ANIMATION_NAMED, 0U, name, nullptr};
}

[[nodiscard]] inline strata_theme_animation_spec theme_inline_animation(
    const strata_theme_declared_animation* animation
) noexcept {
    return strata_theme_animation_spec{
        STRATA_THEME_ANIMATION_INLINE, 0U, strata_string_view{}, animation
    };
}

class Surface;

namespace detail {

struct RuntimeControl final {
    explicit RuntimeControl(strata_runtime* value) noexcept : value(value) {}
    ~RuntimeControl() {
        if (value != nullptr && strata_runtime_release(value).status != STRATA_STATUS_OK) {
            std::terminate();
        }
    }
    RuntimeControl(const RuntimeControl&) = delete;
    RuntimeControl& operator=(const RuntimeControl&) = delete;
    strata_runtime* value;
    std::mutex host_snapshot_mutex;
    std::uint64_t next_host_snapshot_generation = 1U;
};

[[nodiscard]] inline std::uint64_t publish_host_snapshot(
    const std::shared_ptr<RuntimeControl>& control,
    const std::string_view id,
    const std::string_view value_json
) {
    if (control == nullptr || control->value == nullptr) {
        throw std::logic_error("host snapshot publication requires a live runtime");
    }
    const std::scoped_lock lock(control->host_snapshot_mutex);
    std::uint64_t retained_generation = 0U;
    const strata_result retained = strata_runtime_get_host_snapshot_generation(
        control->value,
        strata_string_view{id.data(), id.size()},
        &retained_generation
    );
    if (retained.status != STRATA_STATUS_NOT_FOUND) {
        require_ok(retained, "host snapshot generation read");
        if (retained_generation == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("host snapshot generation exhausted");
        }
        control->next_host_snapshot_generation = std::max(
            control->next_host_snapshot_generation,
            retained_generation + 1U
        );
    }
    const std::uint64_t generation = control->next_host_snapshot_generation;
    if (generation == 0U || generation == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("host snapshot generation exhausted");
    }
    const strata_host_snapshot_config snapshot{
        sizeof(strata_host_snapshot_config),
        strata_string_view{id.data(), id.size()},
        generation,
        strata_string_view{value_json.data(), value_json.size()},
    };
    require_ok(
        strata_runtime_publish_host_snapshot(control->value, &snapshot),
        "host snapshot publication"
    );
    control->next_host_snapshot_generation = generation + 1U;
    return generation;
}

struct ByteCapture final {
    std::vector<std::uint8_t> value;
    bool failed = false;
};

inline void capture_bytes(void* const user_data, const strata_bytes_view bytes) noexcept {
    auto& capture = *static_cast<ByteCapture*>(user_data);
    try {
        if (bytes.size == 0U) capture.value.clear();
        else capture.value.assign(bytes.data, bytes.data + bytes.size);
    } catch (...) {
        capture.failed = true;
    }
}

struct StringCapture final {
    std::string value;
    bool failed = false;
};

inline void capture_string(void* const user_data, const strata_string_view text) noexcept {
    auto& capture = *static_cast<StringCapture*>(user_data);
    try {
        if (text.size == 0U) capture.value.clear();
        else capture.value.assign(text.data, text.size);
    } catch (...) {
        capture.failed = true;
    }
}

} // namespace detail

class Runtime final {
public:
    explicit Runtime(const strata_runtime_config& config) {
        strata_runtime* value = nullptr;
        require_ok(strata_runtime_create(&config, &value), "runtime creation");
        try {
            control_ = std::make_shared<detail::RuntimeControl>(value);
        } catch (...) {
            if (strata_runtime_release(value).status != STRATA_STATUS_OK) std::terminate();
            throw;
        }
    }

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) noexcept = default;
    Runtime& operator=(Runtime&&) noexcept = default;

    [[nodiscard]] strata_runtime* native_handle() const noexcept {
        return control_ != nullptr ? control_->value : nullptr;
    }

    [[nodiscard]] strata_runtime_memory_info memory_info() const {
        strata_runtime_memory_info info{sizeof(strata_runtime_memory_info)};
        require_ok(
            strata_runtime_get_memory_info(native_handle(), &info),
            "runtime memory telemetry"
        );
        return info;
    }

    /** The materials this application declares a source for on one backend. */
    [[nodiscard]] std::vector<strata_material_declaration> material_declarations(
        const std::string_view backend
    ) const {
        const strata_string_view backend_view{backend.data(), backend.size()};
        std::size_t count = 0U;
        require_ok(
            strata_runtime_read_material_declarations(
                native_handle(), backend_view, nullptr, 0U, &count
            ),
            "material declaration count"
        );
        std::vector<strata_material_declaration> declarations(count);
        if (count == 0U) return declarations;
        require_ok(
            strata_runtime_read_material_declarations(
                native_handle(), backend_view, declarations.data(), declarations.size(), &count
            ),
            "material declaration read"
        );
        declarations.resize(count);
        return declarations;
    }

    void configure_application(const strata_application_config& config) {
        require_ok(
            strata_runtime_configure_application(native_handle(), &config),
            "application configuration"
        );
    }

    void set_durable_store(const strata_durable_store_adapter& adapter) {
        require_ok(
            strata_runtime_set_durable_store_adapter(native_handle(), &adapter),
            "durable store installation"
        );
    }

    void set_async_host(const strata_async_host_adapter& adapter) {
        require_ok(
            strata_runtime_set_async_host_adapter(native_handle(), &adapter),
            "async host installation"
        );
    }

    [[nodiscard]] std::optional<std::string> durable_shell_value_json(
        const std::string_view key
    ) const {
        detail::StringCapture capture;
        const strata_value_json_sink sink{
            sizeof(strata_value_json_sink), &capture, &detail::capture_string,
        };
        const strata_result result = strata_runtime_read_durable_shell_value_json(
            native_handle(), strata_string_view{key.data(), key.size()}, &sink
        );
        if (result.status == STRATA_STATUS_NOT_FOUND) return std::nullopt;
        require_ok(result, "durable shell value read");
        if (capture.failed) throw std::bad_alloc();
        return std::move(capture.value);
    }

    void write_durable_shell_value_json(
        const std::string_view key,
        const std::string_view value_json
    ) {
        require_ok(
            strata_runtime_write_durable_shell_value_json(
                native_handle(),
                strata_string_view{key.data(), key.size()},
                strata_string_view{value_json.data(), value_json.size()}
            ),
            "durable shell value write"
        );
    }

    void flush_durable_state() {
        require_ok(strata_runtime_flush_durable_state(native_handle()), "durable state flush");
    }

    void async_progress(const std::uint64_t request_id, const strata_async_progress& progress) {
        require_ok(
            strata_runtime_async_progress(native_handle(), request_id, &progress),
            "async progress publication"
        );
    }

    void async_succeed(const std::uint64_t request_id, const std::string_view value_json) {
        require_ok(
            strata_runtime_async_succeed_json(
                native_handle(), request_id,
                strata_string_view{value_json.data(), value_json.size()}
            ),
            "async success publication"
        );
    }

    void async_fail(
        const std::uint64_t request_id,
        const std::string_view message,
        const std::string_view code = {}
    ) {
        require_ok(
            strata_runtime_async_fail(
                native_handle(), request_id,
                strata_string_view{message.data(), message.size()},
                strata_string_view{code.data(), code.size()}
            ),
            "async failure publication"
        );
    }

    /** Publishes one immutable host root with a runtime-owned monotonic generation. */
    [[nodiscard]] std::uint64_t publish_host_snapshot(
        const std::string_view id,
        const std::string_view value_json
    ) {
        return detail::publish_host_snapshot(control_, id, value_json);
    }

    [[nodiscard]] strata_activation_info activate(const strata_activation_config& config) {
        strata_activation_info info{sizeof(strata_activation_info)};
        require_ok(
            strata_runtime_compile_and_activate(native_handle(), &config, &info),
            "application activation"
        );
        return info;
    }

    [[nodiscard]] strata_activation_info activate(
        const strata_compiled_activation_config& config
    ) {
        strata_activation_info info{sizeof(strata_activation_info)};
        require_ok(
            strata_runtime_activate_compiled_module(native_handle(), &config, &info),
            "compiled application activation"
        );
        return info;
    }

    [[nodiscard]] Surface create_surface(const strata_surface_config& config) const;

    /** Explicitly releases an empty runtime; live Surfaces make this throw and remain recoverable. */
    void close() {
        if (control_ == nullptr || control_->value == nullptr) return;
        require_ok(strata_runtime_release(control_->value), "runtime release");
        control_->value = nullptr;
        control_.reset();
    }

private:
    friend class host::Bindings;
    std::shared_ptr<detail::RuntimeControl> control_;
};

class Surface final {
public:
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    Surface(Surface&& other) noexcept
        : owner_(std::move(other.owner_)), value_(std::exchange(other.value_, nullptr)) {}

    Surface& operator=(Surface&& other) noexcept {
        if (this == &other) return *this;
        if (value_ != nullptr) std::terminate();
        owner_ = std::move(other.owner_);
        value_ = std::exchange(other.value_, nullptr);
        return *this;
    }

    /** A live Surface must be closed through the acknowledged packet barrier or abandoned. */
    ~Surface() {
        if (value_ != nullptr) std::terminate();
    }

    [[nodiscard]] strata_surface* native_handle() const noexcept { return value_; }

    [[nodiscard]] strata_surface_frame_info frame(const std::int64_t time_nanoseconds) {
        strata_surface_frame_info info{sizeof(strata_surface_frame_info)};
        require_ok(strata_surface_frame(value_, time_nanoseconds, &info), "surface frame");
        return info;
    }

    [[nodiscard]] std::vector<std::uint8_t> render_packet() const {
        detail::ByteCapture capture;
        const strata_bytes_sink sink{
            sizeof(strata_bytes_sink), &capture, &detail::capture_bytes,
        };
        require_ok(strata_surface_read_render_packet(value_, &sink), "render packet read");
        if (capture.failed) throw std::bad_alloc();
        return std::move(capture.value);
    }

    /**
     * Enters terminal two-phase teardown and copies the resource-only release packet. Submit the
     * packet while the host GPU owner is alive, then acknowledge_release_packet() and close().
     * Copying this packet does not acknowledge host consumption. Repeated calls are idempotent.
     */
    [[nodiscard]] std::vector<std::uint8_t> prepare_release_packet() {
        detail::ByteCapture capture;
        const strata_bytes_sink sink{
            sizeof(strata_bytes_sink), &capture, &detail::capture_bytes,
        };
        require_ok(
            strata_surface_prepare_release_packet(value_, &sink),
            "surface release packet preparation"
        );
        if (capture.failed) throw std::bad_alloc();
        return std::move(capture.value);
    }

    void acknowledge_release_packet() {
        require_ok(
            strata_surface_acknowledge_release_packet(value_),
            "surface release packet acknowledgement"
        );
    }

    [[nodiscard]] std::string frame_json() const {
        detail::StringCapture capture;
        const strata_value_json_sink sink{
            sizeof(strata_value_json_sink), &capture, &detail::capture_string,
        };
        require_ok(strata_surface_read_frame_json(value_, &sink), "frame JSON read");
        if (capture.failed) throw std::bad_alloc();
        return std::move(capture.value);
    }

    void reload_resources() {
        require_ok(strata_surface_reload_resources(value_), "surface resource reload");
    }

    [[nodiscard]] bool register_theme(const strata_theme& theme) {
        std::uint32_t changed = 0U;
        require_ok(
            strata_surface_register_theme(value_, &theme, &changed),
            "surface theme registration"
        );
        return changed != 0U;
    }

    [[nodiscard]] bool set_theme(const strata_theme& theme) {
        std::uint32_t changed = 0U;
        require_ok(
            strata_surface_set_theme(value_, &theme, &changed),
            "surface root theme selection"
        );
        return changed != 0U;
    }

    [[nodiscard]] bool unregister_theme(const std::string_view name) {
        std::uint32_t removed = 0U;
        require_ok(
            strata_surface_unregister_theme(
                value_, strata_string_view{name.data(), name.size()}, &removed
            ),
            "surface theme removal"
        );
        return removed != 0U;
    }

    [[nodiscard]] bool set_scoped_theme(
        const std::string_view node_key,
        const strata_theme& theme
    ) {
        std::uint32_t changed = 0U;
        require_ok(
            strata_surface_set_scoped_theme(
                value_,
                strata_string_view{node_key.data(), node_key.size()},
                &theme,
                &changed
            ),
            "surface scoped theme mutation"
        );
        return changed != 0U;
    }

    [[nodiscard]] bool clear_scoped_theme(const std::string_view node_key) {
        std::uint32_t removed = 0U;
        require_ok(
            strata_surface_clear_scoped_theme(
                value_, strata_string_view{node_key.data(), node_key.size()}, &removed
            ),
            "surface scoped theme removal"
        );
        return removed != 0U;
    }

    [[nodiscard]] bool animate_scroll_to(const strata_scroll_animation_request& request) {
        std::uint32_t started = 0U;
        require_ok(
            strata_surface_animate_scroll_to(value_, &request, &started),
            "surface scroll animation"
        );
        return started != 0U;
    }

    [[nodiscard]] bool animate_scroll_to(
        const std::string_view key,
        const std::optional<double> x,
        const std::optional<double> y,
        const std::string_view timing = "standard",
        const std::optional<std::int64_t> duration_nanoseconds = std::nullopt
    ) {
        const strata_scroll_animation_request request{
            sizeof(strata_scroll_animation_request),
            strata_string_view{key.data(), key.size()},
            x.has_value() ? 1U : 0U,
            y.has_value() ? 1U : 0U,
            x.value_or(0.0),
            y.value_or(0.0),
            strata_string_view{timing.data(), timing.size()},
            duration_nanoseconds.has_value() ? 1U : 0U,
            0U,
            duration_nanoseconds.value_or(0),
        };
        return animate_scroll_to(request);
    }

    void close() {
        if (value_ == nullptr) return;
        require_ok(strata_surface_release(value_), "surface release");
        value_ = nullptr;
        owner_.reset();
    }

    /** Explicitly bypasses the packet barrier when delivery is impossible. */
    void abandon() {
        if (value_ == nullptr) return;
        require_ok(strata_surface_abandon(value_), "surface abandon");
        value_ = nullptr;
        owner_.reset();
    }

private:
    friend class Runtime;
    Surface(std::shared_ptr<detail::RuntimeControl> owner, strata_surface* value) noexcept
        : owner_(std::move(owner)), value_(value) {}

    std::shared_ptr<detail::RuntimeControl> owner_;
    strata_surface* value_ = nullptr;
};

inline Surface Runtime::create_surface(const strata_surface_config& config) const {
    strata_surface* value = nullptr;
    require_ok(
        strata_runtime_create_surface(native_handle(), &config, &value),
        "surface creation"
    );
    return Surface(control_, value);
}

[[nodiscard]] constexpr strata_string_view view(const std::string_view value) noexcept {
    return strata_string_view{value.data(), value.size()};
}

} // namespace strata
