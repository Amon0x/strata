#include <strata/strata.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "abi_internal.hpp"
#include "abi_extension.hpp"
#include "abi_support.hpp"
#include "core/diagnostics.hpp"
#include "core/utf8.hpp"
#include "data/json.hpp"
#include "font/opentype.hpp"
#include "resource/image.hpp"
#include "resource/resource.hpp"
#include "ui/frame_snapshot.hpp"
#include "ui/input.hpp"
#include "ui/layout.hpp"
#include "ui/render/packet.hpp"
#include "ui/surface.hpp"

namespace {

class HostServiceError final : public std::runtime_error {
public:
    HostServiceError(const strata_status status, std::string message)
        : std::runtime_error(std::move(message)), status_(status) {}

    [[nodiscard]] strata_status status() const noexcept { return status_; }

private:
    strata_status status_;
};

[[nodiscard]] bool valid_view(strata_string_view value, bool allow_empty) noexcept;
[[nodiscard]] std::string copied_string(strata_string_view value);

/** One immutable adapter selection used for an entire candidate resource transaction. */
class ResourceAdapterTransaction final {
public:
    explicit ResourceAdapterTransaction(strata_runtime& runtime)
        : runtime_(runtime), adapter_(runtime.resources) {}

    [[nodiscard]] std::uint64_t generation() const noexcept {
        return adapter_.has_value() ? adapter_->generation : 0U;
    }

    void verify_current() const {
        const auto& current = runtime_.resources;
        const bool same = current.has_value() == adapter_.has_value() &&
            (!adapter_.has_value() ||
             (current->generation == adapter_->generation &&
              current->struct_size == adapter_->struct_size &&
              current->load == adapter_->load &&
              current->user_data == adapter_->user_data));
        if (!same) {
            throw HostServiceError(
                STRATA_STATUS_SERVICE_UNAVAILABLE,
                "The resource adapter changed during Surface candidate construction; retry against the newer generation."
            );
        }
    }

