#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <strata/diagnostic.hpp>
#include <strata/input.hpp>

namespace strata {

inline constexpr strata_capabilities portable_host_capabilities =
    STRATA_CAPABILITY_CORE_LIFECYCLE |
    STRATA_CAPABILITY_CALLER_CLOCK |
    STRATA_CAPABILITY_IMMUTABLE_SNAPSHOTS |
    STRATA_CAPABILITY_STABLE_IDENTITIES |
    STRATA_CAPABILITY_HOST_SNAPSHOTS |
    STRATA_CAPABILITY_VALUE_JSON |
    STRATA_CAPABILITY_APPLICATION_LIFECYCLE |
    STRATA_CAPABILITY_COMPILER_ACTIVATION |
    STRATA_CAPABILITY_COMPILED_MODULE_ACTIVATION |
    STRATA_CAPABILITY_ACTION_DISPATCH |
    STRATA_CAPABILITY_RESOURCE_ADAPTER |
    STRATA_CAPABILITY_CLIPBOARD_IME_ADAPTER |
    STRATA_CAPABILITY_EFFECT_ADAPTER |
    STRATA_CAPABILITY_SURFACE_RUNTIME |
    STRATA_CAPABILITY_SURFACE_RENDER_PACKET |
    STRATA_CAPABILITY_SURFACE_EXTENSIONS |
    STRATA_CAPABILITY_ALLOCATOR_TELEMETRY |
    STRATA_CAPABILITY_SOURCE_MAP_LOOKUP |
    STRATA_CAPABILITY_SURFACE_EVENT_DRAIN |
    STRATA_CAPABILITY_DIAGNOSTIC_SNAPSHOTS |
    STRATA_CAPABILITY_PROFILER_SNAPSHOTS |
    STRATA_CAPABILITY_SURFACE_THEMES |
    STRATA_CAPABILITY_DURABLE_STATE |
    STRATA_CAPABILITY_ASYNC_HOST_DATA;

struct RuntimeOptions final {
    strata_capabilities required_capabilities = portable_host_capabilities;
    std::uint64_t stable_identity_seed = UINT64_C(0x5354524154414350);
    std::function<std::int64_t()> clock = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    };
    std::function<void(const Diagnostic&)> diagnostic{};
    /** Optional low-level allocator escape hatch; its callbacks must outlive Runtime. */
    strata_allocator allocator{};
};

struct ApplicationOptions final {
    std::string id{};
    std::string schemas_json{};
    std::vector<std::string> extension_schemas_json{};
};

struct SourceModule final {
    std::string source_id{};
    std::string text{};
};

struct SourceActivation final {
    using Loader = std::function<std::optional<SourceModule>(
        std::string_view importer_source_id,
        std::string_view import_path
    )>;

    std::uint64_t generation = 1U;
    std::string entry_source_id{};
    std::string entry_text{};
    Loader load_module{};
};

enum class ActivationStatus {
    activated,
    rejected_generation,
    rejected_compile,
    rejected_unit,
    rejected_capability,
};

struct ActivationInfo final {
    ActivationStatus status = ActivationStatus::rejected_unit;
    bool state_migrated = false;
    std::uint64_t attempted_generation = 0U;
    std::optional<std::uint64_t> active_generation;
    std::uint64_t diagnostic_count = 0U;

    [[nodiscard]] bool activated() const noexcept {
        return status == ActivationStatus::activated;
    }
};

enum class ActionDispatchStatus {
    no_action,
    handled,
    forwarded,
    ignored,
    unhandled,
    failed,
};

struct ActionDispatch final {
    std::string action_id{};
    std::string payload_json = "null";
    std::string event_kind = "host";
    std::optional<std::string> source_key{};
    std::string event_value_json = "null";
    std::optional<std::string> state_scope{};
    bool dynamic = false;
};

struct ActionDispatchInfo final {
    ActionDispatchStatus status = ActionDispatchStatus::no_action;
    std::uint64_t handler_count = 0U;
};

struct MaterialDeclaration final {
    std::string id{};
    std::string blend_mode{};
    std::string fallback{};
    std::string source{};
};

enum class EffectPassKind { blur, shader, shadow };

struct EffectPassDeclaration final {
    std::string effect_id{};
    std::uint32_t index = 0U;
    EffectPassKind kind = EffectPassKind::blur;
    double radius = 0.0;
    std::uint32_t downsample = 1U;
    std::optional<std::uint32_t> radius_parameter{};
    std::optional<std::uint32_t> downsample_parameter{};
    std::string source{};
};

enum class SurfaceRootRole { screen, overlay };

struct FontResource final {
    std::string id{};
    std::string resource_id{};
};

enum class ImageSampling { nearest, linear };

/** One PNG or static SVG exposed to authored Image/icon properties under a logical id. */
struct ImageResource final {
    std::string id{};
    std::string resource_id{};
    ImageSampling sampling = ImageSampling::linear;
};

/** Owned creation document. All ABI views are generated transiently inside create_surface(). */
struct SurfaceOptions final {
    std::string id{};
    SurfaceRootRole root_role = SurfaceRootRole::screen;
    std::string root_name{};
    SurfaceEnvironment environment{};
    std::vector<FontResource> fonts{};
    std::vector<ImageResource> images{};
    /** Extension descriptor ownership remains with the caller for the Surface lifetime. */
    const strata_surface_extension_bundle* extensions = nullptr;
};

} // namespace strata
