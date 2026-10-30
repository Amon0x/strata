#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "font/opentype.hpp"
#include "font/raster.hpp"

namespace strata::font {

enum class AtlasTextureFormat : std::uint8_t {
    r8,
    rgba8,
    /** R8 coverage sampled bilinearly (render_packet.hpp texture_format_r8_filtered). */
    r8_filtered,
    /** RGBA8 sampled texel-exact: subpixel glyphs (texture_format_rgba8_exact). */
    rgba8_exact,
};

/** Numbered as the render packet numbers its resource records. */
enum class AtlasOperationKind : std::uint8_t {
    create = 0U,
    upload = 1U,
    release = 2U,
    /** A region the backend rasterizes from the raster job in `bytes` (strata/raster_job.hpp). */
    raster = 4U,
};

struct AtlasPixelRect final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

struct AtlasOperation final {
    AtlasOperationKind kind = AtlasOperationKind::create;
    std::string texture;
    AtlasTextureFormat format = AtlasTextureFormat::r8;
    AtlasPixelRect region;
    std::vector<std::uint8_t> bytes;
};

struct GlyphAtlasEntry final {
    std::string texture;
    std::uint32_t page_index = 0U;
    std::uint64_t generation = 0U;
    double u = 0.0;
    double v = 0.0;
    double uv_width = 0.0;
    double uv_height = 0.0;
    RasterPlaneBounds plane_bounds_layout_pixels;
    double atlas_pixel_range = 1.0;
    double layout_pixel_range = 1.0;
    GlyphRasterMode mode = GlyphRasterMode::coverage;
    SubpixelPhase phase;
};

/** A raster job (strata/raster_job.hpp) and the content it fills. */
struct RasterRequest final {
    std::vector<std::uint8_t> job;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    /** The content's place on the device grid. */
    RasterPlaneBounds plane_bounds;
};

/**
 * A region of a page held for one animated picture, rasterized again in place each frame rather
 * than cached under a new key: its texture and place stay, so the draws over it keep their
 * batches and their geometry slots.
 */
struct RasterSlot final {
    std::string texture;
    std::size_t page = 0U;
    /** The page's resets and the atlas's generation when it was reserved: either moving frees it. */
    std::uint64_t page_epoch = 0U;
    std::uint64_t generation = 0U;
    AtlasPixelRect upload;
    GlyphRasterMode mode = GlyphRasterMode::coverage;
};

struct GlyphAtlasConfig final {
    std::uint32_t page_size = 1'024U;
    std::uint32_t maximum_pages = 8U;
    std::uint32_t maximum_cached_glyphs = 4'096U;
    /** Bounds full-generation recycling work admitted during one explicit safe preparation phase.
     */
    std::uint32_t maximum_generation_recycles_per_preparation = 1U;
    std::uint32_t gutter_pixels = 1U;
    /**
     * Coverage pages are sampled bilinearly instead of texel-exact: for shape masks, which a
     * rotated draw or a scaled presentation group resamples. Glyphs, drawn on whole texels, keep
     * texel-exact sampling.
     */
    bool filtered_coverage = false;
    /**
     * Pages filled only by raster jobs (request_raster), which the backend rasterizes: no copy of
     * their texels is kept on the CPU.
     */
    bool backend_rasterized = false;
    CoverageRasterConfig coverage;
    MsdfRasterConfig msdf;
};

struct GlyphAtlasWarmupRequest final {
    std::string_view font_id;
    const OpenTypeFont* font = nullptr;
    std::uint16_t glyph = 0U;
    double pixel_size = 0.0;
    SubpixelPhase phase;
    std::uint32_t font_style_flags = 0U;
    GlyphRasterMode mode = GlyphRasterMode::coverage;
    /** How far beyond its edge an MSDF glyph's distance must reach (see GlyphAtlas::request). */
    double reach = 0.0;
    /** Rasterize without hinting (see GlyphAtlas::request). */
    bool unhinted = false;
};

/**
 * Per-surface atlas packer and host upload planner. Immutable raster bitmaps are process-shared by
 * parsed font identity; atlas coordinates, pages, resource ownership, and invalidation stay local.
 */
class GlyphAtlas final {
  public:
    explicit GlyphAtlas(std::string_view owner_id, GlyphAtlasConfig config = {});
    ~GlyphAtlas();

    [[nodiscard]] const GlyphAtlasConfig& config() const noexcept {
        return config_;
    }

    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;
    GlyphAtlas(GlyphAtlas&&) = delete;
    GlyphAtlas& operator=(GlyphAtlas&&) = delete;