    [[nodiscard]] strata::resource::ResourceBytes load(const strata_string_view resource_id) const {
        if (!valid_view(resource_id, false)) {
            throw std::invalid_argument("resource id view is invalid");
        }
        const std::string id = copied_string(resource_id);
        if (!strata::core::valid_utf8(id)) {
            throw std::invalid_argument("resource id must be valid UTF-8");
        }
        verify_current();
        if (!adapter_.has_value()) {
            throw HostServiceError(
                STRATA_STATUS_SERVICE_UNAVAILABLE,
                "Surface resource loading requires a configured resource adapter."
            );
        }
        strata_bytes_view bytes{};
        const strata_status status = adapter_->load(
            adapter_->user_data,
            strata_string_view{id.data(), id.size()},
            &bytes
        );
        // A callback may reentrantly install a newer adapter. Its borrowed bytes must never be
        // mixed into a candidate bearing the new generation, even when the callback succeeded.
        verify_current();
        if (status != STRATA_STATUS_OK) {
            throw HostServiceError(status, "The host resource adapter rejected a surface resource.");
        }
        if (bytes.data == nullptr || bytes.size == 0U) {
            throw HostServiceError(
                STRATA_STATUS_SERVICE_UNAVAILABLE,
                "The host resource adapter returned an empty surface resource."
            );
        }
        constexpr std::size_t maximum_surface_resource_bytes = 64U * 1'024U * 1'024U;
        if (bytes.size > maximum_surface_resource_bytes) {
            throw std::invalid_argument("surface resource exceeds the 64 MiB per-resource limit");
        }
        return strata::resource::ResourceBytes(bytes.data, bytes.data + bytes.size);
    }

private:
    strata_runtime& runtime_;
    std::optional<strata_resource_adapter> adapter_;
};

[[nodiscard]] strata_result invalid_argument() noexcept {
    return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
}

[[nodiscard]] strata_result runtime_failure(
    strata_runtime& runtime,
    const strata_status status,
    const char* const code,
    const char* const message
) noexcept {
    return runtime.core.diagnostics().emit(
        status,
        STRATA_DIAGNOSTIC_ERROR,
        code,
        message
    );
}

[[nodiscard]] strata_result surface_failure(
    const strata_surface& surface,
    const strata_status status,
    const char* const code,
    const char* const message
) noexcept {
    return runtime_failure(*surface.owner, status, code, message);
}

[[nodiscard]] strata_result terminal_surface_failure(const strata_surface& surface) noexcept {
    return strata::abi_detail::terminal_surface_failure(surface);
}

[[nodiscard]] bool valid_view(const strata_string_view value, const bool allow_empty) noexcept {
    return (value.data != nullptr || value.size == 0U) && (allow_empty || value.size != 0U);
}

[[nodiscard]] std::string copied_string(const strata_string_view value) {
    return value.size == 0U ? std::string{} : std::string(value.data, value.size);
}

[[nodiscard]] strata::ui::PointSnapPolicy point_snapping(const strata_point_snap_policy value) {
    switch (value) {
    case STRATA_POINT_SNAP_NONE: return strata::ui::PointSnapPolicy::none;
    case STRATA_POINT_SNAP_NEAREST: return strata::ui::PointSnapPolicy::nearest;
    default: throw std::invalid_argument("surface point-snapping policy is invalid");
    }
}

[[nodiscard]] strata::ui::RectangleSnapPolicy rectangle_snapping(
    const strata_rectangle_snap_policy value
) {
    switch (value) {
    case STRATA_RECTANGLE_SNAP_NONE: return strata::ui::RectangleSnapPolicy::none;
    case STRATA_RECTANGLE_SNAP_NEAREST: return strata::ui::RectangleSnapPolicy::nearest;
    case STRATA_RECTANGLE_SNAP_OUTWARD: return strata::ui::RectangleSnapPolicy::outward;
    default: throw std::invalid_argument("surface rectangle-snapping policy is invalid");
    }
}

[[nodiscard]] strata::ui::SurfaceDensity density(const strata_surface_density value) {
    switch (value) {
    case STRATA_SURFACE_DENSITY_COMPACT: return strata::ui::SurfaceDensity::compact;
    case STRATA_SURFACE_DENSITY_COMFORTABLE: return strata::ui::SurfaceDensity::comfortable;
    default: throw std::invalid_argument("surface density is invalid");
    }
}

[[nodiscard]] strata::ui::PointerPrecision pointer_precision(
    const strata_pointer_precision value
) {
    switch (value) {
    case STRATA_POINTER_PRECISION_NONE: return strata::ui::PointerPrecision::none;
    case STRATA_POINTER_PRECISION_COARSE: return strata::ui::PointerPrecision::coarse;
    case STRATA_POINTER_PRECISION_FINE: return strata::ui::PointerPrecision::fine;
    default: throw std::invalid_argument("surface pointer precision is invalid");
    }
}

[[nodiscard]] strata::ui::SurfaceEnvironment environment(
    const strata_surface_environment& value
) {
    constexpr strata_surface_input_capabilities known_input =
        STRATA_SURFACE_INPUT_POINTER |
        STRATA_SURFACE_INPUT_KEYBOARD |
        STRATA_SURFACE_INPUT_TOUCH |
        STRATA_SURFACE_INPUT_IME |
        STRATA_SURFACE_INPUT_CLIPBOARD |
        STRATA_SURFACE_INPUT_CONTROLLER;
    if (value.struct_size < sizeof(strata_surface_environment) || value.generation == 0U ||
        value.reserved != 0U || value.reduced_motion > 1U ||
        (value.input_capabilities & ~known_input) != 0U) {
        throw std::invalid_argument("surface environment header, flags, or generation is invalid");
    }
    const bool has_pointer =
        (value.input_capabilities & STRATA_SURFACE_INPUT_POINTER) != 0U;
    const strata::ui::PointerPrecision precision = pointer_precision(value.pointer_precision);
    if (has_pointer == (precision == strata::ui::PointerPrecision::none)) {
        throw std::invalid_argument(
            "surface pointer capability and pointer precision must describe the same device"
        );
    }
    strata::ui::SurfaceEnvironment result{
        value.generation,
        value.framebuffer_width,
        value.framebuffer_height,
        value.logical_width,
        value.logical_height,
        value.scale,
        strata::ui::Edges{
            value.safe_inset_left,
            value.safe_inset_top,
            value.safe_inset_right,
            value.safe_inset_bottom,
        },
        point_snapping(value.point_snapping),
        rectangle_snapping(value.rectangle_snapping),
        density(value.density),
        value.reduced_motion != 0U,
        strata::ui::SurfaceInputCapabilities{
            has_pointer,
            precision,
            (value.input_capabilities & STRATA_SURFACE_INPUT_KEYBOARD) != 0U,
            (value.input_capabilities & STRATA_SURFACE_INPUT_TOUCH) != 0U,
            (value.input_capabilities & STRATA_SURFACE_INPUT_IME) != 0U,
            (value.input_capabilities & STRATA_SURFACE_INPUT_CLIPBOARD) != 0U,
            (value.input_capabilities & STRATA_SURFACE_INPUT_CONTROLLER) != 0U,
        },
    };
    result.validate();
    return result;
}

[[nodiscard]] strata::runtime::LayerRole root_role(const strata_surface_root_role value) {
    switch (value) {
    case STRATA_SURFACE_ROOT_SCREEN: return strata::runtime::LayerRole::screen;
    case STRATA_SURFACE_ROOT_OVERLAY: return strata::runtime::LayerRole::overlay;
    default: throw std::invalid_argument("surface root role is invalid");
    }
}

[[nodiscard]] strata::resource::TextureSampling texture_sampling(
    const strata_texture_sampling value
) {
    switch (value) {
    case STRATA_TEXTURE_SAMPLING_NEAREST: return strata::resource::TextureSampling::nearest;
    case STRATA_TEXTURE_SAMPLING_LINEAR: return strata::resource::TextureSampling::linear;
    default: throw std::invalid_argument("surface texture sampling is invalid");
    }
}

[[nodiscard]] std::vector<strata_surface_texture_binding> texture_bindings(
    const strata_surface_config& config
) {
    constexpr std::size_t maximum_texture_count = 256U;
    if (config.texture_count == 0U) {
        if (config.textures != nullptr) {
            throw std::invalid_argument("an empty Surface texture table must use a null pointer");
        }
        return {};
    }
    if (config.textures == nullptr || config.texture_count > maximum_texture_count) {
        throw std::invalid_argument("Surface texture table pointer or count is invalid");
    }
    std::vector<strata_surface_texture_binding> result;
    result.reserve(config.texture_count);
    for (std::size_t index = 0U; index < config.texture_count; ++index) {
        const strata_surface_texture_resource& entry = config.textures[index];
        if (!valid_view(entry.id, false) || !valid_view(entry.resource_id, false) ||
            entry.reserved != 0U) {
            throw std::invalid_argument("Surface texture entries are incomplete");
        }
        std::string id = copied_string(entry.id);
        std::string resource_id = copied_string(entry.resource_id);
        if (!strata::core::valid_utf8(id) || !strata::core::valid_utf8(resource_id)) {
            throw std::invalid_argument("Surface texture ids must be valid UTF-8");
        }
        if (std::ranges::any_of(result, [&](const auto& texture) { return texture.id == id; })) {
            throw std::invalid_argument("Surface logical texture ids must be unique");
        }
        static_cast<void>(texture_sampling(entry.sampling));
        result.push_back(strata_surface_texture_binding{
            std::move(id),
            std::move(resource_id),
            entry.sampling,
        });
    }
    return result;
}

[[nodiscard]] std::vector<strata::resource::EncodedTextureResource> texture_resources(
    const ResourceAdapterTransaction& resources,
    const std::span<const strata_surface_texture_binding> bindings,
    const std::string_view host_resource_namespace
) {
    if (host_resource_namespace.empty()) {
        throw std::invalid_argument("Surface host resource namespace must not be empty");
    }
    std::vector<strata::resource::EncodedTextureResource> result;
    result.reserve(bindings.size());
    for (std::size_t index = 0U; index < bindings.size(); ++index) {
        const strata_surface_texture_binding& binding = bindings[index];
        const strata_string_view resource_id{binding.resource_id.data(), binding.resource_id.size()};
        strata::resource::ResourceBytes bytes = resources.load(resource_id);
        const strata::resource::ImageDimensions dimensions = strata::resource::inspect_png(bytes);
        result.push_back(strata::resource::EncodedTextureResource{
            binding.id,
            std::string(host_resource_namespace) + "/static/" + std::to_string(index),
            texture_sampling(binding.sampling),
            strata::resource::ImageEncoding::png,
            dimensions,
            std::move(bytes),
        });
    }
    return result;
}

[[nodiscard]] std::vector<strata_surface_font_binding> font_bindings(
    const strata_surface_config& config
) {
    constexpr std::size_t maximum_font_count = 64U;
    if (config.font_count == 0U) {
        if (config.fonts != nullptr) {
            throw std::invalid_argument("an empty Surface font table must use a null pointer");
        }
        return {};
    }
    if (config.fonts == nullptr || config.font_count > maximum_font_count) {
        throw std::invalid_argument("Surface font table pointer or count is invalid");
    }
    std::vector<strata_surface_font_binding> result;
    result.reserve(config.font_count);
    for (std::size_t index = 0U; index < config.font_count; ++index) {
        const strata_surface_font_resource& entry = config.fonts[index];
        if (!valid_view(entry.id, false) || !valid_view(entry.resource_id, false)) {
            throw std::invalid_argument("Surface font entries require logical and resource ids");
        }
        std::string id = copied_string(entry.id);
        std::string resource_id = copied_string(entry.resource_id);
        if (!strata::core::valid_utf8(id) || !strata::core::valid_utf8(resource_id)) {
            throw std::invalid_argument("Surface font ids must be valid UTF-8");
        }
        if (std::ranges::any_of(result, [&](const auto& font) { return font.id == id; })) {
            throw std::invalid_argument("Surface logical font ids must be unique");
        }
        result.push_back(strata_surface_font_binding{std::move(id), std::move(resource_id)});
    }
    return result;
}

[[nodiscard]] std::shared_ptr<const strata::ui::TextEngine> text_engine(
    const ResourceAdapterTransaction& resources,
    const std::span<const strata_surface_font_binding> bindings
) {
    if (bindings.empty()) return {};
    strata::ui::TextEngine::FontRegistry fonts;
    for (const strata_surface_font_binding& binding : bindings) {
        const strata_string_view resource_id{binding.resource_id.data(), binding.resource_id.size()};
        auto parsed = strata::font::OpenTypeFont::parse(resources.load(resource_id));
        if (!fonts.emplace(binding.id, std::move(parsed)).second) {
            throw std::invalid_argument("Surface logical font ids must be unique");
        }
    }
    return std::make_shared<const strata::ui::TextEngine>(std::move(fonts));
}

[[nodiscard]] strata::ui::KeyModifiers modifiers(const strata_key_modifiers value) {
    constexpr strata_key_modifiers known =
        STRATA_KEY_MODIFIER_SHIFT |
        STRATA_KEY_MODIFIER_CONTROL |
        STRATA_KEY_MODIFIER_ALT |
        STRATA_KEY_MODIFIER_SUPER;
    if ((value & ~known) != 0U) throw std::invalid_argument("input modifiers are invalid");
    return strata::ui::KeyModifiers{
        (value & STRATA_KEY_MODIFIER_SHIFT) != 0U,
        (value & STRATA_KEY_MODIFIER_CONTROL) != 0U,
        (value & STRATA_KEY_MODIFIER_ALT) != 0U,
        (value & STRATA_KEY_MODIFIER_SUPER) != 0U,
    };
}

void require_finite(const double value, const char* const message) {
    if (!std::isfinite(value)) throw std::invalid_argument(message);
}

[[nodiscard]] std::string input_text(
    const strata_string_view value,
    const bool allow_empty
) {
    if (!valid_view(value, allow_empty)) throw std::invalid_argument("input text view is invalid");
    std::string result = copied_string(value);
    if (!strata::core::valid_utf8(result)) throw std::invalid_argument("input text is not valid UTF-8");
    return result;
}

[[nodiscard]] strata::ui::SurfaceInputEvent input_event(const strata_input_event& value) {
    if (value.struct_size < sizeof(strata_input_event)) {
        throw std::invalid_argument("input event structure is incomplete");
    }
    if (value.version != STRATA_INPUT_EVENT_VERSION_2 || value.reserved != 0U) {
        throw std::invalid_argument("input event version or reserved field is invalid");
    }
    if (value.timestamp_nanoseconds < 0) {
        throw std::invalid_argument("input event timestamp must be non-negative");
    }
    const strata::ui::KeyEventType key_type = [&] {
        if (value.key_action == STRATA_KEY_PRESS) return strata::ui::KeyEventType::press;
        if (value.key_action == STRATA_KEY_RELEASE) return strata::ui::KeyEventType::release;
        if (value.key_action == STRATA_KEY_REPEAT) return strata::ui::KeyEventType::repeat;
        throw std::invalid_argument("key event action is invalid");
    }();
    const strata::ui::KeyModifiers event_modifiers = modifiers(value.modifiers);
    switch (value.kind) {
    case STRATA_INPUT_POINTER_MOVE:
    case STRATA_INPUT_POINTER_PRESS:
    case STRATA_INPUT_POINTER_RELEASE:
    case STRATA_INPUT_POINTER_CANCEL: {
        require_finite(value.x, "pointer x-coordinate must be finite");
        require_finite(value.y, "pointer y-coordinate must be finite");
        strata::ui::PointerEventType type = strata::ui::PointerEventType::move;
        if (value.kind == STRATA_INPUT_POINTER_PRESS) type = strata::ui::PointerEventType::press;
        else if (value.kind == STRATA_INPUT_POINTER_RELEASE) type = strata::ui::PointerEventType::release;
        else if (value.kind == STRATA_INPUT_POINTER_CANCEL) type = strata::ui::PointerEventType::cancel;
        return strata::ui::PointerInputEvent{
            strata::ui::Point{value.x, value.y},
            type,
            value.pointer_id,
            value.button,
            event_modifiers,
            strata::ui::Point{value.delta_x, value.delta_y},
            value.timestamp_nanoseconds,
        };
    }
    case STRATA_INPUT_SCROLL:
        require_finite(value.x, "scroll x-coordinate must be finite");
        require_finite(value.y, "scroll y-coordinate must be finite");
        require_finite(value.delta_x, "horizontal scroll delta must be finite");
        require_finite(value.delta_y, "vertical scroll delta must be finite");
        return strata::ui::ScrollInputEvent{
            strata::ui::Point{value.x, value.y},
            value.delta_x,
            value.delta_y,
            event_modifiers,
            value.timestamp_nanoseconds,
        };
    case STRATA_INPUT_KEY:
        return strata::ui::KeyInputEvent{
            input_text(value.text, false), event_modifiers, key_type, value.timestamp_nanoseconds,
        };
    case STRATA_INPUT_TEXT:
        return strata::ui::TextInputEvent{input_text(value.text, true), value.timestamp_nanoseconds};
    case STRATA_INPUT_IME_PREEDIT: {
        std::string text = input_text(value.text, true);
        if (value.selection_start > text.size() || value.selection_end > text.size()) {
            throw std::invalid_argument("IME selection lies outside the preedit UTF-8 buffer");
        }
        return strata::ui::ImePreeditInputEvent{
            std::move(text),
            static_cast<std::size_t>(value.selection_start),
            static_cast<std::size_t>(value.selection_end),
            value.timestamp_nanoseconds,
        };
    }
    case STRATA_INPUT_NAVIGATION:
        return strata::ui::NavigationInputEvent{
            input_text(value.text, false), key_type, event_modifiers, value.timestamp_nanoseconds,
        };
    default: throw std::invalid_argument("input event kind is invalid");
    }
}

} // namespace

