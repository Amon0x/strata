#include <strata/strata.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "abi_extension.hpp"
#include "abi_internal.hpp"
#include "abi_support.hpp"
#include "base/utf8.hpp"
#include "core/diagnostics.hpp"
#include "data/json.hpp"
#include "font/opentype.hpp"
#include "resource/image.hpp"
#include "resource/image_stream.hpp"
#include "resource/resource.hpp"
#include "resource/svg_image.hpp"
#include "ui/frame_snapshot.hpp"
#include "ui/input.hpp"
#include "ui/layout.hpp"
#include "ui/render/material_registry.hpp"
#include "ui/render/packet.hpp"
#include "ui/surface.hpp"
#include "ui/svg_image.hpp"

static_assert(strata::ui::render_packet_version == STRATA_RENDER_PACKET_VERSION_CURRENT,
              "the core encodes the packet version the C ABI publishes");
static_assert(static_cast<std::uint32_t>(strata::ui::PacketBlend::opaque) ==
                  STRATA_RENDER_BLEND_OPAQUE &&
              static_cast<std::uint32_t>(strata::ui::PacketBlend::straight_alpha) ==
                  STRATA_RENDER_BLEND_STRAIGHT_ALPHA &&
              static_cast<std::uint32_t>(strata::ui::PacketBlend::premultiplied_alpha) ==
                  STRATA_RENDER_BLEND_PREMULTIPLIED_ALPHA &&
              static_cast<std::uint32_t>(strata::ui::PacketBlend::additive) ==
                  STRATA_RENDER_BLEND_ADDITIVE &&
              static_cast<std::uint32_t>(strata::ui::PacketBlend::multiply) ==
                  STRATA_RENDER_BLEND_MULTIPLY &&
              static_cast<std::uint32_t>(strata::ui::PacketBlend::clipped_multiply) ==
                  STRATA_RENDER_BLEND_CLIPPED_MULTIPLY,
              "the engine's packet blends must be the C ABI's");
static_assert(static_cast<std::uint32_t>(strata::ui::PacketClipCoverage::straight_alpha) ==
                  STRATA_RENDER_CLIP_COVERAGE_STRAIGHT_ALPHA &&
              static_cast<std::uint32_t>(strata::ui::PacketClipCoverage::premultiplied_alpha) ==
                  STRATA_RENDER_CLIP_COVERAGE_PREMULTIPLIED_ALPHA &&
              static_cast<std::uint32_t>(strata::ui::PacketClipCoverage::hard) ==
                  STRATA_RENDER_CLIP_COVERAGE_HARD &&
              static_cast<std::uint32_t>(strata::ui::PacketClipCoverage::multiply) ==
                  STRATA_RENDER_CLIP_COVERAGE_MULTIPLY,
              "the engine's clip coverage modes must be the C ABI's");
static_assert(strata::ui::effect_sample_reach == STRATA_EFFECT_SAMPLE_REACH,
              "the engine's effect sample reach must be the C ABI's");

namespace {

using strata::abi_detail::AbiCall;
using strata::abi_detail::EntryGuard;
using strata::abi_detail::has_struct;
using strata::abi_detail::Reentry;
using strata::abi_detail::StatusError;
using strata::abi_detail::surface_call;
using strata::abi_detail::write_struct;

/** Host resource-adapter failure; reported with the adapter's own status. */
[[nodiscard]] StatusError resource_unavailable(const strata_status status, std::string message) {
    return StatusError(status, "STRATA.SURFACE.RESOURCE_UNAVAILABLE", std::move(message));
}

[[nodiscard]] bool valid_view(strata_string_view value, bool allow_empty) noexcept;
[[nodiscard]] std::string copied_string(strata_string_view value);

/** Immutable resource provider borrowed from the owning Runtime. */
class ResourceLoader final {
  public:
    explicit ResourceLoader(const strata_runtime& runtime) : adapter_(runtime.resources) {}

    [[nodiscard]] strata::resource::ResourceBytes load(const strata_string_view resource_id) const {
        if (!valid_view(resource_id, false)) {
            throw std::invalid_argument("resource id view is invalid");
        }
        const std::string id = copied_string(resource_id);
        if (!strata::base::valid_utf8(id)) {
            throw std::invalid_argument("resource id must be valid UTF-8");
        }
        if (!adapter_.has_value()) {
            throw resource_unavailable(
                STRATA_STATUS_SERVICE_UNAVAILABLE,
                "Surface resource loading requires a configured resource adapter.");
        }
        strata_bytes_view bytes{};
        const strata_status status =
            adapter_->load(adapter_->user_data, strata_string_view{id.data(), id.size()}, &bytes);
        if (status != STRATA_STATUS_OK) {
            throw resource_unavailable(status,
                                       "The host resource adapter rejected a surface resource.");
        }
        if (bytes.data == nullptr || bytes.size == 0U) {
            throw resource_unavailable(
                STRATA_STATUS_SERVICE_UNAVAILABLE,
                "The host resource adapter returned an empty surface resource.");
        }
        constexpr std::size_t maximum_surface_resource_bytes = 64U * 1'024U * 1'024U;
        if (bytes.size > maximum_surface_resource_bytes) {
            throw std::invalid_argument("surface resource exceeds the 64 MiB per-resource limit");
        }
        return strata::resource::ResourceBytes(bytes.data, bytes.data + bytes.size);
    }

