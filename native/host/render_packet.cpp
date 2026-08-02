#include <strata/render_packet.hpp>
#include <algorithm>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <cmath>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace strata::host {
namespace {

class Reader final {
public:
    explicit Reader(const std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::uint32_t u32() {
        require(4U);
        const std::uint32_t value = static_cast<std::uint32_t>(bytes_[offset_]) |
            static_cast<std::uint32_t>(bytes_[offset_ + 1U]) << 8U |
            static_cast<std::uint32_t>(bytes_[offset_ + 2U]) << 16U |
            static_cast<std::uint32_t>(bytes_[offset_ + 3U]) << 24U;
        offset_ += 4U;
        return value;
    }

    [[nodiscard]] std::uint64_t u64() {
        const std::uint64_t low = u32();
        const std::uint64_t high = u32();
        return low | high << 32U;
    }

    [[nodiscard]] double number() { return std::bit_cast<double>(u64()); }

    [[nodiscard]] bool boolean() {
        const std::uint32_t value = u32();
        if (value > 1U) throw std::invalid_argument("render packet boolean is invalid");
        return value != 0U;
    }

    [[nodiscard]] std::string text() {
        const std::span<const std::uint8_t> value = raw(count());
        if (value.empty()) return {};
        return std::string(reinterpret_cast<const char*>(value.data()), value.size());
    }

    [[nodiscard]] std::uint32_t count() {
        constexpr std::uint32_t maximum = 64U * 1'024U * 1'024U;
        const std::uint32_t value = u32();
        if (value > maximum) throw std::length_error("render packet collection exceeds host limit");
        return value;
    }

    [[nodiscard]] std::span<const std::uint8_t> raw(const std::size_t size) {
        require(size);
        const std::span<const std::uint8_t> result = bytes_.subspan(offset_, size);
        offset_ += size;
        return result;
    }

    [[nodiscard]] Reader record() { return Reader(raw(count())); }

    void exhausted(const std::string_view label) const {
        if (offset_ != bytes_.size()) {
            throw std::invalid_argument(std::string(label) + " contains trailing bytes");
        }
    }

private:
    void require(const std::size_t size) const {
        if (offset_ > bytes_.size() || size > bytes_.size() - offset_) {
            throw std::invalid_argument("render packet is truncated");
        }
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0U;
};

[[nodiscard]] ResourceOperation resource(const std::uint32_t kind, Reader input) {
    ResourceOperation result;
    result.kind = kind;
    result.texture = input.text();
    if (result.texture.empty()) throw std::invalid_argument("render texture id is empty");
    if (kind == 2U) {
        input.exhausted("texture release");
        return result;
    }
    if (kind == 3U) {
        result.format = input.u32();
        result.sampling = input.u32();
        if (result.format != 0U) throw std::invalid_argument("encoded texture format is unknown");
        if (result.sampling > 1U) throw std::invalid_argument("texture sampling is unknown");
        result.width = input.count();
        result.height = input.count();
        const std::span<const std::uint8_t> bytes = input.raw(input.count());
        if (result.width == 0U || result.height == 0U || bytes.empty()) {
            throw std::invalid_argument("encoded texture dimensions and payload must be positive");
        }
        result.bytes.assign(bytes.begin(), bytes.end());
        input.exhausted("encoded texture");
        return result;
    }
    if (kind > 2U) throw std::invalid_argument("render packet resource kind is unknown");
    result.format = input.u32();
    if (result.format > 1U) throw std::invalid_argument("atlas texture format is unknown");
    result.x = input.count();
    result.y = input.count();
    result.width = input.count();
    result.height = input.count();
    if (result.width == 0U || result.height == 0U) {
        throw std::invalid_argument("atlas dimensions must be positive");
    }
    if (kind == 0U && (result.x != 0U || result.y != 0U)) {
        throw std::invalid_argument("atlas create origin must be zero");
    }
    if (kind == 1U) {
        const std::span<const std::uint8_t> bytes = input.raw(input.count());
        const std::uint64_t expected = static_cast<std::uint64_t>(result.width) * result.height *
            (result.format == 0U ? 1U : 4U);
        if (expected != bytes.size()) {
            throw std::invalid_argument("atlas upload byte count does not match its region");
        }
        result.bytes.assign(bytes.begin(), bytes.end());
    }
    input.exhausted("atlas resource");
    return result;
}

[[nodiscard]] Scissor scissor(Reader& input) {
    return Scissor{input.u32(), input.u32(), input.u32(), input.u32()};
}

void validate_geometry(const RenderPacket& packet) {
    const std::size_t vertex_count = packet.vertices.size() / 88U;
    for (const SubmissionBatch& batch : packet.batches) {
        const DrawBatch* draw = std::get_if<DrawBatch>(&batch);
        if (draw == nullptr) continue;
        if (draw->base_vertex > vertex_count || draw->first_index > packet.indices.size() ||
            draw->index_count > packet.indices.size() - draw->first_index) {
            throw std::invalid_argument("render batch geometry range exceeds the packet payload");
        }
        const std::size_t available_vertices = vertex_count - draw->base_vertex;
        const std::size_t end = static_cast<std::size_t>(draw->first_index) + draw->index_count;
        for (std::size_t index = draw->first_index; index < end; ++index) {
            if (packet.indices[index] >= available_vertices) {
                throw std::invalid_argument("render batch index exceeds the packet vertex payload");
            }
        }
    }
}

[[nodiscard]] bool same_batch_shape(
    const std::vector<SubmissionBatch>& left,
    const std::vector<SubmissionBatch>& right
) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (left[index].index() != right[index].index()) return false;
        const EffectBatch* left_effect = std::get_if<EffectBatch>(&left[index]);
        const EffectBatch* right_effect = std::get_if<EffectBatch>(&right[index]);
        if (left_effect != nullptr && right_effect != nullptr &&
            left_effect->kind != right_effect->kind) {
            return false;
        }
    }
    return true;
}

} // namespace

