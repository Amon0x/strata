#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "font/opentype.hpp"
#include "font/raster.hpp"

namespace strata::font {

enum class AtlasTextureFormat : std::uint8_t {
    r8,
    rgba8,
};

enum class AtlasOperationKind : std::uint8_t {
    create,
    upload,
    release,
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

/** Throwing preparation for an otherwise noexcept nonterminal host-resource invalidation. */
struct AtlasResourceInvalidationPlan final {
    std::vector<AtlasOperation> releases;
    std::uint64_t generation = 0U;
    std::size_t page_count = 0U;
    std::size_t operation_count = 0U;
};

struct GlyphAtlasConfig final {
    std::uint32_t page_size = 1'024U;
    std::uint32_t maximum_pages = 8U;
    std::uint32_t maximum_cached_glyphs = 4'096U;
    /** Bounds full-generation recycling work admitted during one explicit safe preparation phase. */
    std::uint32_t maximum_generation_recycles_per_preparation = 1U;
    std::uint32_t gutter_pixels = 1U;
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
};

/**
 * Per-surface atlas packer and host upload planner. Immutable raster bitmaps are process-shared by
 * parsed font identity; atlas coordinates, pages, resource ownership, and invalidation stay local.
 */
class GlyphAtlas final {
public:
    explicit GlyphAtlas(std::string_view owner_id, GlyphAtlasConfig config = {});
    ~GlyphAtlas();

    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;
    GlyphAtlas(GlyphAtlas&&) = delete;
    GlyphAtlas& operator=(GlyphAtlas&&) = delete;

    /** Invalidates scale-dependent coverage pages and plans explicit host releases. */
    void adopt_display_scale(double display_scale);
    /** Invalidates every page after a host resource reload and plans explicit host releases. */
    void invalidate_resources();
    /** Builds releases and reserves pending-operation capacity without changing logical state. */
    [[nodiscard]] AtlasResourceInvalidationPlan plan_resource_invalidation();
    /** Applies a matching prepared plan without allocation or failure. */
    void commit_resource_invalidation(AtlasResourceInvalidationPlan plan) noexcept;

    /**
     * Opens the frame-safe glyph preparation phase. If a prior submission hit capacity, old pages
     * are reclaimed here before any warmup requests. Returns true when the atlas generation moved.
     * A caller that caches atlas-backed geometry must discard it when this returns true or when
     * generation() changes during preparation.
     */
    [[nodiscard]] bool begin_frame_preparation();
    /** Seals the atlas against page recycling before atlas-backed geometry is published. */
    void end_frame_preparation() noexcept;

    [[nodiscard]] std::optional<GlyphAtlasEntry> request_coverage(
        std::string_view font_id,
        const OpenTypeFont& font,
        std::uint16_t glyph,
        double pixel_size,
        SubpixelPhase phase,
        std::uint32_t font_style_flags = 0U
    );

    /** Requests an explicit raster mode; ordinary callers default to size-specific grayscale. */
    [[nodiscard]] std::optional<GlyphAtlasEntry> request(
        std::string_view font_id,
        const OpenTypeFont& font,
        std::uint16_t glyph,
        double pixel_size,
        SubpixelPhase coverage_phase,
        std::uint32_t font_style_flags = 0U,
        GlyphRasterMode mode = GlyphRasterMode::coverage
    );

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

private:
    struct Key;
    struct Page;
    struct Allocation;

    [[nodiscard]] std::optional<Allocation> allocate(
        std::uint32_t width,
        std::uint32_t height,
        GlyphRasterMode mode
    );
    [[nodiscard]] Allocation create_page_and_allocate(
        std::uint32_t width,
        std::uint32_t height,
        GlyphRasterMode mode
    );
    [[nodiscard]] GlyphAtlasEntry insert(
        const Key& key,
        const GlyphRasterBitmap& bitmap,
        const Allocation& allocation
    );
    [[nodiscard]] GlyphAtlasEntry insert_bitmap(
        Key& key,
        const GlyphRasterBitmap& bitmap
    );
    [[nodiscard]] std::optional<GlyphAtlasEntry> request_mode(
        std::string_view font_id,
        const OpenTypeFont& font,
        std::uint16_t glyph,
        double pixel_size,
        SubpixelPhase phase,
        GlyphRasterMode mode,
        std::uint32_t font_style_flags
    );
    [[nodiscard]] bool recycle_for_capacity(Key& key);
    void flush_pending_uploads();
    void flush_page_upload(std::size_t page_index);
    void invalidate_pages();

    std::string owner_token_;
    GlyphAtlasConfig config_;
    double display_scale_ = 1.0;
    std::uint64_t generation_ = 1U;
    std::vector<Page> pages_;
    std::map<Key, GlyphAtlasEntry> cache_;
    std::vector<AtlasOperation> operations_;
    bool frame_preparation_active_ = false;
    bool reclamation_pending_ = false;
    std::uint32_t preparation_recycles_ = 0U;
    std::uint64_t generation_recycle_count_ = 0U;
};

} // namespace strata::font