  private:
    const std::optional<strata_resource_adapter>& adapter_;
};

[[nodiscard]] strata_result invalid_argument() noexcept {
    return strata::abi_detail::result(STRATA_STATUS_INVALID_ARGUMENT);
}

[[nodiscard]] strata_result runtime_failure(strata_runtime& runtime, const strata_status status,
                                            const char* const code,
                                            const char* const message) noexcept {
    return strata::abi_detail::emit(runtime.core.diagnostics(), status, STRATA_DIAGNOSTIC_ERROR, code, message);
}

[[nodiscard]] strata_result surface_failure(const strata_surface& surface,
                                            const strata_status status, const char* const code,
                                            const char* const message) noexcept {
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
    case STRATA_POINT_SNAP_NONE:
        return strata::ui::PointSnapPolicy::none;
    case STRATA_POINT_SNAP_NEAREST:
        return strata::ui::PointSnapPolicy::nearest;
    default:
        throw std::invalid_argument("surface point-snapping policy is invalid");
    }
}

[[nodiscard]] strata::ui::RectangleSnapPolicy
rectangle_snapping(const strata_rectangle_snap_policy value) {
    switch (value) {
    case STRATA_RECTANGLE_SNAP_NONE:
        return strata::ui::RectangleSnapPolicy::none;
    case STRATA_RECTANGLE_SNAP_NEAREST:
        return strata::ui::RectangleSnapPolicy::nearest;
    case STRATA_RECTANGLE_SNAP_OUTWARD:
        return strata::ui::RectangleSnapPolicy::outward;
    default:
        throw std::invalid_argument("surface rectangle-snapping policy is invalid");
    }
}

[[nodiscard]] strata::ui::SurfaceDensity density(const strata_surface_density value) {
    switch (value) {
    case STRATA_SURFACE_DENSITY_COMPACT:
        return strata::ui::SurfaceDensity::compact;
    case STRATA_SURFACE_DENSITY_COMFORTABLE:
        return strata::ui::SurfaceDensity::comfortable;
    default:
        throw std::invalid_argument("surface density is invalid");
    }
}

[[nodiscard]] strata::ui::PointerPrecision pointer_precision(const strata_pointer_precision value) {
    switch (value) {
    case STRATA_POINTER_PRECISION_NONE:
        return strata::ui::PointerPrecision::none;
    case STRATA_POINTER_PRECISION_COARSE:
        return strata::ui::PointerPrecision::coarse;
    case STRATA_POINTER_PRECISION_FINE:
        return strata::ui::PointerPrecision::fine;
    default:
        throw std::invalid_argument("surface pointer precision is invalid");
    }
}

[[nodiscard]] strata::ui::SurfaceEnvironment environment(const strata_surface_environment& value) {
    constexpr strata_surface_input_capabilities known_input =
        STRATA_SURFACE_INPUT_POINTER | STRATA_SURFACE_INPUT_KEYBOARD | STRATA_SURFACE_INPUT_TOUCH |
        STRATA_SURFACE_INPUT_IME | STRATA_SURFACE_INPUT_CLIPBOARD | STRATA_SURFACE_INPUT_CONTROLLER;
    if (!has_struct(&value) || value.generation == 0U || value.reserved != 0U ||
        value.reduced_motion > 1U || (value.input_capabilities & ~known_input) != 0U) {
        throw std::invalid_argument("surface environment header, flags, or generation is invalid");
    }
    const bool has_pointer = (value.input_capabilities & STRATA_SURFACE_INPUT_POINTER) != 0U;
    const strata::ui::PointerPrecision precision = pointer_precision(value.pointer_precision);
    if (has_pointer == (precision == strata::ui::PointerPrecision::none)) {
        throw std::invalid_argument(
            "surface pointer capability and pointer precision must describe the same device");
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
    case STRATA_SURFACE_ROOT_SCREEN:
        return strata::runtime::LayerRole::screen;
    case STRATA_SURFACE_ROOT_OVERLAY:
        return strata::runtime::LayerRole::overlay;
    default:
        throw std::invalid_argument("surface root role is invalid");
    }
}

[[nodiscard]] strata::resource::TextureSampling image_sampling(const strata_image_sampling value) {
    switch (value) {
    case STRATA_IMAGE_SAMPLING_NEAREST:
        return strata::resource::TextureSampling::nearest;
    case STRATA_IMAGE_SAMPLING_LINEAR:
        return strata::resource::TextureSampling::linear;
    default:
        throw std::invalid_argument("surface image sampling is invalid");
    }
}

[[nodiscard]] std::vector<strata_surface_image_binding>
image_bindings(const strata_surface_config& config) {
    constexpr std::size_t maximum_image_count = 256U;
    if (config.image_count == 0U) {
        if (config.images != nullptr) {
            throw std::invalid_argument("an empty Surface image table must use a null pointer");
        }
        return {};
    }
    if (config.images == nullptr || config.image_count > maximum_image_count) {
        throw std::invalid_argument("Surface image table pointer or count is invalid");
    }
    std::vector<strata_surface_image_binding> result;
    result.reserve(config.image_count);
    for (std::size_t index = 0U; index < config.image_count; ++index) {
        const strata_surface_image_resource& entry = config.images[index];
        if (!valid_view(entry.id, false) || !valid_view(entry.resource_id, false) ||
            entry.reserved != 0U) {
            throw std::invalid_argument("Surface image entries are incomplete");
        }
        std::string id = copied_string(entry.id);
        std::string resource_id = copied_string(entry.resource_id);
        if (!strata::base::valid_utf8(id) || !strata::base::valid_utf8(resource_id)) {
            throw std::invalid_argument("Surface image ids must be valid UTF-8");
        }
        if (std::ranges::any_of(result, [&](const auto& image) { return image.id == id; })) {
            throw std::invalid_argument("Surface logical image ids must be unique");
        }
        static_cast<void>(image_sampling(entry.sampling));
        result.push_back(strata_surface_image_binding{
            std::move(id),
            std::move(resource_id),
            entry.sampling,
        });
    }
    return result;
}

