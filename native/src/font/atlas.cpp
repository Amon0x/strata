#include "font/atlas.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

namespace strata::font {
namespace {

[[nodiscard]] std::string owner_token(const std::string_view owner_id) {
    // Length-prefixing is injective for arbitrary owner bytes; a fixed-width hash would make
    // cross-runtime host resource ownership merely improbable rather than collision-free.
    return std::to_string(owner_id.size()) + ":" + std::string(owner_id);
}

[[nodiscard]] AtlasTextureFormat format_for(const GlyphRasterMode mode,
                                            const bool filtered_coverage) noexcept {
    if (mode == GlyphRasterMode::subpixel)
        return AtlasTextureFormat::rgba8_exact;
    if (mode != GlyphRasterMode::coverage)
        return AtlasTextureFormat::rgba8;
    return filtered_coverage ? AtlasTextureFormat::r8_filtered : AtlasTextureFormat::r8;
}

/** The end of a page's texture name, which says what its texels hold. */
[[nodiscard]] const char* page_suffix(const GlyphRasterMode mode) noexcept {
    return mode == GlyphRasterMode::coverage   ? "/coverage"
           : mode == GlyphRasterMode::subpixel ? "/subpixel"
                                               : "/msdf";
}

[[nodiscard]] std::size_t bytes_per_pixel(const GlyphRasterMode mode) noexcept {
    return mode == GlyphRasterMode::coverage ? 1U : 4U;
}

void advance(std::uint64_t& generation) {
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
        throw FontError("glyph atlas generation exhausted");
    }
    ++generation;
}

struct RasterCacheKey final {
    std::uintptr_t font_identity = 0U;
    std::uint16_t glyph = 0U;
    std::uint32_t font_style_flags = 0U;
    std::uint64_t pixel_size = 0U;
    std::uint64_t display_scale = 0U;
    GlyphRasterMode mode = GlyphRasterMode::coverage;
    SubpixelPhase phase;
    std::array<std::uint64_t, 12U> config{};

    [[nodiscard]] friend auto operator<=>(const RasterCacheKey&, const RasterCacheKey&) = default;
};

struct RasterCacheIdentity final {
    RasterCacheKey key;
    std::shared_ptr<const void> font_owner;
};

using SharedRaster = std::shared_ptr<const std::optional<GlyphRasterBitmap>>;

class SharedGlyphRasterCache final {
  public:
    [[nodiscard]] SharedRaster find(const RasterCacheKey& key) {
        const std::scoped_lock lock(mutex_);
        const auto found = entries_.find(key);
        if (found == entries_.end())
            return {};
        found->second.last_use = ++clock_;
        return found->second.raster;
    }

    [[nodiscard]] SharedRaster insert(RasterCacheIdentity identity, SharedRaster raster) {
        const std::scoped_lock lock(mutex_);
        if (const auto found = entries_.find(identity.key); found != entries_.end()) {
            found->second.last_use = ++clock_;
            return found->second.raster;
        }
        const std::size_t byte_count =
            raster != nullptr && raster->has_value() ? raster->value().bytes.size() : 0U;
        total_bytes_ += byte_count;
        entries_.emplace(identity.key, Entry{
                                           std::move(identity.font_owner),
                                           std::move(raster),
                                           byte_count,
                                           ++clock_,
                                       });
        trim();
        return entries_.find(identity.key)->second.raster;
    }

  private:
    struct Entry final {
        std::shared_ptr<const void> font_owner;
        SharedRaster raster;
        std::size_t byte_count = 0U;
        std::uint64_t last_use = 0U;
    };

    void trim() {
        constexpr std::size_t maximum_entries = 8'192U;
        constexpr std::size_t maximum_bytes = 128U * 1'024U * 1'024U;
        while (entries_.size() > maximum_entries ||
               (total_bytes_ > maximum_bytes && entries_.size() > 1U)) {
            const auto oldest = std::ranges::min_element(
                entries_, {}, [](const auto& entry) { return entry.second.last_use; });
            total_bytes_ -= oldest->second.byte_count;
            entries_.erase(oldest);
        }
    }