namespace strata::abi_detail {

void destroy_surface(
    strata_surface* const surface,
    const bool unlink_from_runtime
) noexcept {
    if (surface == nullptr) return;
    if (unlink_from_runtime && surface->owner != nullptr) {
        std::erase(surface->owner->surfaces, surface);
    }
    const core::HostAllocator allocator = surface->allocator;
    std::destroy_at(surface);
    allocator.deallocate(surface, sizeof(strata_surface), alignof(strata_surface));
}

} // namespace strata::abi_detail

extern "C" {

strata_result strata_runtime_create_surface(
    strata_runtime* const runtime,
    const strata_surface_config* const config,
    strata_surface** const out_surface
) {
    constexpr std::size_t surface_config_v1_size = offsetof(
        strata_surface_config,
        extensions
    );
    if (out_surface != nullptr) *out_surface = nullptr;
    if (runtime == nullptr) return invalid_argument();
    if (config == nullptr || out_surface == nullptr ||
        config->struct_size < surface_config_v1_size ||
        !valid_view(config->id, false) || !valid_view(config->root_name, false) ||
        config->reserved != 0U) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_SURFACE_CONFIG",
            "Surface creation requires a complete configuration, id, root role/name, and environment."
        );
    }
    if (!runtime->core.has_application()) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.APPLICATION.NOT_CONFIGURED",
            "Configure an application before creating a surface."
        );
    }
    const strata::core::HostAllocator allocator = runtime->core.allocator();
    void* const storage = allocator.allocate(sizeof(strata_surface), alignof(strata_surface));
    if (storage == nullptr) {
        return runtime_failure(
            *runtime,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "The surface handle could not be allocated."
        );
    }
    try {
        std::string id = copied_string(config->id);
        std::string root_name = copied_string(config->root_name);
        if (!strata::core::valid_utf8(id) || !strata::core::valid_utf8(root_name)) {
            throw std::invalid_argument("surface id and root name must be valid UTF-8");
        }
        if (std::ranges::any_of(runtime->surfaces, [&](const strata_surface* const surface) {
                return surface != nullptr && surface->core.id() == id;
            })) {
            throw std::invalid_argument("Surface ids must be unique within a Runtime");
        }
        const strata_surface_extension_bundle* const extension_bundle =
            config->struct_size >= sizeof(strata_surface_config) ? config->extensions : nullptr;
        strata::abi_detail::ExtensionRegistries extensions =
            strata::abi_detail::extension_registries(extension_bundle);
        std::vector<strata_surface_font_binding> owned_fonts = font_bindings(*config);
        std::vector<strata_surface_texture_binding> owned_texture_bindings =
            texture_bindings(*config);
        std::string owned_resource_namespace = runtime->allocate_surface_resource_namespace();
        const ResourceAdapterTransaction resource_transaction(*runtime);
        std::shared_ptr<const strata::ui::TextEngine> owned_text_engine =
            text_engine(resource_transaction, owned_fonts);
        std::vector<strata::resource::EncodedTextureResource> owned_textures =
            texture_resources(resource_transaction, owned_texture_bindings, owned_resource_namespace);
        // This is the final fallible/callback-sensitive gate before candidate publication.
        resource_transaction.verify_current();
        strata_surface* const created = std::construct_at(
            static_cast<strata_surface*>(storage),
            allocator,
            runtime,
            std::move(id),
            root_role(config->root_role),
            std::move(root_name),
            std::move(owned_resource_namespace),
            environment(config->environment),
            std::move(owned_fonts),
            std::move(owned_texture_bindings),
            std::move(owned_text_engine),
            std::move(owned_textures),
            std::move(extensions.widgets),
            std::move(extensions.behaviors)
        );
        const std::uint64_t resource_generation = resource_transaction.generation();
        created->materialized_resource_generation = resource_generation;
        created->required_resource_generation = resource_generation;
        try {
            runtime->surfaces.push_back(created);
        } catch (...) {
            std::destroy_at(created);
            throw;
        }
        *out_surface = created;
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        if (*out_surface == nullptr) {
            allocator.deallocate(storage, sizeof(strata_surface), alignof(strata_surface));
        }
        return runtime_failure(
            *runtime,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Surface creation exhausted memory."
        );
    } catch (const HostServiceError& error) {
        allocator.deallocate(storage, sizeof(strata_surface), alignof(strata_surface));
        return runtime_failure(
            *runtime,
            error.status(),
            "STRATA.SURFACE.RESOURCE_UNAVAILABLE",
            error.what()
        );
    } catch (const std::invalid_argument& error) {
        allocator.deallocate(storage, sizeof(strata_surface), alignof(strata_surface));
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.SURFACE.INVALID_CONFIGURATION",
            error.what()
        );
    } catch (...) {
        allocator.deallocate(storage, sizeof(strata_surface), alignof(strata_surface));
        return runtime_failure(
            *runtime,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.UNCAUGHT_EXCEPTION",
            "Surface creation failed inside the C ABI exception boundary."
        );
    }
}

