#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <strata/strata.h>

#include "core/allocator.hpp"
#include "core/runtime.hpp"
#include "core/utf8.hpp"
#include "font/atlas.hpp"
#include "resource/image.hpp"
#include "resource/svg_image.hpp"
#include "runtime/application.hpp"
#include "runtime/host_services.hpp"
#include "ui/render/packet.hpp"
#include "ui/surface.hpp"

struct strata_surface;

namespace strata::abi_detail {

/** Allocates a process-unique resource-owner identity without ever publishing zero. */
inline std::uint64_t allocate_runtime_resource_owner_id() {
    static std::atomic<std::uint64_t> next{1U};
    std::uint64_t candidate = next.load(std::memory_order_relaxed);
    for (;;) {
        if (candidate == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("runtime resource-owner identity exhausted");
        }
        if (next.compare_exchange_weak(
                candidate,
                candidate + 1U,
                std::memory_order_relaxed,
                std::memory_order_relaxed
            )) {
            return candidate;
        }
    }
}

} // namespace strata::abi_detail

struct strata_surface_font_binding final {
    std::string id;
    std::string resource_id;
};

struct strata_surface_image_binding final {
    std::string id;
    std::string resource_id;
    strata_image_sampling sampling = STRATA_IMAGE_SAMPLING_LINEAR;
};

struct strata_runtime final {
    strata_runtime(
        const strata::core::HostAllocator allocator,
        const strata_clock clock,
        const strata_diagnostic_sink diagnostics,
        const std::uint64_t stable_identity_seed
    )
        : resource_owner_id(strata::abi_detail::allocate_runtime_resource_owner_id()),
          core(allocator, clock, diagnostics, stable_identity_seed),
          host_services(
              [this] { return clipboard.has_value(); },
              [this] {
                  if (!clipboard.has_value()) {
                      return strata::runtime::HostClipboardRead{
                          STRATA_STATUS_SERVICE_UNAVAILABLE, std::nullopt,
                      };
                  }
                  strata_string_view borrowed{nullptr, 0U};
                  const strata_status status = clipboard->read(
                      clipboard->user_data, &borrowed
                  );
                  if (status != STRATA_STATUS_OK) {
                      return strata::runtime::HostClipboardRead{status, std::nullopt};
                  }
                  if (borrowed.data == nullptr && borrowed.size != 0U) {
                      return strata::runtime::HostClipboardRead{
                          STRATA_STATUS_INVALID_ARGUMENT, std::nullopt,
                      };
                  }
                  std::string copied = borrowed.size == 0U
                      ? std::string{}
                      : std::string(borrowed.data, borrowed.size);
                  if (!strata::core::valid_utf8(copied)) {
                      return strata::runtime::HostClipboardRead{
                          STRATA_STATUS_INVALID_UTF8, std::nullopt,
                      };
                  }
                  return strata::runtime::HostClipboardRead{
                      STRATA_STATUS_OK, std::move(copied),
                  };
              },
              [this](const std::string_view text) {
                  if (!clipboard.has_value()) return STRATA_STATUS_SERVICE_UNAVAILABLE;
                  return clipboard->write(
                      clipboard->user_data,
                      strata_string_view{text.data(), text.size()}
                  );
              },
              [this] { return ime.has_value(); },
              [this](const bool active) {
                  if (!ime.has_value()) return STRATA_STATUS_SERVICE_UNAVAILABLE;
                  return ime->set_active(ime->user_data, active ? 1U : 0U);
              },
              [this](const strata::runtime::HostServiceRect rect) {
                  if (!ime.has_value()) return STRATA_STATUS_SERVICE_UNAVAILABLE;
                  return ime->set_cursor_rect(
                      ime->user_data,
                      strata_rect{rect.x, rect.y, rect.width, rect.height}
                  );
              },
              [this] { return effects.has_value(); },
              [this](const std::string_view id, const std::string_view payload_json) {
                  if (!effects.has_value()) return STRATA_STATUS_SERVICE_UNAVAILABLE;
                  return effects->emit(
                      effects->user_data,
                      strata_string_view{id.data(), id.size()},
                      strata_string_view{payload_json.data(), payload_json.size()}
                  );
              }
          ) {}

    [[nodiscard]] std::string allocate_surface_resource_namespace() {
        if (next_surface_resource_owner_id == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("surface resource-owner identity exhausted");
        }
        std::string result = "strata:native/" + std::to_string(resource_owner_id) + "/" +
            std::to_string(next_surface_resource_owner_id);
        ++next_surface_resource_owner_id;
        return result;
    }