    std::mutex mutex_;
    std::map<RasterCacheKey, Entry> entries_;
    std::size_t total_bytes_ = 0U;
    std::uint64_t clock_ = 0U;
};

[[nodiscard]] SharedGlyphRasterCache& shared_raster_cache() {
    static SharedGlyphRasterCache cache;
    return cache;
}

[[nodiscard]] std::array<std::uint64_t, 12U>
raster_config_identity(const GlyphRasterMode mode, const CoverageRasterConfig& coverage,
                       const MsdfRasterConfig& msdf, const SubpixelOrder order) noexcept {
    std::array<std::uint64_t, 12U> result{};
    if (mode == GlyphRasterMode::coverage || mode == GlyphRasterMode::color ||
        mode == GlyphRasterMode::subpixel) {
        result = {
            coverage.maximum_dimension,
            coverage.padding_pixels,
            static_cast<std::uint64_t>(coverage.hinting),
            std::bit_cast<std::uint64_t>(coverage.stem_darkening_pixels),
            std::bit_cast<std::uint64_t>(coverage.stem_darkening_taper_start_physical_pixel_size),
            std::bit_cast<std::uint64_t>(coverage.stem_darkening_taper_end_physical_pixel_size),
            std::bit_cast<std::uint64_t>(coverage.transfer_strength),
            mode == GlyphRasterMode::subpixel ? static_cast<std::uint64_t>(order) : 0U,
        };
    } else {
        result = {
            msdf.maximum_dimension,
            msdf.padding_pixels,
            msdf.oversampling,
            std::bit_cast<std::uint64_t>(msdf.pixel_range),
            std::bit_cast<std::uint64_t>(msdf.flattening_tolerance_pixels),
            std::bit_cast<std::uint64_t>(msdf.corner_dot_threshold),
        };
    }
    return result;
}

[[nodiscard]] RasterCacheIdentity
raster_cache_identity(const OpenTypeFont& font, const std::uint16_t glyph, const double pixel_size,
                      const double display_scale, const SubpixelPhase phase,
                      const GlyphRasterMode mode, const std::uint32_t font_style_flags,
                      const CoverageRasterConfig& coverage, const MsdfRasterConfig& msdf,
                      const SubpixelOrder order) {
    std::shared_ptr<const void> owner = font.cache_identity();
    return RasterCacheIdentity{
        RasterCacheKey{
            reinterpret_cast<std::uintptr_t>(owner.get()),
            glyph,
            font_style_flags,
            std::bit_cast<std::uint64_t>(pixel_size),
            std::bit_cast<std::uint64_t>(mode != GlyphRasterMode::msdf ? display_scale : 1.0),
            mode,
            phase,
            raster_config_identity(mode, coverage, msdf, order),
        },
        std::move(owner),
    };
}

/**
 * A colour glyph: each layer's coverage, at the glyph's size, composited over the layers before
 * it in its palette colour, into one straight-RGBA bitmap over all of them.
 */
[[nodiscard]] std::optional<GlyphRasterBitmap>
rasterize_color(const OpenTypeFont& font, const std::uint16_t glyph, const double pixel_size,
                const double display_scale, const SubpixelPhase phase,
                const std::uint32_t font_style_flags, const CoverageRasterConfig& coverage) {
    struct Layer final {
        GlyphRasterBitmap bitmap;
        std::uint32_t rgba = 0U;
    };
    std::vector<Layer> layers;
    double left = std::numeric_limits<double>::infinity();
    double top = -std::numeric_limits<double>::infinity();
    double right = -std::numeric_limits<double>::infinity();
    double bottom = std::numeric_limits<double>::infinity();
    for (const ColorGlyphLayer& layer : font.color_layers(glyph)) {
        std::optional<GlyphRasterBitmap> bitmap = rasterize_coverage(
            font, layer.glyph, pixel_size, display_scale, phase, coverage, font_style_flags);
        if (!bitmap.has_value())
            continue;
        const RasterPlaneBounds& plane = bitmap->plane_bounds_layout_pixels;
        left = std::min(left, plane.left);
        top = std::max(top, plane.top);
        right = std::max(right, plane.right);
        bottom = std::min(bottom, plane.bottom);
        layers.push_back(Layer{std::move(*bitmap), layer.foreground ? 0xFF000000U : layer.rgba});
    }
    if (layers.empty())
        return std::nullopt;
    const double scale = display_scale > 0.0 ? display_scale : 1.0;
    const auto width = static_cast<std::uint32_t>(std::lround((right - left) * scale));
    const auto height = static_cast<std::uint32_t>(std::lround((top - bottom) * scale));
    if (width == 0U || height == 0U || width > 4096U || height > 4096U)
        return std::nullopt;
    GlyphRasterBitmap result;
    result.width = width;
    result.height = height;
    result.mode = GlyphRasterMode::color;
    result.bytes.assign(static_cast<std::size_t>(width) * height * 4U, 0U);
    result.plane_bounds_layout_pixels = RasterPlaneBounds{left, bottom, right, top};
    result.plane_bounds_font_units = layers.front().bitmap.plane_bounds_font_units;
    result.layout_pixels_per_font_unit = layers.front().bitmap.layout_pixels_per_font_unit;
    result.atlas_pixels_per_font_unit = layers.front().bitmap.atlas_pixels_per_font_unit;
    std::vector<float> premultiplied(static_cast<std::size_t>(width) * height * 4U, 0.0F);
    for (const Layer& layer : layers) {
        const RasterPlaneBounds& plane = layer.bitmap.plane_bounds_layout_pixels;
        const auto offset_x = static_cast<long>(std::lround((plane.left - left) * scale));
        const auto offset_y = static_cast<long>(std::lround((top - plane.top) * scale));
        const float red = static_cast<float>(layer.rgba & 0xFFU) / 255.0F;
        const float green = static_cast<float>((layer.rgba >> 8U) & 0xFFU) / 255.0F;
        const float blue = static_cast<float>((layer.rgba >> 16U) & 0xFFU) / 255.0F;
        const float alpha = static_cast<float>(layer.rgba >> 24U) / 255.0F;
        for (std::uint32_t y = 0U; y < layer.bitmap.height; ++y) {
            const long target_y = offset_y + static_cast<long>(y);
            if (target_y < 0 || target_y >= static_cast<long>(height))
                continue;
            for (std::uint32_t x = 0U; x < layer.bitmap.width; ++x) {
                const long target_x = offset_x + static_cast<long>(x);
                if (target_x < 0 || target_x >= static_cast<long>(width))
                    continue;
                const float cover =
                    alpha *
                    static_cast<float>(
                        layer.bitmap.bytes[static_cast<std::size_t>(y) * layer.bitmap.width + x]) /
                    255.0F;
                if (cover <= 0.0F)
                    continue;
                float* const out =
                    premultiplied.data() + (static_cast<std::size_t>(target_y) * width +
                                            static_cast<std::size_t>(target_x)) *
                                               4U;
                // Over: this layer above what the layers before it laid down.
                out[0] = red * cover + out[0] * (1.0F - cover);
                out[1] = green * cover + out[1] * (1.0F - cover);
                out[2] = blue * cover + out[2] * (1.0F - cover);
                out[3] = cover + out[3] * (1.0F - cover);
            }
        }
    }
    for (std::size_t texel = 0U; texel < premultiplied.size(); texel += 4U) {
        const float a = premultiplied[texel + 3U];
        const auto channel = [a](const float value) {
            return static_cast<std::uint8_t>(
                std::lround(std::clamp(a > 0.0F ? value / a : 0.0F, 0.0F, 1.0F) * 255.0F));
        };
        result.bytes[texel] = channel(premultiplied[texel]);
        result.bytes[texel + 1U] = channel(premultiplied[texel + 1U]);
        result.bytes[texel + 2U] = channel(premultiplied[texel + 2U]);
        result.bytes[texel + 3U] =
            static_cast<std::uint8_t>(std::lround(std::clamp(a, 0.0F, 1.0F) * 255.0F));
    }
    return result;
}

[[nodiscard]] std::optional<GlyphRasterBitmap>
rasterize(const OpenTypeFont& font, const std::uint16_t glyph, const double pixel_size,
          const double display_scale, const SubpixelPhase phase, const GlyphRasterMode mode,
          const std::uint32_t font_style_flags, const CoverageRasterConfig& coverage,
          const MsdfRasterConfig& msdf, const SubpixelOrder order) {
    if (mode == GlyphRasterMode::color)
        return rasterize_color(font, glyph, pixel_size, display_scale, phase, font_style_flags,
                               coverage);
    if (mode == GlyphRasterMode::subpixel)
        return rasterize_subpixel(font, glyph, pixel_size, display_scale, order, phase, coverage,
                                  font_style_flags);
    return mode == GlyphRasterMode::coverage
               ? rasterize_coverage(font, glyph, pixel_size, display_scale, phase, coverage,
                                    font_style_flags)
               : rasterize_msdf(font, glyph, pixel_size, msdf, font_style_flags);
}

} // namespace