strata_result strata_surface_prepare_release_packet(
    strata_surface* const surface,
    const strata_bytes_sink* const sink
) {
    if (surface == nullptr || surface->owner == nullptr) return invalid_argument();
    if (sink == nullptr || sink->struct_size < sizeof(strata_bytes_sink) ||
        sink->emit == nullptr) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_BYTES_SINK",
            "Preparing a Surface release packet requires a complete byte sink."
        );
    }
    try {
        if (!surface->release_packet_prepared) {
            surface->core.cancel_interactions();
            const std::uint64_t last_frame = surface->core.last_frame().frame_index;
            const std::uint64_t release_frame = last_frame == std::numeric_limits<std::uint64_t>::max()
                ? last_frame
                : last_frame + 1U;
            static_cast<void>(surface->host_render_packet_cache.prepare_resource_release(
                release_frame,
                surface->glyph_atlas,
                surface->textures
            ));
            surface->frame_json.clear();
            surface->frame_snapshot_available = false;
            surface->release_packet_prepared = true;
        }
        const std::vector<std::uint8_t>& packet = surface->current_render_packet();
        sink->emit(sink->user_data, strata_bytes_view{packet.data(), packet.size()});
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return surface_failure(
            *surface,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Preparing the Surface release packet exhausted memory."
        );
    } catch (...) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.CALLBACK_FAILED",
            "Surface release-packet preparation or delivery failed inside the C ABI boundary."
        );
    }
}

