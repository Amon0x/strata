#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <strata/host.hpp>
#include <strata/render_packet.hpp>
#include <strata/strata.hpp>

#include "host/extensions.hpp"

namespace strata::host {

/** Capabilities every Session requires; installed adapters add their own. */
inline constexpr strata_capabilities session_capabilities =
    STRATA_CAPABILITY_CORE_LIFECYCLE | STRATA_CAPABILITY_CALLER_CLOCK |
    STRATA_CAPABILITY_HOST_SNAPSHOTS | STRATA_CAPABILITY_APPLICATION_LIFECYCLE |
    STRATA_CAPABILITY_COMPILER_ACTIVATION | STRATA_CAPABILITY_ACTION_DISPATCH |
    STRATA_CAPABILITY_RESOURCE_ADAPTER | STRATA_CAPABILITY_SURFACE_RUNTIME |
    STRATA_CAPABILITY_SURFACE_RENDER_PACKET;

/** What a host decides before a Session's runtime exists. */
struct SessionOptions final {
    std::string application_id;
    std::uint64_t stable_identity_seed = 0U;
    /** Capabilities required beyond session_capabilities and the installed adapters'. */
    strata_capabilities capabilities = 0U;
    /** Application schema document; empty when the application declares none. */
    std::string schemas;
    /**
     * Extension packages for an application whose schemas declare none. When the schemas declare
     * packages, a non-empty list must name the same ones.
     */
    std::vector<std::string> extension_packages;
    std::vector<std::filesystem::path> extension_search_paths;
    /**
     * Reads a source module by resource id. Throws host::ResourceNotFound when it is missing and
     * std::invalid_argument when the id is not a resource id.
     */
    std::function<std::string(std::string_view resource_id)> read_text;
    /** Receives every diagnostic the runtime publishes. */
    std::function<void(const strata_diagnostic&)> on_diagnostic;
    /** Owner label of the Session's host bindings; empty uses the application id. */
    std::string bindings_owner;
};

/**
 * Host services for the runtime. Callback owners must outlive the Session. Resources, clipboard,
 * IME and effects are installed before the application is configured; durable state and async
 * host data, which belong to the configured application, after it.
 */
struct SessionAdapters final {
    std::optional<strata_resource_adapter> resources;
    std::optional<strata_clipboard_adapter> clipboard;
    std::optional<strata_ime_adapter> ime;
    std::optional<strata_effect_adapter> effects;
    std::optional<strata_durable_store_adapter> durable_store;
    std::optional<strata_async_host_adapter> async_host;
};

struct SessionFont final {
    std::string id;
    /** Bundled face used unless one of `system` is installed. */
    std::string resource;
    /** Installed font file names in preference order, such as "segoeui.ttf". */
    std::vector<std::string> system{};
};

struct SessionImage final {
    std::string id;
    std::string resource;
    std::uint32_t sampling = STRATA_IMAGE_SAMPLING_LINEAR;
};

struct SessionSurface final {
    std::string id;
    strata_surface_root_role root_role = STRATA_SURFACE_ROOT_OVERLAY;
    std::string root_name;
    strata_surface_environment environment{};
    std::vector<SessionFont> fonts;
    std::vector<SessionImage> images;
};

/**
 * One application runtime and its Surface, independent of window system and renderer. A Session
 * owns the runtime's caller clock and diagnostics, installs host adapters, configures the
 * application with its extension packages, activates source or compiled modules, creates the
 * Surface, frames it into decoded packets, records host-frame telemetry, and performs the ordered
 * release barrier. Hosts own windows, input translation and the backend that draws packets.
 */
class Session final {
  public:
    Session(SessionOptions options, const SessionAdapters& adapters);
    /** Abandons a Surface that was not closed; a closed Session releases nothing twice. */
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    [[nodiscard]] strata::Runtime& runtime();
    [[nodiscard]] Bindings& bindings();
    [[nodiscard]] const std::string& application_id() const noexcept;
    [[nodiscard]] const SelectedExtensions& extensions() const noexcept;

    /** The caller clock the runtime reads; frame() advances it. */
    [[nodiscard]] std::int64_t time() const noexcept;
    void set_time(std::int64_t time_nanoseconds) noexcept;

    /**
     * Compiles `module_id` read through `read_text`, resolving imports relative to the importer.
     * Every module id read is remembered in module_resources().
     */
    [[nodiscard]] strata_activation_info activate_source(std::string_view module_id,
                                                         std::uint64_t generation);
    /** Activates source text already read, for a host that keeps the accepted text itself. */
    [[nodiscard]] strata_activation_info activate_source(std::string_view module_id,
                                                         std::string_view text,
                                                         std::uint64_t generation);
    [[nodiscard]] strata_activation_info activate_compiled(std::span<const std::uint8_t> artifact,
                                                           std::uint64_t generation);
    [[nodiscard]] const std::vector<std::string>& module_resources() const noexcept;

    /** Creates the Session's one Surface; fonts prefer an installed `system` face. */
    void create_surface(const SessionSurface& surface);
    [[nodiscard]] bool has_surface() const noexcept;
    [[nodiscard]] strata::Surface& surface();
    /** Adopts a new environment generation; false when the Surface rejected it. */
    [[nodiscard]] bool adopt_environment(const strata_surface_environment& environment);
    /** Enqueues a batch atomically; throws unless every event was accepted. */
    void enqueue(std::span<const strata_input_event> events);

    /** Synchronizes host bindings, frames the Surface at `time_nanoseconds`, decodes its packet. */
    [[nodiscard]] SurfacePacketFrame frame(std::int64_t time_nanoseconds);
    /** Publishes the backend's work for the packet the latest frame produced. */
    void record_host_frame(const RenderPacket& packet, std::int64_t submit_nanoseconds = 0);

    /**
     * The ordered release barrier: prepares the terminal packet, hands it to `consume`, which must
     * release the backend's resources before returning, acknowledges it and closes the Surface.
     */
    void close_surface(const std::function<void(const RenderPacket&)>& consume);
    /** Closes the Surface as close_surface does, then the runtime. */
    void close(const std::function<void(const RenderPacket&)>& consume);
    /** Gives up the Surface without the barrier; the host discards its backend resources. */
    void abandon() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::host