struct GlyphAtlas::Key final {
    std::string font_id;
    std::uint16_t glyph = 0U;
    std::uint32_t font_style_flags = 0U;
    std::uint64_t pixel_size = 0U;
    std::uint64_t display_scale = 0U;
    GlyphRasterMode mode = GlyphRasterMode::coverage;
    SubpixelPhase phase;
    std::uint64_t generation = 0U;
    /** An MSDF glyph's widened reach, in whole layout pixels (0: the configured range). */
    std::uint32_t reach = 0U;
    /** A coverage or subpixel glyph rasterized without hinting. */
    bool unhinted = false;

    [[nodiscard]] friend auto operator<=>(const Key&, const Key&) = default;
};

struct GlyphAtlas::Page final {
    std::string texture;
    std::uint32_t index = 0U;
    GlyphRasterMode mode = GlyphRasterMode::coverage;
    std::uint32_t next_x = 0U;
    std::uint32_t next_y = 0U;
    std::uint32_t row_height = 0U;
    std::vector<std::uint8_t> pixels;
    std::optional<AtlasPixelRect> dirty;
    /** The preparation that last packed into or drew from this page. */
    std::uint64_t used_preparation = 0U;
    /** Counts the resets that dropped what the page held, so a slot reserved before one knows. */
    std::uint64_t epoch = 0U;
};

struct GlyphAtlas::Allocation final {
    std::size_t page = 0U;
    AtlasPixelRect upload;
    AtlasPixelRect content;
};

GlyphAtlas::GlyphAtlas(const std::string_view owner_id, GlyphAtlasConfig config)
    : owner_token_(owner_token(owner_id)), config_(std::move(config)) {
    if (owner_id.empty() || config_.page_size == 0U || config_.maximum_pages == 0U ||
        config_.maximum_cached_glyphs == 0U ||
        config_.maximum_generation_recycles_per_preparation == 0U ||
        config_.gutter_pixels > config_.page_size / 2U) {
        throw std::invalid_argument("glyph atlas owner and limits must be valid");
    }
    config_.coverage.maximum_dimension = config_.page_size;
    config_.msdf.maximum_dimension = config_.page_size;
}

GlyphAtlas::~GlyphAtlas() = default;

void GlyphAtlas::adopt_display_scale(const double display_scale) {
    if (!std::isfinite(display_scale) || display_scale <= 0.0) {
        throw std::invalid_argument("glyph atlas display scale must be finite and positive");
    }
    if (display_scale_ == display_scale)
        return;
    display_scale_ = display_scale;
    invalidate_pages();
}

void GlyphAtlas::adopt_subpixel_order(const SubpixelOrder order) {
    if (subpixel_order_ == order)
        return;
    subpixel_order_ = order;
    invalidate_pages();
}

SubpixelOrder GlyphAtlas::subpixel_order() const noexcept {
    return subpixel_order_;
}

bool GlyphAtlas::begin_frame_preparation() {
    if (frame_preparation_active_) {
        throw std::logic_error("glyph atlas frame preparation is already active");
    }
    frame_preparation_active_ = true;
    preparation_recycles_ = 0U;
    ++preparation_serial_;
    retention_known_ = false;
    if (!reclamation_pending_)
        return false;
    invalidate_pages();
    reclamation_pending_ = false;
    ++preparation_recycles_;
    ++generation_recycle_count_;
    return true;
}

void GlyphAtlas::end_frame_preparation() noexcept {
    frame_preparation_active_ = false;
    preparation_recycles_ = 0U;
}

std::optional<GlyphAtlasEntry>
GlyphAtlas::request_coverage(const std::string_view font_id, const OpenTypeFont& font,
                             const std::uint16_t glyph, const double pixel_size,
                             const SubpixelPhase phase, const std::uint32_t font_style_flags) {
    return request_mode(font_id, font, glyph, pixel_size, phase, GlyphRasterMode::coverage,
                        font_style_flags);
}

std::optional<GlyphAtlasEntry>
GlyphAtlas::request(const std::string_view font_id, const OpenTypeFont& font,
                    const std::uint16_t glyph, const double pixel_size,
                    const SubpixelPhase coverage_phase, const std::uint32_t font_style_flags,
                    const GlyphRasterMode mode, const double reach, const bool unhinted) {
    const bool positioned = mode == GlyphRasterMode::coverage || mode == GlyphRasterMode::subpixel;
    return request_mode(font_id, font, glyph, pixel_size,
                        positioned ? coverage_phase : SubpixelPhase{}, mode, font_style_flags,
                        reach, unhinted);
}

struct GlyphAtlas::Resolved final {
    Key key;
    MsdfRasterConfig msdf;
    CoverageRasterConfig coverage;
};

