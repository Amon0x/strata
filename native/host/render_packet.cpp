#include <algorithm>
#include <strata/render_packet.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <array>
#include <limits>
#include <map>
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

    [[nodiscard]] std::uint8_t u8() {
        require(1U);
        return bytes_[offset_++];
    }

    [[nodiscard]] std::uint16_t u16() {
        require(2U);
        const auto value = static_cast<std::uint16_t>(bytes_[offset_] | bytes_[offset_ + 1U] << 8U);
        offset_ += 2U;
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

    /**
     * How many of `count` records of at least `minimum_bytes` each the unread bytes could hold.
     * Counts come from the packet, so storage is reserved only for what can actually follow;
     * a truncated packet is still rejected by the reads themselves.
     */
    [[nodiscard]] std::size_t plausible(const std::uint32_t count,
                                        const std::size_t minimum_bytes) const noexcept {
        return std::min<std::size_t>(count, (bytes_.size() - offset_) / minimum_bytes);
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
    if (kind == resource_image) {
        result.format = texture_format_rgba8;
        result.sampling = input.u32();
        if (result.sampling > 1U)
            throw std::invalid_argument("texture sampling is unknown");
        result.width = input.count();
        result.height = input.count();
        const std::span<const std::uint8_t> bytes = input.raw(input.count());
        if (result.width == 0U || result.height == 0U) {
            throw std::invalid_argument("image dimensions must be positive");
        }
        if (static_cast<std::uint64_t>(result.width) * result.height * 4U != bytes.size()) {
            throw std::invalid_argument("RGBA8 image payload differs from its dimensions");
        }
        result.bytes.assign(bytes.begin(), bytes.end());
        input.exhausted("image");
        return result;
    }
    if (kind > 2U && kind != resource_raster)
        throw std::invalid_argument("render packet resource kind is unknown");
    result.format = input.u32();
    if (result.format > texture_format_r8_filtered)
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
                                       (result.format == texture_format_rgba8 ? 4U : 1U);
        if (expected != bytes.size()) {
            throw std::invalid_argument("atlas upload byte count does not match its region");
        }
        result.bytes.assign(bytes.begin(), bytes.end());
    }
    if (kind == resource_raster) {
        const std::span<const std::uint8_t> bytes = input.raw(input.count());
        result.raster = decode_raster_job(bytes, result.width, result.height, result.format);
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
        clip.shape = input.u32();
        clip.sides = input.number();
        for (double& value : clip.fade)
            value = input.number();
        if (clip.shape > 3U || !std::isfinite(clip.sides) || clip.sides < 0.0 ||
            clip.sides > 64.0 || std::ranges::any_of(clip.fade, [](const double value) {
                return !std::isfinite(value) || value < 0.0;
            })) {
            throw std::invalid_argument("render rounded clip shape is outside the portable domain");
        }
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
    const std::size_t vertex_count = packet.vertices.size() / packet_vertex_bytes;
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
    result.reserve(input.plausible(count, 8U));
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

/** Left, top, right and bottom of the box around a rectangle the group places. */
[[nodiscard]] std::array<double, 4U> placed_box(const PresentationGroup& group, const double x,
                                                const double y, const double width,
                                                const double height) noexcept {
    std::array<double, 4U> result{
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
    };
    for (const auto [corner_x, corner_y] : {std::pair{x, y}, std::pair{x + width, y},
                                            std::pair{x, y + height},
                                            std::pair{x + width, y + height}}) {
        const double placed_x = group.place_x(corner_x, corner_y);
        const double placed_y = group.place_y(corner_x, corner_y);
        result[0U] = std::min(result[0U], placed_x);
        result[1U] = std::min(result[1U], placed_y);
        result[2U] = std::max(result[2U], placed_x);
        result[3U] = std::max(result[3U], placed_y);
    }
    return result;
}

[[nodiscard]] PresentationGroup group_of(const std::vector<PresentationGroup>& groups,
                                         const std::uint32_t index) noexcept {
    return index < groups.size() ? groups[index] : PresentationGroup{};
}

[[nodiscard]] std::uint32_t vertex_group(const std::uint8_t* const vertex) noexcept {
    float z = 0.0F;
    std::memcpy(&z, vertex + 2U * sizeof(float), sizeof(z));
    // Matches the vertex stage's min((uint)(z + 0.5), 511).
    return std::isfinite(z) && z > 0.0F
               ? static_cast<std::uint32_t>(
                     std::min(z + 0.5F, static_cast<float>(maximum_presentation_group)))
               : 0U;
}

using GroupExtents = std::vector<std::array<double, 4U>>;

/** Layout-space bounds of every group's vertices; empty when the table has no groups. */
[[nodiscard]] GroupExtents group_extents(const std::span<const std::uint8_t> vertices,
                                         const std::vector<PresentationGroup>& groups) {
    GroupExtents result;
    if (groups.empty())
        return result;
    constexpr double infinity = std::numeric_limits<double>::infinity();
    result.assign(groups.size(), {infinity, infinity, -infinity, -infinity});
    for (std::size_t offset = 0U; offset + packet_vertex_bytes <= vertices.size(); offset += packet_vertex_bytes) {
        const std::uint8_t* const vertex = vertices.data() + offset;
        const std::uint32_t group = vertex_group(vertex);
        if (group == 0U || group >= result.size())
            continue;
        float x = 0.0F;
        float y = 0.0F;
        std::memcpy(&x, vertex, sizeof(x));
        std::memcpy(&y, vertex + sizeof(x), sizeof(y));
        std::array<double, 4U>& extent = result[group];
        extent[0U] = std::min(extent[0U], static_cast<double>(x));
        extent[1U] = std::min(extent[1U], static_cast<double>(y));
        extent[2U] = std::max(extent[2U], static_cast<double>(x));
        extent[3U] = std::max(extent[3U], static_cast<double>(y));
    }
    return result;
}

/** Where a changed group's content was and now is: the only area its table change touches. */
void append_group_regions(const std::vector<PresentationGroup>& previous,
                          const std::vector<PresentationGroup>& current,
                          const GroupExtents& extents,
                          std::vector<GeometryDirtyRegion>& output) {
    const std::size_t count = std::min(extents.size(), std::max(previous.size(), current.size()));
    for (std::uint32_t index = 1U; index < count; ++index) {
        const PresentationGroup before = group_of(previous, index);
        const PresentationGroup after = group_of(current, index);
        const std::array<double, 4U>& extent = extents[index];
        if (before == after || !(extent[0U] <= extent[2U] && extent[1U] <= extent[3U]))
            continue;
        for (const PresentationGroup& group : {before, after}) {
            const std::array<double, 4U> placed = placed_box(
                group, extent[0U], extent[1U], extent[2U] - extent[0U], extent[3U] - extent[1U]);
            // One logical pixel of slack covers the backends' device-pixel translation snap.
            output.push_back(GeometryDirtyRegion{
                placed[0U] - 1.0,
                placed[1U] - 1.0,
                placed[2U] - placed[0U] + 2.0,
                placed[3U] - placed[1U] + 2.0,
            });
        }
    }
}

/** Presented bounds of a vertex patch: old vertices under the old table, new under the new. */
[[nodiscard]] GeometryDirtyRegion dirty_region(
    const std::span<const std::uint8_t> previous,
    const GeometryPatch& patch,
    const std::vector<PresentationGroup>& previous_groups,
    const std::vector<PresentationGroup>& groups
) {
    double minimum_x = std::numeric_limits<double>::infinity();
    double minimum_y = std::numeric_limits<double>::infinity();
    double maximum_x = -std::numeric_limits<double>::infinity();
    double maximum_y = -std::numeric_limits<double>::infinity();
    const auto include = [&](const std::uint8_t* const bytes,
                             const std::vector<PresentationGroup>& table) {
        float x = 0.0F;
        float y = 0.0F;
        std::memcpy(&x, bytes, sizeof(x));
        std::memcpy(&y, bytes + sizeof(x), sizeof(y));
        if (!std::isfinite(x) || !std::isfinite(y)) {
            throw std::invalid_argument("render vertex patch contains a non-finite position");
        }
        const PresentationGroup group = group_of(table, vertex_group(bytes));
        const double presented_x = group.place_x(x, y);
        const double presented_y = group.place_y(x, y);
        minimum_x = std::min(minimum_x, presented_x);
        minimum_y = std::min(minimum_y, presented_y);
        maximum_x = std::max(maximum_x, presented_x);
        maximum_y = std::max(maximum_y, presented_y);
    };
    for (std::size_t offset = 0U; offset < patch.bytes.size(); offset += packet_vertex_bytes) {
        include(previous.data() + patch.offset + offset, previous_groups);
        include(patch.bytes.data() + offset, groups);
    }
    return GeometryDirtyRegion{
        minimum_x,
        minimum_y,
        std::max(0.0, maximum_x - minimum_x),
        std::max(0.0, maximum_y - minimum_y),
    };
}

/**
 * Texel rectangles, normalized to the texture (left, top, right, bottom), that one packet's
 * resource operations replaced. Creation, an encoded image and release change the whole texture;
 * an upload changes its region of a texture whose size is known. `sizes` is advanced through the
 * operations, so an upload after a create in the same packet resolves against it.
 */
using TextureChanges = std::map<std::string, std::vector<std::array<double, 4U>>, std::less<>>;

[[nodiscard]] TextureChanges texture_changes(const std::vector<ResourceOperation>& operations,
                                             TextureSizes& sizes) {
    TextureChanges result;
    for (const ResourceOperation& operation : operations) {
        std::vector<std::array<double, 4U>>& rectangles = result[operation.texture];
        if (operation.kind == resource_upload || operation.kind == resource_raster) {
            const auto size = sizes.find(operation.texture);
            if (size != sizes.end()) {
                const double width = size->second[0U];
                const double height = size->second[1U];
                rectangles.push_back({
                    operation.x / width,
                    operation.y / height,
                    (static_cast<double>(operation.x) + operation.width) / width,
                    (static_cast<double>(operation.y) + operation.height) / height,
                });
                continue;
            }
        } else if (operation.kind == resource_release) {
            if (const auto size = sizes.find(operation.texture); size != sizes.end())
                sizes.erase(size);
        } else {
            sizes.insert_or_assign(operation.texture,
                                   std::array<std::uint32_t, 2U>{operation.width, operation.height});
        }
        rectangles.push_back({0.0, 0.0, 1.0, 1.0});
    }
    return result;
}

/**
 * Appends the presented bounds of the draws whose sampled texels `changes` replaced: what a
 * retained effect sample over them read is stale although no vertex moved. The unified material
 * samples its texture at the interpolated vertex UV, so a triangle whose UV bounds (plus a texel
 * for filtering) miss every changed rectangle is unaffected; an authored material may sample
 * anywhere, so every triangle of its draw counts.
 */
void append_texture_regions(const RenderPacket& packet,
                            const std::vector<PresentationGroup>& groups,
                            const TextureChanges& changes, const TextureSizes& sizes,
                            std::vector<GeometryDirtyRegion>& output) {
    if (changes.empty())
        return;
    constexpr double infinity = std::numeric_limits<double>::infinity();
    for (const SubmissionBatch& batch : packet.batches) {
        const DrawBatch* const draw = std::get_if<DrawBatch>(&batch);
        if (draw == nullptr || !draw->texture.has_value())
            continue;
        const auto changed = changes.find(*draw->texture);
        if (changed == changes.end())
            continue;
        const bool exact = packet.materials[draw->material] == "strata:unified_ui";
        double margin_u = 0.0;
        double margin_v = 0.0;
        if (const auto size = sizes.find(*draw->texture); size != sizes.end()) {
            margin_u = 1.0 / std::max<std::uint32_t>(1U, size->second[0U]);
            margin_v = 1.0 / std::max<std::uint32_t>(1U, size->second[1U]);
        }
        double left = infinity;
        double top = infinity;
        double right = -infinity;
        double bottom = -infinity;
        const std::size_t end = static_cast<std::size_t>(draw->first_index) + draw->index_count;
        for (std::size_t first = draw->first_index; first + 3U <= end; first += 3U) {
            std::array<const std::uint8_t*, 3U> corners{};
            for (std::size_t corner = 0U; corner < corners.size(); ++corner) {
                const std::size_t vertex =
                    static_cast<std::size_t>(packet.indices[first + corner]) + draw->base_vertex;
                corners[corner] = packet.vertices.data() + vertex * packet_vertex_bytes;
            }
            if (exact) {
                double u0 = infinity;
                double v0 = infinity;
                double u1 = -infinity;
                double v1 = -infinity;
                for (const std::uint8_t* const corner : corners) {
                    float u = 0.0F;
                    float v = 0.0F;
                    std::memcpy(&u, corner + 12U, sizeof(u));
                    std::memcpy(&v, corner + 16U, sizeof(v));
                    u0 = std::min(u0, static_cast<double>(u));
                    v0 = std::min(v0, static_cast<double>(v));
                    u1 = std::max(u1, static_cast<double>(u));
                    v1 = std::max(v1, static_cast<double>(v));
                }
                // A NaN coordinate makes every comparison false, so such a triangle is kept.
                const bool reaches = std::ranges::any_of(
                    changed->second, [&](const std::array<double, 4U>& rectangle) {
                        return !(u1 + margin_u < rectangle[0U] || u0 - margin_u > rectangle[2U] ||
                                 v1 + margin_v < rectangle[1U] || v0 - margin_v > rectangle[3U]);
                    });
                if (!reaches)
                    continue;
            }
            for (const std::uint8_t* const corner : corners) {
                float x = 0.0F;
                float y = 0.0F;
                std::memcpy(&x, corner, sizeof(x));
                std::memcpy(&y, corner + sizeof(x), sizeof(y));
                const PresentationGroup group = group_of(groups, vertex_group(corner));
                const double presented_x = group.translate_x + group.scale_x * x;
                const double presented_y = group.translate_y + group.scale_y * y;
                if (!std::isfinite(presented_x) || !std::isfinite(presented_y))
                    continue;
                left = std::min(left, presented_x);
                top = std::min(top, presented_y);
                right = std::max(right, presented_x);
                bottom = std::max(bottom, presented_y);
            }
        }
        if (left <= right && top <= bottom) {
            // One logical pixel of slack, as for group moves, covers edge antialiasing.
            output.push_back(GeometryDirtyRegion{
                left - 1.0,
                top - 1.0,
                right - left + 2.0,
                bottom - top + 2.0,
            });
        }
    }
}

[[nodiscard]] std::vector<PresentationGroup> presentation_groups(Reader& input) {
    const std::uint32_t count = input.count();
    if (count > maximum_presentation_group) {
        throw std::invalid_argument("render group table exceeds the maximum group index");
    }
    std::vector<PresentationGroup> result;
    std::uint32_t previous = 0U;
    for (std::uint32_t record = 0U; record < count; ++record) {
        const std::uint32_t index = input.u32();
        if (index <= previous || index > maximum_presentation_group) {
            throw std::invalid_argument("render group indices must ascend within 1..511");
        }
        PresentationGroup group;
        group.scale_x = input.number();
        group.scale_y = input.number();
        group.translate_x = input.number();
        group.translate_y = input.number();
        group.opacity = input.number();
        group.tone = input.number();
        group.shear_x = input.number();
        group.shear_y = input.number();
        group.project_x = input.number();
        group.project_y = input.number();
        group.project_w = input.number();
        if (!std::isfinite(group.scale_x) || !std::isfinite(group.scale_y) ||
            !std::isfinite(group.shear_x) || !std::isfinite(group.shear_y) ||
            !std::isfinite(group.project_x) || !std::isfinite(group.project_y) ||
            !std::isfinite(group.project_w) || !std::isfinite(group.translate_x) ||
            !std::isfinite(group.translate_y) || !std::isfinite(group.opacity) ||
            group.opacity < 0.0 || group.opacity > 1.0 || !std::isfinite(group.tone) ||
            group.tone < 0.0 || group.tone > 1.0) {
            throw std::invalid_argument("render group is outside the portable domain");
        }
        result.resize(index + 1U);
        result[index] = group;
        previous = index;
    }
    return result;
}

/** A table of distinct, non-empty ids that batches name by u16 index. */
[[nodiscard]] std::vector<std::string> id_table(Reader& input, const std::string_view label) {
    const std::uint32_t count = input.count();
    if (count > std::numeric_limits<std::uint16_t>::max())
        throw std::invalid_argument("render " + std::string(label) + " table exceeds u16 indices");
    std::vector<std::string> result;
    result.reserve(input.plausible(count, 5U));
    for (std::uint32_t index = 0U; index < count; ++index) {
        std::string id = input.text();
        if (id.empty() || std::ranges::find(result, id) != result.end()) {
            throw std::invalid_argument("render " + std::string(label) +
                                        " table holds an empty or repeated id");
        }
        result.push_back(std::move(id));
    }
    return result;
}

[[nodiscard]] std::uint32_t group_index(Reader& input) {
    const std::uint32_t index = input.u32();
    if (index > maximum_presentation_group)
        throw std::invalid_argument("render batch group index exceeds the maximum group index");
    return index;
}

/**
 * The presented box surrounds what a turned group draws. Map presented pixels back into the group
 * and mask by the batch's own rounded rectangle there, so a blur or effect keeps its turned shape;
 * clips authored inside the group map back the same way.
 */
void mask_turned_group(const PresentationGroup& group, const double x, const double y,
                       const double width, const double height,
                       const std::array<double, 4U>& radii, std::vector<RoundedClip>& clips) {
    if (group.shear_x == 0.0 && group.shear_y == 0.0)
        return;
    const double determinant = group.scale_x * group.scale_y - group.shear_x * group.shear_y;
    if (determinant == 0.0 || !std::isfinite(determinant))
        return;
    const double a = group.scale_y / determinant;
    const double b = -group.shear_x / determinant;
    const double c = -group.shear_y / determinant;
    const double d = group.scale_x / determinant;
    const std::array<double, 6U> inverse{
        a, b, -(a * group.translate_x + b * group.translate_y),
        c, d, -(c * group.translate_x + d * group.translate_y),
    };
    for (RoundedClip& clip : clips) {
        const std::array<double, 6U> m = clip.inverse_transform;
        clip.inverse_transform = {
            m[0U] * inverse[0U] + m[1U] * inverse[3U],
            m[0U] * inverse[1U] + m[1U] * inverse[4U],
            m[0U] * inverse[2U] + m[1U] * inverse[5U] + m[2U],
            m[3U] * inverse[0U] + m[4U] * inverse[3U],
            m[3U] * inverse[1U] + m[4U] * inverse[4U],
            m[3U] * inverse[2U] + m[4U] * inverse[5U] + m[5U],
        };
    }
    if (clips.size() < maximum_rounded_clip_depth)
        clips.push_back(RoundedClip{x, y, width, height, radii, inverse});
}

/** Writes the presented bounds of a group-local blur/effect batch into `target` (same kind). */
void present(const SubmissionBatch& local, const std::vector<PresentationGroup>& groups,
             SubmissionBatch& target) {
    // A rotated group's effects are sampled over the box around their turned rectangle.
    const auto place = [](const PresentationGroup& group, const double x, const double y,
                          const double width, const double height, auto& output) {
        const std::array<double, 4U> placed = placed_box(group, x, y, width, height);
        output.x = placed[0U];
        output.y = placed[1U];
        output.width = placed[2U] - placed[0U];
        output.height = placed[3U] - placed[1U];
    };
    if (const auto* blur = std::get_if<BlurBatch>(&local); blur != nullptr) {
        const PresentationGroup group = group_of(groups, blur->group);
        BlurBatch& output = std::get<BlurBatch>(target);
        place(group, blur->x, blur->y, blur->width, blur->height, output);
        // The batch is presented again whenever the table moves: start from its own clips.
        output.rounded_clips = blur->rounded_clips;
        mask_turned_group(group, blur->x, blur->y, blur->width, blur->height, {},
                          output.rounded_clips);
    } else if (const auto* effect = std::get_if<EffectBatch>(&local); effect != nullptr) {
        const PresentationGroup group = group_of(groups, effect->group);
        EffectBatch& output = std::get<EffectBatch>(target);
        place(group, effect->x, effect->y, effect->width, effect->height, output);
        // A rotated group keeps lengths; its corners scale by the square root of its area scale.
        const double radius_scale =
            group.shear_x == 0.0 && group.shear_y == 0.0
                ? std::min(std::abs(group.scale_x), std::abs(group.scale_y))
                : std::sqrt(std::abs(group.scale_x * group.scale_y - group.shear_x * group.shear_y));
        for (std::size_t corner = 0U; corner < output.radii.size(); ++corner)
            output.radii[corner] = effect->radii[corner] * radius_scale;
        output.opacity = effect->opacity * group.opacity;
        output.tone = group.tone;
        // Filter lengths move with the group as its bounds do.
        output.filters = effect->filters;
        const double along_x = std::hypot(group.scale_x, group.shear_y);
        const double along_y = std::hypot(group.shear_x, group.scale_y);
        const double area =
            std::sqrt(std::abs(group.scale_x * group.scale_y - group.shear_x * group.shear_y));
        for (LayerFilter& filter : output.filters) {
            std::vector<double>& values = filter.values;
            if (filter.kind == LayerFilterKind::blur && values.size() >= 2U) {
                values[0] *= along_x;
                values[1] *= along_y;
            } else if (filter.kind == LayerFilterKind::drop_shadow && values.size() >= 7U) {
                const double x = values[4];
                const double y = values[5];
                values[4] = group.scale_x * x + group.shear_x * y;
                values[5] = group.shear_y * x + group.scale_y * y;
                values[6] *= area;
            }
        }
        output.rounded_clips = effect->rounded_clips;
        mask_turned_group(group, effect->x, effect->y, effect->width, effect->height,
                          effect->radii, output.rounded_clips);
    }
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
        if (batches_saved_) {
            working_.batches = std::move(batches_);
            working_.materials = std::move(materials_);
            working_.effects = std::move(effects_);
        }
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
        materials_ = std::move(working_.materials);
        effects_ = std::move(working_.effects);
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
                working_.vertices.data() + patch.offset,
                working_.vertices.data() + patch.offset + patch.bytes.size()
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
    /**
     * Whether the batches, or the ids they name, differ from the retained epoch's. Sample
     * revisions are the decoder's own annotation, assigned after this comparison.
     */
    [[nodiscard]] bool batches_changed(const RenderPacket& current) const noexcept {
        if (batches_.size() != current.batches.size() || materials_ != current.materials ||
            effects_ != current.effects) {
            return true;
        }
        for (std::size_t index = 0U; index < batches_.size(); ++index) {
            const auto* previous = std::get_if<EffectBatch>(&batches_[index]);
            const auto* next = std::get_if<EffectBatch>(&current.batches[index]);
            if (previous != nullptr && next != nullptr) {
                EffectBatch unannotated = *previous;
                unannotated.sample_revision = next->sample_revision;
                if (unannotated != *next)
                    return true;
            } else if (batches_[index] != current.batches[index]) {
                return true;
            }
        }
        return false;
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
    std::vector<std::string> materials_;
    std::vector<std::string> effects_;
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
        throw std::invalid_argument("render packet decoder requires protocol v16");
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
    if (vertex_bytes % packet_vertex_bytes != 0U || index_count > std::numeric_limits<std::uint32_t>::max() / 4U) {
        throw std::invalid_argument("render packet geometry counts are invalid");
    }
    std::vector<PresentationGroup> groups = presentation_groups(input);

    std::vector<ResourceOperation> resources;
    resources.reserve(input.plausible(resource_count, 8U));
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
        const bool groups_changed = groups != retained_->groups;
        std::vector<GeometryDirtyRegion> group_regions;
        if (groups_changed)
            append_group_regions(retained_->groups, groups, group_extents_, group_regions);
        TextureSizes sizes = texture_sizes_;
        const TextureChanges changed_textures = texture_changes(resources, sizes);
        retained_->frame_index = frame_index;
        retained_->planned_draw_count = planned_draw_count;
        retained_->skipped_draw_count = skipped_draw_count;
        retained_->resources = std::move(resources);
        retained_->full_geometry_payload = false;
        retained_->vertex_patches.clear();
        retained_->index_patches.clear();
        retained_->geometry_dirty_all = false;
        retained_->geometry_dirty_regions.swap(group_regions);
        if (groups_changed) {
            for (const auto& [position, local] : grouped_batches_)
                present(local, groups, retained_->batches[position]);
            retained_->groups = std::move(groups);
        }
        append_texture_regions(*retained_, retained_->groups, changed_textures, sizes,
                               retained_->geometry_dirty_regions);
        texture_sizes_.swap(sizes);
        assign_sample_revisions(*retained_);
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
    // A complete payload decodes into the buffers of the packet before last, so steady frames
    // reuse their allocations; the retained packet stays intact until the new one is valid.
    RenderPacket result = has_geometry_patches ? std::move(*retained_)
                          : spare_.has_value() ? std::move(*spare_)
                                               : RenderPacket{};
    if (!has_geometry_patches)
        spare_.reset();
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
            packet_vertex_bytes,
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
            result.geometry_dirty_regions.push_back(
                dirty_region(result.vertices, patch, result.groups, groups));
        }
        result.geometry_dirty_all = !result.index_patches.empty();
        patch_transaction->apply(result.vertex_patches, result.index_patches);
        patch_transaction->save_batches();
    } else {
        const std::span<const std::uint8_t> vertices = input.raw(vertex_bytes);
        result.vertices.assign(vertices.begin(), vertices.end());
        const std::span<const std::uint8_t> indices =
            input.raw(static_cast<std::size_t>(index_count) * sizeof(std::uint32_t));
        result.indices.resize(index_count);
        if constexpr (std::endian::native == std::endian::little) {
            if (!indices.empty())
                std::memcpy(result.indices.data(), indices.data(), indices.size());
        } else {
            for (std::size_t index = 0U; index < index_count; ++index) {
                const std::uint8_t* const at = indices.data() + index * sizeof(std::uint32_t);
                result.indices[index] = static_cast<std::uint32_t>(at[0U]) |
                                        static_cast<std::uint32_t>(at[1U]) << 8U |
                                        static_cast<std::uint32_t>(at[2U]) << 16U |
                                        static_cast<std::uint32_t>(at[3U]) << 24U;
            }
        }
    }
    result.materials = id_table(input, "material");
    result.effects = id_table(input, "effect");
    result.batches.clear();
    result.batches.reserve(input.plausible(batch_count, 8U));
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
            draw.material = batch.u16();
            const std::uint8_t blend = batch.u8();
            const std::uint8_t coverage = batch.u8();
            if (draw.material >= result.materials.size() || blend >= draw_blend_count ||
                coverage > STRATA_RENDER_CLIP_COVERAGE_MULTIPLY) {
                throw std::invalid_argument(
                    "render batch material, blend or clip coverage is outside its domain");
            }
            draw.blend = static_cast<DrawBlend>(blend);
            draw.clip_coverage = static_cast<ClipCoverage>(coverage);
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
            blur.group = group_index(batch);
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
            effect.group = group_index(batch);
            effect.x = batch.number();
            effect.y = batch.number();
            effect.width = batch.number();
            effect.height = batch.number();
            for (double& radius : effect.radii)
                radius = batch.number();
            effect.effect = batch.u16();
            if (effect.effect >= result.effects.size() || batch.u16() != 0U)
                throw std::invalid_argument("render effect batch names no declared effect");
            effect.opacity = batch.number();
            effect.refresh_rate = batch.number();
            const std::uint32_t backdrop_source = batch.u32();
            if (backdrop_source > static_cast<std::uint32_t>(EffectBackdropSource::surface)) {
                throw std::invalid_argument("render effect backdrop source is unknown");
            }
            effect.backdrop_source = static_cast<EffectBackdropSource>(backdrop_source);
            effect.probe = batch.u64();
            effect.read_reach = batch.number();
            effect.parameter_count = batch.u32();
            if (effect.parameter_count > effect.parameters.size() ||
                !std::isfinite(effect.read_reach) || effect.read_reach < 0.0 ||
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
            if (effect.kind != EffectBatchKind::backdrop && effect.probe != 0U) {
                throw std::invalid_argument("content effects cannot probe a backdrop");
            }
            for (std::uint32_t parameter = 0U; parameter < effect.parameter_count; ++parameter) {
                effect.parameters[parameter] = batch.number();
                if (!std::isfinite(effect.parameters[parameter])) {
                    throw std::invalid_argument("render effect parameter is not finite");
                }
            }
            const std::uint32_t composite = batch.u32();
            const std::uint32_t filter_count = batch.u32();
            if (composite >= layer_composite_count || filter_count > maximum_layer_filters) {
                throw std::invalid_argument("render layer composite or filters are unknown");
            }
            if (effect.kind == EffectBatchKind::backdrop &&
                (composite != STRATA_LAYER_COMPOSITE_SOURCE_OVER || filter_count != 0U)) {
                throw std::invalid_argument("backdrop effects cannot composite as layers");
            }
            effect.composite = static_cast<LayerComposite>(composite);
            effect.filters.reserve(filter_count);
            for (std::uint32_t filter_index = 0U; filter_index < filter_count; ++filter_index) {
                LayerFilter filter;
                const std::uint32_t filter_kind = batch.u32();
                const std::uint32_t value_count = batch.u32();
                if (filter_kind < STRATA_LAYER_FILTER_BLUR ||
                    filter_kind > STRATA_LAYER_FILTER_DROP_SHADOW ||
                    value_count > maximum_layer_filter_values) {
                    throw std::invalid_argument("render layer filter is unknown");
                }
                filter.kind = static_cast<LayerFilterKind>(filter_kind);
                filter.values.resize(value_count);
                for (double& value : filter.values) {
                    value = batch.number();
                    if (!std::isfinite(value))
                        throw std::invalid_argument("render layer filter value is not finite");
                }
                effect.filters.push_back(std::move(filter));
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
    std::vector<std::pair<std::size_t, SubmissionBatch>> grouped;
    for (std::size_t position = 0U; position < result.batches.size(); ++position) {
        const SubmissionBatch& batch = result.batches[position];
        const BlurBatch* blur = std::get_if<BlurBatch>(&batch);
        const EffectBatch* effect = std::get_if<EffectBatch>(&batch);
        if ((blur != nullptr && blur->group != 0U) || (effect != nullptr && effect->group != 0U))
            grouped.emplace_back(position, batch);
    }
    for (const auto& [position, local] : grouped)
        present(local, groups, result.batches[position]);
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
    const bool batches_changed =
        !has_geometry_patches || patch_transaction->batches_changed(result);
    if (has_geometry_patches && batches_changed) {
        result.geometry_dirty_all = true;
        result.geometry_dirty_regions.clear();
    }
    GroupExtents extents = group_extents(result.vertices, groups);
    if (!result.geometry_dirty_all && groups != result.groups)
        append_group_regions(result.groups, groups, extents, result.geometry_dirty_regions);
    // A vertex patch keeps the vertex count, so ranges validated for the retained epoch stay valid
    // unless an index or a batch changed.
    if (batches_changed || !result.index_patches.empty())
        validate_geometry(result);
    if (retained_geometry) {
        if (result.vertices != retained_->vertices || result.indices != retained_->indices) {
            throw std::invalid_argument(
                "retained render geometry epoch changed its vertex or index payload");
        }
        if (!same_batch_shape(result.batches, retained_->batches) ||
            result.materials != retained_->materials || result.effects != retained_->effects) {
            throw std::invalid_argument("retained render geometry epoch changed its batch shape");
        }
    }
    TextureSizes sizes = texture_sizes_;
    const TextureChanges changed_textures = texture_changes(result.resources, sizes);
    if (!result.geometry_dirty_all) {
        append_texture_regions(result, groups, changed_textures, sizes,
                               result.geometry_dirty_regions);
    }
    result.groups = std::move(groups);
    if (!has_geometry_patches && retained_.has_value())
        spare_ = std::move(*retained_);
    retained_ = std::move(result);
    if (patch_transaction.has_value()) patch_transaction->commit();
    grouped_batches_.swap(grouped);
    group_extents_.swap(extents);
    texture_sizes_.swap(sizes);
    assign_sample_revisions(*retained_);
    return *retained_;
}

void RenderPacketDecoder::assign_sample_revisions(RenderPacket& packet) {
    const auto touched = [&packet](const EffectBatch& effect) {
        if (packet.geometry_dirty_all)
            return true;
        const double left = effect.x - effect.read_reach;
        const double top = effect.y - effect.read_reach;
        const double right = effect.x + effect.width + effect.read_reach;
        const double bottom = effect.y + effect.height + effect.read_reach;
        return std::ranges::any_of(packet.geometry_dirty_regions,
                                   [&](const GeometryDirtyRegion& region) {
                                       return region.x <= right &&
                                              region.x + region.width >= left &&
                                              region.y <= bottom &&
                                              region.y + region.height >= top;
                                   });
    };
    std::map<std::pair<std::uint32_t, bool>, EffectSample> next;
    for (SubmissionBatch& batch : packet.batches) {
        EffectBatch* const effect = std::get_if<EffectBatch>(&batch);
        if (effect == nullptr)
            continue;
        EffectBatch signature = *effect;
        signature.sample_revision = 0U;
        const std::string& id = packet.effects[effect->effect];
        const std::pair<std::uint32_t, bool> key{effect->source_order,
                                                 effect->kind == EffectBatchKind::content_begin};
        const auto prior = effect_samples_.find(key);
        const bool current = prior != effect_samples_.end() &&
                             prior->second.signature == signature && prior->second.id == id &&
                             !touched(signature);
        effect->sample_revision = current ? prior->second.revision : next_sample_revision_++;
        next.insert_or_assign(key, EffectSample{std::move(signature), id, effect->sample_revision});
    }
    effect_samples_.swap(next);
}

void RenderPacketDecoder::reset() noexcept {
    retained_.reset();
    spare_.reset();
    grouped_batches_.clear();
    group_extents_.clear();
    texture_sizes_.clear();
    // Revisions keep counting, so no sample a backend retained before the reset can match.
    effect_samples_.clear();
}

} // namespace strata::host