    /** Invalidates scale-dependent coverage pages and plans explicit host releases. */
    void adopt_display_scale(double display_scale);
    /** The subpixel order subpixel glyphs are rasterized for; a change drops the pages. */
    void adopt_subpixel_order(SubpixelOrder order);
    [[nodiscard]] SubpixelOrder subpixel_order() const noexcept;

    /**
     * Opens the frame-safe glyph preparation phase. If a prior submission hit capacity, old pages
     * are reclaimed here before any warmup requests. Returns true when the atlas generation moved.
     * A caller that caches atlas-backed geometry must discard it when this returns true or when
     * generation() changes during preparation.
     */
    [[nodiscard]] bool begin_frame_preparation();
    /** Seals the atlas against page recycling before atlas-backed geometry is published. */
    void end_frame_preparation() noexcept;

    [[nodiscard]] std::optional<GlyphAtlasEntry>
    request_coverage(std::string_view font_id, const OpenTypeFont& font, std::uint16_t glyph,
                     double pixel_size, SubpixelPhase phase, std::uint32_t font_style_flags = 0U);

    /**
     * Requests an explicit raster mode; ordinary callers default to size-specific grayscale. An
     * MSDF glyph whose distance must reach `reach` layout pixels beyond its edge (text with an
     * outline, a glow or a shadow) rasterizes with a range and padding wide enough for it, as an
     * entry of its own.
     */
    [[nodiscard]] std::optional<GlyphAtlasEntry>
    request(std::string_view font_id, const OpenTypeFont& font, std::uint16_t glyph,
            double pixel_size, SubpixelPhase coverage_phase, std::uint32_t font_style_flags = 0U,
            GlyphRasterMode mode = GlyphRasterMode::coverage, double reach = 0.0,
            bool unhinted = false);

    /**
     * The pages something still drawn uses, given before planning a frame. With it, a full atlas
     * resets one page outside this set that nothing has used this preparation and packs into it,
     * instead of recycling every page (and every plan that draws from them). Without it, as for
     * glyphs, a full atlas recycles.
     */
    void retain_pages(std::span<const std::string> textures);
    /**
     * A picture that is not a glyph (a shape's coverage, or joined shapes' colour), cached under
     * `key`, which must name everything its texels depend on. A miss calls `encode` with the
     * content's offset within its packed region (the gutter) for the raster job the backend
     * rasterizes there, and packs a region for its content. Coverage is R8 and
     * `GlyphRasterMode::msdf` RGBA8. Returns nothing when `encode` does, when the content exceeds
     * a page, or when the pages are full until the next safe frame reclaims them.
     */
    [[nodiscard]] std::optional<GlyphAtlasEntry>
    request_raster(std::string_view key, GlyphRasterMode mode,
                   const std::function<std::optional<RasterRequest>(std::uint32_t)>& encode);
    /** Pixels of clear border every packed region has around its content. */
    [[nodiscard]] std::uint32_t gutter() const noexcept { return config_.gutter_pixels; }
    /**
     * Reserves a slot for content up to `width` x `height`, packed like a picture but never
     * cached. Nothing when the pages are full until the next safe frame reclaims them.
     */
    [[nodiscard]] std::optional<RasterSlot> reserve_raster_slot(std::uint32_t width,
                                                                std::uint32_t height,
                                                                GlyphRasterMode mode);
    /** Whether a slot still holds its place and fits content of this size. */
    [[nodiscard]] bool raster_slot_fits(const RasterSlot& slot, std::uint32_t width,
                                        std::uint32_t height) const noexcept;
    /**
     * Rasterizes a request (encoded with gutter()) into a slot that fits it, replacing what the
     * slot held; the entry draws its content.
     */
    [[nodiscard]] GlyphAtlasEntry write_raster_slot(const RasterSlot& slot, RasterRequest request);
    /**
     * Rasterizes and packs a unique request set before draw planning. Expensive cache misses run
     * concurrently, while deterministic page allocation and host-operation construction remain
     * serialized. Returns false if bounded capacity recycling moved the atlas generation.
     */
    [[nodiscard]] bool warm(std::span<const GlyphAtlasWarmupRequest> requests);

