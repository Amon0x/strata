#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace strata::host {

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

using SubmissionBatch = std::variant<DrawBatch, BlurBatch>;

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

class RenderPacketDecoder final {
public:
    [[nodiscard]] const RenderPacket& decode(std::span<const std::uint8_t> bytes);
    void reset() noexcept;

private:
    std::optional<RenderPacket> retained_;
};

} // namespace strata::host
