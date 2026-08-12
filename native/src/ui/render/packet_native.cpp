#include "ui/render/packet.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <strata/strata.h>

#include "font/atlas.hpp"
#include "resource/image.hpp"
#include "ui/render/submission.hpp"
#include "ui/text.hpp"

namespace strata::ui {
namespace {

using Bytes = std::vector<std::uint8_t>;

class Writer final {
  public:
    explicit Writer(const std::size_t reserve = 0U) {
        bytes_.reserve(reserve);
    }

    template <typename Integer>
        requires std::is_unsigned_v<Integer>
    void integer(const Integer value) {
        for (std::size_t byte = 0U; byte < sizeof(Integer); ++byte) {
            bytes_.push_back(static_cast<std::uint8_t>(value >> (byte * 8U)));
        }
    }

    void number(const double value) {
        integer(std::bit_cast<std::uint64_t>(value));
    }

    void text(const std::string_view value) {
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("render submission string exceeds uint32");
        }
        integer(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void rect(const Rect value) {
        number(value.x);
        number(value.y);
        number(value.width);
        number(value.height);
    }

    void raw(const std::span<const std::uint8_t> value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    [[nodiscard]] Bytes take() && {
        return std::move(bytes_);
    }

  private:
    Bytes bytes_;
};

[[nodiscard]] Bytes resource_payload(const font::AtlasOperation& operation) {
    Writer output;
    output.text(operation.texture);
    if (operation.kind == font::AtlasOperationKind::release)
        return std::move(output).take();
    output.integer(static_cast<std::uint32_t>(operation.format));
    output.integer(operation.region.x);
    output.integer(operation.region.y);
    output.integer(operation.region.width);
    output.integer(operation.region.height);
    if (operation.kind == font::AtlasOperationKind::upload) {
        if (operation.bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("native atlas upload exceeds uint32");
        }
        output.integer(static_cast<std::uint32_t>(operation.bytes.size()));
        output.raw(operation.bytes);
    }
    return std::move(output).take();
}

[[nodiscard]] Bytes resource_payload(const resource::EncodedTextureResource& texture) {
    if (texture.logical_id.empty() || texture.host_id.empty()) {
        throw std::invalid_argument(
            "encoded texture requires non-empty logical and surface host ids");
    }
    Writer output;
    output.text(texture.host_id);
    output.integer(static_cast<std::uint32_t>(texture.encoding));
    output.integer(static_cast<std::uint32_t>(texture.sampling));
    output.integer(texture.dimensions.width);
    output.integer(texture.dimensions.height);
    if (texture.bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("encoded texture resource exceeds uint32");
    }
    output.integer(static_cast<std::uint32_t>(texture.bytes.size()));
    output.raw(texture.bytes);
    return std::move(output).take();
}

void append_terminal_release(std::vector<font::AtlasOperation>& releases,
                             const std::string_view host_id) {
    if (host_id.empty()) {
        throw std::invalid_argument("terminal texture release requires a non-empty host id");
    }
    const bool duplicate =
        std::ranges::any_of(releases, [host_id](const font::AtlasOperation& release) {
            return release.texture == host_id;
        });
    if (duplicate)
        return;
    releases.push_back(font::AtlasOperation{
        font::AtlasOperationKind::release,
        std::string(host_id),
        font::AtlasTextureFormat::r8,
        {},
        {},
    });
}

[[nodiscard]] std::uint32_t checked_count(std::size_t value, const char* label);

[[nodiscard]] Bytes batch_payload(const SubmissionBatch& batch) {
    Writer output;
    output.integer(batch.source_order);
    output.integer(batch.scissor.x);
    output.integer(batch.scissor.y);
    output.integer(batch.scissor.width);
    output.integer(batch.scissor.height);
    output.integer(checked_count(batch.rounded_clips.size(), "rounded clip count"));
    for (const SubmissionRoundedClip& clip : batch.rounded_clips) {
        output.rect(clip.bounds);
        output.number(clip.radii.top_left);
        output.number(clip.radii.top_right);
        output.number(clip.radii.bottom_right);
        output.number(clip.radii.bottom_left);
        for (const double value : clip.inverse_transform)
            output.number(value);
    }
    if (batch.kind == SubmissionBatchKind::draw) {
        output.text(batch.material);
        output.text(batch.blend_mode);
        output.integer(batch.texture.has_value() ? std::uint32_t{1U} : std::uint32_t{0U});
        if (batch.texture.has_value())
            output.text(*batch.texture);
        output.integer(batch.base_vertex);
        output.integer(batch.first_index);
        output.integer(batch.index_count);
    } else if (batch.kind == SubmissionBatchKind::blur) {
        output.rect(batch.effect_bounds);
        output.number(batch.effect_radius);
        output.integer(batch.effect_downsample);
    } else if (batch.kind != SubmissionBatchKind::content_effect_end) {
        if (!batch.effect.has_value()) {
            throw std::logic_error("render effect batch is missing effect state");
        }
        output.rect(batch.effect_bounds);
        output.number(batch.effect_radii.top_left);
        output.number(batch.effect_radii.top_right);
        output.number(batch.effect_radii.bottom_right);
        output.number(batch.effect_radii.bottom_left);
        output.text(batch.effect->id);
        output.number(batch.effect->opacity);
        output.number(batch.effect->refresh_rate);
        output.integer(static_cast<std::uint32_t>(batch.effect->backdrop_source));
        output.integer(batch.effect->packed_parameter_count);
        for (std::uint32_t index = 0U; index < batch.effect->packed_parameter_count; ++index) {
            output.number(batch.effect->packed_parameters[index]);
        }
    }
    return std::move(output).take();
}

void record(Writer& output, const std::uint32_t kind, const std::span<const std::uint8_t> payload) {
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("render submission record exceeds uint32");
    }
    output.integer(kind);
    output.integer(static_cast<std::uint32_t>(payload.size()));
    output.raw(payload);
}

[[nodiscard]] std::uint32_t checked_count(const std::size_t value, const char* const label) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error(std::string(label) + " exceeds uint32");
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::size_t frame_index_offset = 20U;
constexpr std::uint32_t geometry_payload_flag = 1U;
constexpr std::uint32_t geometry_patch_flag = 2U;

void write_frame_index(Bytes& packet, const std::uint64_t frame_index) {
    if (packet.size() < frame_index_offset + sizeof(frame_index)) {
        throw std::logic_error("cached host render packet header is truncated");
    }
    for (std::size_t byte = 0U; byte < sizeof(frame_index); ++byte) {
        packet[frame_index_offset + byte] = static_cast<std::uint8_t>(frame_index >> (byte * 8U));
    }
}

[[nodiscard]] Bytes
encode_packet(const RenderSubmission& submission, const std::uint64_t frame_index,
              const std::uint64_t geometry_epoch,
              const std::span<const resource::EncodedTextureResource> texture_resources,
              const std::span<const font::AtlasOperation> resources,
              const bool include_geometry = true, const bool patch_geometry = false) {
    std::size_t reserve = 56U;
    if (include_geometry && !patch_geometry) {
        reserve += submission.vertex_bytes.size() +
                   submission.indices.size() * sizeof(std::uint32_t) +
                   submission.batches.size() * 80U;
    } else if (patch_geometry) {
        for (const SubmissionGeometryPatch& patch : submission.vertex_patches) {
            reserve += patch.bytes.size() + 8U;
        }
        for (const SubmissionGeometryPatch& patch : submission.index_patches) {
            reserve += patch.bytes.size() + 8U;
        }
        reserve += submission.batches.size() * 80U + 8U;
    }
    for (const resource::EncodedTextureResource& texture : texture_resources) {
        reserve += texture.bytes.size() + texture.host_id.size() + 40U;
    }
    for (const font::AtlasOperation& operation : resources) {
        reserve += operation.bytes.size() + operation.texture.size() + 40U;
    }
    Writer output(reserve);
    constexpr std::string_view magic = "STRATARP";
    output.raw(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(magic.data()),
                                             magic.size()));
    output.integer(STRATA_RENDER_PACKET_VERSION_CURRENT);
    if (texture_resources.size() > std::numeric_limits<std::size_t>::max() - resources.size()) {
        throw std::length_error("render resource count exceeds size_t");
    }
    output.integer(
        checked_count(texture_resources.size() + resources.size(), "render resource count"));
    output.integer(include_geometry ? checked_count(submission.batches.size(), "render batch count")
                                    : 0U);
    output.integer(frame_index);
    output.integer(geometry_epoch);
    output.integer(patch_geometry     ? geometry_patch_flag
                   : include_geometry ? geometry_payload_flag
                                      : std::uint32_t{0U});
    output.integer((include_geometry || patch_geometry)
                       ? checked_count(submission.vertex_bytes.size(), "render vertex byte count")
                       : 0U);
    output.integer((include_geometry || patch_geometry)
                       ? checked_count(submission.indices.size(), "render index count")
                       : 0U);
    output.integer(checked_count(submission.planned_draws, "render planned draw count"));
    output.integer(checked_count(submission.skipped_draws, "render skipped draw count"));
    // Resource operations preserve atlas order: releases precede creates that may reuse a
    // Surface-scoped host id.
    for (const font::AtlasOperation& operation : resources) {
        const Bytes payload = resource_payload(operation);
        record(output, static_cast<std::uint32_t>(operation.kind), payload);
    }
    for (const resource::EncodedTextureResource& texture : texture_resources) {
        const Bytes payload = resource_payload(texture);
        record(output, 3U, payload);
    }
    if (include_geometry) {
        if (patch_geometry) {
            output.integer(
                checked_count(submission.vertex_patches.size(), "render vertex patch count"));
            for (const SubmissionGeometryPatch& patch : submission.vertex_patches) {
                output.integer(patch.offset);
                output.integer(checked_count(patch.bytes.size(), "render vertex patch bytes"));
                output.raw(patch.bytes);
            }
            output.integer(
                checked_count(submission.index_patches.size(), "render index patch count"));
            for (const SubmissionGeometryPatch& patch : submission.index_patches) {
                output.integer(patch.offset);
                output.integer(checked_count(patch.bytes.size(), "render index patch bytes"));
                output.raw(patch.bytes);
            }
        } else {
            output.raw(submission.vertex_bytes);
            for (const std::uint32_t index : submission.indices)
                output.integer(index);
        }
        for (const SubmissionBatch& batch : submission.batches) {
            const Bytes payload = batch_payload(batch);
            record(output, static_cast<std::uint32_t>(batch.kind), payload);
        }
    }
    return std::move(output).take();
}

} // namespace

const std::vector<std::uint8_t>& HostRenderPacketCache::encode(
    const RenderCommandBuffer& commands, const std::uint64_t frame_index,
    const std::span<const resource::EncodedTextureResource> texture_resources,
    font::GlyphAtlas& glyph_atlas, const TextEngine* const text_engine, const double display_scale,
    const std::int64_t framebuffer_width, const std::int64_t framebuffer_height,
    const double logical_width, const double logical_height) {
    // Cleared only after geometry and every resource operation are retained. This prevents the
    // settled fast path from masking a prior allocation/planning failure on the next frame.
    const bool retrying_incomplete_frame = frame_encoding_incomplete_;
    frame_encoding_incomplete_ = true;
    const bool profile_cold_encode = geometry_packet_.empty();
    const auto cold_encode_started = profile_cold_encode ? std::chrono::steady_clock::now()
                                                         : std::chrono::steady_clock::time_point{};
    if (!texture_resources.empty()) {
        std::vector<resource::TextureResourceDescriptor> next_descriptors;
        next_descriptors.reserve(texture_resources.size());
        for (const resource::EncodedTextureResource& texture : texture_resources) {
            next_descriptors.push_back(texture.descriptor());
        }
        texture_descriptors_.swap(next_descriptors);
    }
    const std::size_t prior_hits = submission_cache_.hit_count();
    const auto submission_started = std::chrono::steady_clock::now();
    const RenderSubmission& submission =
        submission_cache_.resolve(commands, glyph_atlas, text_engine,
                                  RenderSubmissionEnvironment{
                                      display_scale,
                                      framebuffer_width,
                                      framebuffer_height,
                                      logical_width,
                                      logical_height,
                                  },
                                  texture_descriptors_);
    const auto submission_completed = std::chrono::steady_clock::now();
    const auto submission_finished = profile_cold_encode ? std::chrono::steady_clock::now()
                                                         : std::chrono::steady_clock::time_point{};
    // Submission planning has finished mutating the atlas. Keep a stable view of its pending
    // operations until every packet that references them has been encoded and retained.
    const std::span<const font::AtlasOperation> resources = glyph_atlas.pending_operations();
    const bool submission_reused = submission_cache_.hit_count() != prior_hits;
    telemetry_ = HostRenderPacketTelemetry{
        submission.used_vertex_bytes / 88U,
        submission.batches.size(),
        submission.texture_batch_breaks,
        submission.clip_batch_breaks,
        submission.material_batch_breaks,
        submission.effect_batch_breaks,
        submission_reused,
        profile_cold_encode,
        profile_cold_encode ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  submission_finished - cold_encode_started)
                                  .count()
                            : 0,
        profile_cold_encode ? submission.planning_nanos : 0,
        profile_cold_encode ? submission.atlas_warmup_nanos : 0,
        profile_cold_encode ? submission.text_preparation_nanos : 0,
        profile_cold_encode ? submission.mesh_encoding_nanos : 0,
        0,
        0,
    };
    telemetry_.submission_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      submission_completed - submission_started)
                                      .count();
    telemetry_.submission_planning_nanos = submission.planning_nanos;
    telemetry_.submission_atlas_warmup_nanos = submission.atlas_warmup_nanos;
    telemetry_.submission_text_preparation_nanos = submission.text_preparation_nanos;
    telemetry_.submission_mesh_encoding_nanos = submission.mesh_encoding_nanos;
    telemetry_.geometry_topology_reused = submission.geometry_topology_reused;
    telemetry_.candidate_geometry_patch_bytes = submission.candidate_geometry_patch_bytes;
    telemetry_.previous_full_geometry_bytes = submission.previous_full_geometry_bytes;
    telemetry_.full_geometry_bytes = submission.full_geometry_bytes;
    telemetry_.topology_change = submission.topology_change;
    telemetry_.topology_change_item = submission.topology_change_item;
    telemetry_.previous_item_count = submission.previous_item_count;
    telemetry_.item_count = submission.item_count;
    const bool geometry_changed =
        retrying_incomplete_frame || !submission_reused || geometry_packet_.empty();
    const bool patch_geometry = geometry_changed && !retrying_incomplete_frame &&
                                !geometry_packet_.empty() && submission.patch_from_previous;
    telemetry_.geometry_patched = patch_geometry;
    if (patch_geometry) {
        for (const SubmissionGeometryPatch& patch : submission.vertex_patches) {
            telemetry_.geometry_patch_bytes += patch.bytes.size();
        }
        for (const SubmissionGeometryPatch& patch : submission.index_patches) {
            telemetry_.geometry_patch_bytes += patch.bytes.size();
        }
    }
    std::uint64_t encoded_geometry_epoch = geometry_epoch_;
    if (geometry_changed) {
        if (geometry_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("render geometry epoch exhausted");
        }
        encoded_geometry_epoch = geometry_epoch_ + 1U;
        const auto geometry_encode_started = std::chrono::steady_clock::now();
        std::vector<std::uint8_t> next_geometry = encode_packet(
            submission, frame_index, encoded_geometry_epoch, {}, {}, true, patch_geometry);
        static_assert(noexcept(geometry_packet_.swap(next_geometry)));
        geometry_packet_.swap(next_geometry);
        telemetry_.geometry_packet_nanos =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                 geometry_encode_started)
                .count();
        reuse_packet_.clear();
        if (profile_cold_encode) {
            telemetry_.cold_geometry_packet_nanos =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - submission_finished)
                    .count();
        }
    }
    if (texture_resources.empty() && resources.empty()) {
        if (geometry_changed) {
            current_packet_ = &geometry_packet_;
        } else {
            if (reuse_packet_.empty()) {
                reuse_packet_ =
                    encode_packet(submission, frame_index, encoded_geometry_epoch, {}, {}, false);
            } else {
                write_frame_index(reuse_packet_, frame_index);
            }
            current_packet_ = &reuse_packet_;
        }
    } else {
        const auto resource_encode_started = profile_cold_encode
                                                 ? std::chrono::steady_clock::now()
                                                 : std::chrono::steady_clock::time_point{};
        std::vector<std::uint8_t> next_resources =
            encode_packet(submission, frame_index, encoded_geometry_epoch, texture_resources,
                          resources, geometry_changed, patch_geometry);
        static_assert(noexcept(resource_packet_.swap(next_resources)));
        resource_packet_.swap(next_resources);
        current_packet_ = &resource_packet_;
        if (profile_cold_encode) {
            telemetry_.cold_resource_packet_nanos =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - resource_encode_started)
                    .count();
        }
    }
    // This is the final state transition. All retained packet/cache operations above are complete
    // and the noexcept commit merely consumes operations now owned by the host packet.
    glyph_atlas.commit_operations();
    planned_draws_ = submission.planned_draws;
    skipped_draws_ = submission.skipped_draws;
    geometry_epoch_ = encoded_geometry_epoch;
    frame_encoding_incomplete_ = false;
    return *current_packet_;
}