GlyphAtlas::Resolved GlyphAtlas::resolve(const std::string_view font_id, const OpenTypeFont& font,
                                         const std::uint16_t glyph, const double pixel_size,
                                         const SubpixelPhase phase, const GlyphRasterMode mode,
                                         const std::uint32_t font_style_flags,
                                         const double reach, const bool unhinted) const {
    if (font_id.empty())
        throw std::invalid_argument("glyph atlas font id must not be empty");
    // A colour glyph (an emoji) draws its own colours, whatever its text asked for.
    const GlyphRasterMode effective_mode =
        !font.color_layers(glyph).empty() ? GlyphRasterMode::color : mode;
    // The selected face is already part of identity. Collapse known bits supplied intrinsically
    // by that face, while retaining every opaque/forward-compatible bit exactly.
    const std::uint32_t realized_style_flags = (font_style_flags & ~known_font_style_flags) |
                                               synthetic_font_style_flags(font, font_style_flags);
    Resolved result{
        Key{
            std::string(font_id),
            glyph,
            realized_style_flags,
            std::bit_cast<std::uint64_t>(pixel_size),
            std::bit_cast<std::uint64_t>(effective_mode != GlyphRasterMode::msdf ? display_scale_
                                                                                 : 1.0),
            effective_mode,
            effective_mode == GlyphRasterMode::coverage ||
                    effective_mode == GlyphRasterMode::subpixel
                ? phase
                : SubpixelPhase{},
            generation_,
        },
        config_.msdf,
        config_.coverage,
    };
    // Text drawn at a scale that keeps changing (a zoom) is not grid-fitted: hinting would move
    // its stems and x-height a pixel at a time as the size passes through, and the text would
    // shimmer while it scales.
    if (unhinted && (effective_mode == GlyphRasterMode::coverage ||
                     effective_mode == GlyphRasterMode::subpixel)) {
        result.key.unhinted = true;
        result.coverage.hinting = GlyphHinting::none;
    }
    // A distance that must reach further than the configured range: the field widened to
    // cover it on both sides of the edge, with padding to hold it.
    if (effective_mode == GlyphRasterMode::msdf && reach > 0.0 && std::isfinite(reach)) {
        MsdfRasterConfig& msdf = result.msdf;
        result.key.reach = static_cast<std::uint32_t>(std::ceil(std::min(reach, 64.0)));
        const double oversampling = static_cast<double>(std::max(msdf.oversampling, 1U));
        msdf.pixel_range = std::max(
            msdf.pixel_range, 2.0 * (static_cast<double>(result.key.reach) + 1.5) * oversampling);
        msdf.padding_pixels =
            std::max(msdf.padding_pixels,
                     static_cast<std::uint32_t>(std::ceil(msdf.pixel_range * 0.5)) + 2U);
    }
    return result;
}

std::optional<GlyphAtlasEntry>
GlyphAtlas::request_mode(const std::string_view font_id, const OpenTypeFont& font,
                         const std::uint16_t glyph, const double pixel_size,
                         const SubpixelPhase phase, const GlyphRasterMode mode,
                         const std::uint32_t font_style_flags, const double reach,
                         const bool unhinted) {
    Resolved resolved = resolve(font_id, font, glyph, pixel_size, phase, mode, font_style_flags,
                                reach, unhinted);
    Key& key = resolved.key;
    const MsdfRasterConfig& msdf = resolved.msdf;
    const GlyphRasterMode effective_mode = key.mode;
    const std::uint32_t realized_style_flags = key.font_style_flags;
    if (const auto found = cache_.find(key); found != cache_.end()) {
        if (!frame_preparation_active_)
            flush_pending_uploads();
        return found->second;
    }
    RasterCacheIdentity identity =
        raster_cache_identity(font, glyph, pixel_size, display_scale_, key.phase, effective_mode,
                              realized_style_flags, resolved.coverage, msdf, subpixel_order_);
    SharedRaster bitmap = shared_raster_cache().find(identity.key);
    if (bitmap == nullptr) {
        bitmap = std::make_shared<const std::optional<GlyphRasterBitmap>>(
            rasterize(font, glyph, pixel_size, display_scale_, key.phase, effective_mode,
                      realized_style_flags, resolved.coverage, msdf, subpixel_order_));
        bitmap = shared_raster_cache().insert(std::move(identity), std::move(bitmap));
    }
    if (!bitmap->has_value())
        return std::nullopt;
    return insert_bitmap(key, bitmap->value());
}

std::optional<GlyphAtlasEntry> GlyphAtlas::request_raster(
    const std::string_view key, const GlyphRasterMode mode,
    const std::function<std::optional<RasterRequest>(std::uint32_t)>& encode) {
    if (key.empty())
        throw std::invalid_argument("a raster atlas key must not be empty");
    // A leading '$' keeps raster keys apart from every font id.
    Key cache_key{
        "$mask:" + std::string(key),
        0U,
        0U,
        0U,
        std::bit_cast<std::uint64_t>(display_scale_),
        mode,
        SubpixelPhase{},
        generation_,
    };
    if (const auto found = cache_.find(cache_key); found != cache_.end()) {
        if (!frame_preparation_active_)
            flush_pending_uploads();
        if (found->second.page_index < pages_.size())
            pages_[found->second.page_index].used_preparation = preparation_serial_;
        return found->second;
    }
    std::optional<RasterRequest> request = encode(config_.gutter_pixels);
    const std::uint64_t gutter = config_.gutter_pixels;
    if (!request.has_value() || request->width == 0U || request->height == 0U ||
        request->width + gutter * 2U > config_.page_size ||
        request->height + gutter * 2U > config_.page_size) {
        return std::nullopt;
    }
    try {
        const Allocation allocation = place(cache_key, request->width, request->height, mode);
        Page& page = pages_.at(allocation.page);
        page.used_preparation = preparation_serial_;
        operations_.push_back(AtlasOperation{
            AtlasOperationKind::raster,
            page.texture,
            format_for(mode, config_.filtered_coverage),
            allocation.upload,
            std::move(request->job),
        });
        GlyphAtlasEntry entry =
            entry_for(cache_key, allocation, request->plane_bounds, 1.0, 1.0, mode);
        cache_.emplace(cache_key, entry);
        return entry;
    } catch (const FontError&) {
        // Full pages: the shape draws another way until reclamation runs on a safe frame.
        return std::nullopt;
    }
}