/** Past this much decoded memory an animation streams from its bytes instead of being held. */
constexpr std::size_t maximum_animation_bytes = 64U * 1024U * 1024U;
/** Browsers show a GIF frame that asks for under 20 ms for 100 ms instead; so do we. */
constexpr std::uint32_t minimum_frame_delay_ms = 20U;
constexpr std::uint32_t short_frame_delay_ms = 100U;

struct MaterializedImages final {
    std::vector<strata::resource::EncodedTextureResource> textures;
    std::shared_ptr<const strata::resource::SvgImageRegistry> svg_images;
    std::map<std::string, strata::resource::ImageAnimation, std::less<>> animations;
};

[[nodiscard]] MaterializedImages
image_resources(const ResourceLoader& resources,
                const std::span<const strata_surface_image_binding> bindings,
                const std::string_view host_resource_namespace,
                const strata::resource::RuntimeImageStore* const runtime_images) {
    if (host_resource_namespace.empty()) {
        throw std::invalid_argument("Surface host resource namespace must not be empty");
    }
    MaterializedImages result;
    result.textures.reserve(bindings.size());
    std::vector<strata::resource::SvgImageResource> vectors;
    vectors.reserve(bindings.size());
    for (std::size_t index = 0U; index < bindings.size(); ++index) {
        const strata_surface_image_binding& binding = bindings[index];
        const strata_string_view resource_id{binding.resource_id.data(),
                                             binding.resource_id.size()};
        strata::resource::ResourceBytes bytes = resources.load(resource_id);
        // Raster formats are told apart by their signatures, so a file's name never decides how
        // its bytes are read; whatever no raster decoder claims must be an SVG document.
        if (const std::optional<strata::resource::ImageFormat> format =
                strata::resource::sniff_image_format(bytes)) {
            const strata::resource::ImageInfo info = strata::resource::inspect_image(bytes);
            const std::string host_id =
                std::string(host_resource_namespace) + "/static/" + std::to_string(index);
            // An animation that fits the budget becomes one texture per frame (`<id>#k`), each
            // shown in its turn; a longer one plays from its bytes, one texture showing each frame.
            std::optional<strata::resource::DecodedImage> animated;
            std::shared_ptr<strata::resource::ImageStream> stream;
            if (*format != strata::resource::ImageFormat::jpeg) {
                strata::resource::DecodedImage collected;
                collected.width = info.dimensions.width;
                collected.height = info.dimensions.height;
                std::size_t held = 0U;
                bool over = false;
                try {
                    collected.loop_count = strata::resource::stream_image(
                        bytes, [&](std::vector<std::uint8_t>& rgba, const std::uint32_t delay_ms) {
                            if (held + rgba.size() > maximum_animation_bytes &&
                                !collected.frames.empty()) {
                                over = true;
                                return false;
                            }
                            held += rgba.size();
                            collected.frames.push_back(
                                strata::resource::ImageFrame{std::move(rgba), delay_ms});
                            return true;
                        });
                } catch (const std::exception&) {
                    collected.frames.clear();
                }
                if (over) {
                    strata::resource::ImageFrame first = std::move(collected.frames.front());
                    stream = std::make_shared<strata::resource::ImageStream>(
                        std::vector<std::uint8_t>(bytes.begin(), bytes.end()), info.dimensions,
                        first.rgba, first.delay_ms);
                    result.textures.push_back(strata::resource::EncodedTextureResource{
                        binding.id,
                        host_id,
                        image_sampling(binding.sampling),
                        strata::resource::ImageEncoding::rgba8,
                        info.dimensions,
                        std::move(first.rgba),
                        stream,
                    });
                    strata::resource::ImageAnimation animation;
                    animation.stream = std::move(stream);
                    result.animations.insert_or_assign(binding.id, std::move(animation));
                    continue;
                }
                if (collected.frames.size() > 1U)
                    animated = std::move(collected);
            }
            if (!animated.has_value()) {
                result.textures.push_back(strata::resource::EncodedTextureResource{
                    binding.id,
                    host_id,
                    image_sampling(binding.sampling),
                    strata::resource::image_encoding(*format),
                    info.dimensions,
                    std::move(bytes),
                });
                continue;
            }
            strata::resource::ImageAnimation animation;
            animation.loop_count = animated->loop_count;
            std::int64_t end = 0;
            for (std::size_t frame = 0U; frame < animated->frames.size(); ++frame) {
                strata::resource::ImageFrame& pixels = animated->frames[frame];
                const std::uint32_t delay = pixels.delay_ms < minimum_frame_delay_ms
                                                ? short_frame_delay_ms
                                                : pixels.delay_ms;
                end += static_cast<std::int64_t>(delay) * 1'000'000;
                animation.frame_ends_nanos.push_back(end);
                result.textures.push_back(strata::resource::EncodedTextureResource{
                    frame == 0U ? binding.id : binding.id + "#" + std::to_string(frame),
                    frame == 0U ? host_id : host_id + "/" + std::to_string(frame),
                    image_sampling(binding.sampling),
                    strata::resource::ImageEncoding::rgba8,
                    strata::resource::ImageDimensions{animated->width, animated->height},
                    std::move(pixels.rgba),
                });
            }
            result.animations.insert_or_assign(binding.id, std::move(animation));
            continue;
        }
        const std::string source(bytes.begin(), bytes.end());
        if (!strata::base::valid_utf8(source)) {
            throw std::invalid_argument("image resource '" + binding.resource_id +
                                        "' is neither a PNG, JPEG, GIF or WebP image nor UTF-8 "
                                        "SVG");
        }
        try {
            strata::svg::Document document = strata::svg::parse(source);
            strata::ui::validate_svg_image_geometry(document);
            vectors.push_back(strata::resource::SvgImageResource{
                binding.id,
                std::move(document),
            });
        } catch (const strata::svg::ParseError& error) {
            throw std::invalid_argument("SVG image resource '" + binding.resource_id +
                                        "' is invalid at byte " +
                                        std::to_string(error.byte_offset()) + ": " + error.what());
        } catch (const std::invalid_argument& error) {
            throw std::invalid_argument(
                "SVG image resource '" + binding.resource_id +
                "' exceeds the supported UI geometry budget: " + error.what());
        }
    }
    std::map<std::string, strata::resource::ImageDimensions, std::less<>> rasters;
    for (const strata::resource::EncodedTextureResource& texture : result.textures)
        rasters.insert_or_assign(texture.logical_id, texture.dimensions);
    result.svg_images = std::make_shared<const strata::resource::SvgImageRegistry>(
        std::move(vectors), std::move(rasters), runtime_images, std::move(result.animations));
    return result;
}