strata_result strata_surface_acknowledge_release_packet(strata_surface* const surface) {
    if (surface == nullptr || surface->owner == nullptr) return invalid_argument();
    if (!surface->release_packet_prepared) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.SURFACE.RELEASE_PACKET_NOT_PREPARED",
            "Release-packet consumption cannot be acknowledged before terminal preparation."
        );
    }
    surface->release_packet_consumed = true;
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_surface_release(strata_surface* const surface) {
    if (surface == nullptr) return strata::core::result(STRATA_STATUS_OK);
    if (surface->owner == nullptr) return invalid_argument();
    if (!surface->release_packet_prepared || !surface->release_packet_consumed) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.SURFACE.RELEASE_BARRIER_INCOMPLETE",
            "Surface release requires a prepared, synchronously consumed, and acknowledged terminal packet."
        );
    }
    strata::abi_detail::destroy_surface(surface, true);
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_surface_abandon(strata_surface* const surface) {
    if (surface == nullptr) return strata::core::result(STRATA_STATUS_OK);
    if (surface->owner == nullptr) return invalid_argument();
    const strata_result warning = surface->owner->core.diagnostics().emit(
        STRATA_STATUS_OK,
        STRATA_DIAGNOSTIC_WARNING,
        "STRATA.SURFACE.RELEASE_ABANDONED",
        "Surface ownership was abandoned without the ordered terminal packet barrier; the host must independently discard any remaining GPU resources."
    );
    strata::abi_detail::destroy_surface(surface, true);
    return warning;
}

strata_result strata_surface_reload_resources(strata_surface* const surface) {
    if (surface == nullptr || surface->owner == nullptr) return invalid_argument();
    if (surface->release_packet_prepared) return terminal_surface_failure(*surface);
    try {
        const auto reload_started = std::chrono::steady_clock::now();
        const ResourceAdapterTransaction resource_transaction(*surface->owner);
        const std::uint64_t candidate_generation = resource_transaction.generation();
        if (surface->resource_reload_required &&
            candidate_generation != surface->required_resource_generation) {
            throw HostServiceError(
                STRATA_STATUS_SERVICE_UNAVAILABLE,
                "Surface reload did not begin against its required resource-adapter generation"
            );
        }
        std::shared_ptr<const strata::ui::TextEngine> next_text_engine =
            text_engine(resource_transaction, surface->fonts);
        std::vector<strata::resource::EncodedTextureResource> next_textures =
            texture_resources(
                resource_transaction,
                surface->texture_bindings,
                surface->host_resource_namespace
            );
        strata::ui::SurfaceResourceReloadPlan core_plan =
            surface->core.prepare_resource_reload(std::move(next_text_engine));
        resource_transaction.verify_current();
        if (surface->required_resource_generation != candidate_generation) {
            throw HostServiceError(
                STRATA_STATUS_SERVICE_UNAVAILABLE,
                "Resource adapter changed while Surface reload candidates were being prepared"
            );
        }
        strata::font::AtlasResourceInvalidationPlan atlas_plan =
            surface->glyph_atlas.plan_resource_invalidation();

        // Every operation below is an allocation-free commit. A failure above leaves the old
        // engine, layout, atlas, textures, packet cache, and adapter gate intact for retry.
        static_assert(noexcept(next_textures.swap(surface->textures)));
        surface->core.commit_resource_reload(std::move(core_plan));
        surface->glyph_atlas.commit_resource_invalidation(std::move(atlas_plan));
        next_textures.swap(surface->textures);
        surface->texture_resources_pending = true;
        surface->host_render_packet_cache.clear();
        surface->materialized_resource_generation = candidate_generation;
        surface->required_resource_generation = candidate_generation;
        surface->resource_reload_required = false;
        surface->frame_json.clear();
        surface->frame_snapshot_available = false;
        const std::int64_t reload_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - reload_started
        ).count();
        surface->core.note_resource_reload_duration(static_cast<std::uint64_t>(reload_nanos));
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return surface_failure(
            *surface,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Reloading Surface resources exhausted memory; prior resources remain active."
        );
    } catch (const HostServiceError& error) {
        return surface_failure(
            *surface,
            error.status(),
            "STRATA.SURFACE.RESOURCE_RELOAD_REJECTED",
            error.what()
        );
    } catch (const std::exception& error) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.SURFACE.RESOURCE_RELOAD_REJECTED",
            error.what()
        );
    } catch (...) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.UNCAUGHT_EXCEPTION",
            "Surface resource reload failed inside the C ABI exception boundary."
        );
    }
}