std::optional<RasterSlot> GlyphAtlas::reserve_raster_slot(const std::uint32_t width,
                                                          const std::uint32_t height,
                                                          const GlyphRasterMode mode) {
    const std::uint64_t gutter = config_.gutter_pixels;
    if (width == 0U || height == 0U || width + gutter * 2U > config_.page_size ||
        height + gutter * 2U > config_.page_size) {
        return std::nullopt;
    }
    Key key{"$slot", 0U, 0U, 0U, std::bit_cast<std::uint64_t>(display_scale_), mode,
            SubpixelPhase{}, generation_};
    try {
        const Allocation allocation = place(key, width, height, mode);
        Page& page = pages_.at(allocation.page);
        page.used_preparation = preparation_serial_;
        return RasterSlot{page.texture, allocation.page, page.epoch, generation_,
                          allocation.upload, mode};
    } catch (const FontError&) {
        return std::nullopt;
    }
}

bool GlyphAtlas::raster_slot_fits(const RasterSlot& slot, const std::uint32_t width,
                                  const std::uint32_t height) const noexcept {
    const std::uint64_t gutter = config_.gutter_pixels;
    return slot.generation == generation_ && slot.page < pages_.size() &&
           pages_[slot.page].texture == slot.texture && pages_[slot.page].epoch == slot.page_epoch &&
           width + gutter * 2U <= slot.upload.width && height + gutter * 2U <= slot.upload.height;
}

GlyphAtlasEntry GlyphAtlas::write_raster_slot(const RasterSlot& slot, RasterRequest request) {
    if (!raster_slot_fits(slot, request.width, request.height))
        throw std::logic_error("raster slot no longer holds its place or is too small");
    Page& page = pages_[slot.page];
    page.used_preparation = preparation_serial_;
    // The whole slot is written: texels past this frame's content are cleared.
    operations_.push_back(AtlasOperation{
        AtlasOperationKind::raster,
        page.texture,
        format_for(slot.mode, config_.filtered_coverage),
        slot.upload,
        std::move(request.job),
    });
    const Allocation allocation{
        slot.page,
        slot.upload,
        AtlasPixelRect{slot.upload.x + config_.gutter_pixels, slot.upload.y + config_.gutter_pixels,
                       request.width, request.height},
    };
    const Key key{"$slot", 0U, 0U, 0U, std::bit_cast<std::uint64_t>(display_scale_), slot.mode,
                  SubpixelPhase{}, generation_};
    return entry_for(key, allocation, request.plane_bounds, 1.0, 1.0, slot.mode);
}

bool GlyphAtlas::warm(const std::span<const GlyphAtlasWarmupRequest> requests) {
    if (!frame_preparation_active_) {
        throw std::logic_error("glyph atlas warmup requires an active frame preparation");
    }
    const std::uint64_t initial_generation = generation_;
    struct Pending final {
        Key key;
        const OpenTypeFont* font = nullptr;
        RasterCacheIdentity identity;
        SharedRaster raster;
        MsdfRasterConfig msdf;
        CoverageRasterConfig coverage;
    };
    std::vector<Pending> pending;
    pending.reserve(requests.size());
    std::map<Key, std::size_t> unique;

    for (const GlyphAtlasWarmupRequest& request : requests) {
        if (request.font == nullptr || request.font_id.empty()) {
            throw std::invalid_argument("glyph atlas warmup requires a font and non-empty id");
        }
        Resolved resolved =
            resolve(request.font_id, *request.font, request.glyph, request.pixel_size,
                    request.phase, request.mode, request.font_style_flags, request.reach,
                    request.unhinted);
        Key& key = resolved.key;
        if (cache_.contains(key) || unique.contains(key))
            continue;
        RasterCacheIdentity identity = raster_cache_identity(
            *request.font, request.glyph, request.pixel_size, display_scale_, key.phase, key.mode,
            key.font_style_flags, resolved.coverage, resolved.msdf, subpixel_order_);
        SharedRaster raster = shared_raster_cache().find(identity.key);
        unique.emplace(key, pending.size());
        pending.push_back(Pending{
            std::move(key),
            request.font,
            std::move(identity),
            std::move(raster),
            resolved.msdf,
            resolved.coverage,
        });
    }

    std::vector<std::size_t> missing;
    missing.reserve(pending.size());
    for (std::size_t index = 0U; index < pending.size(); ++index) {
        if (pending[index].raster == nullptr)
            missing.push_back(index);
    }

    // Raster misses are independent immutable results. Use a small fixed ceiling rather than a
    // hardware-sized fan-out: one caller lane plus at most three temporary workers reduces a cold
    // atlas stall without monopolizing a host's render/game worker pool. Coverage rasterization
    // obtains one cached FT_Face per thread because FT_Face owns mutable size/transform state.
    constexpr std::size_t maximum_raster_lanes = 4U;
    constexpr std::size_t parallel_raster_threshold = 8U;
    const std::size_t available_lanes =
        std::max<std::size_t>(1U, static_cast<std::size_t>(std::thread::hardware_concurrency()));
    const std::size_t raster_lanes =
        missing.size() < parallel_raster_threshold
            ? 1U
            : std::min({maximum_raster_lanes, available_lanes, missing.size()});
    // Each lane leases a FreeType face per font and selects a size on it; grouping the misses by
    // font and size and handing lanes contiguous runs keeps a lane on one face and one size, so a
    // cold batch builds a few faces and runs each size's hinting program once, not per lane.
    std::ranges::sort(missing, {}, [&pending](const std::size_t index) {
        const Key& key = pending[index].key;
        return std::tie(key.font_id, key.pixel_size, key.mode, key.font_style_flags, key.phase,
                        key.glyph);
    });
    const std::size_t run = std::max<std::size_t>(1U, missing.size() / (raster_lanes * 4U));
    std::atomic_size_t cursor{0U};
    std::atomic_bool failed{false};
    std::mutex failure_mutex;
    std::exception_ptr failure;
    const auto raster_lane = [&] {
        while (!failed.load(std::memory_order_acquire)) {
            const std::size_t first = cursor.fetch_add(run, std::memory_order_relaxed);
            if (first >= missing.size())
                return;
            const std::size_t last = std::min(missing.size(), first + run);
            for (std::size_t position = first; position < last; ++position) {
                Pending& item = pending[missing[position]];
                try {
                    item.raster =
                        std::make_shared<const std::optional<GlyphRasterBitmap>>(rasterize(
                            *item.font, item.key.glyph, std::bit_cast<double>(item.key.pixel_size),
                            display_scale_, item.key.phase, item.key.mode,
                            item.key.font_style_flags, item.coverage, item.msdf,
                            subpixel_order_));
                } catch (...) {
                    {
                        const std::scoped_lock lock(failure_mutex);
                        if (failure == nullptr)
                            failure = std::current_exception();
                    }
                    failed.store(true, std::memory_order_release);
                    return;
                }
            }
        }
    };
    std::vector<std::jthread> workers;
    workers.reserve(raster_lanes - 1U);
    for (std::size_t lane = 1U; lane < raster_lanes; ++lane) {
        workers.emplace_back(raster_lane);
    }
    raster_lane();
    for (std::jthread& worker : workers)
        worker.join();
    if (failure != nullptr)
        std::rethrow_exception(failure);

    for (const std::size_t index : missing) {
        Pending& item = pending[index];
        item.raster =
            shared_raster_cache().insert(std::move(item.identity), std::move(item.raster));
    }
    for (Pending& item : pending) {
        if (item.key.generation != generation_)
            return false;
        if (!item.raster->has_value())
            continue;
        static_cast<void>(insert_bitmap(item.key, item.raster->value()));
        if (generation_ != initial_generation)
            return false;
    }
    flush_pending_uploads();
    return true;
}