const std::vector<std::uint8_t>& HostRenderPacketCache::encode(
    const RenderCommandBuffer& commands, const std::uint64_t frame_index,
    const std::span<const resource::EncodedTextureResource> texture_resources,
    font::GlyphAtlas& glyph_atlas, const TextEngine& text_engine, const double display_scale,
    const std::int64_t framebuffer_width, const std::int64_t framebuffer_height,
    const double logical_width, const double logical_height) {
    return encode(commands, frame_index, texture_resources, glyph_atlas, &text_engine,
                  display_scale, framebuffer_width, framebuffer_height, logical_width,
                  logical_height);
}

bool HostRenderPacketCache::reuse(const std::uint64_t frame_index) {
    if (frame_encoding_incomplete_ || geometry_packet_.empty())
        return false;
    if (reuse_packet_.empty()) {
        RenderSubmission retained_submission;
        retained_submission.planned_draws = planned_draws_;
        retained_submission.skipped_draws = skipped_draws_;
        reuse_packet_ =
            encode_packet(retained_submission, frame_index, geometry_epoch_, {}, {}, false);
    } else {
        write_frame_index(reuse_packet_, frame_index);
    }
    current_packet_ = &reuse_packet_;
    telemetry_.geometry_reused = true;
    return true;
}

const std::vector<std::uint8_t>&
HostRenderPacketCache::prepare_atlas_release(const std::uint64_t frame_index,
                                             font::GlyphAtlas& glyph_atlas) {
    return prepare_resource_release(frame_index, glyph_atlas, {});
}