strata_result strata_surface_adopt_environment(
    strata_surface* const surface,
    const strata_surface_environment* const value,
    uint32_t* const out_adopted
) {
    if (surface == nullptr) return invalid_argument();
    if (surface->release_packet_prepared) return terminal_surface_failure(*surface);
    if (out_adopted != nullptr) *out_adopted = 0U;
    if (value == nullptr || out_adopted == nullptr) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_SURFACE_ENVIRONMENT",
            "Environment adoption requires a complete environment and output pointer."
        );
    }
    try {
        *out_adopted = surface->core.adopt_environment(environment(*value)) ? 1U : 0U;
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return surface_failure(*surface, STRATA_STATUS_OUT_OF_MEMORY, "STRATA.CORE.OUT_OF_MEMORY", "Environment adoption exhausted memory.");
    } catch (const std::invalid_argument& error) {
        return surface_failure(*surface, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.SURFACE.INVALID_ENVIRONMENT", error.what());
    } catch (...) {
        return surface_failure(*surface, STRATA_STATUS_INTERNAL_ERROR, "STRATA.ABI.UNCAUGHT_EXCEPTION", "Environment adoption failed inside the C ABI exception boundary.");
    }
}

strata_result strata_surface_enqueue_input(
    strata_surface* const surface,
    const strata_input_event* const events,
    const size_t event_count,
    strata_surface_input_batch_info* const out_info
) {
    if (surface == nullptr) return invalid_argument();
    if (surface->release_packet_prepared) return terminal_surface_failure(*surface);
    if (out_info == nullptr || out_info->struct_size < sizeof(strata_surface_input_batch_info) ||
        (events == nullptr && event_count != 0U)) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_INPUT_BATCH",
            "Input enqueue requires a contiguous event array and complete output structure."
        );
    }
    *out_info = strata_surface_input_batch_info{
        sizeof(strata_surface_input_batch_info), 0U,
        static_cast<std::uint64_t>(surface->core.input().queued_event_count()),
    };
    try {
        std::vector<strata::ui::SurfaceInputEvent> decoded;
        decoded.reserve(event_count);
        for (std::size_t index = 0U; index < event_count; ++index) {
            decoded.push_back(input_event(events[index]));
        }
        const strata::ui::InputOperationResult enqueued =
            surface->core.input().enqueue(std::move(decoded));
        out_info->accepted_event_count = enqueued.injected_events;
        out_info->queued_event_count = surface->core.input().queued_event_count();
        if (enqueued.injected_events == event_count) {
            return strata::core::result(STRATA_STATUS_OK);
        }
        return surface_failure(
            *surface,
            STRATA_STATUS_INPUT_QUEUE_FULL,
            "STRATA.UI.INPUT_QUEUE_FULL",
            "The atomic surface input batch exceeded the bounded input queue."
        );
    } catch (const std::bad_alloc&) {
        return surface_failure(*surface, STRATA_STATUS_OUT_OF_MEMORY, "STRATA.CORE.OUT_OF_MEMORY", "Input batch enqueue exhausted memory.");
    } catch (const std::invalid_argument& error) {
        return surface_failure(*surface, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.INPUT.INVALID_EVENT", error.what());
    } catch (...) {
        return surface_failure(*surface, STRATA_STATUS_INTERNAL_ERROR, "STRATA.ABI.UNCAUGHT_EXCEPTION", "Input batch enqueue failed inside the C ABI exception boundary.");
    }
}

strata_result strata_surface_cancel_interactions(strata_surface* const surface) {
    if (surface == nullptr) return invalid_argument();
    if (surface->release_packet_prepared) return terminal_surface_failure(*surface);
    try {
        surface->core.cancel_interactions();
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return surface_failure(*surface, STRATA_STATUS_OUT_OF_MEMORY, "STRATA.CORE.OUT_OF_MEMORY", "Interaction cancellation exhausted memory.");
    } catch (...) {
        return surface_failure(*surface, STRATA_STATUS_INTERNAL_ERROR, "STRATA.ABI.UNCAUGHT_EXCEPTION", "Interaction cancellation failed inside the C ABI exception boundary.");
    }
}