    /**
     * Stable view of pending host operations. The view remains valid until the atlas is mutated or
     * commit_operations()/take_operations() is called, allowing packet encoding without copying
     * upload payloads or prematurely draining retry state.
     */
    [[nodiscard]] std::span<const AtlasOperation> pending_operations();
    /** Commits a fully retained ordinary-frame resource packet. */
    void commit_operations() noexcept;
    /** Compatibility draining API for direct atlas consumers. */
    [[nodiscard]] std::vector<AtlasOperation> take_operations();
    /**
     * Takes a non-mutating snapshot of the releases required for terminal host teardown.
     * Pending releases for pages invalidated before the final frame and releases for every live
     * page are included exactly once. Pending creates and uploads are deliberately omitted.
     *
     * The caller must retain its fully encoded host packet before commit_terminal_release(). If
     * snapshot construction or packet encoding throws, the atlas remains unchanged and retryable.
     */
    [[nodiscard]] std::vector<AtlasOperation> plan_terminal_release() const;
    /**
     * Commits a successfully retained terminal release packet. This irreversibly drains atlas
     * pages, glyph entries, and queued operations and is idempotent.
     */
    void commit_terminal_release() noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] std::size_t cached_glyph_count() const noexcept;
    [[nodiscard]] bool reclamation_pending() const noexcept;
    [[nodiscard]] bool frame_preparation_active() const noexcept;
    [[nodiscard]] std::uint64_t generation_recycle_count() const noexcept;
    /** Glyphs rasterized on the CPU since the last call, and the time it took; then zero. */
    struct RasterWork final {
        std::uint64_t glyphs = 0U;
        std::uint64_t nanoseconds = 0U;
    };
    [[nodiscard]] RasterWork take_raster_work() noexcept;

  private:
    struct Key;
    struct Page;
    struct Allocation;

    [[nodiscard]] std::optional<Allocation> allocate(std::uint32_t width, std::uint32_t height,
                                                     GlyphRasterMode mode);
    [[nodiscard]] Allocation create_page_and_allocate(std::uint32_t width, std::uint32_t height,
                                                      GlyphRasterMode mode);
    [[nodiscard]] GlyphAtlasEntry insert(const Key& key, const GlyphRasterBitmap& bitmap,
                                         const Allocation& allocation);
    [[nodiscard]] GlyphAtlasEntry insert_bitmap(Key& key, const GlyphRasterBitmap& bitmap);
    /** Room for a `width` x `height` region: packed, on a reused page, or after a recycle. */
    [[nodiscard]] Allocation place(Key& key, std::uint32_t width, std::uint32_t height,
                                   GlyphRasterMode mode);
    [[nodiscard]] GlyphAtlasEntry entry_for(const Key& key, const Allocation& allocation,
                                            const RasterPlaneBounds& plane_bounds,
                                            double atlas_pixel_range, double layout_pixel_range,
                                            GlyphRasterMode mode) const;
    [[nodiscard]] std::optional<GlyphAtlasEntry>
    request_mode(std::string_view font_id, const OpenTypeFont& font, std::uint16_t glyph,
                 double pixel_size, SubpixelPhase phase, GlyphRasterMode mode,
                 std::uint32_t font_style_flags, double reach = 0.0, bool unhinted = false);
    struct Resolved;
    /** What a glyph request rasterizes as and is cached under; request and warm agree on it. */
    [[nodiscard]] Resolved resolve(std::string_view font_id, const OpenTypeFont& font,
                                   std::uint16_t glyph, double pixel_size, SubpixelPhase phase,
                                   GlyphRasterMode mode, std::uint32_t font_style_flags,
                                   double reach, bool unhinted = false) const;
    [[nodiscard]] bool recycle_for_capacity(Key& key);
    [[nodiscard]] std::optional<Allocation> reuse_unretained_page(std::uint32_t width,
                                                                  std::uint32_t height,
                                                                  GlyphRasterMode mode);
    void flush_pending_uploads();
    void flush_page_upload(std::size_t page_index);
    void invalidate_pages();

    std::string owner_token_;
    GlyphAtlasConfig config_;
    double display_scale_ = 1.0;
    SubpixelOrder subpixel_order_ = SubpixelOrder::rgb;
    std::uint64_t generation_ = 1U;
    std::vector<Page> pages_;
    std::map<Key, GlyphAtlasEntry> cache_;
    /** Pages still drawn from (retain_pages), and whether this preparation was given them. */
    std::unordered_set<std::string> retained_pages_;
    bool retention_known_ = false;
    /** Counts preparations, so a page used during this one is never reset under it. */
    std::uint64_t preparation_serial_ = 0U;
    std::vector<AtlasOperation> operations_;
    bool frame_preparation_active_ = false;
    bool reclamation_pending_ = false;
    std::uint32_t preparation_recycles_ = 0U;
    std::uint64_t generation_recycle_count_ = 0U;
    RasterWork raster_work_;
};

} // namespace strata::font
