#include "font/atlas.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace strata::font {
namespace {

[[nodiscard]] std::string owner_token(const std::string_view owner_id) {
    // Length-prefixing is injective for arbitrary owner bytes; a fixed-width hash would make
    // cross-runtime host resource ownership merely improbable rather than collision-free.
    return std::to_string(owner_id.size()) + ":" + std::string(owner_id);
}

[[nodiscard]] AtlasTextureFormat format_for(const GlyphRasterMode mode) noexcept {
    return mode == GlyphRasterMode::coverage ? AtlasTextureFormat::r8 : AtlasTextureFormat::rgba8;
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
        if (found == entries_.end()) return {};
        found->second.last_use = ++clock_;
        return found->second.raster;
    }

    [[nodiscard]] SharedRaster insert(
        RasterCacheIdentity identity,
        SharedRaster raster
    ) {
        const std::scoped_lock lock(mutex_);
        if (const auto found = entries_.find(identity.key); found != entries_.end()) {
            found->second.last_use = ++clock_;
            return found->second.raster;
        }
        const std::size_t byte_count =
            raster != nullptr && raster->has_value() ? raster->value().bytes.size() : 0U;
        total_bytes_ += byte_count;
        entries_.emplace(
            identity.key,
            Entry{
                std::move(identity.font_owner),
                std::move(raster),
                byte_count,
                ++clock_,
            }
        );
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
                entries_,
                {},
                [](const auto& entry) { return entry.second.last_use; }
            );
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

[[nodiscard]] std::array<std::uint64_t, 12U> raster_config_identity(
    const GlyphRasterMode mode,
    const CoverageRasterConfig& coverage,
    const MsdfRasterConfig& msdf
) noexcept {
    std::array<std::uint64_t, 12U> result{};
    if (mode == GlyphRasterMode::coverage) {
        result = {
            coverage.maximum_dimension,
            coverage.padding_pixels,
            static_cast<std::uint64_t>(coverage.hinting),
            std::bit_cast<std::uint64_t>(coverage.stem_darkening_pixels),
            std::bit_cast<std::uint64_t>(
                coverage.stem_darkening_taper_start_physical_pixel_size
            ),
            std::bit_cast<std::uint64_t>(
                coverage.stem_darkening_taper_end_physical_pixel_size
            ),
            std::bit_cast<std::uint64_t>(coverage.transfer_strength),
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

[[nodiscard]] RasterCacheIdentity raster_cache_identity(
    const OpenTypeFont& font,
    const std::uint16_t glyph,
    const double pixel_size,
    const double display_scale,
    const SubpixelPhase phase,
    const GlyphRasterMode mode,
    const std::uint32_t font_style_flags,
    const CoverageRasterConfig& coverage,
    const MsdfRasterConfig& msdf
) {
    std::shared_ptr<const void> owner = font.cache_identity();
    return RasterCacheIdentity{
        RasterCacheKey{
            reinterpret_cast<std::uintptr_t>(owner.get()),
            glyph,
            font_style_flags,
            std::bit_cast<std::uint64_t>(pixel_size),
            std::bit_cast<std::uint64_t>(
                mode == GlyphRasterMode::coverage ? display_scale : 1.0
            ),
            mode,
            phase,
            raster_config_identity(mode, coverage, msdf),
        },
        std::move(owner),
    };
}

[[nodiscard]] std::optional<GlyphRasterBitmap> rasterize(
    const OpenTypeFont& font,
    const std::uint16_t glyph,
    const double pixel_size,
    const double display_scale,
    const SubpixelPhase phase,
    const GlyphRasterMode mode,
    const std::uint32_t font_style_flags,
    const CoverageRasterConfig& coverage,
    const MsdfRasterConfig& msdf
) {
    return mode == GlyphRasterMode::coverage
        ? rasterize_coverage(
              font,
              glyph,
              pixel_size,
              display_scale,
              phase,
              coverage,
              font_style_flags
          )
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
    if (display_scale_ == display_scale) return;
    display_scale_ = display_scale;
    invalidate_pages();
}

void GlyphAtlas::invalidate_resources() {
    commit_resource_invalidation(plan_resource_invalidation());
}

AtlasResourceInvalidationPlan GlyphAtlas::plan_resource_invalidation() {
    if (frame_preparation_active_) {
        throw std::logic_error("glyph atlas cannot invalidate resources during frame preparation");
    }
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw FontError("glyph atlas generation exhausted");
    }
    if (pages_.size() > std::numeric_limits<std::size_t>::max() - operations_.size()) {
        throw std::length_error("glyph atlas resource invalidation exceeds size_t");
    }

    AtlasResourceInvalidationPlan plan;
    plan.generation = generation_;
    plan.page_count = pages_.size();
    plan.operation_count = operations_.size();
    plan.releases.reserve(pages_.size());
    for (const Page& page : pages_) {
        plan.releases.push_back(AtlasOperation{
            AtlasOperationKind::release,
            page.texture,
            format_for(page.mode),
            {},
            {},
        });
    }
    // Capacity changes are not logical atlas mutation. Once this succeeds, moving every prepared
    // release into the pending queue cannot allocate.
    operations_.reserve(operations_.size() + plan.releases.size());
    return plan;
}

void GlyphAtlas::commit_resource_invalidation(AtlasResourceInvalidationPlan plan) noexcept {
    if (plan.generation != generation_ || plan.page_count != pages_.size() ||
        plan.operation_count != operations_.size()) {
        std::terminate();
    }
    static_assert(std::is_nothrow_move_constructible_v<AtlasOperation>);
    for (AtlasOperation& release : plan.releases) {
        operations_.push_back(std::move(release));
    }
    pages_.clear();
    cache_.clear();
    reclamation_pending_ = false;
    ++generation_;
}

bool GlyphAtlas::begin_frame_preparation() {
    if (frame_preparation_active_) {
        throw std::logic_error("glyph atlas frame preparation is already active");
    }
    frame_preparation_active_ = true;
    preparation_recycles_ = 0U;
    if (!reclamation_pending_) return false;
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

std::optional<GlyphAtlasEntry> GlyphAtlas::request_coverage(
    const std::string_view font_id,
    const OpenTypeFont& font,
    const std::uint16_t glyph,
    const double pixel_size,
    const SubpixelPhase phase,
    const std::uint32_t font_style_flags
) {
    return request_mode(
        font_id, font, glyph, pixel_size, phase, GlyphRasterMode::coverage, font_style_flags
    );
}

std::optional<GlyphAtlasEntry> GlyphAtlas::request(
    const std::string_view font_id,
    const OpenTypeFont& font,
    const std::uint16_t glyph,
    const double pixel_size,
    const SubpixelPhase coverage_phase,
    const std::uint32_t font_style_flags,
    const GlyphRasterMode mode
) {
    return request_mode(
        font_id,
        font,
        glyph,
        pixel_size,
        mode == GlyphRasterMode::coverage ? coverage_phase : SubpixelPhase{},
        mode,
        font_style_flags
    );
}

std::optional<GlyphAtlasEntry> GlyphAtlas::request_mode(
    const std::string_view font_id,
    const OpenTypeFont& font,
    const std::uint16_t glyph,
    const double pixel_size,
    const SubpixelPhase phase,
    const GlyphRasterMode mode,
    const std::uint32_t font_style_flags
) {
    if (font_id.empty()) throw std::invalid_argument("glyph atlas font id must not be empty");
    // The selected face is already part of identity. Collapse known bits supplied intrinsically
    // by that face, while retaining every opaque/forward-compatible bit exactly.
    const std::uint32_t realized_style_flags =
        (font_style_flags & ~known_font_style_flags) |
        synthetic_font_style_flags(font, font_style_flags);
    Key key{
        std::string(font_id),
        glyph,
        realized_style_flags,
        std::bit_cast<std::uint64_t>(pixel_size),
        std::bit_cast<std::uint64_t>(
            mode == GlyphRasterMode::coverage ? display_scale_ : 1.0
        ),
        mode,
        phase,
        generation_,
    };
    if (const auto found = cache_.find(key); found != cache_.end()) {
        if (!frame_preparation_active_) flush_pending_uploads();
        return found->second;
    }
    RasterCacheIdentity identity = raster_cache_identity(
        font,
        glyph,
        pixel_size,
        display_scale_,
        phase,
        mode,
        realized_style_flags,
        config_.coverage,
        config_.msdf
    );
    SharedRaster bitmap = shared_raster_cache().find(identity.key);
    if (bitmap == nullptr) {
        bitmap = std::make_shared<const std::optional<GlyphRasterBitmap>>(rasterize(
            font,
            glyph,
            pixel_size,
            display_scale_,
            phase,
            mode,
            realized_style_flags,
            config_.coverage,
            config_.msdf
        ));
        bitmap = shared_raster_cache().insert(std::move(identity), std::move(bitmap));
    }
    if (!bitmap->has_value()) return std::nullopt;
    return insert_bitmap(key, bitmap->value());
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
    };
    std::vector<Pending> pending;
    pending.reserve(requests.size());
    std::map<Key, std::size_t> unique;

    for (const GlyphAtlasWarmupRequest& request : requests) {
        if (request.font == nullptr || request.font_id.empty()) {
            throw std::invalid_argument("glyph atlas warmup requires a font and non-empty id");
        }
        const GlyphRasterMode mode = request.mode;
        const SubpixelPhase phase =
            mode == GlyphRasterMode::coverage ? request.phase : SubpixelPhase{};
        const std::uint32_t realized_style_flags =
            (request.font_style_flags & ~known_font_style_flags) |
            synthetic_font_style_flags(*request.font, request.font_style_flags);
        Key key{
            std::string(request.font_id),
            request.glyph,
            realized_style_flags,
            std::bit_cast<std::uint64_t>(request.pixel_size),
            std::bit_cast<std::uint64_t>(
                mode == GlyphRasterMode::coverage ? display_scale_ : 1.0
            ),
            mode,
            phase,
            generation_,
        };
        if (cache_.contains(key) || unique.contains(key)) continue;
        RasterCacheIdentity identity = raster_cache_identity(
            *request.font,
            request.glyph,
            request.pixel_size,
            display_scale_,
            phase,
            mode,
            realized_style_flags,
            config_.coverage,
            config_.msdf
        );
        SharedRaster raster = shared_raster_cache().find(identity.key);
        unique.emplace(key, pending.size());
        pending.push_back(Pending{
            std::move(key),
            request.font,
            std::move(identity),
            std::move(raster),
        });
    }

    std::vector<std::size_t> missing;
    missing.reserve(pending.size());
    for (std::size_t index = 0U; index < pending.size(); ++index) {
        if (pending[index].raster == nullptr) missing.push_back(index);
    }

    // Raster misses are independent immutable results. Use a small fixed ceiling rather than a
    // hardware-sized fan-out: one caller lane plus at most three temporary workers reduces a cold
    // atlas stall without monopolizing a host's render/game worker pool. Coverage rasterization
    // obtains one cached FT_Face per thread because FT_Face owns mutable size/transform state.
    constexpr std::size_t maximum_raster_lanes = 4U;
    constexpr std::size_t parallel_raster_threshold = 8U;
    const std::size_t available_lanes = std::max<std::size_t>(
        1U,
        static_cast<std::size_t>(std::thread::hardware_concurrency())
    );
    const std::size_t raster_lanes = missing.size() < parallel_raster_threshold
        ? 1U
        : std::min({maximum_raster_lanes, available_lanes, missing.size()});
    std::atomic_size_t cursor{0U};
    std::atomic_bool failed{false};
    std::mutex failure_mutex;
    std::exception_ptr failure;
    const auto raster_lane = [&] {
        while (!failed.load(std::memory_order_acquire)) {
            const std::size_t position = cursor.fetch_add(1U, std::memory_order_relaxed);
            if (position >= missing.size()) return;
            Pending& item = pending[missing[position]];
            try {
                item.raster = std::make_shared<const std::optional<GlyphRasterBitmap>>(
                    rasterize(
                        *item.font,
                        item.key.glyph,
                        std::bit_cast<double>(item.key.pixel_size),
                        display_scale_,
                        item.key.phase,
                        item.key.mode,
                        item.key.font_style_flags,
                        config_.coverage,
                        config_.msdf
                    )
                );
            } catch (...) {
                {
                    const std::scoped_lock lock(failure_mutex);
                    if (failure == nullptr) failure = std::current_exception();
                }
                failed.store(true, std::memory_order_release);
                return;
            }
        }
    };
    std::vector<std::jthread> workers;
    workers.reserve(raster_lanes - 1U);
    for (std::size_t lane = 1U; lane < raster_lanes; ++lane) {
        workers.emplace_back(raster_lane);
    }
    raster_lane();
    for (std::jthread& worker : workers) worker.join();
    if (failure != nullptr) std::rethrow_exception(failure);

    for (const std::size_t index : missing) {
        Pending& item = pending[index];
        item.raster = shared_raster_cache().insert(
            std::move(item.identity),
            std::move(item.raster)
        );
    }
    for (Pending& item : pending) {
        if (item.key.generation != generation_) return false;
        if (!item.raster->has_value()) continue;
        static_cast<void>(insert_bitmap(item.key, item.raster->value()));
        if (generation_ != initial_generation) return false;
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
        if (operation.kind != AtlasOperationKind::release) continue;
        if (candidate_count == std::numeric_limits<std::size_t>::max()) {
            throw std::length_error("glyph atlas terminal release plan exceeds size_t");
        }
        ++candidate_count;
    }

    std::vector<AtlasOperation> releases;
    releases.reserve(candidate_count);
    const auto append_release = [&releases](
        const std::string& texture,
        const AtlasTextureFormat format
    ) {
        const bool duplicate = std::ranges::any_of(
            releases,
            [&texture](const AtlasOperation& release) { return release.texture == texture; }
        );
        if (duplicate) return;
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
        append_release(page.texture, format_for(page.mode));
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

std::uint64_t GlyphAtlas::generation() const noexcept { return generation_; }
std::size_t GlyphAtlas::cached_glyph_count() const noexcept { return cache_.size(); }
bool GlyphAtlas::reclamation_pending() const noexcept { return reclamation_pending_; }
bool GlyphAtlas::frame_preparation_active() const noexcept { return frame_preparation_active_; }
std::uint64_t GlyphAtlas::generation_recycle_count() const noexcept {
    return generation_recycle_count_;
}

std::optional<GlyphAtlas::Allocation> GlyphAtlas::allocate(
    const std::uint32_t width,
    const std::uint32_t height,
    const GlyphRasterMode mode
) {
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
        if (page.mode != mode) continue;
        if (page.next_x > config_.page_size - upload_width) {
            page.next_x = 0U;
            page.next_y += page.row_height;
            page.row_height = 0U;
        }
        if (page.next_y > config_.page_size - upload_height) continue;
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

GlyphAtlas::Allocation GlyphAtlas::create_page_and_allocate(
    const std::uint32_t width,
    const std::uint32_t height,
    const GlyphRasterMode mode
) {
    const std::uint32_t index = static_cast<std::uint32_t>(pages_.size());
    const std::string texture = "strata:native-font-atlas/" + owner_token_ + "/" +
        std::to_string(generation_) + "/" + std::to_string(index) +
        (mode == GlyphRasterMode::coverage ? "/coverage" : "/msdf");
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
    page.pixels.resize(static_cast<std::size_t>(page_pixels) * channels, 0U);
    pages_.push_back(std::move(page));
    operations_.push_back(AtlasOperation{
        AtlasOperationKind::create,
        texture,
        format_for(mode),
        AtlasPixelRect{0U, 0U, config_.page_size, config_.page_size},
        {},
    });
    const std::optional<Allocation> allocation = allocate(width, height, mode);
    if (!allocation.has_value()) throw FontError("new glyph atlas page rejected a bounded allocation");
    return *allocation;
}

GlyphAtlasEntry GlyphAtlas::insert(
    const Key& key,
    const GlyphRasterBitmap& bitmap,
    const Allocation& allocation
) {
    Page& page = pages_.at(allocation.page);
    const std::size_t channels = bytes_per_pixel(bitmap.mode);
    const std::size_t upload_pixels =
        static_cast<std::size_t>(allocation.upload.width) * allocation.upload.height;
    if (upload_pixels > std::numeric_limits<std::size_t>::max() / channels) {
        throw FontError("glyph atlas upload byte count overflows the host size");
    }
    std::vector<std::uint8_t> upload(upload_pixels * channels, 0U);
    for (std::uint32_t row = 0U; row < allocation.upload.height; ++row) {
        const std::uint32_t source_y = static_cast<std::uint32_t>(std::clamp(
            static_cast<std::int64_t>(row) - static_cast<std::int64_t>(config_.gutter_pixels),
            std::int64_t{0},
            static_cast<std::int64_t>(bitmap.height - 1U)
        ));
        for (std::uint32_t column = 0U; column < allocation.upload.width; ++column) {
            const std::uint32_t source_x = static_cast<std::uint32_t>(std::clamp(
                static_cast<std::int64_t>(column) - static_cast<std::int64_t>(config_.gutter_pixels),
                std::int64_t{0},
                static_cast<std::int64_t>(bitmap.width - 1U)
            ));
            const std::size_t source =
                (static_cast<std::size_t>(source_y) * bitmap.width + source_x) * channels;
            const std::size_t destination =
                (static_cast<std::size_t>(row) * allocation.upload.width + column) * channels;
            std::copy_n(
                bitmap.bytes.begin() + static_cast<std::ptrdiff_t>(source),
                channels,
                upload.begin() + static_cast<std::ptrdiff_t>(destination)
            );
        }
    }
    const std::size_t atlas_row_bytes = static_cast<std::size_t>(config_.page_size) * channels;
    const std::size_t upload_row_bytes = static_cast<std::size_t>(allocation.upload.width) * channels;
    for (std::uint32_t row = 0U; row < allocation.upload.height; ++row) {
        const std::size_t source = static_cast<std::size_t>(row) * upload_row_bytes;
        const std::size_t destination =
            static_cast<std::size_t>(allocation.upload.y + row) * atlas_row_bytes +
            static_cast<std::size_t>(allocation.upload.x) * channels;
        std::copy_n(upload.begin() + static_cast<std::ptrdiff_t>(source),
                    upload_row_bytes,
                    page.pixels.begin() + static_cast<std::ptrdiff_t>(destination));
    }
    if (page.dirty.has_value()) {
        const std::uint32_t left = std::min(page.dirty->x, allocation.upload.x);
        const std::uint32_t top = std::min(page.dirty->y, allocation.upload.y);
        const std::uint32_t right = std::max(
            page.dirty->x + page.dirty->width,
            allocation.upload.x + allocation.upload.width
        );
        const std::uint32_t bottom = std::max(
            page.dirty->y + page.dirty->height,
            allocation.upload.y + allocation.upload.height
        );
        page.dirty = AtlasPixelRect{left, top, right - left, bottom - top};
    } else {
        page.dirty = allocation.upload;
    }
    if (!frame_preparation_active_) flush_page_upload(allocation.page);
    const double page_size = static_cast<double>(config_.page_size);
    GlyphAtlasEntry entry{
        page.texture,
        page.index,
        generation_,
        static_cast<double>(allocation.content.x) / page_size,
        static_cast<double>(allocation.content.y) / page_size,
        static_cast<double>(allocation.content.width) / page_size,
        static_cast<double>(allocation.content.height) / page_size,
        bitmap.plane_bounds_layout_pixels,
        bitmap.atlas_pixel_range,
        bitmap.layout_pixel_range,
        bitmap.mode,
        key.phase,
    };
    cache_.emplace(key, entry);
    return entry;
}

GlyphAtlasEntry GlyphAtlas::insert_bitmap(Key& key, const GlyphRasterBitmap& bitmap) {
    if (cache_.size() >= config_.maximum_cached_glyphs) {
        if (!recycle_for_capacity(key)) {
            throw FontError(
                "glyph atlas cache limit reached; reclamation is scheduled for a safe frame"
            );
        }
    }
    std::optional<Allocation> allocation = allocate(bitmap.width, bitmap.height, bitmap.mode);
    if (!allocation.has_value()) {
        if (pages_.size() >= config_.maximum_pages) {
            if (!recycle_for_capacity(key)) {
                throw FontError(
                    "glyph atlas pages are full; reclamation is scheduled for a safe frame"
                );
            }
            allocation = allocate(bitmap.width, bitmap.height, bitmap.mode);
        }
        if (!allocation.has_value()) {
            allocation = create_page_and_allocate(bitmap.width, bitmap.height, bitmap.mode);
        }
    }
    return insert(key, bitmap, *allocation);
}

void GlyphAtlas::flush_page_upload(const std::size_t page_index) {
    Page& page = pages_.at(page_index);
    if (!page.dirty.has_value()) return;
    const AtlasPixelRect region = *page.dirty;
    const std::size_t channels = bytes_per_pixel(page.mode);
    const std::size_t row_bytes = static_cast<std::size_t>(region.width) * channels;
    if (region.height > std::numeric_limits<std::size_t>::max() / row_bytes) {
        throw FontError("glyph atlas dirty upload byte count overflows the host size");
    }
    std::vector<std::uint8_t> upload(row_bytes * region.height);
    const std::size_t atlas_row_bytes = static_cast<std::size_t>(config_.page_size) * channels;
    for (std::uint32_t row = 0U; row < region.height; ++row) {
        const std::size_t source =
            static_cast<std::size_t>(region.y + row) * atlas_row_bytes +
            static_cast<std::size_t>(region.x) * channels;
        const std::size_t destination = static_cast<std::size_t>(row) * row_bytes;
        std::copy_n(page.pixels.begin() + static_cast<std::ptrdiff_t>(source),
                    row_bytes,
                    upload.begin() + static_cast<std::ptrdiff_t>(destination));
    }
    operations_.push_back(AtlasOperation{
        AtlasOperationKind::upload,
        page.texture,
        format_for(page.mode),
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
            format_for(page.mode),
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