strata_result strata_surface_dispatch_action_json(
    strata_surface* const surface,
    const strata_action_dispatch_config* const config,
    strata_action_dispatch_info* const out_info
) {
    if (surface == nullptr) return invalid_argument();
    if (surface->release_packet_prepared) return terminal_surface_failure(*surface);
    if (!strata::abi_detail::valid_action_dispatch_config(config) || out_info == nullptr ||
        out_info->struct_size < sizeof(strata_action_dispatch_info)) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_SURFACE_ACTION_DISPATCH",
            "Surface action dispatch requires an action id, payload/event JSON, event kind, and output structure."
        );
    }
    *out_info = strata_action_dispatch_info{
        sizeof(strata_action_dispatch_info), STRATA_ACTION_DISPATCH_FAILED, 0U, 0U,
    };
    try {
        const std::string source = copied_string(config->source_key);
        const strata::runtime::ActionDispatchOutcome outcome = surface->core.dispatch_action(
            copied_string(config->action_id),
            strata::abi_detail::decoded_value(config->payload_json, true),
            copied_string(config->event_kind),
            source.empty() ? std::nullopt : std::optional<std::string>(source),
            strata::abi_detail::decoded_value(config->event_value_json, true),
            strata::abi_detail::dynamic_action_dispatch(*config)
        );
        *out_info = strata_action_dispatch_info{
            sizeof(strata_action_dispatch_info),
            strata::abi_detail::dispatch_status(outcome.status),
            0U,
            static_cast<std::uint64_t>(outcome.handler_owners.size()),
        };
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const strata::data::JsonError&) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ACTION.INVALID_JSON",
            "Surface action payload or event value is not valid strict JSON."
        );
    } catch (const std::invalid_argument& error) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ACTION.INVALID_SURFACE_DISPATCH",
            error.what()
        );
    } catch (const std::bad_alloc&) {
        return surface_failure(
            *surface,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Surface action dispatch exhausted memory."
        );
    } catch (...) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.UNCAUGHT_EXCEPTION",
            "Surface action dispatch failed inside the C ABI exception boundary."
        );
    }
}

strata_result strata_surface_frame(
    strata_surface* const surface,
    const int64_t frame_time_nanoseconds,
    strata_surface_frame_info* const out_info
) {
    if (surface == nullptr) return invalid_argument();
    if (surface->release_packet_prepared) return terminal_surface_failure(*surface);
    if (surface->resource_reload_required) {
        return surface_failure(
            *surface,
            STRATA_STATUS_SERVICE_UNAVAILABLE,
            "STRATA.SURFACE.RESOURCE_RELOAD_REQUIRED",
            "The runtime resource adapter changed; reload this Surface before framing it."
        );
    }
    if (out_info == nullptr || out_info->struct_size < sizeof(strata_surface_frame_info)) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_SURFACE_FRAME_OUTPUT",
            "Surface framing requires a complete output structure."
        );
    }
    *out_info = strata_surface_frame_info{sizeof(strata_surface_frame_info)};
    try {
        const strata::ui::SurfaceFrame frame = surface->core.frame(frame_time_nanoseconds);
        const auto packet_started = std::chrono::steady_clock::now();
        const strata::ui::TextEngine* const engine = surface->core.text_engine();
        const bool settled = frame.operations.render.nodes_visited == 0U &&
            frame.operations.render.commands_emitted == 0U &&
            !surface->texture_resources_pending;
        if (!settled || !surface->host_render_packet_cache.reuse(frame.frame_index)) {
            static_cast<void>(surface->host_render_packet_cache.encode(
                surface->core.render_commands(),
                frame.frame_index,
                surface->texture_resources_pending
                    ? std::span<const strata::resource::EncodedTextureResource>(surface->textures)
                    : std::span<const strata::resource::EncodedTextureResource>{},
                surface->glyph_atlas,
                engine,
                surface->core.environment().scale,
                surface->core.environment().framebuffer_width,
                surface->core.environment().framebuffer_height,
                surface->core.environment().logical_width,
                surface->core.environment().logical_height
            ));
        }
        surface->texture_resources_pending = false;
        const std::int64_t packet_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - packet_started
        ).count();
        const strata::ui::HostRenderPacketTelemetry& packet =
            surface->host_render_packet_cache.telemetry();
        surface->core.profiler().record_counters({
            {strata::runtime::ProfilerCounter::packet_bytes,
             static_cast<std::uint64_t>(surface->current_render_packet().size())},
            {strata::runtime::ProfilerCounter::batch_texture_breaks, packet.texture_batch_breaks},
            {strata::runtime::ProfilerCounter::batch_clip_breaks, packet.clip_batch_breaks},
            {strata::runtime::ProfilerCounter::batch_material_breaks, packet.material_batch_breaks},
            {strata::runtime::ProfilerCounter::batch_effect_breaks, packet.effect_batch_breaks},
            {strata::runtime::ProfilerCounter::encoded_geometry_cache_hits,
             packet.geometry_reused ? 1U : 0U},
            {strata::runtime::ProfilerCounter::encoded_geometry_cache_misses,
             packet.geometry_reused ? 0U : 1U},
            {strata::runtime::ProfilerCounter::render_submission_nanos,
             static_cast<std::uint64_t>(packet_nanos)},
        });
        surface->core.profiler().record_external_timing("packet-encode", packet_nanos);
        if (packet.cold_encode_profiled) {
            surface->core.profiler().record_external_timing(
                "packet-cold-submission",
                packet.cold_submission_nanos
            );
            surface->core.profiler().record_external_timing(
                "packet-cold-submission-plan",
                packet.cold_submission_planning_nanos
            );
            surface->core.profiler().record_external_timing(
                "packet-cold-submission-atlas",
                packet.cold_submission_atlas_warmup_nanos
            );
            surface->core.profiler().record_external_timing(
                "packet-cold-submission-text",
                packet.cold_submission_text_preparation_nanos
            );
            surface->core.profiler().record_external_timing(
                "packet-cold-submission-mesh",
                packet.cold_submission_mesh_encoding_nanos
            );
            surface->core.profiler().record_external_timing(
                "packet-cold-geometry",
                packet.cold_geometry_packet_nanos
            );
            surface->core.profiler().record_external_timing(
                "packet-cold-resources",
                packet.cold_resource_packet_nanos
            );
        }
        // Canonical inspection JSON is expensive and optional; materialize it on first read.
        surface->frame_json.clear();
        surface->frame_snapshot_available = true;
        *out_info = strata_surface_frame_info{
            sizeof(strata_surface_frame_info),
            frame.frame_index,
            frame.frame_time_nanos,
            static_cast<std::uint64_t>(frame.operations.input_events_processed),
            static_cast<std::uint64_t>(frame.lifecycle_input.events.size()),
            static_cast<std::uint64_t>(frame.lifecycle_input.action_outcomes.size()),
            static_cast<std::uint64_t>(surface->core.render_commands().size()),
            surface->core.semantics().generation(),
            static_cast<std::uint64_t>(surface->core.input().queued_event_count()),
            static_cast<std::uint64_t>(surface->current_render_packet().size()),
        };
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return surface_failure(*surface, STRATA_STATUS_OUT_OF_MEMORY, "STRATA.CORE.OUT_OF_MEMORY", "Surface frame exhausted memory.");
    } catch (const std::invalid_argument& error) {
        return surface_failure(*surface, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.SURFACE.INVALID_FRAME", error.what());
    } catch (const std::exception& error) {
        const std::string message =
            std::string("Surface framing failed inside the C ABI exception boundary: ") +
            error.what();
        return surface_failure(
            *surface,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.UNCAUGHT_EXCEPTION",
            message.c_str()
        );
    } catch (...) {
        return surface_failure(*surface, STRATA_STATUS_INTERNAL_ERROR, "STRATA.ABI.UNCAUGHT_EXCEPTION", "Surface framing failed inside the C ABI exception boundary with a non-standard exception.");
    }
}