std::vector<AtlasOperation> GlyphAtlas::take_operations() {
    flush_pending_uploads();
    std::vector<AtlasOperation> result;
    result.swap(operations_);
    return result;
}

std::span<const AtlasOperation> GlyphAtlas::pending_operations() {
    flush_pending_uploads();
    return std::span<const AtlasOperation>(operations_);
}

void GlyphAtlas::commit_operations() noexcept {
    operations_.clear();
}

std::vector<AtlasOperation> GlyphAtlas::plan_terminal_release() const {
    std::size_t candidate_count = pages_.size();
    for (const AtlasOperation& operation : operations_) {
        if (operation.kind != AtlasOperationKind::release)
            continue;
        if (candidate_count == std::numeric_limits<std::size_t>::max()) {
            throw std::length_error("glyph atlas terminal release plan exceeds size_t");
        }
        ++candidate_count;
    }

    std::vector<AtlasOperation> releases;
    releases.reserve(candidate_count);
    const auto append_release = [&releases](const std::string& texture,
                                            const AtlasTextureFormat format) {
        const bool duplicate =
            std::ranges::any_of(releases, [&texture](const AtlasOperation& release) {
                return release.texture == texture;
            });
        if (duplicate)
            return;
        releases.push_back(AtlasOperation{
            AtlasOperationKind::release,
            texture,
            format,
            {},
            {},
        });
    };

    // A release can remain queued after its page was invalidated but before a normal frame handed
    // the operation to the host. Preserve those releases without replaying unsubmitted payloads.
    for (const AtlasOperation& operation : operations_) {
        if (operation.kind == AtlasOperationKind::release) {
            append_release(operation.texture, operation.format);
        }
    }
    for (const Page& page : pages_) {
        append_release(page.texture, format_for(page.mode, config_.filtered_coverage));
    }
    return releases;
}

void GlyphAtlas::commit_terminal_release() noexcept {
    pages_.clear();
    cache_.clear();
    operations_.clear();
    frame_preparation_active_ = false;
    reclamation_pending_ = false;
    preparation_recycles_ = 0U;
}

std::uint64_t GlyphAtlas::generation() const noexcept {
    return generation_;
}
std::size_t GlyphAtlas::cached_glyph_count() const noexcept {
    return cache_.size();
}
bool GlyphAtlas::reclamation_pending() const noexcept {
    return reclamation_pending_;
}
bool GlyphAtlas::frame_preparation_active() const noexcept {
    return frame_preparation_active_;
}
std::uint64_t GlyphAtlas::generation_recycle_count() const noexcept {
    return generation_recycle_count_;
}

std::optional<GlyphAtlas::Allocation> GlyphAtlas::allocate(const std::uint32_t width,
                                                           const std::uint32_t height,
                                                           const GlyphRasterMode mode) {
    const std::uint64_t gutter = config_.gutter_pixels;
    const std::uint64_t upload_width64 = static_cast<std::uint64_t>(width) + gutter * 2U;
    const std::uint64_t upload_height64 = static_cast<std::uint64_t>(height) + gutter * 2U;
    if (upload_width64 > config_.page_size || upload_height64 > config_.page_size) {
        throw FontError("glyph bitmap plus atlas gutter exceeds the page size");
    }
    const auto upload_width = static_cast<std::uint32_t>(upload_width64);
    const auto upload_height = static_cast<std::uint32_t>(upload_height64);
    for (std::size_t index = 0U; index < pages_.size(); ++index) {
        Page& page = pages_[index];
        if (page.mode != mode)
            continue;
        if (page.next_x > config_.page_size - upload_width) {
            page.next_x = 0U;
            page.next_y += page.row_height;
            page.row_height = 0U;
        }
        if (page.next_y > config_.page_size - upload_height)
            continue;
        const AtlasPixelRect upload{page.next_x, page.next_y, upload_width, upload_height};
        page.next_x += upload_width;
        page.row_height = std::max(page.row_height, upload_height);
        return Allocation{
            index,
            upload,
            AtlasPixelRect{
                upload.x + config_.gutter_pixels,
                upload.y + config_.gutter_pixels,
                width,
                height,
            },
        };
    }
    return std::nullopt;
}