[[nodiscard]] std::vector<strata_surface_font_binding>
font_bindings(const strata_surface_config& config) {
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
        if (!strata::base::valid_utf8(id) || !strata::base::valid_utf8(resource_id)) {
            throw std::invalid_argument("Surface font ids must be valid UTF-8");
        }
        if (std::ranges::any_of(result, [&](const auto& font) { return font.id == id; })) {
            throw std::invalid_argument("Surface logical font ids must be unique");
        }
        result.push_back(strata_surface_font_binding{std::move(id), std::move(resource_id)});
    }
    return result;
}

[[nodiscard]] std::shared_ptr<const strata::ui::TextEngine>
text_engine(const ResourceLoader& resources,
            const std::span<const strata_surface_font_binding> bindings) {
    if (bindings.empty())
        return {};
    strata::ui::TextEngine::FontRegistry fonts;
    for (const strata_surface_font_binding& binding : bindings) {
        const strata_string_view resource_id{binding.resource_id.data(),
                                             binding.resource_id.size()};
        auto parsed = strata::font::OpenTypeFont::parse(resources.load(resource_id));
        if (!fonts.emplace(binding.id, std::move(parsed)).second) {
            throw std::invalid_argument("Surface logical font ids must be unique");
        }
    }
    return std::make_shared<const strata::ui::TextEngine>(std::move(fonts));
}

[[nodiscard]] strata::ui::KeyModifiers modifiers(const strata_key_modifiers value) {
    constexpr strata_key_modifiers known = STRATA_KEY_MODIFIER_SHIFT | STRATA_KEY_MODIFIER_CONTROL |
                                           STRATA_KEY_MODIFIER_ALT | STRATA_KEY_MODIFIER_SUPER;
    if ((value & ~known) != 0U)
        throw std::invalid_argument("input modifiers are invalid");
    return strata::ui::KeyModifiers{
        (value & STRATA_KEY_MODIFIER_SHIFT) != 0U,
        (value & STRATA_KEY_MODIFIER_CONTROL) != 0U,
        (value & STRATA_KEY_MODIFIER_ALT) != 0U,
        (value & STRATA_KEY_MODIFIER_SUPER) != 0U,
    };
}

void require_finite(const double value, const char* const message) {
    if (!std::isfinite(value))
        throw std::invalid_argument(message);
}

[[nodiscard]] std::string input_text(const strata_string_view value, const bool allow_empty) {
    if (!valid_view(value, allow_empty))
        throw std::invalid_argument("input text view is invalid");
    std::string result = copied_string(value);
    if (!strata::base::valid_utf8(result))
        throw std::invalid_argument("input text is not valid UTF-8");
    return result;
}