strata_result strata_surface_read_frame_json(
    const strata_surface* const surface,
    const strata_value_json_sink* const sink
) {
    if (surface == nullptr) return invalid_argument();
    if (sink == nullptr || sink->struct_size < sizeof(strata_value_json_sink) ||
        sink->emit == nullptr) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_VALUE_SINK",
            "Reading a surface frame requires a complete JSON sink."
        );
    }
    if (!surface->frame_snapshot_available) {
        return surface_failure(
            *surface,
            STRATA_STATUS_NOT_FOUND,
            "STRATA.SURFACE.FRAME_UNAVAILABLE",
            "Frame the surface before reading its canonical frame snapshot."
        );
    }
    try {
        if (surface->frame_json.empty()) {
            const auto inspection_started = std::chrono::steady_clock::now();
            surface->frame_json = strata::data::encode_canonical_json(
                strata::ui::surface_frame_snapshot(surface->core, surface->core.last_frame())
            );
            const std::int64_t inspection_nanos =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - inspection_started
                ).count();
            auto& profiler = const_cast<strata::ui::Surface&>(surface->core).profiler();
            profiler.record(
                strata::runtime::ProfilerCounter::ui_inspector_nanos,
                static_cast<std::uint64_t>(inspection_nanos)
            );
            profiler.record_external_timing("inspection", inspection_nanos);
        }
        sink->emit(
            sink->user_data,
            strata_string_view{surface->frame_json.data(), surface->frame_json.size()}
        );
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return surface_failure(
            *surface,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Surface frame JSON materialization exhausted memory."
        );
    } catch (...) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.CALLBACK_FAILED",
            "Surface frame JSON delivery failed inside the C ABI boundary."
        );
    }
}

strata_result strata_surface_read_diagnostics(
    const strata_surface* const surface,
    const strata_diagnostics_snapshot_sink* const sink
) {
    if (surface == nullptr) return invalid_argument();
    return strata_runtime_read_diagnostics(surface->owner, sink);
}

strata_result strata_surface_clear_diagnostics(strata_surface* const surface) {
    if (surface == nullptr) return invalid_argument();
    return strata_runtime_clear_diagnostics(surface->owner);
}

strata_result strata_surface_drain_events_json(
    strata_surface* const surface,
    const strata_value_json_sink* const sink
) {
    if (surface == nullptr) return invalid_argument();
    if (surface->release_packet_prepared) return terminal_surface_failure(*surface);
    if (sink == nullptr || sink->struct_size < sizeof(strata_value_json_sink) ||
        sink->emit == nullptr) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_VALUE_SINK",
            "Draining surface events requires a complete JSON sink."
        );
    }
    try {
        strata::ui::SurfaceEventDrain drain = surface->core.drain_events();
        strata::data::JsonValue::Array records;
        records.reserve(drain.records.size());
        for (strata::ui::SurfaceEventRecord& record : drain.records) {
            records.emplace_back(strata::data::JsonValue::Object{
                {"sequence", strata::data::JsonValue(static_cast<std::int64_t>(record.sequence))},
                {"frameIndex", strata::data::JsonValue(static_cast<std::int64_t>(record.frame_index))},
                {"event", std::move(record.event)},
                {"actionOutcome", std::move(record.action_outcome)},
            });
        }
        const std::string encoded = strata::data::encode_canonical_json(
            strata::data::JsonValue(strata::data::JsonValue::Object{
                {"droppedCount", strata::data::JsonValue(
                    static_cast<std::int64_t>(drain.dropped_count)
                )},
                {"records", strata::data::JsonValue(std::move(records))},
            })
        );
        sink->emit(sink->user_data, strata_string_view{encoded.data(), encoded.size()});
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return surface_failure(
            *surface,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Surface event drain exhausted memory."
        );
    } catch (...) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.CALLBACK_FAILED",
            "Surface event delivery failed inside the C ABI boundary."
        );
    }
}

strata_result strata_surface_read_render_packet(
    const strata_surface* const surface,
    const strata_bytes_sink* const sink
) {
    if (surface == nullptr) return invalid_argument();
    if (sink == nullptr || sink->struct_size < sizeof(strata_bytes_sink) ||
        sink->emit == nullptr) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_BYTES_SINK",
            "Reading a surface render packet requires a complete byte sink."
        );
    }
    const std::vector<std::uint8_t>& packet = surface->current_render_packet();
    if (packet.empty()) {
        return surface_failure(
            *surface,
            STRATA_STATUS_NOT_FOUND,
            "STRATA.SURFACE.FRAME_UNAVAILABLE",
            "Frame the surface before reading its render packet."
        );
    }
    try {
        sink->emit(
            sink->user_data,
            strata_bytes_view{packet.data(), packet.size()}
        );
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.CALLBACK_FAILED",
            "Surface render packet delivery failed inside the C ABI boundary."
        );
    }
}

} // extern "C"
