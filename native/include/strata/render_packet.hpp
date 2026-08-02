#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace strata::host {

/** Maximum balanced CONTENT-effect nesting accepted by the packet and bundled renderers. */
inline constexpr std::size_t maximum_content_effect_depth = 4U;

inline constexpr std::uint32_t resource_create = 0U;
inline constexpr std::uint32_t resource_upload = 1U;
inline constexpr std::uint32_t resource_release = 2U;
inline constexpr std::uint32_t resource_encoded_image = 3U;
inline constexpr std::uint32_t packet_flag_geometry_payload = 1U;

inline constexpr std::uint32_t texture_format_r8 = 0U;
inline constexpr std::uint32_t texture_format_rgba8 = 1U;
inline constexpr std::uint32_t texture_sampling_nearest = 0U;
inline constexpr std::uint32_t texture_sampling_linear = 1U;

/** One ordered GPU-resource mutation decoded from the current packet. */
struct ResourceOperation final {
    std::uint32_t kind = 0U;
    std::string texture;
    std::uint32_t format = 0U;
    std::uint32_t sampling = 0U;
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<std::uint8_t> bytes;
};

struct Scissor final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

/** Indexed geometry submission. Vertex records use the fixed 88-byte layout. */
struct DrawBatch final {
    std::uint32_t source_order = 0U;
    Scissor scissor;
    std::string material;
    std::string blend_mode;
    std::optional<std::string> texture;
    std::uint32_t base_vertex = 0U;
    std::uint32_t first_index = 0U;
    std::uint32_t index_count = 0U;
};

struct BlurBatch final {
    std::uint32_t source_order = 0U;
    Scissor scissor;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    double radius = 0.0;
    std::uint32_t downsample = 1U;
};

enum class EffectBatchKind : std::uint32_t {
    backdrop = 2U,
    content_begin = 3U,
};

struct EffectBatch final {
    EffectBatchKind kind = EffectBatchKind::backdrop;
    std::uint32_t source_order = 0U;
    Scissor scissor;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    std::array<double, 4U> radii{};
    std::string effect;
    double opacity = 1.0;
    std::array<double, 16U> parameters{};
    std::uint32_t parameter_count = 0U;
};

struct ContentEffectEndBatch final {
    std::uint32_t source_order = 0U;
    Scissor scissor;
};

using SubmissionBatch = std::variant<
    DrawBatch,
    BlurBatch,
    EffectBatch,
    ContentEffectEndBatch
>;

/** Backend-ready render plan. Geometry is retained when a packet repeats its geometry epoch. */
struct RenderPacket final {
    std::uint64_t frame_index = 0U;
    std::uint64_t geometry_epoch = 0U;
    std::uint32_t planned_draw_count = 0U;
    std::uint32_t skipped_draw_count = 0U;
    std::vector<ResourceOperation> resources;
    std::vector<std::uint8_t> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<SubmissionBatch> batches;
};

/**
 * Stateful decoder for one ordered Surface/backend packet stream. Compact packets depend on the
 * latest full geometry epoch, so consume every framed packet and reset only when discarding the
 * stream.
 */
class RenderPacketDecoder final {
  public:
    [[nodiscard]] const RenderPacket& decode(std::span<const std::uint8_t> bytes);
    void reset() noexcept;

  private:
    std::optional<RenderPacket> retained_;
};

} // namespace strata::host