[[nodiscard]] strata::ui::SurfaceInputEvent input_event(const strata_input_event& value) {
    if (!has_struct(&value)) {
        throw std::invalid_argument("input event structure is incomplete");
    }
    if (value.version != STRATA_INPUT_EVENT_VERSION_2 || value.reserved != 0U) {
        throw std::invalid_argument("input event version or reserved field is invalid");
    }
    if (value.timestamp_nanoseconds < 0) {
        throw std::invalid_argument("input event timestamp must be non-negative");
    }
    const strata::ui::KeyEventType key_type = [&] {
        if (value.key_action == STRATA_KEY_PRESS)
            return strata::ui::KeyEventType::press;
        if (value.key_action == STRATA_KEY_RELEASE)
            return strata::ui::KeyEventType::release;
        if (value.key_action == STRATA_KEY_REPEAT)
            return strata::ui::KeyEventType::repeat;
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
        if (value.kind == STRATA_INPUT_POINTER_PRESS)
            type = strata::ui::PointerEventType::press;
        else if (value.kind == STRATA_INPUT_POINTER_RELEASE)
            type = strata::ui::PointerEventType::release;
        else if (value.kind == STRATA_INPUT_POINTER_CANCEL)
            type = strata::ui::PointerEventType::cancel;
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
            strata::ui::Point{value.x, value.y}, value.delta_x, value.delta_y, event_modifiers,
            value.timestamp_nanoseconds,
        };
    case STRATA_INPUT_KEY:
        return strata::ui::KeyInputEvent{
            input_text(value.text, false),
            event_modifiers,
            key_type,
            value.timestamp_nanoseconds,
        };
    case STRATA_INPUT_TEXT:
        return strata::ui::TextInputEvent{input_text(value.text, true),
                                          value.timestamp_nanoseconds};
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
            input_text(value.text, false),
            key_type,
            event_modifiers,
            value.timestamp_nanoseconds,
        };
    case STRATA_INPUT_FILE_DRAG:
    case STRATA_INPUT_FILE_DROP:
    case STRATA_INPUT_FILE_DRAG_LEAVE: {
        require_finite(value.x, "file drag x-coordinate must be finite");
        require_finite(value.y, "file drag y-coordinate must be finite");
        strata::ui::PlatformInputEvent files;
        files.kind = value.kind == STRATA_INPUT_FILE_DRAG ? strata::ui::PlatformInputKind::file_drag
                     : value.kind == STRATA_INPUT_FILE_DROP
                         ? strata::ui::PlatformInputKind::file_drop
                         : strata::ui::PlatformInputKind::file_leave;
        files.position = strata::ui::Point{value.x, value.y};
        const std::string paths = input_text(value.text, true);
        for (std::size_t start = 0U; start < paths.size();) {
            const std::size_t end = std::min(paths.find('\n', start), paths.size());
            if (end > start)
                files.files.push_back(paths.substr(start, end - start));
            start = end + 1U;
        }
        files.timestamp_nanos = value.timestamp_nanoseconds;
        return files;
    }
    case STRATA_INPUT_PLATFORM_EVENT: {
        strata::ui::PlatformInputEvent said;
        said.kind = strata::ui::PlatformInputKind::event;
        said.result_json = input_text(value.text, false);
        said.timestamp_nanos = value.timestamp_nanoseconds;
        return said;
    }
    case STRATA_INPUT_PLATFORM_RESULT: {
        strata::ui::PlatformInputEvent answer;
        answer.kind = strata::ui::PlatformInputKind::result;
        answer.request = value.selection_start;
        answer.result_json = input_text(value.text, true);
        answer.timestamp_nanos = value.timestamp_nanoseconds;
        return answer;
    }
    default:
        throw std::invalid_argument("input event kind is invalid");
    }
}

} // namespace

namespace strata::abi_detail {

void destroy_surface(strata_surface* const surface, const bool unlink_from_runtime) noexcept {
    if (surface == nullptr)
        return;
    if (unlink_from_runtime && surface->owner != nullptr) {
        std::erase(surface->owner->surfaces, surface);
    }
    const base::HostAllocator allocator = surface->allocator;
    std::destroy_at(surface);
    allocator.deallocate(surface, sizeof(strata_surface), alignof(strata_surface));
}

} // namespace strata::abi_detail

extern "C" {

strata_result strata_runtime_create_surface(strata_runtime* const runtime,
                                            const strata_surface_config* const provided_config,
                                            strata_surface** const out_surface) noexcept {
    if (out_surface != nullptr)
        *out_surface = nullptr;
    const EntryGuard entry(runtime, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (runtime == nullptr)
        return invalid_argument();
    // Fields newer than the caller's layout (such as `extensions`) read as absent.
    const std::optional<strata_surface_config> copied_config =
        strata::abi_detail::read_struct(provided_config);
    const strata_surface_config* const config =
        copied_config.has_value() ? &*copied_config : nullptr;
    if (config == nullptr || out_surface == nullptr || !valid_view(config->id, false) ||
        !valid_view(config->root_name, false) || config->reserved != 0U) {
        return runtime_failure(*runtime, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.ABI.INVALID_SURFACE_CONFIG",
                               "Surface creation requires a complete configuration, id, root "
                               "role/name, and environment.");
    }
    if (!runtime->core.has_application()) {
        return runtime_failure(*runtime, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.APPLICATION.NOT_CONFIGURED",
                               "Configure an application before creating a surface.");
    }
    const strata::base::HostAllocator allocator = runtime->core.allocator();
    void* const storage = allocator.allocate(sizeof(strata_surface), alignof(strata_surface));
    if (storage == nullptr) {
        return runtime_failure(*runtime, STRATA_STATUS_OUT_OF_MEMORY, "STRATA.CORE.OUT_OF_MEMORY",
                               "The surface handle could not be allocated.");
    }
    constexpr AbiCall call{"Surface creation", "STRATA.SURFACE.INVALID_CONFIGURATION"};
    return strata::abi_detail::runtime_call(runtime, call, [&] {
        try {
            std::string id = copied_string(config->id);
            std::string root_name = copied_string(config->root_name);
            if (!strata::base::valid_utf8(id) || !strata::base::valid_utf8(root_name)) {
                throw std::invalid_argument("surface id and root name must be valid UTF-8");
            }
            if (std::ranges::any_of(runtime->surfaces, [&](const strata_surface* const surface) {
                    return surface != nullptr && surface->core.id() == id;
                })) {
                throw std::invalid_argument("Surface ids must be unique within a Runtime");
            }
            const strata_surface_extension_bundle* const extension_bundle = config->extensions;
            strata::abi_detail::ExtensionRegistries extensions =
                strata::abi_detail::extension_registries(extension_bundle,
                                                         runtime->core.diagnostics());
            std::vector<strata_surface_font_binding> owned_fonts = font_bindings(*config);
            std::vector<strata_surface_image_binding> owned_image_bindings =
                image_bindings(*config);
            std::string owned_resource_namespace = runtime->allocate_surface_resource_namespace();
            const ResourceLoader resources(*runtime);
            std::shared_ptr<const strata::ui::TextEngine> owned_text_engine =
                text_engine(resources, owned_fonts);
            MaterializedImages owned_images =
                image_resources(resources, owned_image_bindings, owned_resource_namespace,
                                &runtime->core.images());
            strata_surface* const created = std::construct_at(
                static_cast<strata_surface*>(storage), allocator, runtime, std::move(id),
                root_role(config->root_role), std::move(root_name),
                std::move(owned_resource_namespace), environment(config->environment),
                std::move(owned_fonts), std::move(owned_image_bindings),
                std::move(owned_text_engine), std::move(owned_images.svg_images),
                std::move(owned_images.textures), std::move(extensions.widgets),
                std::move(extensions.behaviors));
            try {
                runtime->surfaces.push_back(created);
            } catch (...) {
                std::destroy_at(created);
                throw;
            }
            *out_surface = created;
            return strata::abi_detail::result(STRATA_STATUS_OK);
        } catch (...) {
            if (*out_surface == nullptr) {
                allocator.deallocate(storage, sizeof(strata_surface), alignof(strata_surface));
            }
            throw;
        }
    });
}

strata_result strata_surface_prepare_release_packet(strata_surface* const surface,
                                                    const strata_bytes_sink* const sink) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr || surface->owner == nullptr)
        return invalid_argument();
    if (sink == nullptr || !has_struct(sink) || sink->emit == nullptr) {
        return surface_failure(*surface, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.ABI.INVALID_BYTES_SINK",
                               "Preparing a Surface release packet requires a complete byte sink.");
    }
    return surface_call(surface, AbiCall{"Surface release-packet preparation or delivery"}, [&] {
        if (!surface->packets.release_prepared()) {
            static_cast<void>(surface->packets.prepare_release(surface->core));
            surface->frame_json.clear();
            surface->frame_snapshot_available = false;
        }
        const std::vector<std::uint8_t>& packet = surface->packets.packet();
        sink->emit(sink->user_data, strata_bytes_view{packet.data(), packet.size()});
        return strata::abi_detail::result(STRATA_STATUS_OK);
    });
}

