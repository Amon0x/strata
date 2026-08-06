#include <algorithm>
#include <strata/render_packet.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
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

    [[nodiscard]] double number() {
        return std::bit_cast<double>(u64());
    }

    [[nodiscard]] bool boolean() {
        const std::uint32_t value = u32();
        if (value > 1U)
            throw std::invalid_argument("render packet boolean is invalid");
        return value != 0U;
    }

    [[nodiscard]] std::string text() {
        const std::span<const std::uint8_t> value = raw(count());
        if (value.empty())
            return {};
        return std::string(reinterpret_cast<const char*>(value.data()), value.size());
    }

    [[nodiscard]] std::uint32_t count() {
        constexpr std::uint32_t maximum = 64U * 1'024U * 1'024U;
        const std::uint32_t value = u32();
        if (value > maximum)
            throw std::length_error("render packet collection exceeds host limit");
        return value;
    }

    [[nodiscard]] std::span<const std::uint8_t> raw(const std::size_t size) {
        require(size);
        const std::span<const std::uint8_t> result = bytes_.subspan(offset_, size);
        offset_ += size;
        return result;
    }

    [[nodiscard]] Reader record() {
        return Reader(raw(count()));
    }

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
    if (result.texture.empty())
        throw std::invalid_argument("render texture id is empty");
    if (kind == 2U) {
        input.exhausted("texture release");
        return result;
    }
    if (kind == 3U) {
        result.format = input.u32();
        result.sampling = input.u32();
        if (result.format != 0U)
            throw std::invalid_argument("encoded texture format is unknown");
        if (result.sampling > 1U)
            throw std::invalid_argument("texture sampling is unknown");
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
    if (kind > 2U)
        throw std::invalid_argument("render packet resource kind is unknown");
    result.format = input.u32();
    if (result.format > 1U)
        throw std::invalid_argument("atlas texture format is unknown");
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

[[nodiscard]] std::vector<RoundedClip> rounded_clips(Reader& input) {
    const std::uint32_t count = input.count();
    if (count > maximum_rounded_clip_depth) {
        throw std::invalid_argument("render rounded clip stack exceeds the maximum depth");
    }
    std::vector<RoundedClip> result(count);
    for (RoundedClip& clip : result) {
        clip.x = input.number();
        clip.y = input.number();
        clip.width = input.number();
        clip.height = input.number();
        for (double& radius : clip.radii) radius = input.number();
        for (double& value : clip.inverse_transform) value = input.number();
        const double determinant =
            clip.inverse_transform[0U] * clip.inverse_transform[4U] -
            clip.inverse_transform[1U] * clip.inverse_transform[3U];
        if (!std::isfinite(clip.x) || !std::isfinite(clip.y) ||
            !std::isfinite(clip.width) || !std::isfinite(clip.height) ||
            clip.width < 0.0 || clip.height < 0.0 ||
            !std::isfinite(determinant) || determinant == 0.0 ||
            std::ranges::any_of(
                clip.radii,
                [](const double value) { return !std::isfinite(value) || value < 0.0; }) ||
            std::ranges::any_of(
                clip.inverse_transform,
                [](const double value) { return !std::isfinite(value); })) {
            throw std::invalid_argument(
                "render rounded clip is outside the portable domain"
            );
        }
    }
    return result;
}

void validate_geometry(const RenderPacket& packet) {
    const std::size_t vertex_count = packet.vertices.size() / 88U;
    for (const SubmissionBatch& batch : packet.batches) {
        const DrawBatch* draw = std::get_if<DrawBatch>(&batch);
        if (draw == nullptr)
            continue;
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

[[nodiscard]] bool same_batch_shape(const std::vector<SubmissionBatch>& left,
                                    const std::vector<SubmissionBatch>& right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (left[index].index() != right[index].index())
            return false;
        const EffectBatch* left_effect = std::get_if<EffectBatch>(&left[index]);
        const EffectBatch* right_effect = std::get_if<EffectBatch>(&right[index]);
        if (left_effect != nullptr && right_effect != nullptr &&
            left_effect->kind != right_effect->kind) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::vector<GeometryPatch> geometry_patches(
    Reader& input,
    const std::size_t total_bytes,
    const std::size_t alignment,
    const std::string_view label
) {
    std::vector<GeometryPatch> result;
    const std::uint32_t count = input.count();
    result.reserve(count);
    std::size_t previous_end = 0U;
    for (std::uint32_t index = 0U; index < count; ++index) {
        GeometryPatch patch;
        patch.offset = input.u32();
        const std::span<const std::uint8_t> bytes = input.raw(input.count());
        const std::size_t end = static_cast<std::size_t>(patch.offset) + bytes.size();
        if (patch.offset % alignment != 0U || bytes.empty() ||
            bytes.size() % alignment != 0U || patch.offset < previous_end ||
            patch.offset > total_bytes || bytes.size() > total_bytes - patch.offset) {
            throw std::invalid_argument(
                "render " + std::string(label) + " patch is outside retained geometry"
            );
        }
        patch.bytes.assign(bytes.begin(), bytes.end());
        previous_end = end;
        result.push_back(std::move(patch));
    }
    return result;
}

[[nodiscard]] GeometryDirtyRegion dirty_region(
    const std::span<const std::uint8_t> previous,
    const GeometryPatch& patch
) {
    double minimum_x = std::numeric_limits<double>::infinity();
    double minimum_y = std::numeric_limits<double>::infinity();
    double maximum_x = -std::numeric_limits<double>::infinity();
    double maximum_y = -std::numeric_limits<double>::infinity();
    const auto include = [&](const std::uint8_t* const bytes) {
        float x = 0.0F;
        float y = 0.0F;
        std::memcpy(&x, bytes, sizeof(x));
        std::memcpy(&y, bytes + sizeof(x), sizeof(y));
        if (!std::isfinite(x) || !std::isfinite(y)) {
            throw std::invalid_argument("render vertex patch contains a non-finite position");
        }
        minimum_x = std::min(minimum_x, static_cast<double>(x));
        minimum_y = std::min(minimum_y, static_cast<double>(y));
        maximum_x = std::max(maximum_x, static_cast<double>(x));
        maximum_y = std::max(maximum_y, static_cast<double>(y));
    };
    for (std::size_t offset = 0U; offset < patch.bytes.size(); offset += 88U) {
        include(previous.data() + patch.offset + offset);
        include(patch.bytes.data() + offset);
    }
    return GeometryDirtyRegion{
        minimum_x,
        minimum_y,
        std::max(0.0, maximum_x - minimum_x),
        std::max(0.0, maximum_y - minimum_y),
    };
}

[[nodiscard]] bool batches_changed(
    const std::vector<SubmissionBatch>& previous,
    const std::vector<SubmissionBatch>& current
) noexcept {
    return previous != current;
}

class PatchDecodeTransaction final {
public:
    PatchDecodeTransaction(
        std::optional<RenderPacket>& retained,
        RenderPacket& working
    )
        : retained_(retained),
          working_(working),
          frame_index_(working.frame_index),
          geometry_epoch_(working.geometry_epoch),
          planned_draw_count_(working.planned_draw_count),
          skipped_draw_count_(working.skipped_draw_count),
          resources_(std::move(working.resources)),
          full_geometry_payload_(working.full_geometry_payload),
          vertex_patches_(std::move(working.vertex_patches)),
          index_patches_(std::move(working.index_patches)),
          geometry_dirty_all_(working.geometry_dirty_all),
          geometry_dirty_regions_(std::move(working.geometry_dirty_regions)) {}

    PatchDecodeTransaction(const PatchDecodeTransaction&) = delete;
    PatchDecodeTransaction& operator=(const PatchDecodeTransaction&) = delete;

    ~PatchDecodeTransaction() {
        if (committed_) return;
        for (const GeometryPatch& patch : undo_vertices_) {
            std::memcpy(
                working_.vertices.data() + patch.offset,
                patch.bytes.data(),
                patch.bytes.size()
            );
        }
        std::uint8_t* const index_bytes =
            reinterpret_cast<std::uint8_t*>(working_.indices.data());
        for (const GeometryPatch& patch : undo_indices_) {
            std::memcpy(
                index_bytes + patch.offset,
                patch.bytes.data(),
                patch.bytes.size()
            );
        }
        if (batches_saved_) working_.batches = std::move(batches_);
        working_.frame_index = frame_index_;
        working_.geometry_epoch = geometry_epoch_;
        working_.planned_draw_count = planned_draw_count_;
        working_.skipped_draw_count = skipped_draw_count_;
        working_.resources = std::move(resources_);
        working_.full_geometry_payload = full_geometry_payload_;
        working_.vertex_patches = std::move(vertex_patches_);
        working_.index_patches = std::move(index_patches_);
        working_.geometry_dirty_all = geometry_dirty_all_;
        working_.geometry_dirty_regions = std::move(geometry_dirty_regions_);
        retained_ = std::move(working_);
    }

    void save_batches() {
        batches_ = std::move(working_.batches);
        batches_saved_ = true;
    }

    void apply(
        const std::span<const GeometryPatch> vertex_patches,
        const std::span<const GeometryPatch> index_patches
    ) {
        undo_vertices_.reserve(vertex_patches.size());
        for (const GeometryPatch& patch : vertex_patches) {
            GeometryPatch undo{patch.offset, {}};
            undo.bytes.assign(
                working_.vertices.begin() + patch.offset,
                working_.vertices.begin() + patch.offset + patch.bytes.size()
            );
            undo_vertices_.push_back(std::move(undo));
            std::memcpy(
                working_.vertices.data() + patch.offset,
                patch.bytes.data(),
                patch.bytes.size()
            );
        }
        const std::uint8_t* const current_indices =
            reinterpret_cast<const std::uint8_t*>(working_.indices.data());
        undo_indices_.reserve(index_patches.size());
        for (const GeometryPatch& patch : index_patches) {
            GeometryPatch undo{patch.offset, {}};
            undo.bytes.assign(
                current_indices + patch.offset,
                current_indices + patch.offset + patch.bytes.size()
            );
            undo_indices_.push_back(std::move(undo));
            std::memcpy(
                reinterpret_cast<std::uint8_t*>(working_.indices.data()) + patch.offset,
                patch.bytes.data(),
                patch.bytes.size()
            );
        }
    }

    void commit() noexcept { committed_ = true; }
    [[nodiscard]] const std::vector<SubmissionBatch>& previous_batches() const noexcept {
        return batches_;
    }

private:
    std::optional<RenderPacket>& retained_;
    RenderPacket& working_;
    std::uint64_t frame_index_ = 0U;
    std::uint64_t geometry_epoch_ = 0U;
    std::uint32_t planned_draw_count_ = 0U;
    std::uint32_t skipped_draw_count_ = 0U;
    std::vector<ResourceOperation> resources_;
    bool full_geometry_payload_ = false;
    std::vector<GeometryPatch> vertex_patches_;
    std::vector<GeometryPatch> index_patches_;
    bool geometry_dirty_all_ = false;
    std::vector<GeometryDirtyRegion> geometry_dirty_regions_;
    std::vector<SubmissionBatch> batches_;
    std::vector<GeometryPatch> undo_vertices_;
    std::vector<GeometryPatch> undo_indices_;
    bool batches_saved_ = false;
    bool committed_ = false;
};

} // namespace

const RenderPacket& RenderPacketDecoder::decode(const std::span<const std::uint8_t> bytes) {
    Reader input(bytes);
    const std::span<const std::uint8_t> magic = input.raw(8U);
    if (std::string_view(reinterpret_cast<const char*>(magic.data()), magic.size()) != "STRATARP" ||
        input.u32() != STRATA_RENDER_PACKET_VERSION_CURRENT) {
        throw std::invalid_argument("render packet decoder requires protocol v10");
    }
    const std::uint32_t resource_count = input.count();
    const std::uint32_t batch_count = input.count();
    const std::uint64_t frame_index = input.u64();
    const std::uint64_t geometry_epoch = input.u64();
    const std::uint32_t flags = input.u32();
    if ((flags & ~(packet_flag_geometry_payload | packet_flag_geometry_patches)) != 0U ||
        flags == (packet_flag_geometry_payload | packet_flag_geometry_patches)) {
        throw std::invalid_argument("render packet flags are outside the supported domain");
    }
    const bool has_geometry_payload = (flags & packet_flag_geometry_payload) != 0U;
    const bool has_geometry_patches = (flags & packet_flag_geometry_patches) != 0U;
    const std::uint32_t vertex_bytes = input.count();
    const std::uint32_t index_count = input.count();
    const std::uint32_t planned_draw_count = input.count();
    const std::uint32_t skipped_draw_count = input.count();
    if (vertex_bytes % 88U != 0U || index_count > std::numeric_limits<std::uint32_t>::max() / 4U) {
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
    if (!has_geometry_payload && !has_geometry_patches) {
        if (batch_count != 0U || vertex_bytes != 0U || index_count != 0U) {
            throw std::invalid_argument(
                "retained render packet unexpectedly carries geometry counts");
        }
        input.exhausted("retained render packet");
        if (!retained_geometry) {
            throw std::invalid_argument(
                "retained render packet references an unavailable geometry epoch");
        }
        retained_->frame_index = frame_index;
        retained_->planned_draw_count = planned_draw_count;
        retained_->skipped_draw_count = skipped_draw_count;
        retained_->resources = std::move(resources);
        retained_->full_geometry_payload = false;
        retained_->vertex_patches.clear();
        retained_->index_patches.clear();
        retained_->geometry_dirty_all = false;
        retained_->geometry_dirty_regions.clear();
        return *retained_;
    }

    if (has_geometry_patches) {
        if (!retained_.has_value() ||
            retained_->geometry_epoch == std::numeric_limits<std::uint64_t>::max() ||
            geometry_epoch != retained_->geometry_epoch + 1U ||
            vertex_bytes != retained_->vertices.size() ||
            index_count != retained_->indices.size()) {
            throw std::invalid_argument(
                "render geometry patch does not match the preceding retained epoch"
            );
        }
    }
    RenderPacket result = has_geometry_patches ? std::move(*retained_) : RenderPacket{};
    std::optional<PatchDecodeTransaction> patch_transaction;
    if (has_geometry_patches) {
        patch_transaction.emplace(retained_, result);
    }
    result.frame_index = frame_index;
    result.geometry_epoch = geometry_epoch;
    result.planned_draw_count = planned_draw_count;
    result.skipped_draw_count = skipped_draw_count;
    result.resources = std::move(resources);
    result.full_geometry_payload = has_geometry_payload;
    result.geometry_dirty_all = has_geometry_payload;
    result.geometry_dirty_regions.clear();
    result.vertex_patches.clear();
    result.index_patches.clear();
    if (has_geometry_patches) {
        result.vertex_patches = geometry_patches(
            input,
            result.vertices.size(),
            88U,
            "vertex"
        );
        result.index_patches = geometry_patches(
            input,
            result.indices.size() * sizeof(std::uint32_t),
            sizeof(std::uint32_t),
            "index"
        );
        result.geometry_dirty_regions.reserve(result.vertex_patches.size());
        for (const GeometryPatch& patch : result.vertex_patches) {
            result.geometry_dirty_regions.push_back(dirty_region(result.vertices, patch));
        }
        result.geometry_dirty_all = !result.index_patches.empty();
        patch_transaction->apply(result.vertex_patches, result.index_patches);
        patch_transaction->save_batches();
    } else {
        const std::span<const std::uint8_t> vertices = input.raw(vertex_bytes);
        result.vertices.assign(vertices.begin(), vertices.end());
        result.indices.clear();
        result.indices.reserve(index_count);
        for (std::uint32_t index = 0U; index < index_count; ++index)
            result.indices.push_back(input.u32());
    }
    result.batches.clear();
    result.batches.reserve(batch_count);
    for (std::uint32_t index = 0U; index < batch_count; ++index) {
        const std::uint32_t kind = input.u32();
        Reader batch = input.record();
        const std::uint32_t source_order = batch.u32();
        const Scissor clip = scissor(batch);
        std::vector<RoundedClip> rounded = rounded_clips(batch);
        if (kind == 0U) {
            DrawBatch draw;
            draw.source_order = source_order;
            draw.scissor = clip;
            draw.rounded_clips = std::move(rounded);
            draw.material = batch.text();
            draw.blend_mode = batch.text();
            if (draw.material.empty() || draw.blend_mode.empty()) {
                throw std::invalid_argument(
                    "render batch material and blend mode must not be empty");
            }
            if (batch.boolean())
                draw.texture = batch.text();
            draw.base_vertex = batch.u32();
            draw.first_index = batch.u32();
            draw.index_count = batch.u32();
            result.batches.emplace_back(std::move(draw));
        } else if (kind == 1U) {
            BlurBatch blur;
            blur.source_order = source_order;
            blur.scissor = clip;
            blur.rounded_clips = std::move(rounded);
            blur.x = batch.number();
            blur.y = batch.number();
            blur.width = batch.number();
            blur.height = batch.number();
            blur.radius = batch.number();
            blur.downsample = batch.u32();
            if (!std::isfinite(blur.x) || !std::isfinite(blur.y) || !std::isfinite(blur.width) ||
                !std::isfinite(blur.height) || !std::isfinite(blur.radius) || blur.width < 0.0 ||
                blur.height < 0.0 || blur.radius < 0.0 || blur.downsample == 0U) {
                throw std::invalid_argument("render blur batch is outside the portable domain");
            }
            result.batches.emplace_back(blur);
        } else if (kind == 2U || kind == 3U) {
            EffectBatch effect;
            effect.kind = kind == 2U ? EffectBatchKind::backdrop : EffectBatchKind::content_begin;
            effect.source_order = source_order;
            effect.scissor = clip;
            effect.rounded_clips = std::move(rounded);
            effect.x = batch.number();
            effect.y = batch.number();
            effect.width = batch.number();
            effect.height = batch.number();
            for (double& radius : effect.radii)
                radius = batch.number();
            effect.effect = batch.text();
            effect.opacity = batch.number();
            effect.refresh_rate = batch.number();
            const std::uint32_t backdrop_source = batch.u32();
            if (backdrop_source > static_cast<std::uint32_t>(EffectBackdropSource::surface)) {
                throw std::invalid_argument("render effect backdrop source is unknown");
            }
            effect.backdrop_source = static_cast<EffectBackdropSource>(backdrop_source);
            effect.parameter_count = batch.u32();
            if (effect.effect.empty() || effect.parameter_count > effect.parameters.size() ||
                !std::isfinite(effect.x) || !std::isfinite(effect.y) ||
                !std::isfinite(effect.width) || !std::isfinite(effect.height) ||
                effect.width < 0.0 || effect.height < 0.0 || !std::isfinite(effect.opacity) ||
                effect.opacity < 0.0 || effect.opacity > 1.0 ||
                !std::isfinite(effect.refresh_rate) || effect.refresh_rate < 0.0 ||
                std::ranges::any_of(
                    effect.radii,
                    [](const double value) { return !std::isfinite(value) || value < 0.0; })) {
                throw std::invalid_argument("render effect batch is outside the portable domain");
            }
            if (effect.kind != EffectBatchKind::backdrop &&
                effect.backdrop_source != EffectBackdropSource::current) {
                throw std::invalid_argument(
                    "content effects cannot select a backdrop source"
                );
            }
            for (std::uint32_t parameter = 0U; parameter < effect.parameter_count; ++parameter) {
                effect.parameters[parameter] = batch.number();
                if (!std::isfinite(effect.parameters[parameter])) {
                    throw std::invalid_argument("render effect parameter is not finite");
                }
            }
            result.batches.emplace_back(std::move(effect));
        } else if (kind == 4U) {
            result.batches.emplace_back(
                ContentEffectEndBatch{source_order, clip, std::move(rounded)}
            );
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
                    "render content effect stack exceeds the maximum depth");
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
    if (has_geometry_patches &&
        batches_changed(
            patch_transaction->previous_batches(),
            result.batches
        )) {
        result.geometry_dirty_all = true;
        result.geometry_dirty_regions.clear();
    }
    validate_geometry(result);
    if (retained_geometry) {
        if (result.vertices != retained_->vertices || result.indices != retained_->indices) {
            throw std::invalid_argument(
                "retained render geometry epoch changed its vertex or index payload");
        }
        if (!same_batch_shape(result.batches, retained_->batches)) {
            throw std::invalid_argument("retained render geometry epoch changed its batch shape");
        }
    }
    retained_ = std::move(result);
    if (patch_transaction.has_value()) patch_transaction->commit();
    return *retained_;
}

void RenderPacketDecoder::reset() noexcept {
    retained_.reset();
}

} // namespace strata::host