    const std::uint64_t resource_owner_id;
    std::uint64_t next_surface_resource_owner_id = 1U;
    // Adapters outlive core so ApplicationContext's final write-behind flush remains valid.
    std::optional<strata_resource_adapter> resources;
    std::optional<strata_durable_store_adapter> durable_store;
    std::optional<strata_async_host_adapter> async_host;
    std::optional<strata_clipboard_adapter> clipboard;
    std::optional<strata_ime_adapter> ime;
    std::optional<strata_effect_adapter> effects;
    strata::core::Runtime core;
    /** One runtime-owned service boundary shared by every Surface/InputRouter. */
    strata::runtime::HostServices host_services;
    std::vector<strata_surface*> surfaces;
};

struct strata_snapshot final {
    strata_snapshot(
        const strata::core::HostAllocator host_allocator,
        const strata::core::SnapshotData snapshot_data
    ) noexcept
        : allocator(host_allocator), data(snapshot_data) {}

    strata::core::HostAllocator allocator;
    const strata::core::SnapshotData data;
};

struct strata_action_registration final {
    strata_action_registration(
        const strata::core::HostAllocator host_allocator,
        strata::runtime::ActionRegistration host_registration
    ) noexcept
        : allocator(host_allocator), registration(std::move(host_registration)) {}

    strata::core::HostAllocator allocator;
    strata::runtime::ActionRegistration registration;
};

struct strata_application_state_snapshot final {
    strata_application_state_snapshot(
        const strata::core::HostAllocator host_allocator,
        strata::runtime::StateSnapshot snapshot
    ) : allocator(host_allocator), data(std::move(snapshot)) {}

    strata::core::HostAllocator allocator;
    strata::runtime::StateSnapshot data;
};

struct strata_surface final {
    strata_surface(
        const strata::core::HostAllocator host_allocator,
        strata_runtime* const host_runtime,
        std::string id,
        const strata::runtime::LayerRole root_role,
        std::string root_name,
        std::string resource_namespace,
        strata::ui::SurfaceEnvironment environment,
        std::vector<strata_surface_font_binding> font_bindings,
        std::vector<strata_surface_image_binding> owned_image_bindings,
        std::shared_ptr<const strata::ui::TextEngine> text_engine,
        std::shared_ptr<const strata::resource::SvgImageRegistry> svg_images,
        std::vector<strata::resource::EncodedTextureResource> texture_resources,
        strata::ui::WidgetRegistry widget_registry,
        strata::ui::BehaviorRegistry behavior_registry
    )
        : allocator(host_allocator),
          owner(host_runtime),
          host_resource_namespace(std::move(resource_namespace)),
          host_service_owner_token(host_resource_namespace + "/host-owner"),
          glyph_atlas(host_resource_namespace),
          fonts(std::move(font_bindings)),
          image_bindings(std::move(owned_image_bindings)),
          textures(std::move(texture_resources)),
          core(
              std::move(id),
              host_runtime->core.application(),
              root_role,
              std::move(root_name),
              std::move(environment),
              std::move(text_engine),
              std::move(widget_registry),
              std::move(behavior_registry),
              &host_runtime->host_services,
              strata::ui::Theme{},
              host_service_owner_token,
              std::move(svg_images)
          ) {}

    [[nodiscard]] const std::vector<std::uint8_t>& current_render_packet() const noexcept {
        return host_render_packet_cache.packet();
    }

    strata::core::HostAllocator allocator;
    strata_runtime* owner;
    /** Collision-free host-facing namespace; source adapter ids never escape through packets. */
    std::string host_resource_namespace;
    /**
     * Process-unique, non-user-facing owner identity for runtime-shared host-service arbitration.
     * Surface/InputRouter construction must use this token instead of the public Surface id.
     */
    std::string host_service_owner_token;
    strata::font::GlyphAtlas glyph_atlas;
    std::vector<strata_surface_font_binding> fonts;
    std::vector<strata_surface_image_binding> image_bindings;
    std::vector<strata::resource::EncodedTextureResource> textures;
    bool texture_resources_pending = true;
    /** A runtime adapter swap must not mix old materialized resources with the new loader. */
    bool resource_reload_required = false;
    std::uint64_t materialized_resource_generation = 0U;
    std::uint64_t required_resource_generation = 0U;
    /** Preparing the final atlas-release packet is a terminal Surface lifecycle transition. */
    bool release_packet_prepared = false;
    /** Set only after the host confirms synchronous consumption of the terminal packet. */
    bool release_packet_consumed = false;
    strata::ui::HostRenderPacketCache host_render_packet_cache;
    strata::ui::Surface core;
    mutable std::string frame_json;
    bool frame_snapshot_available = false;
};

namespace strata::abi_detail {

void destroy_surface(strata_surface* surface, bool unlink_from_runtime) noexcept;

} // namespace strata::abi_detail