const std::vector<std::uint8_t>& HostRenderPacketCache::prepare_resource_release(
    const std::uint64_t frame_index, font::GlyphAtlas& glyph_atlas,
    const std::span<const resource::EncodedTextureResource> static_textures) {
    if (terminal_release_prepared_)
        return resource_packet_;

    // Snapshotting and encoding are deliberately non-mutating. An allocation failure therefore
    // leaves live pages, pending releases and the retained static descriptor table retryable.
    std::vector<font::AtlasOperation> operations;
    std::vector<font::AtlasOperation> atlas_releases = glyph_atlas.plan_terminal_release();
    operations.insert(operations.end(), std::make_move_iterator(atlas_releases.begin()),
                      std::make_move_iterator(atlas_releases.end()));
    for (const resource::TextureResourceDescriptor& texture : texture_descriptors_) {
        append_terminal_release(operations, texture.host_id);
    }
    for (const resource::EncodedTextureResource& texture : static_textures) {
        if (texture.logical_id.empty()) {
            throw std::invalid_argument("terminal texture release requires a non-empty logical id");
        }
        append_terminal_release(operations, texture.host_id);
    }
    const RenderSubmission empty_submission;
    if (geometry_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("render geometry epoch exhausted");
    }
    const std::uint64_t next_geometry_epoch = geometry_epoch_ + 1U;
    std::vector<std::uint8_t> next_packet =
        encode_packet(empty_submission, frame_index, next_geometry_epoch, {}, operations);

    // std::vector::swap with the standard allocator is noexcept: install the complete packet and
    // make every remaining cache transition before irreversibly draining the atlas.
    static_assert(noexcept(resource_packet_.swap(next_packet)));
    resource_packet_.swap(next_packet);
    geometry_epoch_ = next_geometry_epoch;
    current_packet_ = &resource_packet_;
    terminal_release_prepared_ = true;
    submission_cache_.clear();
    texture_descriptors_.clear();
    geometry_packet_.clear();
    reuse_packet_.clear();
    telemetry_ = {};
    planned_draws_ = 0U;
    skipped_draws_ = 0U;
    frame_encoding_incomplete_ = false;
    glyph_atlas.commit_terminal_release();
    return resource_packet_;
}

void HostRenderPacketCache::clear() noexcept {
    submission_cache_.clear();
    texture_descriptors_.clear();
    geometry_packet_.clear();
    reuse_packet_.clear();
    resource_packet_.clear();
    current_packet_ = &geometry_packet_;
    telemetry_ = {};
    planned_draws_ = 0U;
    skipped_draws_ = 0U;
    frame_encoding_incomplete_ = false;
    terminal_release_prepared_ = false;
}

const std::vector<std::uint8_t>& HostRenderPacketCache::packet() const noexcept {
    return *current_packet_;
}

const HostRenderPacketTelemetry& HostRenderPacketCache::telemetry() const noexcept {
    return telemetry_;
}

} // namespace strata::ui