GlyphAtlas::Allocation GlyphAtlas::create_page_and_allocate(const std::uint32_t width,
                                                            const std::uint32_t height,
                                                            const GlyphRasterMode mode) {
    const std::uint32_t index = static_cast<std::uint32_t>(pages_.size());
    const std::string texture = "strata:native-font-atlas/" + owner_token_ + "/" +
                                std::to_string(generation_) + "/" + std::to_string(index) +
                                page_suffix(mode);
    const std::size_t channels = bytes_per_pixel(mode);
    const std::uint64_t page_pixels =
        static_cast<std::uint64_t>(config_.page_size) * config_.page_size;
    if (page_pixels > std::numeric_limits<std::size_t>::max() / channels) {
        throw FontError("glyph atlas page byte count overflows the host size");
    }
    Page page;
    page.texture = texture;
    page.index = index;
    page.mode = mode;
    if (!config_.backend_rasterized)
        page.pixels.resize(static_cast<std::size_t>(page_pixels) * channels, 0U);
    pages_.push_back(std::move(page));
    operations_.push_back(AtlasOperation{
        AtlasOperationKind::create,
        texture,
        format_for(mode, config_.filtered_coverage),
        AtlasPixelRect{0U, 0U, config_.page_size, config_.page_size},
        {},
    });
    const std::optional<Allocation> allocation = allocate(width, height, mode);
    if (!allocation.has_value())
        throw FontError("new glyph atlas page rejected a bounded allocation");
    return *allocation;
}

GlyphAtlasEntry GlyphAtlas::insert(const Key& key, const GlyphRasterBitmap& bitmap,
                                   const Allocation& allocation) {
    Page& page = pages_.at(allocation.page);
    const std::size_t channels = bytes_per_pixel(bitmap.mode);
    const std::size_t upload_pixels =
        static_cast<std::size_t>(allocation.upload.width) * allocation.upload.height;
    if (upload_pixels > std::numeric_limits<std::size_t>::max() / channels) {
        throw FontError("glyph atlas upload byte count overflows the host size");
    }
    // Row by row straight into the page: the bitmap's row, its edge pixels repeated across the
    // gutter on either side, and the first and last rows repeated above and below.
    const std::size_t atlas_row_bytes = static_cast<std::size_t>(config_.page_size) * channels;
    const std::size_t bitmap_row_bytes = static_cast<std::size_t>(bitmap.width) * channels;
    const std::uint32_t gutter = config_.gutter_pixels;
    for (std::uint32_t row = 0U; row < allocation.upload.height; ++row) {
        const auto source_y = static_cast<std::uint32_t>(std::clamp(
            static_cast<std::int64_t>(row) - static_cast<std::int64_t>(gutter), std::int64_t{0},
            static_cast<std::int64_t>(bitmap.height - 1U)));
        const std::uint8_t* source = bitmap.bytes.data() + source_y * bitmap_row_bytes;
        std::uint8_t* destination =
            page.pixels.data() + static_cast<std::size_t>(allocation.upload.y + row) * atlas_row_bytes +
            static_cast<std::size_t>(allocation.upload.x) * channels;
        const std::uint32_t left_gutter = std::min(gutter, allocation.upload.width);
        for (std::uint32_t column = 0U; column < left_gutter; ++column)
            std::memcpy(destination + static_cast<std::size_t>(column) * channels, source, channels);
        const std::uint32_t inner =
            std::min(bitmap.width, allocation.upload.width - left_gutter);
        std::memcpy(destination + static_cast<std::size_t>(left_gutter) * channels, source,
                    static_cast<std::size_t>(inner) * channels);
        const std::uint8_t* last = source + static_cast<std::size_t>(bitmap.width - 1U) * channels;
        for (std::uint32_t column = left_gutter + inner; column < allocation.upload.width; ++column)
            std::memcpy(destination + static_cast<std::size_t>(column) * channels, last, channels);
    }
    page.used_preparation = preparation_serial_;
    if (page.dirty.has_value()) {
        const std::uint32_t left = std::min(page.dirty->x, allocation.upload.x);
        const std::uint32_t top = std::min(page.dirty->y, allocation.upload.y);
        const std::uint32_t right = std::max(page.dirty->x + page.dirty->width,
                                             allocation.upload.x + allocation.upload.width);
        const std::uint32_t bottom = std::max(page.dirty->y + page.dirty->height,
                                              allocation.upload.y + allocation.upload.height);
        page.dirty = AtlasPixelRect{left, top, right - left, bottom - top};
    } else {
        page.dirty = allocation.upload;
    }
    if (!frame_preparation_active_)
        flush_page_upload(allocation.page);
    GlyphAtlasEntry entry = entry_for(key, allocation, bitmap.plane_bounds_layout_pixels,
                                      bitmap.atlas_pixel_range, bitmap.layout_pixel_range,
                                      bitmap.mode);
    cache_.emplace(key, entry);
    return entry;
}

GlyphAtlasEntry GlyphAtlas::entry_for(const Key& key, const Allocation& allocation,
                                      const RasterPlaneBounds& plane_bounds,
                                      const double atlas_pixel_range,
                                      const double layout_pixel_range,
                                      const GlyphRasterMode mode) const {
    const Page& page = pages_.at(allocation.page);
    const double page_size = static_cast<double>(config_.page_size);
    return GlyphAtlasEntry{
        page.texture,
        page.index,
        generation_,
        static_cast<double>(allocation.content.x) / page_size,
        static_cast<double>(allocation.content.y) / page_size,
        static_cast<double>(allocation.content.width) / page_size,
        static_cast<double>(allocation.content.height) / page_size,
        plane_bounds,
        atlas_pixel_range,
        layout_pixel_range,
        mode,
        key.phase,
    };
}