const RenderPacket& RenderPacketDecoder::decode(const std::span<const std::uint8_t> bytes) {
    Reader input(bytes);
    const std::span<const std::uint8_t> magic = input.raw(8U);
    if (std::string_view(reinterpret_cast<const char*>(magic.data()), magic.size()) != "STRATARP" ||
        input.u32() != 6U) {
        throw std::invalid_argument("render packet decoder requires protocol v6");
    }
    const std::uint32_t resource_count = input.count();
    const std::uint32_t batch_count = input.count();
    const std::uint64_t frame_index = input.u64();
    const std::uint64_t geometry_epoch = input.u64();
    const std::uint32_t flags = input.u32();
    if ((flags & ~packet_flag_geometry_payload) != 0U) {
        throw std::invalid_argument("render packet flags are outside the supported domain");
    }
    const bool has_geometry_payload = (flags & packet_flag_geometry_payload) != 0U;
    const std::uint32_t vertex_bytes = input.count();
    const std::uint32_t index_count = input.count();
    const std::uint32_t planned_draw_count = input.count();
    const std::uint32_t skipped_draw_count = input.count();
    if (vertex_bytes % 88U != 0U ||
        index_count > std::numeric_limits<std::uint32_t>::max() / 4U) {
        throw std::invalid_argument("render packet geometry counts are invalid");
    }

    std::vector<ResourceOperation> resources;
    resources.reserve(resource_count);
    for (std::uint32_t index = 0U; index < resource_count; ++index) {
        const std::uint32_t kind = input.u32();
        resources.push_back(resource(kind, input.record()));
    }
    const bool retained_geometry =
        retained_.has_value() && retained_->geometry_epoch == geometry_epoch;
    if (!has_geometry_payload) {
        if (batch_count != 0U || vertex_bytes != 0U || index_count != 0U) {
            throw std::invalid_argument(
                "retained render packet unexpectedly carries geometry counts"
            );
        }
        input.exhausted("retained render packet");
        if (!retained_geometry) {
            throw std::invalid_argument(
                "retained render packet references an unavailable geometry epoch"
            );
        }
        retained_->frame_index = frame_index;
        retained_->planned_draw_count = planned_draw_count;
        retained_->skipped_draw_count = skipped_draw_count;
        retained_->resources = std::move(resources);
        return *retained_;
    }

    RenderPacket result;
    result.frame_index = frame_index;
    result.geometry_epoch = geometry_epoch;
    result.planned_draw_count = planned_draw_count;
    result.skipped_draw_count = skipped_draw_count;
    result.resources = std::move(resources);
    const std::span<const std::uint8_t> vertices = input.raw(vertex_bytes);
    result.vertices.assign(vertices.begin(), vertices.end());
    result.indices.reserve(index_count);
    for (std::uint32_t index = 0U; index < index_count; ++index) result.indices.push_back(input.u32());
    result.batches.reserve(batch_count);
    for (std::uint32_t index = 0U; index < batch_count; ++index) {
        const std::uint32_t kind = input.u32();
        Reader batch = input.record();
        const std::uint32_t source_order = batch.u32();
        const Scissor clip = scissor(batch);
        if (kind == 0U) {
            DrawBatch draw;
            draw.source_order = source_order;
            draw.scissor = clip;
            draw.material = batch.text();
            draw.blend_mode = batch.text();
            if (draw.material.empty() || draw.blend_mode.empty()) {
                throw std::invalid_argument("render batch material and blend mode must not be empty");
            }
            if (batch.boolean()) draw.texture = batch.text();
            draw.base_vertex = batch.u32();
            draw.first_index = batch.u32();
            draw.index_count = batch.u32();
            result.batches.emplace_back(std::move(draw));
        } else if (kind == 1U) {
            BlurBatch blur;
            blur.source_order = source_order;
            blur.scissor = clip;
            blur.x = batch.number();
            blur.y = batch.number();
            blur.width = batch.number();
            blur.height = batch.number();
            blur.radius = batch.number();
            blur.downsample = batch.u32();
            if (!std::isfinite(blur.x) || !std::isfinite(blur.y) ||
                !std::isfinite(blur.width) || !std::isfinite(blur.height) ||
                !std::isfinite(blur.radius) || blur.width < 0.0 || blur.height < 0.0 ||
                blur.radius < 0.0 || blur.downsample == 0U) {
                throw std::invalid_argument("render blur batch is outside the portable domain");
            }
            result.batches.emplace_back(blur);
        } else if (kind == 2U || kind == 3U) {
            EffectBatch effect;
            effect.kind = kind == 2U
                ? EffectBatchKind::backdrop
                : EffectBatchKind::content_begin;
            effect.source_order = source_order;
            effect.scissor = clip;
            effect.x = batch.number();
            effect.y = batch.number();
            effect.width = batch.number();
            effect.height = batch.number();
            for (double& radius : effect.radii) radius = batch.number();
            effect.effect = batch.text();
            effect.opacity = batch.number();
            effect.parameter_count = batch.u32();
            if (effect.effect.empty() || effect.parameter_count > effect.parameters.size() ||
                !std::isfinite(effect.x) || !std::isfinite(effect.y) ||
                !std::isfinite(effect.width) || !std::isfinite(effect.height) ||
                effect.width < 0.0 || effect.height < 0.0 ||
                !std::isfinite(effect.opacity) || effect.opacity < 0.0 ||
                effect.opacity > 1.0 ||
                std::ranges::any_of(effect.radii, [](const double value) {
                    return !std::isfinite(value) || value < 0.0;
                })) {
                throw std::invalid_argument(
                    "render effect batch is outside the portable domain"
                );
            }
            for (std::uint32_t parameter = 0U;
                 parameter < effect.parameter_count;
                 ++parameter) {
                effect.parameters[parameter] = batch.number();
                if (!std::isfinite(effect.parameters[parameter])) {
                    throw std::invalid_argument(
                        "render effect parameter is not finite"
                    );
                }
            }
            result.batches.emplace_back(std::move(effect));
        } else if (kind == 4U) {
            result.batches.emplace_back(ContentEffectEndBatch{source_order, clip});
        } else {
            throw std::invalid_argument("render packet batch kind is unknown");
        }
        batch.exhausted("render batch");
    }
    input.exhausted("render packet");
    std::size_t effect_depth = 0U;
    for (const SubmissionBatch& batch : result.batches) {
        if (const EffectBatch* effect = std::get_if<EffectBatch>(&batch);
            effect != nullptr && effect->kind == EffectBatchKind::content_begin) {
            if (effect_depth == maximum_content_effect_depth) {
                throw std::invalid_argument(
                    "render content effect stack exceeds the maximum depth"
                );
            }
            ++effect_depth;
        } else if (std::holds_alternative<ContentEffectEndBatch>(batch)) {
            if (effect_depth == 0U) {
                throw std::invalid_argument("render content effect stack underflow");
            }
            --effect_depth;
        }
    }
    if (effect_depth != 0U) {
        throw std::invalid_argument("render content effect stack is unbalanced");
    }
    validate_geometry(result);
    if (retained_geometry) {
        if (result.vertices != retained_->vertices ||
            result.indices != retained_->indices) {
            throw std::invalid_argument(
                "retained render geometry epoch changed its vertex or index payload"
            );
        }
        if (!same_batch_shape(result.batches, retained_->batches)) {
            throw std::invalid_argument(
                "retained render geometry epoch changed its batch shape"
            );
        }
    }
    retained_ = std::move(result);
    return *retained_;
}

void RenderPacketDecoder::reset() noexcept { retained_.reset(); }

} // namespace strata::host