strata_result strata_surface_acknowledge_release_packet(strata_surface* const surface) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr || surface->owner == nullptr)
        return invalid_argument();
    if (!surface->packets.release_prepared()) {
        return surface_failure(
            *surface, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.SURFACE.RELEASE_PACKET_NOT_PREPARED",
            "Release-packet consumption cannot be acknowledged before terminal preparation.");
    }
    surface->release_packet_consumed = true;
    return strata::abi_detail::result(STRATA_STATUS_OK);
}

strata_result strata_surface_release(strata_surface* const surface) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr)
        return strata::abi_detail::result(STRATA_STATUS_OK);
    if (surface->owner == nullptr)
        return invalid_argument();
    if (!surface->packets.release_prepared() || !surface->release_packet_consumed) {
        return surface_failure(*surface, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.SURFACE.RELEASE_BARRIER_INCOMPLETE",
                               "Surface release requires a prepared, synchronously consumed, and "
                               "acknowledged terminal packet.");
    }
    strata::abi_detail::destroy_surface(surface, true);
    return strata::abi_detail::result(STRATA_STATUS_OK);
}

strata_result strata_surface_abandon(strata_surface* const surface) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr)
        return strata::abi_detail::result(STRATA_STATUS_OK);
    if (surface->owner == nullptr)
        return invalid_argument();
    const strata_result warning = strata::abi_detail::emit(surface->owner->core.diagnostics(), 
        STRATA_STATUS_OK, STRATA_DIAGNOSTIC_WARNING, "STRATA.SURFACE.RELEASE_ABANDONED",
        "Surface ownership was abandoned without the ordered terminal packet barrier; the host "
        "must independently discard any remaining GPU resources.");
    strata::abi_detail::destroy_surface(surface, true);
    return warning;
}

strata_result strata_surface_adopt_environment(strata_surface* const surface,
                                               const strata_surface_environment* const value,
                                               uint32_t* const out_adopted) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr)
        return invalid_argument();
    if (surface->packets.release_prepared())
        return terminal_surface_failure(*surface);
    if (out_adopted != nullptr)
        *out_adopted = 0U;
    if (value == nullptr || out_adopted == nullptr) {
        return surface_failure(
            *surface, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.ABI.INVALID_SURFACE_ENVIRONMENT",
            "Environment adoption requires a complete environment and output pointer.");
    }
    constexpr AbiCall call{"Environment adoption", "STRATA.SURFACE.INVALID_ENVIRONMENT"};
    return surface_call(surface, call, [&] {
        *out_adopted = surface->core.adopt_environment(environment(*value)) ? 1U : 0U;
        return strata::abi_detail::result(STRATA_STATUS_OK);
    });
}

strata_result
strata_surface_enqueue_input(strata_surface* const surface, const strata_input_event* const events,
                             const size_t event_count,
                             strata_surface_input_batch_info* const out_info) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr)
        return invalid_argument();
    if (surface->packets.release_prepared())
        return terminal_surface_failure(*surface);
    if (out_info == nullptr || !has_struct(out_info) || (events == nullptr && event_count != 0U)) {
        return surface_failure(
            *surface, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.ABI.INVALID_INPUT_BATCH",
            "Input enqueue requires a contiguous event array and complete output structure.");
    }
    static_cast<void>(write_struct(
        out_info, strata_surface_input_batch_info{
                      sizeof(strata_surface_input_batch_info),
                      0U,
                      static_cast<std::uint64_t>(surface->core.input().queued_event_count()),
                  }));
    return surface_call(surface, AbiCall{"Input batch enqueue", "STRATA.INPUT.INVALID_EVENT"}, [&] {
        std::vector<strata::ui::SurfaceInputEvent> decoded;
        decoded.reserve(event_count);
        for (std::size_t index = 0U; index < event_count; ++index) {
            const std::optional<strata_input_event> event =
                strata::abi_detail::read_struct_element(events, index);
            if (!event.has_value())
                throw std::invalid_argument("input event structure is incomplete");
            decoded.push_back(input_event(*event));
        }
        const strata::ui::InputOperationResult enqueued =
            surface->core.input().enqueue(std::move(decoded));
        out_info->accepted_event_count = enqueued.injected_events;
        out_info->queued_event_count = surface->core.input().queued_event_count();
        if (enqueued.injected_events == event_count) {
            return strata::abi_detail::result(STRATA_STATUS_OK);
        }
        return surface_failure(*surface, STRATA_STATUS_INPUT_QUEUE_FULL,
                               "STRATA.UI.INPUT_QUEUE_FULL",
                               "The atomic surface input batch exceeded the bounded input queue.");
    });
}