GlyphAtlasEntry GlyphAtlas::insert_bitmap(Key& key, const GlyphRasterBitmap& bitmap) {
    const Allocation allocation = place(key, bitmap.width, bitmap.height, bitmap.mode);
    return insert(key, bitmap, allocation);
}

GlyphAtlas::Allocation GlyphAtlas::place(Key& key, const std::uint32_t width,
                                         const std::uint32_t height, const GlyphRasterMode mode) {
    if (cache_.size() >= config_.maximum_cached_glyphs) {
        if (!recycle_for_capacity(key)) {
            throw FontError(
                "glyph atlas cache limit reached; reclamation is scheduled for a safe frame");
        }
    }
    std::optional<Allocation> allocation = allocate(width, height, mode);
    if (!allocation.has_value() && pages_.size() >= config_.maximum_pages)
        allocation = reuse_unretained_page(width, height, mode);
    if (!allocation.has_value()) {
        if (pages_.size() >= config_.maximum_pages) {
            if (!recycle_for_capacity(key)) {
                throw FontError(
                    "glyph atlas pages are full; reclamation is scheduled for a safe frame");
            }
            allocation = allocate(width, height, mode);
        }
        if (!allocation.has_value())
            allocation = create_page_and_allocate(width, height, mode);
    }
    return *allocation;
}

void GlyphAtlas::flush_page_upload(const std::size_t page_index) {
    Page& page = pages_.at(page_index);
    if (!page.dirty.has_value())
        return;
    const AtlasPixelRect region = *page.dirty;
    const std::size_t channels = bytes_per_pixel(page.mode);
    const std::size_t row_bytes = static_cast<std::size_t>(region.width) * channels;
    if (region.height > std::numeric_limits<std::size_t>::max() / row_bytes) {
        throw FontError("glyph atlas dirty upload byte count overflows the host size");
    }
    std::vector<std::uint8_t> upload(row_bytes * region.height);
    const std::size_t atlas_row_bytes = static_cast<std::size_t>(config_.page_size) * channels;
    for (std::uint32_t row = 0U; row < region.height; ++row) {
        const std::size_t source = static_cast<std::size_t>(region.y + row) * atlas_row_bytes +
                                   static_cast<std::size_t>(region.x) * channels;
        const std::size_t destination = static_cast<std::size_t>(row) * row_bytes;
        std::copy_n(page.pixels.begin() + static_cast<std::ptrdiff_t>(source), row_bytes,
                    upload.begin() + static_cast<std::ptrdiff_t>(destination));
    }
    operations_.push_back(AtlasOperation{
        AtlasOperationKind::upload,
        page.texture,
        format_for(page.mode, config_.filtered_coverage),
        region,
        std::move(upload),
    });
    page.dirty.reset();
}

void GlyphAtlas::flush_pending_uploads() {
    for (std::size_t page = 0U; page < pages_.size(); ++page) {
        flush_page_upload(page);
    }
}

void GlyphAtlas::retain_pages(const std::span<const std::string> textures) {
    retained_pages_.clear();
    retained_pages_.insert(textures.begin(), textures.end());
    retention_known_ = true;
}

std::optional<GlyphAtlas::Allocation> GlyphAtlas::reuse_unretained_page(
    const std::uint32_t width, const std::uint32_t height, const GlyphRasterMode mode) {
    if (!retention_known_ || !frame_preparation_active_)
        return std::nullopt;
    // A page of the wanted kind first; then one of the other kind, made again as this kind, so a
    // frame that swaps coverage masks for colour pictures still never empties the whole atlas.
    for (const bool same_mode : {true, false}) {
        for (Page& page : pages_) {
            if ((page.mode == mode) != same_mode || page.used_preparation == preparation_serial_ ||
                retained_pages_.contains(page.texture)) {
                continue;
            }
            // Nothing drawn uses it: forget what it held and pack from its corner again. Its old
            // texels stay until overwritten; no entry reaches them.
            std::erase_if(cache_, [&page](const auto& entry) {
                return entry.second.texture == page.texture;
            });
            if (!same_mode) {
                operations_.push_back(AtlasOperation{
                    AtlasOperationKind::release,
                    page.texture,
                    format_for(page.mode, config_.filtered_coverage),
                    {},
                    {},
                });
                page.texture = "strata:native-font-atlas/" + owner_token_ + "/" +
                               std::to_string(generation_) + "/" + std::to_string(page.index) +
                               "/" + std::to_string(preparation_serial_) +
                               page_suffix(mode);
                page.mode = mode;
                if (!config_.backend_rasterized) {
                    page.pixels.assign(static_cast<std::size_t>(config_.page_size) *
                                           config_.page_size * bytes_per_pixel(mode),
                                       0U);
                }
                page.dirty.reset();
                operations_.push_back(AtlasOperation{
                    AtlasOperationKind::create,
                    page.texture,
                    format_for(mode, config_.filtered_coverage),
                    AtlasPixelRect{0U, 0U, config_.page_size, config_.page_size},
                    {},
                });
            }
            page.next_x = 0U;
            page.next_y = 0U;
            page.row_height = 0U;
            ++page.epoch;
            std::optional<Allocation> allocation = allocate(width, height, mode);
            if (allocation.has_value())
                return allocation;
        }
    }
    return std::nullopt;
}

bool GlyphAtlas::recycle_for_capacity(Key& key) {
    if (!frame_preparation_active_ ||
        preparation_recycles_ >= config_.maximum_generation_recycles_per_preparation) {
        reclamation_pending_ = true;
        return false;
    }
    invalidate_pages();
    reclamation_pending_ = false;
    ++preparation_recycles_;
    ++generation_recycle_count_;
    key.generation = generation_;
    return true;
}

void GlyphAtlas::invalidate_pages() {
    for (const Page& page : pages_) {
        operations_.push_back(AtlasOperation{
            AtlasOperationKind::release,
            page.texture,
            format_for(page.mode, config_.filtered_coverage),
            {},
            {},
        });
    }
    pages_.clear();
    cache_.clear();
    reclamation_pending_ = false;
    advance(generation_);
}

} // namespace strata::font