strata_result strata_surface_cancel_interactions(strata_surface* const surface) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr)
        return invalid_argument();
    if (surface->packets.release_prepared())
        return terminal_surface_failure(*surface);
    return surface_call(surface, AbiCall{"Interaction cancellation"}, [&] {
        surface->core.cancel_interactions();
        return strata::abi_detail::result(STRATA_STATUS_OK);
    });
}

strata_result strata_surface_reveal(strata_surface* const surface) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr)
        return invalid_argument();
    if (surface->packets.release_prepared())
        return terminal_surface_failure(*surface);
    return surface_call(surface, AbiCall{"Surface reveal"}, [&] {
        surface->core.reveal();
        return strata::abi_detail::result(STRATA_STATUS_OK);
    });
}

strata_result strata_surface_report_backdrop(strata_surface* const surface,
                                             const strata_backdrop_sample* const samples,
                                             const size_t sample_count) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr || (samples == nullptr && sample_count != 0U))
        return invalid_argument();
    if (surface->packets.release_prepared())
        return terminal_surface_failure(*surface);
    return surface_call(surface, AbiCall{"Surface backdrop report"}, [&] {
        std::vector<strata::ui::Surface::BackdropSample> reports;
        reports.reserve(sample_count);
        for (size_t index = 0U; index < sample_count; ++index)
            reports.push_back({samples[index].probe, samples[index].luminance});
        surface->core.report_backdrop(reports);
        return strata::abi_detail::result(STRATA_STATUS_OK);
    });
}

strata_result
strata_surface_dispatch_action_json(strata_surface* const surface,
                                    const strata_action_dispatch_config* const config,
                                    strata_action_dispatch_info* const out_info) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr)
        return invalid_argument();
    if (surface->packets.release_prepared())
        return terminal_surface_failure(*surface);
    if (!strata::abi_detail::valid_action_dispatch_config(config) || out_info == nullptr ||
        !has_struct(out_info)) {
        return surface_failure(*surface, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.ABI.INVALID_SURFACE_ACTION_DISPATCH",
                               "Surface action dispatch requires an action id, payload/event JSON, "
                               "event kind, and output structure.");
    }
    static_cast<void>(write_struct(out_info, strata_action_dispatch_info{
                                                 sizeof(strata_action_dispatch_info),
                                                 STRATA_ACTION_DISPATCH_FAILED,
                                                 0U,
                                                 0U,
                                             }));
    constexpr AbiCall call{
        "Surface action dispatch",
        "STRATA.ACTION.INVALID_SURFACE_DISPATCH",
        "STRATA.ACTION.INVALID_JSON",
    };
    return surface_call(surface, call, [&] {
        const std::string source = copied_string(config->source_key);
        const strata::runtime::ActionDispatchOutcome outcome = surface->core.dispatch_action(
            copied_string(config->action_id),
            strata::abi_detail::decoded_value(config->payload_json, true),
            copied_string(config->event_kind),
            source.empty() ? std::nullopt : std::optional<std::string>(source),
            strata::abi_detail::decoded_value(config->event_value_json, true),
            strata::abi_detail::dynamic_action_dispatch(*config));
        static_cast<void>(
            write_struct(out_info, strata_action_dispatch_info{
                                       sizeof(strata_action_dispatch_info),
                                       strata::abi_detail::dispatch_status(outcome.status),
                                       0U,
                                       static_cast<std::uint64_t>(outcome.handler_owners.size()),
                                   }));
        return strata::abi_detail::result(STRATA_STATUS_OK);
    });
}

strata_result strata_surface_frame(strata_surface* const surface,
                                   const int64_t frame_time_nanoseconds,
                                   strata_surface_frame_info* const out_info) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr)
        return invalid_argument();
    if (surface->packets.release_prepared())
        return terminal_surface_failure(*surface);
    if (out_info == nullptr || !has_struct(out_info)) {
        return surface_failure(*surface, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.ABI.INVALID_SURFACE_FRAME_OUTPUT",
                               "Surface framing requires a complete output structure.");
    }
    *out_info = {};
    out_info->struct_size = sizeof(strata_surface_frame_info);
    return surface_call(surface, AbiCall{"Surface framing", "STRATA.SURFACE.INVALID_FRAME"}, [&] {
        const strata::ui::SurfaceFrame frame = surface->core.frame(frame_time_nanoseconds);
        const std::vector<std::uint8_t>& packet =
            surface->packets.encode(surface->core, frame, surface->owner->core.images());
        // Canonical inspection JSON is expensive and optional; materialize it on first read.
        surface->frame_json.clear();
        surface->frame_snapshot_available = true;
        static_cast<void>(write_struct(
            out_info, strata_surface_frame_info{
                          sizeof(strata_surface_frame_info),
                          frame.frame_index,
                          frame.frame_time_nanos,
                          static_cast<std::uint64_t>(frame.operations.input_events_processed),
                          static_cast<std::uint64_t>(frame.lifecycle_input.events.size()),
                          static_cast<std::uint64_t>(frame.lifecycle_input.action_outcomes.size()),
                          static_cast<std::uint64_t>(surface->core.render_commands().size()),
                          surface->core.semantics().generation(),
                          static_cast<std::uint64_t>(surface->core.input().queued_event_count()),
                          static_cast<std::uint64_t>(packet.size()),
                      }));
        return strata::abi_detail::result(STRATA_STATUS_OK);
    });
}

strata_result strata_surface_read_frame_json(const strata_surface* const surface,
                                             const strata_value_json_sink* const sink) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr)
        return invalid_argument();
    if (sink == nullptr || !has_struct(sink) || sink->emit == nullptr) {
        return surface_failure(*surface, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.ABI.INVALID_VALUE_SINK",
                               "Reading a surface frame requires a complete JSON sink.");
    }
    if (!surface->frame_snapshot_available) {
        return surface_failure(*surface, STRATA_STATUS_NOT_FOUND,
                               "STRATA.SURFACE.FRAME_UNAVAILABLE",
                               "Frame the surface before reading its canonical frame snapshot.");
    }
    return surface_call(surface, AbiCall{"Surface frame JSON delivery"}, [&] {
        if (surface->frame_json.empty()) {
            const auto inspection_started = std::chrono::steady_clock::now();
            // Compact: a deep tree spends most of an indented document on whitespace. Hosts that
            // show frames to people re-encode them canonically (see the headless capture writer).
            surface->frame_json = strata::data::encode_json_line(
                strata::ui::surface_frame_snapshot(surface->core, surface->core.last_frame()));
            const std::int64_t inspection_nanos =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - inspection_started)
                    .count();
            auto& profiler = const_cast<strata::ui::Surface&>(surface->core).profiler();
            profiler.record(strata::runtime::ProfilerCounter::ui_inspector_nanos,
                            static_cast<std::uint64_t>(inspection_nanos));
            profiler.record_external_timing("inspection", inspection_nanos);
        }
        sink->emit(sink->user_data,
                   strata_string_view{surface->frame_json.data(), surface->frame_json.size()});
        return strata::abi_detail::result(STRATA_STATUS_OK);
    });
}

strata_result
strata_surface_read_diagnostics(const strata_surface* const surface,
                                const strata_diagnostics_snapshot_sink* const sink) noexcept {
    if (surface == nullptr)
        return invalid_argument();
    return strata_runtime_read_diagnostics(surface->owner, sink);
}

strata_result strata_surface_clear_diagnostics(strata_surface* const surface) noexcept {
    if (surface == nullptr)
        return invalid_argument();
    return strata_runtime_clear_diagnostics(surface->owner);
}

strata_result strata_surface_drain_events_json(strata_surface* const surface,
                                               const strata_value_json_sink* const sink) noexcept {
    const EntryGuard entry(surface, Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr)
        return invalid_argument();
    if (surface->packets.release_prepared())
        return terminal_surface_failure(*surface);
    if (sink == nullptr || !has_struct(sink) || sink->emit == nullptr) {
        return surface_failure(*surface, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.ABI.INVALID_VALUE_SINK",
                               "Draining surface events requires a complete JSON sink.");
    }
    return surface_call(surface, AbiCall{"Surface event delivery"}, [&] {
        strata::ui::SurfaceEventDrain drain = surface->core.drain_events();
        strata::data::JsonValue::Array records;
        records.reserve(drain.records.size());
        for (strata::ui::SurfaceEventRecord& record : drain.records) {
            records.emplace_back(strata::data::JsonValue::Object{
                {"sequence", strata::data::JsonValue(static_cast<std::int64_t>(record.sequence))},
                {"frameIndex",
                 strata::data::JsonValue(static_cast<std::int64_t>(record.frame_index))},
                {"event", std::move(record.event)},
                {"actionOutcome", std::move(record.action_outcome)},
            });
        }
        const std::string encoded = strata::data::encode_canonical_json(
            strata::data::JsonValue(strata::data::JsonValue::Object{
                {"droppedCount",
                 strata::data::JsonValue(static_cast<std::int64_t>(drain.dropped_count))},
                {"records", strata::data::JsonValue(std::move(records))},
            }));
        sink->emit(sink->user_data, strata_string_view{encoded.data(), encoded.size()});
        return strata::abi_detail::result(STRATA_STATUS_OK);
    });
}

strata_result strata_surface_read_render_packet(const strata_surface* const surface,
                                                const strata_bytes_sink* const sink) noexcept {
    const EntryGuard entry(surface, Reentry::allowed);
    if (entry.refused())
        return entry.refusal();
    if (surface == nullptr)
        return invalid_argument();
    if (sink == nullptr || !has_struct(sink) || sink->emit == nullptr) {
        return surface_failure(*surface, STRATA_STATUS_INVALID_ARGUMENT,
                               "STRATA.ABI.INVALID_BYTES_SINK",
                               "Reading a surface render packet requires a complete byte sink.");
    }
    const std::vector<std::uint8_t>& packet = surface->packets.packet();
    if (packet.empty()) {
        return surface_failure(*surface, STRATA_STATUS_NOT_FOUND,
                               "STRATA.SURFACE.FRAME_UNAVAILABLE",
                               "Frame the surface before reading its render packet.");
    }
    return surface_call(surface, AbiCall{"Surface render packet delivery"}, [&] {
        sink->emit(sink->user_data, strata_bytes_view{packet.data(), packet.size()});
        return strata::abi_detail::result(STRATA_STATUS_OK);
    });
}

} // extern "C"
